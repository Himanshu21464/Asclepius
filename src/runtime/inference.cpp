// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/runtime.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <thread>

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
    std::uint64_t      committed_seq = 0;  // 0 = not committed yet

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

const TenantId& Inference::tenant() const noexcept { return impl_->ctx.tenant(); }

std::string_view Inference::id() const noexcept { return impl_->ctx.id(); }

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
    impl_->ledger_body["input_size"]  = static_cast<std::uint64_t>(post_input.size());
    impl_->ledger_body["output_hash"] = out_hash.hex();
    impl_->ledger_body["output_size"] = static_cast<std::uint64_t>(post_output.size());
    auto latency_ns = (Time::now() - impl_->ctx.started_at()).count();
    impl_->ledger_body["latency_ns"] = latency_ns;

    metrics.inc("inference.ok");
    // Record latency in seconds into a histogram so operators get
    // distribution data (p50, p95, p99) directly in Prometheus.
    metrics.observe("inference_latency_seconds",
                    static_cast<double>(latency_ns) / 1e9);
    return post_output;
}

Result<std::string> Inference::run_with_timeout(
        std::string input,
        const ModelCallback& model_call,
        std::chrono::milliseconds timeout) {

    if (impl_->completed) {
        return Error::invalid("inference.run called twice on the same handle");
    }
    impl_->completed = true;
    auto& metrics = impl_->rt->metrics();
    metrics.inc("inference.attempts");

    // Pre-guard (same path as run()).
    auto in_v = impl_->rt->policies().evaluate_input(impl_->ctx, std::move(input));
    if (!in_v) {
        metrics.inc("inference.blocked.input");
        impl_->ledger_body["status"]     = "blocked.input";
        impl_->ledger_body["block_code"] = to_string(in_v.error().code());
        impl_->ledger_body["block_msg"]  = std::string{in_v.error().what()};
        return in_v.error();
    }
    const std::string post_input = std::move(in_v).value();
    auto in_hash = hash(post_input);

    // Run the model on a detachable worker thread; wait up to `timeout`.
    // The promise/future lives in a shared_ptr so the worker can outlive
    // this function: if we time out we detach() and the worker continues
    // running until done; its result is dropped (no listener on the
    // promise). std::thread::detach is safe because the promise's
    // shared state owns its memory via the shared_ptr.
    auto promise = std::make_shared<std::promise<Result<std::string>>>();
    auto future  = promise->get_future();
    // Capture inputs by value: if we time out and detach the worker, the
    // worker may outlive this stack frame, so it cannot reference any of
    // our locals. Copy the callback (std::function) and the post-policy
    // input into the lambda's storage.
    std::thread worker([promise, cb = model_call, in = post_input]() {
        try {
            promise->set_value(cb(in));
        } catch (...) {
            try { promise->set_exception(std::current_exception()); }
            catch (...) { /* set_exception itself can throw on already-satisfied;
                              swallow. */ }
        }
    });

    if (future.wait_for(timeout) != std::future_status::ready) {
        // Mark the inference as timed out. Detach the worker; the
        // promise's shared state outlives both of us via shared_ptr.
        worker.detach();
        metrics.inc("inference.timeout");
        impl_->ledger_body["status"]     = "timeout";
        impl_->ledger_body["timeout_ms"] = static_cast<std::int64_t>(timeout.count());
        impl_->ledger_body["input_hash"] = in_hash.hex();
        impl_->ledger_body["input_size"] = static_cast<std::uint64_t>(post_input.size());
        return Error::timeout("model_call exceeded "
            + std::to_string(timeout.count()) + "ms");
    }
    worker.join();
    auto out_r = future.get();
    if (!out_r) {
        metrics.inc("inference.model_error");
        impl_->ledger_body["status"]    = "model_error";
        impl_->ledger_body["error_msg"] = std::string{out_r.error().what()};
        return out_r.error();
    }

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
    impl_->ledger_body["input_size"]  = static_cast<std::uint64_t>(post_input.size());
    impl_->ledger_body["output_hash"] = out_hash.hex();
    impl_->ledger_body["output_size"] = static_cast<std::uint64_t>(post_output.size());
    auto latency_ns = (Time::now() - impl_->ctx.started_at()).count();
    impl_->ledger_body["latency_ns"] = latency_ns;

    metrics.inc("inference.ok");
    metrics.observe("inference_latency_seconds",
                    static_cast<double>(latency_ns) / 1e9);
    return post_output;
}

Result<std::string> Inference::run_cancellable(
        std::string input,
        const ModelCallback& model_call,
        CancelToken token,
        std::chrono::milliseconds poll_interval) {

    if (impl_->completed) {
        return Error::invalid("inference.run called twice on the same handle");
    }
    impl_->completed = true;
    auto& metrics = impl_->rt->metrics();
    metrics.inc("inference.attempts");

    // Honour pre-cancelled tokens: skip the model call entirely so the
    // caller pays no work for a token they already cancelled.
    if (token.is_cancelled()) {
        metrics.inc("inference.cancelled");
        impl_->ledger_body["status"] = "cancelled";
        impl_->ledger_body["cancel_phase"] = "pre_input";
        return Error::cancelled("cancelled before input policies ran");
    }

    auto in_v = impl_->rt->policies().evaluate_input(impl_->ctx, std::move(input));
    if (!in_v) {
        metrics.inc("inference.blocked.input");
        impl_->ledger_body["status"]     = "blocked.input";
        impl_->ledger_body["block_code"] = to_string(in_v.error().code());
        impl_->ledger_body["block_msg"]  = std::string{in_v.error().what()};
        return in_v.error();
    }
    const std::string post_input = std::move(in_v).value();
    auto in_hash = hash(post_input);

    if (token.is_cancelled()) {
        metrics.inc("inference.cancelled");
        impl_->ledger_body["status"]     = "cancelled";
        impl_->ledger_body["cancel_phase"] = "pre_model";
        impl_->ledger_body["input_hash"] = in_hash.hex();
        impl_->ledger_body["input_size"] = static_cast<std::uint64_t>(post_input.size());
        return Error::cancelled("cancelled before model_call dispatch");
    }

    auto promise = std::make_shared<std::promise<Result<std::string>>>();
    auto future  = promise->get_future();
    std::thread worker([promise, cb = model_call, in = post_input]() {
        try {
            promise->set_value(cb(in));
        } catch (...) {
            try { promise->set_exception(std::current_exception()); }
            catch (...) { /* already-satisfied; swallow. */ }
        }
    });

    // Poll the future and the cancel flag together. We sleep for at most
    // poll_interval at a time so cancellation latency is bounded.
    const auto step = poll_interval.count() <= 0
        ? std::chrono::milliseconds{1}
        : poll_interval;
    while (true) {
        if (future.wait_for(step) == std::future_status::ready) break;
        if (token.is_cancelled()) {
            worker.detach();
            metrics.inc("inference.cancelled");
            impl_->ledger_body["status"]      = "cancelled";
            impl_->ledger_body["cancel_phase"] = "model_inflight";
            impl_->ledger_body["input_hash"]  = in_hash.hex();
            impl_->ledger_body["input_size"]  = static_cast<std::uint64_t>(post_input.size());
            return Error::cancelled("cancelled while model_call was running");
        }
    }
    worker.join();
    auto out_r = future.get();
    if (!out_r) {
        metrics.inc("inference.model_error");
        impl_->ledger_body["status"]    = "model_error";
        impl_->ledger_body["error_msg"] = std::string{out_r.error().what()};
        return out_r.error();
    }

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
    impl_->ledger_body["input_size"]  = static_cast<std::uint64_t>(post_input.size());
    impl_->ledger_body["output_hash"] = out_hash.hex();
    impl_->ledger_body["output_size"] = static_cast<std::uint64_t>(post_output.size());
    auto latency_ns = (Time::now() - impl_->ctx.started_at()).count();
    impl_->ledger_body["latency_ns"] = latency_ns;

    metrics.inc("inference.ok");
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
    impl_->committed     = true;
    impl_->committed_seq = e.value().header.seq;
    return Result<void>::ok();
}

Result<std::uint64_t> Inference::seq() const noexcept {
    if (!impl_->committed) {
        return Error::invalid("inference.seq called before commit");
    }
    return impl_->committed_seq;
}

bool Inference::is_committed() const noexcept {
    return impl_ && impl_->committed;
}

std::string Inference::actor_str() const {
    if (!impl_) return std::string{};
    return std::string{impl_->ctx.actor().str()};
}

bool Inference::was_committed_after(Time t) const noexcept {
    if (!impl_ || !impl_->committed) return false;
    return impl_->ctx.started_at() > t;
}

bool Inference::was_blocked() const noexcept {
    if (!impl_) return false;
    auto it = impl_->ledger_body.find("status");
    if (it == impl_->ledger_body.end() || !it->is_string()) return false;
    // Read the string in-place (no copy). "blocked." is 8 bytes; only
    // accept exact prefix on a known status string. We avoid pulling
    // <string_view> here to keep the accessor allocation-free.
    const auto& s = it->get_ref<const std::string&>();
    static constexpr char kPrefix[] = "blocked.";
    constexpr std::size_t kLen = sizeof(kPrefix) - 1;
    if (s.size() < kLen) return false;
    return std::memcmp(s.data(), kPrefix, kLen) == 0;
}

bool Inference::has_completed() const noexcept {
    return impl_ && impl_->completed;
}

bool Inference::failed() const noexcept {
    if (!impl_ || !impl_->completed) return false;
    auto it = impl_->ledger_body.find("status");
    if (it == impl_->ledger_body.end() || !it->is_string()) return false;
    const auto& s = it->get_ref<const std::string&>();
    // Match was_blocked()'s pattern: avoid string-view allocations on
    // the hot path by comparing in-place against known terminal-fail
    // statuses.
    static constexpr char kBlocked[] = "blocked.";
    constexpr std::size_t kBlockedLen = sizeof(kBlocked) - 1;
    if (s.size() >= kBlockedLen &&
        std::memcmp(s.data(), kBlocked, kBlockedLen) == 0) {
        return true;
    }
    return s == "model_error" || s == "timeout" || s == "cancelled";
}

std::string Inference::trace_summary() const {
    if (!impl_) return std::string{};
    // Format:
    //   inf=<id> patient=<patient> model=<model> status=<status> elapsed=<ms>ms
    // Build with reserve() to keep this allocation-light on the hot
    // logging path. The status field is the only one that can be
    // empty (pre-run); the rest carry their canonical id strings.
    const auto& c = impl_->ctx;
    const std::string st = status();
    std::string id_s{c.id()};
    std::string patient_s{c.patient().str()};
    std::string model_s{c.model().str()};
    std::string elapsed_s = std::to_string(elapsed_ms());

    std::string out;
    out.reserve(id_s.size() + patient_s.size() + model_s.size()
                + st.size() + elapsed_s.size() + 48);
    out += "inf=";       out += id_s;
    out += " patient=";  out += patient_s;
    out += " model=";    out += model_s;
    out += " status=";   out += st;
    out += " elapsed=";  out += elapsed_s;
    out += "ms";
    return out;
}

nlohmann::json Inference::body_snapshot() const {
    if (!impl_) return nlohmann::json::object();
    // Deep copy by value so callers can't mutate our pending body.
    return impl_->ledger_body;
}

bool Inference::has_run() const noexcept {
    return impl_ && impl_->completed;
}

std::string Inference::status() const {
    if (!impl_) return std::string{};
    auto it = impl_->ledger_body.find("status");
    if (it == impl_->ledger_body.end() || !it->is_string()) return std::string{};
    return it->get<std::string>();
}

std::string Inference::input_hash() const {
    if (!impl_) return std::string{};
    auto it = impl_->ledger_body.find("input_hash");
    if (it == impl_->ledger_body.end() || !it->is_string()) return std::string{};
    return it->get<std::string>();
}

std::string Inference::output_hash() const {
    if (!impl_) return std::string{};
    auto it = impl_->ledger_body.find("output_hash");
    if (it == impl_->ledger_body.end() || !it->is_string()) return std::string{};
    return it->get<std::string>();
}

Result<std::size_t> Inference::input_size() const {
    if (!impl_ || !impl_->completed) {
        return Error::invalid("input_size called before run");
    }
    auto it = impl_->ledger_body.find("input_size");
    if (it == impl_->ledger_body.end() || !it->is_number_unsigned()) {
        return Error::not_found("input_size not recorded for this run");
    }
    return static_cast<std::size_t>(it->get<std::uint64_t>());
}

Result<std::size_t> Inference::output_size() const {
    if (!impl_ || !impl_->completed) {
        return Error::invalid("output_size called before run");
    }
    // Symmetric with output_hash semantics: only the success path lands
    // a canonical output. Surface that explicitly so callers don't have
    // to disambiguate "size missing because the run failed" from "size
    // missing because the field wasn't recorded."
    auto status_it = impl_->ledger_body.find("status");
    const bool ok = status_it != impl_->ledger_body.end() &&
                    status_it->is_string() &&
                    status_it->get_ref<const std::string&>() == "ok";
    if (!ok) {
        return Error::not_found("output_size only recorded on status=ok runs");
    }
    auto it = impl_->ledger_body.find("output_size");
    if (it == impl_->ledger_body.end() || !it->is_number_unsigned()) {
        return Error::not_found("output_size not recorded for this run");
    }
    return static_cast<std::size_t>(it->get<std::uint64_t>());
}

std::int64_t Inference::elapsed_ms() const noexcept {
    if (!impl_) return 0;
    auto ns = (Time::now() - impl_->ctx.started_at()).count();
    return ns / 1'000'000;
}

std::int64_t Inference::age_ms() const noexcept {
    // Pure alias for elapsed_ms() — exists so call sites that read as
    // "how old is this handle?" don't have to mentally translate
    // "elapsed since started_at." Forwards directly so the two stay
    // in lockstep if elapsed_ms() ever grows a different floor / clamp.
    return elapsed_ms();
}

Result<void> Inference::commit_idempotent(std::size_t lookback) {
    if (!impl_->completed) {
        return Error::invalid("inference.commit_idempotent called before run");
    }
    if (impl_->committed) {
        return Result<void>::ok();
    }

    // Scan the last `lookback` entries for a prior commit of THIS
    // inference_id. The substrate's body is canonical-JSON so a
    // straight string-search for `"inference_id":"<id>"` is sound and
    // dramatically cheaper than json::parse on each entry.
    const std::string my_id = std::string{impl_->ctx.id()};
    const std::string needle = "\"inference_id\":\"" + my_id + "\"";

    auto tail = impl_->rt->ledger().tail(lookback);
    if (!tail) return tail.error();
    for (const auto& prior : tail.value()) {
        if (prior.header.event_type != "inference.committed") continue;
        if (prior.body_json.find(needle) != std::string::npos) {
            // Found a prior commit for the same inference_id — treat
            // this handle as already committed; do not re-append.
            impl_->committed     = true;
            impl_->committed_seq = prior.header.seq;
            impl_->rt->metrics().inc("inference.idempotent_dedupe");
            return Result<void>::ok();
        }
    }

    // Not seen — fall through to a normal commit.
    return commit();
}

Result<void> Inference::ensure_committed() {
    // Idempotent in-handle commit. commit() is itself idempotent
    // within the lifetime of a single handle (a second call on a
    // committed handle returns ok), so this is essentially that path
    // expressed once at the call site. The distinct method exists so
    // sidecar shutdown loops can write `inf.ensure_committed()` and
    // not have to know whether they already committed earlier on the
    // fast path — different shape from commit_idempotent(), which
    // pays for a chain scan to dedupe across handles.
    if (!impl_) {
        return Error::invalid("ensure_committed called on null handle");
    }
    if (impl_->committed) {
        return Result<void>::ok();
    }
    return commit();
}

Result<void> Inference::add_metadata(std::string_view key, nlohmann::json value) {
    if (key.empty()) {
        return Error::invalid("metadata key is empty");
    }
    // Reserved top-level keys — refuse to let metadata shadow runtime
    // bookkeeping. The reserved set matches the keys this file writes to
    // ledger_body directly. Any future addition of top-level fields must
    // be added here.
    static const std::array<std::string_view, 14> kReserved = {
        "status", "input_hash", "output_hash", "latency_ns",
        "block_code", "block_msg", "error_msg", "timeout_ms",
        "cancel_phase", "inference_id", "model", "metadata",
        "input_size", "output_size",
    };
    for (auto r : kReserved) {
        if (key == r) {
            return Error::invalid("metadata key is reserved");
        }
    }
    if (impl_->committed) {
        return Error::invalid("metadata after commit");
    }
    if (!impl_->ledger_body.contains("metadata") ||
        !impl_->ledger_body["metadata"].is_object()) {
        impl_->ledger_body["metadata"] = nlohmann::json::object();
    }
    impl_->ledger_body["metadata"][std::string{key}] = std::move(value);
    return Result<void>::ok();
}

Result<bool> Inference::has_metadata(std::string_view key) const {
    if (key.empty()) {
        return Error::invalid("metadata key is empty");
    }
    auto md = impl_->ledger_body.find("metadata");
    if (md == impl_->ledger_body.end() || !md->is_object()) {
        return false;
    }
    return md->contains(std::string{key});
}

Result<nlohmann::json> Inference::get_metadata(std::string_view key) const {
    if (key.empty()) {
        return Error::invalid("metadata key is empty");
    }
    auto md = impl_->ledger_body.find("metadata");
    if (md == impl_->ledger_body.end() || !md->is_object()) {
        return Error::not_found("metadata key not found");
    }
    auto it = md->find(std::string{key});
    if (it == md->end()) {
        return Error::not_found("metadata key not found");
    }
    return *it;
}

void Inference::clear_metadata(std::string_view key) noexcept {
    if (!impl_) return;
    if (key.empty()) return;
    if (impl_->committed) return;
    auto md = impl_->ledger_body.find("metadata");
    if (md == impl_->ledger_body.end() || !md->is_object()) return;
    md->erase(std::string{key});
}

Result<void> Inference::tag(std::string_view label) {
    // Sugar — equivalent to add_metadata("tag", <label as JSON string>).
    // Empty label is rejected here so the diagnostic names the right
    // method ("tag", not "metadata"). All other rules — reserved-key
    // semantics, post-commit rejection, replace-on-duplicate — fall out
    // of forwarding to add_metadata under the fixed key "tag".
    if (label.empty()) {
        return Error::invalid("tag label is empty");
    }
    return add_metadata("tag", nlohmann::json(std::string{label}));
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

Result<void> Inference::attach_ground_truth(nlohmann::json truth,
                                            std::string    source) {
    if (source.empty()) {
        return Error::invalid("attach_ground_truth requires non-empty source");
    }
    if (!impl_->committed) {
        auto c = commit();
        if (!c) return c.error();
    }
    GroundTruth gt;
    gt.inference_id = std::string{impl_->ctx.id()};
    gt.truth        = std::move(truth);
    gt.source       = std::move(source);
    gt.captured_at  = Time::now();
    return impl_->rt->evaluation().attach_ground_truth(std::move(gt));
}

Result<void> Inference::observe_drift(std::string_view feature, double value) {
    return impl_->rt->drift().observe(feature, value);
}

Result<void> Inference::observe_drift_named(std::string_view feature, double value) {
    // Sugar — forwards verbatim to observe_drift(). Kept as a thin
    // pass-through so the "named" framing stays cheap; if the
    // underlying contract ever grows new error paths, both spellings
    // inherit them automatically.
    return observe_drift(feature, value);
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
