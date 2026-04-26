// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/runtime.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <random>
#include <unordered_set>

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
    rt.impl_->install_drift_to_ledger_bridge();
    if (auto r = rt.impl_->install_consent_lifecycle(); !r) return r.error();
    return rt;
}

Result<Runtime> Runtime::open(std::filesystem::path db_path, KeyStore key) {
    auto led = Ledger::open(std::move(db_path), std::move(key));
    if (!led) return led.error();
    Runtime rt;
    rt.impl_ = std::make_unique<Impl>(std::move(led).value());
    rt.impl_->install_drift_to_ledger_bridge();
    if (auto r = rt.impl_->install_consent_lifecycle(); !r) return r.error();
    return rt;
}

Result<Runtime> Runtime::open_uri(const std::string& uri) {
    auto led = Ledger::open_uri(uri);
    if (!led) return led.error();
    Runtime rt;
    rt.impl_ = std::make_unique<Impl>(std::move(led).value());
    rt.impl_->install_drift_to_ledger_bridge();
    if (auto r = rt.impl_->install_consent_lifecycle(); !r) return r.error();
    return rt;
}

Result<Runtime> Runtime::open_uri(const std::string& uri, KeyStore key) {
    auto led = Ledger::open_uri(uri, std::move(key));
    if (!led) return led.error();
    Runtime rt;
    rt.impl_ = std::make_unique<Impl>(std::move(led).value());
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

}  // namespace asclepius
