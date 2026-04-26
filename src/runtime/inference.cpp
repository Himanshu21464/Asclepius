// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/runtime.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>

namespace asclepius {

namespace {

std::string mint_inference_id() {
    using namespace std::chrono;
    static std::atomic<std::uint64_t> counter{0};
    auto t = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    auto n = counter.fetch_add(1);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "inf_%016llx_%08x",
                  static_cast<unsigned long long>(t),
                  static_cast<unsigned int>(n));
    return std::string{buf};
}

}  // namespace

// ---- Inference::Impl ----------------------------------------------------

struct Inference::Impl {
    Runtime*           rt   = nullptr;
    InferenceContext   ctx;
    std::string        consent_token_id;
    nlohmann::json     ledger_body = nlohmann::json::object();
    bool               committed   = false;
    bool               completed   = false;

    Impl(Runtime* runtime, InferenceContext c, std::string token)
        : rt(runtime), ctx(std::move(c)), consent_token_id(std::move(token)) {}
};

Inference::Inference(std::unique_ptr<Impl> i) : impl_(std::move(i)) {}
Inference::Inference(Inference&&) noexcept = default;
Inference& Inference::operator=(Inference&&) noexcept = default;

Inference::~Inference() {
    // If we ran but never committed, record an aborted-inference event.
    if (impl_ && impl_->completed && !impl_->committed) {
        nlohmann::json b;
        b["inference_id"] = std::string{impl_->ctx.id()};
        b["model"]        = std::string(impl_->ctx.model().str());
        b["actor"]        = std::string(impl_->ctx.actor().str());
        b["patient"]      = std::string(impl_->ctx.patient().str());
        b["encounter"]    = std::string(impl_->ctx.encounter().str());
        b["purpose"]      = to_string(impl_->ctx.purpose());
        b["status"]       = "aborted";
        // Best-effort log; ignore errors during destruction.
        (void)impl_->rt->ledger().append("inference.aborted",
                                          std::string(impl_->ctx.actor().str()),
                                          std::move(b));
    }
}

const InferenceContext& Inference::ctx() const noexcept { return impl_->ctx; }

Result<std::string> Inference::run(std::string input, const ModelCallback& model_call) {
    if (impl_->completed) {
        return Error::invalid("inference already run");
    }
    impl_->completed = true;

    auto& metrics = impl_->rt->metrics();
    metrics.inc("inference.attempts");

    // Pre-guard.
    auto in_v = impl_->rt->policies().evaluate_input(impl_->ctx, std::move(input));
    if (!in_v) {
        metrics.inc("inference.blocked.input");
        impl_->ledger_body["status"]     = "blocked.input";
        impl_->ledger_body["block_code"] = to_string(in_v.error().code());
        impl_->ledger_body["block_msg"]  = std::string{in_v.error().what()};
        return in_v.error();
    }

    // Hash the (post-guard) input for the audit body. We do not store the
    // input itself by default — only its hash — to keep PHI out of the
    // ledger. Callers that want full input retention can attach the raw
    // text in the ledger body via a custom hook (future API).
    const std::string post_input = std::move(in_v).value();
    auto in_hash = hash(post_input);

    // Model call.
    auto out_r = model_call(post_input);
    if (!out_r) {
        metrics.inc("inference.model_error");
        impl_->ledger_body["status"]    = "model_error";
        impl_->ledger_body["error_msg"] = std::string{out_r.error().what()};
        return out_r.error();
    }

    // Post-guard.
    auto out_v = impl_->rt->policies().evaluate_output(impl_->ctx, std::move(out_r).value());
    if (!out_v) {
        metrics.inc("inference.blocked.output");
        impl_->ledger_body["status"]     = "blocked.output";
        impl_->ledger_body["block_code"] = to_string(out_v.error().code());
        impl_->ledger_body["block_msg"]  = std::string{out_v.error().what()};
        return out_v.error();
    }

    const std::string post_output = std::move(out_v).value();
    auto out_hash = hash(post_output);

    impl_->ledger_body["status"]      = "ok";
    impl_->ledger_body["input_hash"]  = in_hash.hex();
    impl_->ledger_body["output_hash"] = out_hash.hex();
    auto latency_ns = (Time::now() - impl_->ctx.started_at()).count();
    impl_->ledger_body["latency_ns"] = latency_ns;

    metrics.inc("inference.ok");
    // Record latency in seconds into a histogram so operators get
    // distribution data (p50, p95, p99) directly in Prometheus.
    metrics.observe("inference_latency_seconds",
                    static_cast<double>(latency_ns) / 1e9);
    return post_output;
}

Result<void> Inference::commit() {
    if (!impl_->completed) {
        return Error::invalid("inference.commit called before run");
    }
    if (impl_->committed) {
        return Result<void>::ok();
    }
    nlohmann::json body         = impl_->ledger_body;
    body["inference_id"]        = std::string{impl_->ctx.id()};
    body["model"]               = std::string(impl_->ctx.model().str());
    body["actor"]               = std::string(impl_->ctx.actor().str());
    body["patient"]             = std::string(impl_->ctx.patient().str());
    body["encounter"]           = std::string(impl_->ctx.encounter().str());
    body["purpose"]             = to_string(impl_->ctx.purpose());
    body["consent_token_id"]    = impl_->consent_token_id;
    body["started_at"]          = impl_->ctx.started_at().iso8601();

    auto e = impl_->rt->ledger().append("inference.committed",
                                         std::string(impl_->ctx.actor().str()),
                                         std::move(body),
                                         std::string(impl_->ctx.tenant().str()));
    if (!e) return e.error();
    impl_->committed = true;
    return Result<void>::ok();
}

Result<void> Inference::capture_override(std::string rationale, nlohmann::json corrected) {
    if (!impl_->committed) {
        // Allow override capture even before commit; we'll auto-commit on
        // the assumption the caller meant to keep the inference visible.
        auto c = commit();
        if (!c) return c.error();
    }
    OverrideEvent ev;
    ev.inference_id = std::string{impl_->ctx.id()};
    ev.rationale    = std::move(rationale);
    ev.corrected    = std::move(corrected);
    ev.clinician    = impl_->ctx.actor();
    ev.occurred_at  = Time::now();
    return impl_->rt->evaluation().capture_override(std::move(ev));
}

Result<void> Inference::observe_drift(std::string_view feature, double value) {
    return impl_->rt->drift().observe(feature, value);
}

// ---- Runtime::begin_inference -------------------------------------------

Result<Inference> Runtime::begin_inference(InferenceSpec spec) {
    if (spec.model.empty() || spec.actor.empty() ||
        spec.patient.empty() || spec.encounter.empty()) {
        return Error::invalid("InferenceSpec requires model, actor, patient, encounter");
    }

    // Consent check: either a token id is supplied and we validate it, or we
    // look for any non-revoked, non-expired token granting the purpose.
    auto& cr = this->consent();
    bool  permitted = false;

    if (spec.consent_token_id) {
        auto t = cr.get(*spec.consent_token_id);
        if (!t) return t.error();
        const auto& tok = t.value();
        if (tok.revoked)                   return Error{ErrorCode::consent_expired, "token revoked"};
        if (tok.expires_at <= Time::now()) return Error{ErrorCode::consent_expired, "token expired"};
        if (tok.patient != spec.patient)
            return Error::denied("token patient mismatch");
        for (auto p : tok.purposes) if (p == spec.purpose) { permitted = true; break; }
        if (!permitted) return Error::denied("token does not permit this purpose");
    } else {
        auto allowed = cr.permits(spec.patient, spec.purpose);
        if (!allowed)               return allowed.error();
        if (!allowed.value())       return Error{ErrorCode::consent_missing, "no active consent"};
        permitted = true;
    }

    this->metrics().inc("inference.begin");

    auto pimpl = std::make_unique<Inference::Impl>(
        this,
        InferenceContext{
            mint_inference_id(),
            std::move(spec.model),
            std::move(spec.actor),
            std::move(spec.patient),
            std::move(spec.encounter),
            spec.purpose,
            std::move(spec.tenant),
            Time::now(),
        },
        spec.consent_token_id.value_or(std::string{}));

    return Inference{std::move(pimpl)};
}

}  // namespace asclepius
