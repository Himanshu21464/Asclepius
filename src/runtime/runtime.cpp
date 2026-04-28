// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/runtime.hpp"

#include <nlohmann/json.hpp>
#include <sodium.h>
#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
#include <random>
#include <set>
#include <system_error>
#include <thread>
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

Result<LedgerEntry> Runtime::record_event(std::string event_type,
                                          std::string actor,
                                          nlohmann::json body,
                                          std::string tenant) {
    // Direct sugar over Ledger::append. The runtime adds no implicit
    // fields — sidecars use this to record their own operational
    // events ("config.reloaded", "shutdown.initiated") into the same
    // chain that carries the inference lifecycle, so a single audit
    // walk covers both. Forwards verbatim, errors propagate from the
    // backend.
    return impl_->ledger.append(std::move(event_type),
                                std::move(actor),
                                std::move(body),
                                std::move(tenant));
}

Result<void> Runtime::record_shutdown(const std::string& reason) {
    // Tombstone for a graceful shutdown. Body intentionally minimal —
    // operators replaying the chain already have ts and actor in the
    // header; we only carry reason and a redundant iso8601 timestamp
    // for human-readable audit reports that may not unpack the
    // header. Errors from the backend propagate so callers know
    // whether the tombstone landed; the runtime does not retry.
    nlohmann::json body;
    body["reason"] = reason;
    body["ts"]     = Time::now().iso8601();
    auto e = impl_->ledger.append("runtime.shutdown",
                                  std::string{"system:runtime"},
                                  std::move(body));
    if (!e) return e.error();
    return Result<void>::ok();
}

std::size_t Runtime::ledger_length() const noexcept {
    return static_cast<std::size_t>(impl_->ledger.length());
}

std::string Runtime::head_hash() const {
    return impl_->ledger.head().hex();
}

LedgerCheckpoint Runtime::self_attest() const {
    return impl_->ledger.checkpoint();
}

std::string Runtime::keystore_fingerprint() const {
    return impl_->ledger.attest().fingerprint;
}

std::vector<std::string> Runtime::policy_names() const {
    return impl_->policies.names();
}

bool Runtime::is_chain_empty() const noexcept {
    return impl_->ledger.length() == 0;
}

bool Runtime::is_idle(std::chrono::milliseconds threshold) const {
    // Empty chain is trivially idle — no committed work means no work
    // to wait on. Bounded read: tail(1) is one indexed lookup.
    if (impl_->ledger.length() == 0) {
        return true;
    }
    auto t = impl_->ledger.tail(1);
    if (!t || t.value().empty()) {
        // Backend hiccup — collapse to "not idle" so a graceful-shutdown
        // probe never lies about being safe. Diagnostics belong on
        // health(); this accessor is purely a yes/no gate.
        return false;
    }
    // tail() returns most-recent-first; the freshest entry is at front().
    const auto& last = t.value().front();
    auto since = Time::now() - last.header.ts;
    return since > std::chrono::duration_cast<std::chrono::nanoseconds>(threshold);
}

bool Runtime::is_busy(std::chrono::milliseconds threshold) const {
    // Pure negation of is_idle(threshold). Same backend cost (one
    // tail(1) lookup); same backend-hiccup collapse to a "safer"
    // verdict — there, "safer" was "not idle"; here, "safer" is
    // "not busy" (a sidecar that asks `is_busy()` is usually gating
    // a piece of work it would skip if uncertain about activity).
    return !is_idle(threshold);
}

bool Runtime::wait_for_quiet(std::chrono::milliseconds period,
                              std::chrono::milliseconds timeout) const {
    // Block until the runtime has been idle for at least `period`, or
    // until `timeout` expires. "Idle" here means: the most-recent
    // ledger entry's wall-clock timestamp is older than `period`, and
    // the chain length has not changed since the last poll. We poll
    // length() and tail(1) every 5 ms so cancellation latency stays
    // bounded; the same step matches wait_until_chain_grows for
    // consistency.
    //
    // Empty-chain handling: a chain with zero entries is trivially
    // quiet, but we still need `period` of wall time to elapse from
    // the call's start before declaring victory — otherwise a fresh
    // runtime would report "quiet" on the very first tick, which
    // breaks shutdown loops that expect to observe at least one
    // poll cycle. We capture the call-start time as the empty-chain
    // anchor.
    constexpr auto kStep = std::chrono::milliseconds{5};
    const auto t0       = std::chrono::steady_clock::now();
    const auto deadline = t0 + timeout;

    auto last_length = impl_->ledger.length();
    // Anchor for the "no new commits" window. Initially set to t0 so
    // the empty-chain case can satisfy the contract once `period` has
    // elapsed since the call started.
    auto last_change = t0;

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const auto cur_length = impl_->ledger.length();
        if (cur_length != last_length) {
            // A new entry landed since the last tick; restart the
            // quiet window from this moment.
            last_length = cur_length;
            last_change = now;
        }

        // Determine whether the runtime has been quiet long enough.
        // We use the steady_clock anchor (last_change) for the "no
        // change since" window so the comparison is monotonic and
        // immune to wall-clock skew. Additionally, we require the
        // most-recent entry's wall-clock ts (if any) to be older than
        // `period` — this defends against the case where a commit
        // landed during the brief gap between two polls and was then
        // immediately flushed by another, which steady_clock alone
        // would miss. Both conditions must hold.
        bool quiet_steady = (now - last_change) >= period;
        bool quiet_wall   = true;
        if (cur_length > 0) {
            auto t = impl_->ledger.tail(1);
            if (!t || t.value().empty()) {
                // Backend hiccup — collapse to "not quiet" so the
                // shutdown caller never tears down the runtime on a
                // false-positive.
                quiet_wall = false;
            } else {
                auto since_last_entry =
                    Time::now() - t.value().front().header.ts;
                quiet_wall = since_last_entry >=
                    std::chrono::duration_cast<std::chrono::nanoseconds>(period);
            }
        }
        if (quiet_steady && quiet_wall) return true;

        if (now >= deadline) return false;

        // Sleep until the next tick, but never past the deadline.
        const auto remaining = deadline - now;
        std::this_thread::sleep_for(remaining < kStep ? remaining : kStep);
    }
}

bool Runtime::wait_until_chain_grows(std::uint64_t min_seq,
                                     std::chrono::milliseconds timeout) const {
    // Polling wait. Fast-path the trivial "already satisfied" case
    // (including empty-chain + min_seq=0) so callers don't pay a
    // sleep on no-op waits. After that, sleep in 5 ms steps until
    // the deadline, re-reading length() on each tick. Polling beats
    // an event subscription here because the ledger's append path
    // already serializes concurrent writes — adding a cv would
    // require coordinating with backends that don't currently
    // expose one. 5 ms keeps wake-up cost low while bounding the
    // worst-case overshoot.
    if (impl_->ledger.length() >= min_seq) return true;
    constexpr auto kStep = std::chrono::milliseconds{5};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(kStep);
        if (impl_->ledger.length() >= min_seq) return true;
    }
    // Final read after the loop in case the deadline expired between
    // the last sleep and the predicate check.
    return impl_->ledger.length() >= min_seq;
}

std::string Runtime::generate_trace_id() const {
    // 8 random bytes → 16 hex chars. CSPRNG via libsodium; sodium_init
    // has been called by the time Runtime::open() returned (KeyStore
    // and the hashing helpers both initialise on first use, and the
    // ledger is opened during open()). Lowercase hex matches the
    // existing Hash::hex() vocabulary used elsewhere on the runtime.
    std::array<unsigned char, 8> buf{};
    randombytes_buf(buf.data(), buf.size());
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(buf.size() * 2);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        out[2 * i]     = kHex[(buf[i] >> 4) & 0xF];
        out[2 * i + 1] = kHex[ buf[i]       & 0xF];
    }
    return out;
}

Result<void> Runtime::install_default_policies() {
    impl_->policies.clear();
    impl_->policies.push(make_phi_scrubber());
    impl_->policies.push(make_length_limit(64 * 1024, 64 * 1024));
    return Result<void>::ok();
}

std::size_t Runtime::policy_count() const {
    // Trivial accessor — saves callers grabbing PolicyChain& just to
    // read its size. Mirrors ledger_length(): the same number is
    // surfaced by health() / system_summary(), but those construct
    // their own structs; this is the one-call path for sites that
    // only want the integer.
    return impl_->policies.size();
}

std::string Runtime::version() const {
#ifdef ASCLEPIUS_VERSION_STRING
    return std::string{ASCLEPIUS_VERSION_STRING};
#else
    return std::string{"0.0.0-dev"};
#endif
}

double Runtime::current_load_metric() const {
    // Inferences-per-second over the trailing 60 s window.
    // Approach: pull entries via tail_after_time(now - 60s, n) so we
    // bound the cost (n caps the in-memory copy) while letting the
    // backend filter on ts. We then count those whose event_type ==
    // "inference.committed" — tail_after_time returns every event
    // type, not just inferences, so the post-filter is needed. The
    // 60-second window divisor is fixed; partial windows (a runtime
    // that's been alive for < 60 s) still divide by 60.0 so the rate
    // gauge behaves continuously rather than reporting an inflated
    // "burst rate" against a fractional denominator.
    //
    // Empty-chain fast path: skip the read entirely. Backend errors
    // (denormalised tail, transient SQLite failure) collapse to 0.0 —
    // a load-balancer reading current_load_metric() prefers a
    // numeric "no load" verdict over an exception, matching the
    // documented contract.
    if (impl_->ledger.length() == 0) return 0.0;
    constexpr auto kWindow = std::chrono::seconds{60};
    auto cutoff = Time::now() - std::chrono::duration_cast<
        std::chrono::nanoseconds>(kWindow);
    // Cap the number of entries we pull back. A 60-second burst is
    // not expected to exceed a few thousand commits in practice; the
    // cap protects against pathological floods without losing
    // accuracy at typical loads. tail_after_time keeps memory
    // bounded at min(n, matches).
    constexpr std::size_t kCap = 100'000;
    auto entries = impl_->ledger.tail_after_time(cutoff, kCap);
    if (!entries) return 0.0;
    std::uint64_t commits = 0;
    for (const auto& e : entries.value()) {
        if (e.header.event_type == "inference.committed") ++commits;
    }
    return static_cast<double>(commits) / 60.0;
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

bool Runtime::is_healthy() const noexcept {
    // health() doesn't currently throw, but it's not declared noexcept;
    // a future addition (e.g. checking a SQLite WAL stat) might widen
    // the contract. We catch defensively so this accessor stays
    // truly noexcept for sidecar gates.
    try {
        return health().ok;
    } catch (...) {
        return false;
    }
}

bool Runtime::is_chain_well_formed() const {
    // verify() returns Result<void>; has_value() folds it into a bool.
    // Distinct from is_healthy(), which is broader and avoids the full
    // chain crypto walk. Use this when the caller specifically wants
    // "is the chain mathematically intact?" without handling the Error.
    return impl_->ledger.verify().has_value();
}

std::string Runtime::quick_status() const {
    if (impl_->ledger.length() == 0) {
        return std::string{"EMPTY"};
    }
    auto h = health();
    // U+00B7 MIDDLE DOT (UTF-8: 0xC2 0xB7) — same character as the
    // site's eyebrow separators.
    constexpr const char* kDot = " \xC2\xB7 ";
    std::string out;
    out.reserve(96);
    out += "OK";
    out += kDot;
    out += std::to_string(h.ledger_length);
    out += " entries";
    out += kDot;
    out += std::to_string(h.active_consent_tokens);
    out += " active consent";
    out += kDot;
    out += std::to_string(h.drift_features);
    out += " drift features";
    out += kDot;
    out += std::to_string(h.policy_count);
    out += " policies";
    return out;
}

std::string Runtime::quick_status_line() const {
    // Pure forwarder — quick_status() already returns a single-line
    // string (or "EMPTY" on a fresh chain). Some call sites read
    // more naturally as "give me the quick status line"; this saves
    // the synonym mismatch from biting log/banner code that expects
    // a "_line" suffix on its single-line accessors. No behavioural
    // divergence from quick_status().
    return quick_status();
}

std::string Runtime::status_line() const {
    // Single-line ASCII summary for /healthz text endpoints and CLI
    // banner modes. Mirrors quick_status()'s middle-dot separator
    // (U+00B7, UTF-8 0xC2 0xB7) — strictly, that makes this UTF-8
    // rather than 7-bit ASCII, but it matches the rest of the
    // runtime's shell-friendly summaries and renders in every
    // terminal we've seen. Pulls from version() + health(); never
    // errors. Built with reserve() to keep this allocation-light
    // for /healthz hot paths.
    auto h = health();
    constexpr const char* kDot = " \xC2\xB7 ";
    std::string out;
    out.reserve(160);
    out += "asclepius v";
    out += version();
    out += kDot;
    out += "ledger=";
    out += std::to_string(h.ledger_length);
    out += kDot;
    out += "key=";
    out += h.ledger_key_id;
    out += kDot;
    out += "policies=";
    out += std::to_string(h.policy_count);
    out += kDot;
    out += "drift_features=";
    out += std::to_string(h.drift_features);
    out += kDot;
    out += "active_consent=";
    out += std::to_string(h.active_consent_tokens);
    return out;
}

std::string Runtime::summary_string() const {
    auto h = health();
    // Truncate the head hex to 12 chars for human comparison; the
    // full hex is available on Health::ledger_head_hex if a caller
    // needs it. The string is built line-by-line so it reads cleanly
    // when written to a log or a tty.
    std::string head12 = h.ledger_head_hex.size() > 12
                             ? h.ledger_head_hex.substr(0, 12)
                             : h.ledger_head_hex;
    std::string out;
    out.reserve(256);
    out += "asclepius runtime ";
    out += version();
    out += '\n';
    out += "  ledger length: ";
    out += std::to_string(h.ledger_length);
    out += '\n';
    out += "  ledger head: ";
    out += head12;
    out += '\n';
    out += "  signing key id: ";
    out += h.ledger_key_id;
    out += '\n';
    out += "  signing fingerprint: ";
    out += keystore_fingerprint();
    out += '\n';
    out += "  active consent tokens: ";
    out += std::to_string(h.active_consent_tokens);
    out += '\n';
    out += "  drift features: ";
    out += std::to_string(h.drift_features);
    out += '\n';
    out += "  policies: ";
    out += std::to_string(h.policy_count);
    return out;
}

std::string Runtime::signing_key_id() const {
    return impl_->ledger.key_id();
}

bool Runtime::has_consent_for(const PatientId& patient, Purpose purpose) const {
    return impl_->consent.permits(patient, purpose).value_or(false);
}

bool Runtime::can_serve(const PatientId& patient, Purpose purpose) const {
    // Today this mirrors has_consent_for() — consent is the only gate
    // begin_inference() applies once the spec is well-formed. Kept as
    // a separate method so future gates (rate-limiting, model registry
    // lookup, tenant allowlist) can widen this without churning every
    // pre-flight call site.
    return has_consent_for(patient, purpose);
}

Result<std::vector<LedgerEntry>>
Runtime::recent_inferences(std::size_t n) const {
    if (n == 0) return std::vector<LedgerEntry>{};
    return impl_->ledger.tail_by_event_type("inference.committed", n);
}

std::vector<LedgerEntry> Runtime::recent_drift_events(std::size_t n) const {
    if (n == 0) return {};
    auto r = impl_->ledger.tail_by_event_type("drift.crossed", n);
    if (!r) return {};
    return std::move(r).value();
}

Result<std::vector<std::string>>
Runtime::list_recent_event_types(std::size_t window_ms) const {
    // Distinct event_types appearing in entries within the last
    // `window_ms`, sorted alphabetically. Empty chain or empty window
    // → empty vector. We pull via tail_after_time so the cost stays
    // bounded by matches in the window, not the full chain length.
    // The cap matches current_load_metric()'s defensive ceiling
    // (100k recent entries) — pathological floods saturate at the
    // cap rather than burning unbounded memory.
    if (impl_->ledger.length() == 0) return std::vector<std::string>{};
    constexpr std::size_t kCap = 100'000;
    auto cutoff = Time::now() - std::chrono::duration_cast<
        std::chrono::nanoseconds>(std::chrono::milliseconds{window_ms});
    auto entries = impl_->ledger.tail_after_time(cutoff, kCap);
    if (!entries) return entries.error();
    std::set<std::string> seen;
    for (const auto& e : entries.value()) {
        seen.insert(e.header.event_type);
    }
    return std::vector<std::string>{seen.begin(), seen.end()};
}

Result<std::uint64_t>
Runtime::failure_count_in_window(std::chrono::milliseconds window) const {
    // Count of inference.committed entries whose body's `status`
    // field is anything other than "ok". Used by /healthz to surface
    // a recent error-rate gauge. We pull via tail_after_time and
    // post-filter by event_type + status — a string-search for the
    // canonical-JSON `"status":"ok"` substring is sound (the body is
    // canonical JSON with no whitespace) and dramatically cheaper
    // than json::parse() on each entry. Empty chain trivially
    // returns 0; backend errors propagate.
    if (impl_->ledger.length() == 0) return std::uint64_t{0};
    constexpr std::size_t kCap = 100'000;
    auto cutoff = Time::now() - std::chrono::duration_cast<
        std::chrono::nanoseconds>(window);
    auto entries = impl_->ledger.tail_after_time(cutoff, kCap);
    if (!entries) return entries.error();
    std::uint64_t failures = 0;
    static constexpr const char* kOk = "\"status\":\"ok\"";
    for (const auto& e : entries.value()) {
        if (e.header.event_type != "inference.committed") continue;
        // status==ok ⇒ not a failure; anything else (blocked.*,
        // model_error, timeout, cancelled, missing) ⇒ a failure.
        // We search for the canonical-JSON spelling rather than
        // parsing the body — the substrate writes canonical JSON, so
        // the substring match is unambiguous.
        if (e.body_json.find(kOk) == std::string::npos) {
            ++failures;
        }
    }
    return failures;
}

void Runtime::warm_caches() {
    // Placeholder. Future implementations may pre-touch the SQLite page
    // cache (e.g. a single-row SELECT against the head), pre-resolve
    // policy objects, or eagerly load the signing key. Callers must
    // treat this as a hint, not a contract — there is no guarantee
    // that the next request will be faster, and no error path. Safe
    // to call from any thread; safe to call repeatedly.
}

std::chrono::nanoseconds Runtime::ledger_age() const {
    if (impl_->ledger.length() == 0) {
        return std::chrono::nanoseconds::zero();
    }
    auto first = impl_->ledger.at(1);
    if (!first) {
        // Diagnostics belong on health(); this accessor is best-effort.
        return std::chrono::nanoseconds::zero();
    }
    return Time::now() - first.value().header.ts;
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

std::size_t Runtime::counter_total() const {
    return static_cast<std::size_t>(impl_->metrics.counter_total());
}

std::uint64_t Runtime::counter(std::string_view name) const {
    // One-call sugar over MetricRegistry::count. Same missing-counter
    // semantics as the underlying call (returns 0 silently for both
    // 0-valued and missing names); callers who need to disambiguate
    // can fall back to metrics().counter_value() through the
    // accessor. Matches the framing used by the other one-call
    // accessors on this surface — head_hash(), keystore_fingerprint()
    // — that just save the caller from grabbing a sub-component
    // reference.
    return impl_->metrics.count(name);
}

Result<std::size_t> Runtime::flush_consent_to_metrics() {
    // Materialize consent state into Prometheus-shaped counters. Unlike
    // flush_drift_to_metrics() (which is INC-shaped — each call adds
    // one), the consent counters are SET-shaped: callers want the
    // current absolute counts to land on the metric, not a running
    // total. MetricRegistry doesn't expose set(); the workaround is
    // reset(name) followed by inc(name, value). reset() returns
    // not_found on a never-touched counter — that's fine on the first
    // call. We use a small helper lambda to keep the four writes
    // uniform; if any single write would surface a backend error in
    // a future MetricRegistry revision, surface it as the registry
    // does (today reset() is the only fallible path and only fails on
    // first-touch with not_found, which we tolerate).
    auto& m = impl_->metrics;
    auto put = [&](std::string_view name, std::uint64_t value) {
        // Reset is only meaningful when the counter already exists; on
        // a fresh counter we want the create-on-first-use behaviour
        // of inc(), so swallow not_found here and fall through.
        (void)m.reset(name);
        if (value > 0) {
            m.inc(name, value);
        } else {
            // inc(name, 0) would still register the counter, but we
            // want a zero-valued counter to land. Calling inc with
            // delta=0 is the cheapest way to ensure the name is
            // registered without changing the value (after a reset
            // it stays at 0).
            m.inc(name, 0);
        }
    };
    const auto& cr = impl_->consent;
    put("consent.tokens.active",   static_cast<std::uint64_t>(cr.active_count()));
    put("consent.tokens.total",    static_cast<std::uint64_t>(cr.total_count()));
    put("consent.tokens.expired",  static_cast<std::uint64_t>(cr.expired_count()));
    put("consent.patients.active",
        static_cast<std::uint64_t>(cr.patients_with_active_count()));
    return std::size_t{4};
}

std::uint64_t Runtime::current_seq() const {
    // Sugar over `ledger().length()`. ledger_length() returns size_t
    // (clamped to architecture width); current_seq returns uint64_t
    // to match the LedgerEntry::Header::seq type so callers using
    // the value as a watch cursor or queue metadata don't have to
    // static_cast at every site. Same backend cost as ledger_length().
    return impl_->ledger.length();
}

Result<std::uint64_t> Runtime::flush_drift_to_metrics() {
    // Materialize current drift state into the MetricRegistry by
    // emitting one counter per (feature, severity) pair, named
    // "drift.severity.<feature>.<severity>". Used by sidecars that
    // expose drift through the same Prometheus path as the rest of
    // the runtime metrics: rather than tracking severity transitions
    // on the alert sink (already done — see the drift_to_ledger
    // bridge), this produces a snapshot counter the scrape endpoint
    // can render. Each call increments by 1; readers care about
    // deltas, not absolute values, so a fresh deploy that reads the
    // counters periodically gets a faithful picture without stale
    // history. Returns the number of features flushed; never errors
    // today, but kept Result-shaped so future alerting / mode
    // changes (e.g. a feature_severity backend that can fail) can
    // surface them.
    std::uint64_t flushed = 0;
    auto features = impl_->drift.list_features();
    for (const auto& name : features) {
        auto sev = impl_->drift.feature_severity(name);
        if (!sev) {
            // A registered feature must always resolve to a severity;
            // if it doesn't, the registry is in a broken state and we
            // surface that rather than silently skipping. Diagnostics
            // belong on health(); this path mirrors reset_metrics()
            // — short-circuit the first failure.
            return sev.error();
        }
        std::string counter_name;
        counter_name.reserve(32 + name.size());
        counter_name += "drift.severity.";
        counter_name += name;
        counter_name += '.';
        counter_name += to_string(sev.value());
        impl_->metrics.inc(counter_name);
        ++flushed;
    }
    return flushed;
}

Result<Ledger::Subscription>
Runtime::subscribe_logging(std::function<void(const LedgerEntry&)> sink) {
    // Sugar over Ledger::subscribe(...) — the runtime adds nothing
    // beyond the underlying contract; the wrapper exists so call
    // sites that want a logging hook read as "subscribe a logger" at
    // the top level rather than drilling through ledger() first.
    // The Subscription is move-only and owns the registration; when
    // it goes out of scope the callback is unregistered. Returns
    // Result so future runtime-level gates (e.g. shutdown state, sink
    // sanity checks) can surface as Errors without churning the
    // call site; today it's infallible.
    if (!sink) {
        return Error::invalid("subscribe_logging: sink must be non-null");
    }
    return impl_->ledger.subscribe(std::move(sink));
}

std::string Runtime::env_summary() const {
    nlohmann::json j;
#ifdef ASCLEPIUS_VERSION_STRING
    j["asclepius"] = std::string{ASCLEPIUS_VERSION_STRING};
#else
    j["asclepius"] = std::string{"0.0.0-dev"};
#endif
    // libsodium runtime version — function returns a static string.
    j["libsodium"] = std::string{sodium_version_string()};
    // sqlite header version — macro from sqlite3.h.
    j["sqlite"] = std::string{SQLITE_VERSION};
    // C++ standard. __cplusplus values: 201703L=C++17, 202002L=C++20,
    // 202302L=C++23. Surface the raw long for forward-compat.
    j["cpp_standard"] = static_cast<long>(__cplusplus);
    // Compiler. Order matters: clang defines __GNUC__ for compatibility,
    // so check __clang__ first.
#if defined(__clang__)
    j["compiler"] = std::string{"clang"};
#elif defined(__GNUC__)
    j["compiler"] = std::string{"g++"};
#else
    j["compiler"] = std::string{"unknown"};
#endif
    return j.dump();
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
