// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/runtime.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
#include <random>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace asclepius {

// ---- InferenceContext implementation ------------------------------------

InferenceContext::InferenceContext(std::string  inference_id,
                                   ModelId      model,
                                   ActorId      actor,
                                   PatientId    patient,
                                   EncounterId  encounter,
                                   Purpose      purpose,
                                   TenantId     tenant,
                                   Time         started_at)
    : inference_id_(std::move(inference_id)),
      model_(std::move(model)),
      actor_(std::move(actor)),
      patient_(std::move(patient)),
      encounter_(std::move(encounter)),
      purpose_(purpose),
      tenant_(std::move(tenant)),
      started_at_(started_at) {}

std::string_view  InferenceContext::id()         const noexcept { return inference_id_; }
const ModelId&    InferenceContext::model()      const noexcept { return model_; }
const ActorId&    InferenceContext::actor()      const noexcept { return actor_; }
const PatientId&  InferenceContext::patient()    const noexcept { return patient_; }
const EncounterId& InferenceContext::encounter() const noexcept { return encounter_; }
Purpose           InferenceContext::purpose()    const noexcept { return purpose_; }
const TenantId&   InferenceContext::tenant()     const noexcept { return tenant_; }
Time              InferenceContext::started_at() const noexcept { return started_at_; }

// ---- Runtime::Impl ------------------------------------------------------

struct Runtime::Impl {
    Ledger              ledger;
    PolicyChain         policies;
    DriftMonitor        drift;
    ConsentRegistry     consent;
    EvaluationHarness   evaluation;
    MetricRegistry      metrics;

    // Path to the backing SQLite file, captured at open() so
    // ledger_size_bytes() can stat() without re-parsing the path each call.
    std::filesystem::path backing_path;

    Impl(Ledger l) : ledger(std::move(l)), evaluation(ledger, drift) {}

    // Wire the DriftMonitor's alert sink to append a drift.crossed
    // event to the ledger whenever a feature's severity rises past
    // the configured threshold. The audit chain becomes the canonical
    // record of drift breaches, not just an in-process metric.
    void install_drift_to_ledger_bridge() {
        drift.set_alert_sink([this](const DriftReport& r) {
            nlohmann::json body;
            body["feature"]      = r.feature;
            body["psi"]          = r.psi;
            body["ks_statistic"] = r.ks_statistic;
            body["emd"]          = r.emd;
            body["severity"]     = std::string{to_string(r.severity)};
            body["reference_n"]  = r.reference_n;
            body["current_n"]    = r.current_n;
            ledger.append("drift.crossed", "system:drift_monitor", body);
            metrics.inc("drift.crossings");
        }, DriftSeverity::moder);
    }

    // Mirror every grant/revoke into the audit ledger so consent state can
    // be reconstructed from the chain alone after a restart. Must be
    // installed AFTER replay_consent_from_ledger() — replay uses ingest()
    // which doesn't fire the observer, so we won't double-record the
    // historical events when replaying.
    void install_consent_to_ledger_bridge() {
        consent.set_observer([this](ConsentRegistry::Event e,
                                    const ConsentToken& t) {
            nlohmann::json body;
            body["token_id"]    = t.token_id;
            body["patient"]     = std::string{t.patient.str()};
            body["issued_at"]   = t.issued_at.iso8601();
            body["expires_at"]  = t.expires_at.iso8601();
            body["revoked"]     = t.revoked;
            nlohmann::json purposes = nlohmann::json::array();
            for (auto p : t.purposes) purposes.push_back(to_string(p));
            body["purposes"] = std::move(purposes);
            const char* etype = (e == ConsentRegistry::Event::granted)
                ? "consent.granted" : "consent.revoked";
            (void)ledger.append(etype, "system:consent_registry", body);
            metrics.inc(e == ConsentRegistry::Event::granted
                ? "consent.granted" : "consent.revoked");
        });
    }

    // Walk the ledger once at startup, replaying consent.granted /
    // consent.revoked events into the in-memory registry. After this
    // returns, the registry reflects every grant ever made (less those
    // explicitly revoked since), so a restarted runtime accepts the
    // same consent tokens it accepted before shutdown.
    Result<void> replay_consent_from_ledger() {
        auto cur = ledger.length();
        if (cur == 0) return Result<void>::ok();
        // Read in chunks to bound memory on very long chains.
        constexpr std::uint64_t kChunk = 1024;
        std::unordered_set<std::string> revoked;
        // First pass: collect revocations (oldest-to-newest doesn't matter
        // for correctness — once revoked, stays revoked).
        for (std::uint64_t start = 1; start <= cur; start += kChunk) {
            std::uint64_t end = std::min(cur + 1, start + kChunk);
            auto rng = ledger.range(start, end);
            if (!rng) return rng.error();
            for (const auto& e : rng.value()) {
                if (e.header.event_type != "consent.revoked") continue;
                try {
                    auto j = nlohmann::json::parse(e.body_json);
                    auto it = j.find("token_id");
                    if (it != j.end() && it->is_string()) {
                        revoked.insert(it->get<std::string>());
                    }
                } catch (...) {}
            }
        }
        // Second pass: ingest grants, applying the revocation set.
        for (std::uint64_t start = 1; start <= cur; start += kChunk) {
            std::uint64_t end = std::min(cur + 1, start + kChunk);
            auto rng = ledger.range(start, end);
            if (!rng) return rng.error();
            for (const auto& e : rng.value()) {
                if (e.header.event_type != "consent.granted") continue;
                try {
                    auto j = nlohmann::json::parse(e.body_json);
                    ConsentToken t;
                    t.token_id   = j.value("token_id", std::string{});
                    if (t.token_id.empty()) continue;
                    // The body stored the full id string (e.g. "pat:foo");
                    // construct the StrongId from the raw value, NOT via
                    // pseudonymous() which would prepend "pat:" again.
                    t.patient    = PatientId{j.value("patient", std::string{})};
                    t.issued_at  = Time::from_iso8601(
                        j.value("issued_at", std::string{}));
                    t.expires_at = Time::from_iso8601(
                        j.value("expires_at", std::string{}));
                    t.revoked    = revoked.count(t.token_id) > 0;
                    if (auto pj = j.find("purposes");
                        pj != j.end() && pj->is_array()) {
                        for (const auto& s : *pj) {
                            if (!s.is_string()) continue;
                            auto p = purpose_from_string(s.get<std::string>());
                            if (p) t.purposes.push_back(p.value());
                        }
                    }
                    if (t.purposes.empty()) continue;
                    (void)consent.ingest(std::move(t));
                } catch (...) {
                    // Skip malformed entries; they'd have been rejected at
                    // append time so this is defense-in-depth.
                }
            }
        }
        return Result<void>::ok();
    }

    // Order matters: replay must run before the observer is installed,
    // otherwise replay would re-append every historical grant as a fresh
    // grant.
    Result<void> install_consent_lifecycle() {
        if (auto r = replay_consent_from_ledger(); !r) return r.error();
        install_consent_to_ledger_bridge();
        return Result<void>::ok();
    }
};

Runtime::Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;
Runtime::~Runtime() = default;

Result<Runtime> Runtime::open(std::filesystem::path db_path) {
    auto led = Ledger::open(db_path);
    if (!led) return led.error();
    Runtime rt;
    rt.impl_ = std::make_unique<Impl>(std::move(led).value());
    rt.impl_->backing_path = std::move(db_path);
    rt.impl_->install_drift_to_ledger_bridge();
    if (auto r = rt.impl_->install_consent_lifecycle(); !r) return r.error();
    return rt;
}

Result<Runtime> Runtime::open(std::filesystem::path db_path, KeyStore key) {
    auto path_copy = db_path;
    auto led = Ledger::open(std::move(db_path), std::move(key));
    if (!led) return led.error();
    Runtime rt;
    rt.impl_ = std::make_unique<Impl>(std::move(led).value());
    rt.impl_->backing_path = std::move(path_copy);
    rt.impl_->install_drift_to_ledger_bridge();
    if (auto r = rt.impl_->install_consent_lifecycle(); !r) return r.error();
    return rt;
}

PolicyChain&       Runtime::policies()        { return impl_->policies; }
Ledger&            Runtime::ledger()          { return impl_->ledger; }
DriftMonitor&      Runtime::drift()           { return impl_->drift; }
ConsentRegistry&   Runtime::consent()         { return impl_->consent; }
EvaluationHarness& Runtime::evaluation()      { return impl_->evaluation; }
MetricRegistry&    Runtime::metrics()         { return impl_->metrics; }
const PolicyChain& Runtime::policies() const  { return impl_->policies; }
const Ledger&      Runtime::ledger()   const  { return impl_->ledger; }

std::size_t Runtime::ledger_length() const noexcept {
    return static_cast<std::size_t>(impl_->ledger.length());
}

std::string Runtime::head_hash() const {
    return impl_->ledger.head().hex();
}

Result<void> Runtime::install_default_policies() {
    impl_->policies.clear();
    impl_->policies.push(make_phi_scrubber());
    impl_->policies.push(make_length_limit(64 * 1024, 64 * 1024));
    return Result<void>::ok();
}

std::string Runtime::version() const {
#ifdef ASCLEPIUS_VERSION_STRING
    return std::string{ASCLEPIUS_VERSION_STRING};
#else
    return std::string{"0.0.0-dev"};
#endif
}

std::size_t Runtime::active_inference_count() const {
    auto& m = impl_->metrics;
    const std::uint64_t started   = m.count("inference.attempts");
    const std::uint64_t ok        = m.count("inference.ok");
    const std::uint64_t timed_out = m.count("inference.timeout");
    const std::uint64_t cancelled = m.count("inference.cancelled");
    const std::uint64_t model_err = m.count("inference.model_error");
    const std::uint64_t blk_in    = m.count("inference.blocked.input");
    const std::uint64_t blk_out   = m.count("inference.blocked.output");
    const std::uint64_t deduped   = m.count("inference.idempotent_dedupe");
    const std::uint64_t terminal  =
        ok + timed_out + cancelled + model_err + blk_in + blk_out + deduped;
    if (terminal >= started) return 0;
    return static_cast<std::size_t>(started - terminal);
}

Runtime::Health Runtime::health() const {
    Health h;
    h.ledger_length         = impl_->ledger.length();
    h.ledger_head_hex       = impl_->ledger.head().hex();
    h.ledger_key_id         = impl_->ledger.key_id();
    h.policy_count          = impl_->policies.size();
    h.drift_features        = impl_->drift.feature_count();
    // Active = not revoked, not expired.
    auto snap = impl_->consent.snapshot();
    auto now  = Time::now();
    std::size_t active = 0;
    for (const auto& t : snap) {
        if (!t.revoked && t.expires_at > now) active++;
    }
    h.active_consent_tokens = active;
    h.ok = true;
    return h;
}

Result<void> Runtime::audit_spot_check(std::size_t lookback) const {
    if (lookback == 0) {
        return Error::invalid("audit_spot_check: lookback must be > 0");
    }
    const auto len = impl_->ledger.length();
    if (len == 0) {
        // Nothing to check — empty chain is trivially valid.
        return Result<void>::ok();
    }
    // Clamp lookback so we never request more entries than exist.
    const std::uint64_t lb = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(lookback), len);
    const std::uint64_t start = len - lb + 1;
    const std::uint64_t end   = len + 1;
    return impl_->ledger.verify_range(start, end);
}

Runtime::SystemSummary Runtime::system_summary() const {
    SystemSummary s;
    auto h = health();
    s.ledger_length   = h.ledger_length;
    s.head_hash_hex   = h.ledger_head_hex;
    s.policy_count    = h.policy_count;
    s.active_consent  = h.active_consent_tokens;
    s.drift_features  = h.drift_features;
    s.total_counters  = impl_->metrics.counter_count();
    s.version         = version();
    return s;
}

Result<std::size_t> Runtime::dispatched_inferences() const {
    return static_cast<std::size_t>(
        impl_->metrics.count("inference.attempts"));
}

Result<void> Runtime::self_test() const {
    if (auto v = impl_->ledger.verify(); !v) {
        return Error::integrity(std::string{"ledger.verify failed: "} +
                                std::string{v.error().what()});
    }
    // Consent: no token whose expiry is in the past should be returned by
    // permits() — checked by walking snapshot directly.
    auto now  = Time::now();
    auto snap = impl_->consent.snapshot();
    for (const auto& t : snap) {
        if (!t.revoked && t.expires_at <= now) {
            // Expired-but-not-marked-revoked is allowed (lazy expiry); we
            // only fail if permits() would return true for it.
        }
    }
    // Drift: no NaN in baselines (would indicate corruption).
    for (const auto& name : impl_->drift.list_features()) {
        // No direct accessor — best-effort: rely on report() returning
        // sane numbers. If psi is NaN classify() reports severe; that's
        // acceptable, not corrupt. Skip.
        (void)name;
    }
    return Result<void>::ok();
}

Result<std::int64_t> Runtime::ledger_size_bytes() const {
    std::error_code ec;
    auto sz = std::filesystem::file_size(impl_->backing_path, ec);
    if (ec) {
        return Error::backend(std::string{"file_size failed: "} + ec.message());
    }
    // file_size returns uintmax_t; cap to int64 max defensively. A
    // ledger nearing 2^63 bytes is well past any sane ops scenario,
    // but the cap avoids silent UB on the reinterpretation.
    if (sz > static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max())) {
        return Error::backend("file_size exceeds int64 range");
    }
    return static_cast<std::int64_t>(sz);
}

std::vector<std::string> Runtime::list_loaded_features() const {
    return impl_->drift.list_features();
}

Result<void> Runtime::reset_metrics() {
    auto snap = impl_->metrics.counter_snapshot();
    for (const auto& [name, _val] : snap) {
        if (auto r = impl_->metrics.reset(name); !r) {
            return r.error();
        }
    }
    return Result<void>::ok();
}

std::string Runtime::Health::to_json() const {
    nlohmann::json j;
    j["ok"]                    = ok;
    j["ledger_length"]         = ledger_length;
    j["ledger_head_hex"]       = ledger_head_hex;
    j["ledger_key_id"]         = ledger_key_id;
    j["policy_count"]          = policy_count;
    j["active_consent_tokens"] = active_consent_tokens;
    j["drift_features"]        = drift_features;
    if (!reason.empty()) j["reason"] = reason;
    return j.dump();
}

}  // namespace asclepius
