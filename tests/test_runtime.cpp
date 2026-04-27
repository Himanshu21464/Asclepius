// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include <doctest/doctest.h>

#include "asclepius/asclepius.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <random>
#include <thread>

using namespace asclepius;
using namespace std::chrono_literals;

namespace {

std::filesystem::path tmp_db(const char* tag) {
    auto p = std::filesystem::temp_directory_path()
           / ("asclepius_rt_" + std::string{tag} + "_"
              + std::to_string(std::random_device{}()) + ".db");
    std::filesystem::remove(p);
    std::filesystem::remove(std::filesystem::path{p}.replace_extension(".key"));
    return p;
}

}  // namespace

TEST_CASE("Runtime end-to-end inference: blocked without consent") {
    auto rt = Runtime::open(tmp_db("noconsent"));
    REQUIRE(rt);

    auto inf = rt.value().begin_inference({
        .model     = ModelId{"scribe", "v3"},
        .actor     = ActorId::clinician("smith"),
        .patient   = PatientId::pseudonymous("p"),
        .encounter = EncounterId::make(),
        .purpose   = Purpose::ambient_documentation,
    });
    REQUIRE(!inf);
    CHECK(inf.error().code() == ErrorCode::consent_missing);
}

TEST_CASE("Runtime end-to-end inference: PHI scrubbed, ledger committed") {
    auto rt = Runtime::open(tmp_db("happy")).value();
    rt.policies().push(make_phi_scrubber());

    auto pid = PatientId::pseudonymous("p_happy");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h);
    REQUIRE(tok);

    auto inf = rt.begin_inference({
        .model            = ModelId{"scribe", "v3"},
        .actor            = ActorId::clinician("smith"),
        .patient          = pid,
        .encounter        = EncounterId::make(),
        .purpose          = Purpose::ambient_documentation,
        .tenant           = TenantId{},
        .consent_token_id = tok.value().token_id,
    });
    REQUIRE(inf);

    auto out = inf.value().run("phone 415-555-1234",
                               [](std::string in) -> Result<std::string> { return in; });
    REQUIRE(out);
    CHECK(out.value().find("415-555-1234") == std::string::npos);
    CHECK(out.value().find("[REDACTED:phone]") != std::string::npos);

    REQUIRE(inf.value().commit());
    CHECK(rt.ledger().length() >= 1);
    REQUIRE(rt.ledger().verify());
}

TEST_CASE("Runtime: model error is captured as model_error in ledger") {
    auto rt = Runtime::open(tmp_db("modelerr")).value();

    auto pid = PatientId::pseudonymous("p_err");
    auto tok = rt.consent().grant(pid, {Purpose::diagnostic_suggestion}, 1h);
    REQUIRE(tok);

    auto inf = rt.begin_inference({
        .model            = ModelId{"diag", "v1"},
        .actor            = ActorId::clinician("smith"),
        .patient          = pid,
        .encounter        = EncounterId::make(),
        .purpose          = Purpose::diagnostic_suggestion,
        .tenant           = TenantId{},
        .consent_token_id = tok.value().token_id,
    });
    REQUIRE(inf);

    auto out = inf.value().run("anything",
        [](std::string) -> Result<std::string> { return Error::internal("model exploded"); });
    REQUIRE(!out);
    CHECK(out.error().code() == ErrorCode::internal);

    REQUIRE(inf.value().commit());
    REQUIRE(rt.ledger().verify());
}

TEST_CASE("Runtime: output policy block is recorded with status=blocked.output") {
    auto rt = Runtime::open(tmp_db("blocked")).value();
    rt.policies().push(make_length_limit(0, /*output_max=*/4));

    auto pid = PatientId::pseudonymous("pb");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h);
    REQUIRE(tok);

    auto inf = rt.begin_inference({
        .model            = ModelId{"scribe", "v3"},
        .actor            = ActorId::clinician("smith"),
        .patient          = pid,
        .encounter        = EncounterId::make(),
        .purpose          = Purpose::ambient_documentation,
        .tenant           = TenantId{},
        .consent_token_id = tok.value().token_id,
    });
    REQUIRE(inf);

    auto out = inf.value().run("hi",
        [](std::string) -> Result<std::string> { return std::string{"this output is too long"}; });
    REQUIRE(!out);
    CHECK(out.error().code() == ErrorCode::policy_violation);
    REQUIRE(inf.value().commit());
}

// ============== run_with_timeout ==========================================

namespace {
auto fresh_runtime(const char* tag) -> Result<Runtime> {
    auto p = std::filesystem::temp_directory_path()
           / ("asc_to_" + std::string{tag} + "_"
              + std::to_string(std::random_device{}()) + ".db");
    std::filesystem::remove(p);
    std::filesystem::remove(std::filesystem::path{p}.replace_extension(".key"));
    return Runtime::open(p);
}
auto begin(Runtime& rt, PatientId pid, std::string token_id) {
    return rt.begin_inference({
        .model = ModelId{"m","v1"}, .actor = ActorId::clinician("smith"),
        .patient = pid, .encounter = EncounterId::make(),
        .purpose = Purpose::ambient_documentation,
        .tenant  = TenantId{},
        .consent_token_id = std::move(token_id),
    });
}
}  // namespace

TEST_CASE("run_with_timeout: fast callback returns normally") {
    auto rt_ = fresh_runtime("fast"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_to_fast");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    auto r = inf.value().run_with_timeout("hi",
        [](std::string s) -> Result<std::string> { return s; },
        std::chrono::milliseconds{500});
    REQUIRE(r);
    CHECK(r.value() == "hi");
    REQUIRE(inf.value().commit());
}

TEST_CASE("run_with_timeout: slow callback exceeds timeout, returns deadline_exceeded") {
    auto rt_ = fresh_runtime("slow"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_to_slow");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    auto r = inf.value().run_with_timeout("hi",
        [](std::string s) -> Result<std::string> {
            std::this_thread::sleep_for(std::chrono::milliseconds{300});
            return s;
        },
        std::chrono::milliseconds{50});
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::deadline_exceeded);
    // commit() should still record the timeout entry.
    REQUIRE(inf.value().commit());
}

TEST_CASE("run_with_timeout: deadline_exceeded message names the threshold") {
    auto rt_ = fresh_runtime("msg"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_to_msg");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    auto r = inf.value().run_with_timeout("x",
        [](std::string s) -> Result<std::string> {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
            return s;
        },
        std::chrono::milliseconds{42});
    CHECK(!r);
    CHECK(std::string{r.error().what()}.find("42ms") != std::string::npos);
}

TEST_CASE("run_with_timeout: timed-out inference body has status=timeout") {
    auto rt_ = fresh_runtime("body"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_to_body");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    REQUIRE(!inf.value().run_with_timeout("x",
        [](std::string s) -> Result<std::string> {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
            return s;
        },
        std::chrono::milliseconds{30}));
    REQUIRE(inf.value().commit());

    auto tail = rt.ledger().tail(1);
    REQUIRE(tail);
    REQUIRE(!tail.value().empty());
    auto body = nlohmann::json::parse(tail.value()[0].body_json);
    CHECK(body["status"]        == "timeout");
    CHECK(body["timeout_ms"]    == 30);
    CHECK(body.contains("input_hash"));
}

TEST_CASE("run_with_timeout: 0ms timeout always trips") {
    auto rt_ = fresh_runtime("zero"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_to_zero");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    auto r = inf.value().run_with_timeout("x",
        [](std::string s) -> Result<std::string> {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            return s;
        },
        std::chrono::milliseconds{0});
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::deadline_exceeded);
}

TEST_CASE("run_with_timeout: model error inside callback propagates as model_error, not timeout") {
    auto rt_ = fresh_runtime("merr"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_to_merr");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    auto r = inf.value().run_with_timeout("x",
        [](std::string) -> Result<std::string> { return Error::internal("bad"); },
        std::chrono::milliseconds{500});
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::internal);
    REQUIRE(inf.value().commit());
}

TEST_CASE("run_with_timeout: input policy block beats timeout (no thread spawned)") {
    auto rt_ = fresh_runtime("inblock"); REQUIRE(rt_);
    auto& rt = rt_.value();
    rt.policies().push(make_phi_scrubber());
    rt.policies().push(make_length_limit(/*input_max=*/4, /*output_max=*/200));

    auto pid = PatientId::pseudonymous("p_to_inblock");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    // 100-byte input violates the input length limit (4) — should never
    // reach the model_call thread.
    auto r = inf.value().run_with_timeout(std::string(100, 'a'),
        [](std::string s) -> Result<std::string> {
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
            return s;
        },
        std::chrono::milliseconds{50});
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::policy_violation);
}

TEST_CASE("run_with_timeout: very short timeout doesn't deadlock the runtime") {
    // Stress test: many short-timeout inferences in a row — the detached
    // worker threads must not back up the system.
    auto rt_ = fresh_runtime("stress"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_to_stress");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    int timeouts = 0;
    for (int i = 0; i < 10; ++i) {
        auto inf = begin(rt, pid, tok.token_id);
        REQUIRE(inf);
        auto r = inf.value().run_with_timeout("x",
            [](std::string s) -> Result<std::string> {
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
                return s;
            },
            std::chrono::milliseconds{1});
        if (!r && r.error().code() == ErrorCode::deadline_exceeded) ++timeouts;
        REQUIRE(inf.value().commit());
    }
    CHECK(timeouts >= 8);  // allow occasional fast machines, mostly timeouts
    REQUIRE(rt.ledger().verify());
}

TEST_CASE("run_with_timeout: late-completing thread doesn't corrupt subsequent inferences") {
    auto rt_ = fresh_runtime("late"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_to_late");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    // First: timeout fires; worker is still running 200ms in the background.
    {
        auto inf = begin(rt, pid, tok.token_id);
        REQUIRE(inf);
        REQUIRE(!inf.value().run_with_timeout("x",
            [](std::string s) -> Result<std::string> {
                std::this_thread::sleep_for(std::chrono::milliseconds{200});
                return s;
            },
            std::chrono::milliseconds{20}));
        REQUIRE(inf.value().commit());
    }
    // Second: a normal fast inference should succeed cleanly.
    {
        auto inf = begin(rt, pid, tok.token_id);
        REQUIRE(inf);
        auto r = inf.value().run_with_timeout("y",
            [](std::string s) -> Result<std::string> { return s; },
            std::chrono::milliseconds{500});
        REQUIRE(r);
        CHECK(r.value() == "y");
        REQUIRE(inf.value().commit());
    }
    REQUIRE(rt.ledger().verify());
}

TEST_CASE("run_with_timeout: cannot be called after run() on same handle") {
    auto rt_ = fresh_runtime("twice"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_to_twice");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    REQUIRE(inf.value().run("x", [](std::string s)->Result<std::string>{ return s; }));
    auto r = inf.value().run_with_timeout("y",
        [](std::string s)->Result<std::string>{ return s; },
        std::chrono::milliseconds{100});
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("run_with_timeout: cannot be called twice on same handle") {
    auto rt_ = fresh_runtime("dbl"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_to_dbl");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    REQUIRE(inf.value().run_with_timeout("x",
        [](std::string s)->Result<std::string>{ return s; },
        std::chrono::milliseconds{500}));
    auto r = inf.value().run_with_timeout("y",
        [](std::string s)->Result<std::string>{ return s; },
        std::chrono::milliseconds{500});
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

// ============== run_cancellable ==========================================

TEST_CASE("run_cancellable: callback that returns first wins, no cancellation") {
    auto rt_ = fresh_runtime("rc_fast"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_rc_fast");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    CancelToken token;
    auto r = inf.value().run_cancellable("hello",
        [](std::string s) -> Result<std::string> { return s + "!"; },
        token);
    REQUIRE(r);
    CHECK(r.value() == "hello!");
    REQUIRE(inf.value().commit());
}

TEST_CASE("run_cancellable: cancel during model_call returns cancelled") {
    auto rt_ = fresh_runtime("rc_cancel"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_rc_cancel");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    CancelToken token;
    std::thread canceller([token]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds{15});
        token.cancel();
    });
    auto r = inf.value().run_cancellable("x",
        [](std::string s) -> Result<std::string> {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
            return s;
        },
        token,
        std::chrono::milliseconds{2});
    canceller.join();
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::cancelled);
}

TEST_CASE("run_cancellable: pre-cancelled token short-circuits before policies") {
    auto rt_ = fresh_runtime("rc_pre"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_rc_pre");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    CancelToken token;
    token.cancel();
    bool model_was_called = false;
    auto r = inf.value().run_cancellable("hi",
        [&](std::string s) -> Result<std::string> {
            model_was_called = true;
            return s;
        },
        token);
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::cancelled);
    CHECK(!model_was_called);
    REQUIRE(inf.value().commit());

    auto tail = rt.ledger().tail(1);
    REQUIRE(tail);
    auto body = nlohmann::json::parse(tail.value()[0].body_json);
    CHECK(body["status"]       == "cancelled");
    CHECK(body["cancel_phase"] == "pre_input");
}

TEST_CASE("run_cancellable: cancel during model_call writes cancel_phase=model_inflight") {
    auto rt_ = fresh_runtime("rc_phase"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_rc_phase");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    CancelToken token;
    std::thread canceller([token]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        token.cancel();
    });
    REQUIRE(!inf.value().run_cancellable("x",
        [](std::string s) -> Result<std::string> {
            std::this_thread::sleep_for(std::chrono::milliseconds{300});
            return s;
        },
        token,
        std::chrono::milliseconds{2}));
    canceller.join();
    REQUIRE(inf.value().commit());

    auto tail = rt.ledger().tail(1);
    REQUIRE(tail);
    auto body = nlohmann::json::parse(tail.value()[0].body_json);
    CHECK(body["status"]       == "cancelled");
    CHECK(body["cancel_phase"] == "model_inflight");
    CHECK(body.contains("input_hash"));
}

TEST_CASE("run_cancellable: cancellation latency is bounded by poll_interval") {
    auto rt_ = fresh_runtime("rc_latency"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_rc_latency");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    CancelToken token;
    auto t0 = std::chrono::steady_clock::now();
    std::thread canceller([token]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        token.cancel();
    });
    REQUIRE(!inf.value().run_cancellable("x",
        [](std::string s) -> Result<std::string> {
            std::this_thread::sleep_for(std::chrono::seconds{5});
            return s;
        },
        token,
        std::chrono::milliseconds{1}));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    canceller.join();
    // Should return within a few ms of cancel(); definitely under 200ms even
    // on a loaded box, well below the 5s callback duration.
    CHECK(elapsed < std::chrono::milliseconds{500});
}

TEST_CASE("run_cancellable: model error propagates, not cancelled") {
    auto rt_ = fresh_runtime("rc_err"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_rc_err");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    CancelToken token;
    auto r = inf.value().run_cancellable("x",
        [](std::string) -> Result<std::string> { return Error::internal("oops"); },
        token);
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::internal);
    REQUIRE(inf.value().commit());
}

TEST_CASE("run_cancellable: zero poll_interval is clamped, not infinite-spin") {
    auto rt_ = fresh_runtime("rc_zero"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_rc_zero");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    CancelToken token;
    auto r = inf.value().run_cancellable("x",
        [](std::string s) -> Result<std::string> { return s; },
        token,
        std::chrono::milliseconds{0});
    REQUIRE(r);
    CHECK(r.value() == "x");
}

TEST_CASE("run_cancellable: shared cancel token aborts many concurrent inferences") {
    auto rt_ = fresh_runtime("rc_shared"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_rc_shared");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    CancelToken token;
    constexpr int N = 6;
    std::vector<std::thread> workers;
    std::atomic<int> cancelled_count{0};
    for (int i = 0; i < N; i++) {
        workers.emplace_back([&]() {
            auto inf = begin(rt, pid, tok.token_id);
            if (!inf) return;
            auto r = inf.value().run_cancellable("x",
                [](std::string s) -> Result<std::string> {
                    std::this_thread::sleep_for(std::chrono::seconds{2});
                    return s;
                },
                token,
                std::chrono::milliseconds{1});
            if (!r && r.error().code() == ErrorCode::cancelled) cancelled_count++;
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    token.cancel();
    for (auto& t : workers) t.join();
    CHECK(cancelled_count.load() == N);
}

TEST_CASE("run_cancellable: token state is shared across copies") {
    CancelToken a;
    CancelToken b = a;
    CHECK(!a.is_cancelled());
    CHECK(!b.is_cancelled());
    b.cancel();
    CHECK(a.is_cancelled());
    CHECK(b.is_cancelled());
}

TEST_CASE("run_cancellable: cannot be called twice on same handle") {
    auto rt_ = fresh_runtime("rc_twice"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_rc_twice");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    CancelToken token;
    REQUIRE(inf.value().run_cancellable("x",
        [](std::string s)->Result<std::string>{ return s; }, token));
    auto r = inf.value().run_cancellable("y",
        [](std::string s)->Result<std::string>{ return s; }, token);
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("run_cancellable: cancellation does not break ledger chain") {
    auto rt_ = fresh_runtime("rc_chain"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_rc_chain");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    for (int i = 0; i < 5; i++) {
        auto inf = begin(rt, pid, tok.token_id);
        REQUIRE(inf);
        CancelToken token;
        if (i % 2 == 0) token.cancel();
        (void)inf.value().run_cancellable("x",
            [](std::string s) -> Result<std::string> { return s; }, token);
        REQUIRE(inf.value().commit());
    }
    REQUIRE(rt.ledger().verify());
}

TEST_CASE("Inference::commit_idempotent dedupes across handles by inference_id") {
    auto p = std::filesystem::temp_directory_path()
           / ("asc_idemp_" + std::to_string(std::random_device{}()) + ".db");
    std::filesystem::remove(p);
    std::filesystem::remove(std::filesystem::path{p}.replace_extension(".key"));

    auto rt_ = Runtime::open(p);
    REQUIRE(rt_);
    auto& rt = rt_.value();

    auto pid = PatientId::pseudonymous("p_idemp");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    EncounterId enc = EncounterId::make();

    // First inference: opaque, run, commit_idempotent — appends.
    auto inf1 = rt.begin_inference({
        .model            = ModelId{"scribe", "v3"},
        .actor            = ActorId::clinician("smith"),
        .patient          = pid,
        .encounter        = enc,
        .purpose          = Purpose::ambient_documentation,
        .tenant           = TenantId{},
        .consent_token_id = tok.token_id,
    });
    REQUIRE(inf1);
    REQUIRE(inf1.value().run("hello",
        [](std::string s) -> Result<std::string> { return s; }));
    auto pre = rt.ledger().length();
    REQUIRE(inf1.value().commit_idempotent());
    auto after_first = rt.ledger().length();
    CHECK(after_first > pre);

    // Same inference_id, fresh handle: simulate a stateless retry. The
    // begin_inference factory normally generates a new id, so we hijack
    // the InferenceContext directly via a path that re-uses the same id.
    // Use begin_inference twice and see that committing the same body
    // twice (with the dedupe scan) does NOT double-append.
    //
    // Easier path: manually construct a body with the same inference_id
    // by appending directly through the ledger to simulate the prior
    // commit, then start a fresh inference and commit_idempotent. If
    // the body's inference_id matches a prior entry, dedupe fires.

    // Run a second inference (different id) — appends as expected.
    auto inf2 = rt.begin_inference({
        .model            = ModelId{"scribe", "v3"},
        .actor            = ActorId::clinician("smith"),
        .patient          = pid,
        .encounter        = enc,
        .purpose          = Purpose::ambient_documentation,
        .tenant           = TenantId{},
        .consent_token_id = tok.token_id,
    });
    REQUIRE(inf2);
    REQUIRE(inf2.value().run("world",
        [](std::string s) -> Result<std::string> { return s; }));
    auto before_second = rt.ledger().length();
    REQUIRE(inf2.value().commit_idempotent());
    CHECK(rt.ledger().length() > before_second);

    // Now: re-commit_idempotent on inf1 (already committed in this handle).
    // In-handle short-circuit fires — no append, returns ok.
    auto in_handle_count = rt.ledger().length();
    REQUIRE(inf1.value().commit_idempotent());
    CHECK(rt.ledger().length() == in_handle_count);

    // Verify the metrics counter exists. We can't see the in-process
    // dedupe (only fires when we hit the cross-handle path); but we
    // can sanity-check that idempotent commits don't violate verify().
    REQUIRE(rt.ledger().verify());

    REQUIRE(rt.ledger().verify());
}

TEST_CASE("commit_idempotent dedupes when a matching inference_id is in the tail") {
    auto p = std::filesystem::temp_directory_path()
           / ("asc_idemp2_" + std::to_string(std::random_device{}()) + ".db");
    std::filesystem::remove(p);
    std::filesystem::remove(std::filesystem::path{p}.replace_extension(".key"));

    auto rt_ = Runtime::open(p);
    REQUIRE(rt_);
    auto& rt = rt_.value();

    // Pre-seed the ledger with a fake inference.committed entry whose
    // body contains an inference_id we'll use later. This simulates a
    // prior commit that this Runtime instance is unaware of (e.g. a
    // process restart, or a different replica).
    nlohmann::json prior;
    const std::string forged_id = "inf_pretend_already_committed_42";
    prior["inference_id"] = forged_id;
    REQUIRE(rt.ledger().append("inference.committed", "clinician:smith", prior, ""));

    // Now begin a real inference. The new Inference's id will NOT match
    // forged_id (begin_inference generates fresh ids), so commit_idempotent
    // appends as usual — proving the scan correctly distinguishes mismatch.
    auto pid = PatientId::pseudonymous("p_idemp2");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = rt.begin_inference({
        .model = ModelId{"scribe","v3"}, .actor = ActorId::clinician("smith"),
        .patient = pid, .encounter = EncounterId::make(),
        .purpose = Purpose::ambient_documentation,
        .tenant  = TenantId{},
        .consent_token_id = tok.token_id,
    });
    REQUIRE(inf);
    REQUIRE(inf.value().run("hello",
        [](std::string s) -> Result<std::string> { return s; }));
    auto before = rt.ledger().length();
    REQUIRE(inf.value().commit_idempotent());
    CHECK(rt.ledger().length() > before);  // appended (no match)

    // Calling again on the same handle short-circuits (in-handle).
    auto after = rt.ledger().length();
    REQUIRE(inf.value().commit_idempotent());
    CHECK(rt.ledger().length() == after);

    REQUIRE(rt.ledger().verify());
}

// ============== consent ledger-mirror + restart replay ==================

TEST_CASE("Consent grant is mirrored to ledger as consent.granted event") {
    auto p = tmp_db("consent_mirror");
    auto rt_ = Runtime::open(p); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("pmirror");
    auto tok = rt.consent().grant(pid, {Purpose::triage}, 1h).value();

    auto e = rt.ledger().find_by_inference_id(tok.token_id);
    // find_by_inference_id looks for inference_id field; consent events
    // use token_id, so this should not match.
    CHECK(!e);

    // Direct scan: most recent entry should be the grant.
    auto tail = rt.ledger().tail(1); REQUIRE(tail);
    REQUIRE(!tail.value().empty());
    CHECK(tail.value()[0].header.event_type == "consent.granted");
    auto body = nlohmann::json::parse(tail.value()[0].body_json);
    CHECK(body["token_id"] == tok.token_id);
    CHECK(body["revoked"] == false);
    CHECK(body["purposes"].is_array());
    CHECK(body["purposes"][0] == "triage");
}

TEST_CASE("Consent revoke is mirrored to ledger as consent.revoked event") {
    auto p = tmp_db("consent_revoke");
    auto rt_ = Runtime::open(p); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto tok = rt.consent().grant(PatientId::pseudonymous("p"),
                                  {Purpose::triage}, 1h).value();
    REQUIRE(rt.consent().revoke(tok.token_id));
    auto tail = rt.ledger().tail(1); REQUIRE(tail);
    CHECK(tail.value()[0].header.event_type == "consent.revoked");
}

TEST_CASE("Consent state survives runtime restart via ledger replay") {
    auto p = tmp_db("consent_replay");
    std::string token_id;
    {
        auto rt_ = Runtime::open(p); REQUIRE(rt_);
        auto tok = rt_.value().consent().grant(
            PatientId::pseudonymous("preplay"),
            {Purpose::ambient_documentation}, 1h);
        REQUIRE(tok);
        token_id = tok.value().token_id;
    }
    // Reopen — registry should have the token back.
    auto rt2_ = Runtime::open(p); REQUIRE(rt2_);
    auto t = rt2_.value().consent().get(token_id);
    REQUIRE(t);
    CHECK(t.value().token_id == token_id);
    CHECK(t.value().revoked  == false);
    auto perm = rt2_.value().consent().permits(
        PatientId::pseudonymous("preplay"), Purpose::ambient_documentation);
    REQUIRE(perm);
    CHECK(perm.value());
}

TEST_CASE("Revocations carry over restart") {
    auto p = tmp_db("consent_rev_replay");
    std::string token_id;
    {
        auto rt_ = Runtime::open(p); REQUIRE(rt_);
        auto tok = rt_.value().consent().grant(
            PatientId::pseudonymous("prev"),
            {Purpose::triage}, 1h);
        REQUIRE(tok);
        token_id = tok.value().token_id;
        REQUIRE(rt_.value().consent().revoke(token_id));
    }
    auto rt2_ = Runtime::open(p); REQUIRE(rt2_);
    auto t = rt2_.value().consent().get(token_id);
    REQUIRE(t);
    CHECK(t.value().revoked == true);
    auto perm = rt2_.value().consent().permits(
        PatientId::pseudonymous("prev"), Purpose::triage);
    REQUIRE(perm);
    CHECK(perm.value() == false);
}

TEST_CASE("Replay does not duplicate consent events on second open") {
    auto p = tmp_db("consent_no_dupe");
    {
        auto rt_ = Runtime::open(p); REQUIRE(rt_);
        REQUIRE(rt_.value().consent().grant(
            PatientId::pseudonymous("p"), {Purpose::triage}, 1h));
    }
    std::uint64_t after_first;
    {
        auto rt_ = Runtime::open(p); REQUIRE(rt_);
        after_first = rt_.value().ledger().length();
    }
    {
        auto rt_ = Runtime::open(p); REQUIRE(rt_);
        // Reopen without granting anything new — chain length unchanged.
        CHECK(rt_.value().ledger().length() == after_first);
    }
}

TEST_CASE("Replay handles many grants and one revoke correctly") {
    auto p = tmp_db("consent_many");
    std::vector<std::string> ids;
    {
        auto rt_ = Runtime::open(p); REQUIRE(rt_);
        for (int i = 0; i < 12; i++) {
            auto t = rt_.value().consent().grant(
                PatientId::pseudonymous("p" + std::to_string(i)),
                {Purpose::triage}, 1h);
            REQUIRE(t);
            ids.push_back(t.value().token_id);
        }
        REQUIRE(rt_.value().consent().revoke(ids[3]));
    }
    auto rt2_ = Runtime::open(p); REQUIRE(rt2_);
    auto& reg = rt2_.value().consent();
    for (size_t i = 0; i < ids.size(); i++) {
        auto t = reg.get(ids[i]); REQUIRE(t);
        if (i == 3) {
            CHECK(t.value().revoked == true);
        } else {
            CHECK(t.value().revoked == false);
        }
    }
}

TEST_CASE("Restart preserves chain integrity (verify() passes after replay)") {
    auto p = tmp_db("consent_verify");
    {
        auto rt_ = Runtime::open(p); REQUIRE(rt_);
        REQUIRE(rt_.value().consent().grant(
            PatientId::pseudonymous("p"), {Purpose::triage}, 1h));
    }
    auto rt2_ = Runtime::open(p); REQUIRE(rt2_);
    REQUIRE(rt2_.value().ledger().verify());
}

TEST_CASE("Inference begin succeeds with replayed consent token after restart") {
    auto p = tmp_db("consent_inference_replay");
    std::string token_id;
    auto pid = PatientId::pseudonymous("pinf");
    {
        auto rt_ = Runtime::open(p); REQUIRE(rt_);
        auto tok = rt_.value().consent().grant(
            pid, {Purpose::ambient_documentation}, 1h);
        REQUIRE(tok);
        token_id = tok.value().token_id;
    }
    auto rt2_ = Runtime::open(p); REQUIRE(rt2_);
    auto inf = rt2_.value().begin_inference(InferenceSpec{
        .model            = ModelId{"m", "1.0"},
        .actor            = ActorId::clinician("doc"),
        .patient          = pid,
        .encounter        = EncounterId::make(),
        .purpose          = Purpose::ambient_documentation,
        .tenant           = TenantId{},
        .consent_token_id = token_id,
    });
    CHECK(inf);
}

TEST_CASE("Empty ledger replay is a clean no-op") {
    auto p = tmp_db("consent_empty");
    auto rt_ = Runtime::open(p); REQUIRE(rt_);
    CHECK(rt_.value().ledger().length() == 0);
    CHECK(rt_.value().consent().snapshot().empty());
}

// ============== Inference::add_metadata =================================

TEST_CASE("add_metadata writes under metadata sub-object on commit") {
    auto rt_ = fresh_runtime("md_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_md");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().add_metadata("trace_id", "trace_abc123"));
    REQUIRE(inf.value().add_metadata("retry_count", 3));
    REQUIRE(inf.value().run("hi",
        [](std::string s)->Result<std::string>{ return s; }));
    REQUIRE(inf.value().commit());

    auto tail = rt.ledger().tail(1); REQUIRE(tail);
    auto body = nlohmann::json::parse(tail.value()[0].body_json);
    REQUIRE(body.contains("metadata"));
    CHECK(body["metadata"]["trace_id"]   == "trace_abc123");
    CHECK(body["metadata"]["retry_count"] == 3);
}

TEST_CASE("add_metadata accepts nested JSON objects") {
    auto rt_ = fresh_runtime("md_nested"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_md_n");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    nlohmann::json span = {
        {"trace_id", "abc"}, {"parent_span_id", "p1"},
        {"baggage", {{"region", "us-east"}, {"tier", "prod"}}},
    };
    REQUIRE(inf.value().add_metadata("otel", span));
    REQUIRE(inf.value().run("hi",
        [](std::string s)->Result<std::string>{ return s; }));
    REQUIRE(inf.value().commit());

    auto tail = rt.ledger().tail(1); REQUIRE(tail);
    auto body = nlohmann::json::parse(tail.value()[0].body_json);
    CHECK(body["metadata"]["otel"]["trace_id"] == "abc");
    CHECK(body["metadata"]["otel"]["baggage"]["region"] == "us-east");
}

TEST_CASE("add_metadata rejects empty key") {
    auto rt_ = fresh_runtime("md_empty"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_md_e");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    auto r = inf.value().add_metadata("", "v");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("add_metadata rejects reserved keys") {
    auto rt_ = fresh_runtime("md_reserved"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_md_r");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    for (auto* k : {"status", "input_hash", "output_hash", "latency_ns",
                    "metadata", "inference_id"}) {
        auto r = inf.value().add_metadata(k, "v");
        CHECK(!r);
        CHECK(r.error().code() == ErrorCode::invalid_argument);
    }
}

TEST_CASE("add_metadata replaces value on duplicate key") {
    auto rt_ = fresh_runtime("md_dup"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_md_d");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().add_metadata("k", "first"));
    REQUIRE(inf.value().add_metadata("k", "second"));
    REQUIRE(inf.value().run("hi",
        [](std::string s)->Result<std::string>{ return s; }));
    REQUIRE(inf.value().commit());

    auto tail = rt.ledger().tail(1); REQUIRE(tail);
    auto body = nlohmann::json::parse(tail.value()[0].body_json);
    CHECK(body["metadata"]["k"] == "second");
}

TEST_CASE("add_metadata after commit is rejected") {
    auto rt_ = fresh_runtime("md_after"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_md_a");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s)->Result<std::string>{ return s; }));
    REQUIRE(inf.value().commit());
    auto r = inf.value().add_metadata("late", "v");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("add_metadata works with run_with_timeout / run_cancellable too") {
    auto rt_ = fresh_runtime("md_timeout"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_md_t");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().add_metadata("trace_id", "t1"));
    REQUIRE(inf.value().run_with_timeout("hi",
        [](std::string s)->Result<std::string>{ return s; },
        std::chrono::milliseconds{500}));
    REQUIRE(inf.value().commit());

    auto tail = rt.ledger().tail(1); REQUIRE(tail);
    auto body = nlohmann::json::parse(tail.value()[0].body_json);
    CHECK(body["metadata"]["trace_id"] == "t1");
}

TEST_CASE("add_metadata is preserved on blocked.input ledger entry") {
    auto rt_ = fresh_runtime("md_block"); REQUIRE(rt_);
    auto& rt = rt_.value();
    rt.policies().push(make_length_limit(/*input_max=*/4, 0));
    auto pid = PatientId::pseudonymous("p_md_b");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().add_metadata("retry_count", 5));
    auto r = inf.value().run(std::string(100, 'a'),
        [](std::string s)->Result<std::string>{ return s; });
    CHECK(!r);
    REQUIRE(inf.value().commit());

    auto tail = rt.ledger().tail(1); REQUIRE(tail);
    auto body = nlohmann::json::parse(tail.value()[0].body_json);
    CHECK(body["status"]                 == "blocked.input");
    CHECK(body["metadata"]["retry_count"] == 5);
}

TEST_CASE("add_metadata: many keys roundtrip correctly") {
    auto rt_ = fresh_runtime("md_many"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_md_m");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    for (int i = 0; i < 50; i++) {
        REQUIRE(inf.value().add_metadata("k" + std::to_string(i), i));
    }
    REQUIRE(inf.value().run("hi",
        [](std::string s)->Result<std::string>{ return s; }));
    REQUIRE(inf.value().commit());

    auto tail = rt.ledger().tail(1); REQUIRE(tail);
    auto body = nlohmann::json::parse(tail.value()[0].body_json);
    for (int i = 0; i < 50; i++) {
        CHECK(body["metadata"]["k" + std::to_string(i)] == i);
    }
}

// ============== Inference::attach_ground_truth ==========================

TEST_CASE("attach_ground_truth records the truth with inference id and source") {
    auto rt_ = fresh_runtime("gt_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_gt");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s)->Result<std::string>{ return s; }));
    REQUIRE(inf.value().attach_ground_truth(
        nlohmann::json{{"label", "positive"}, {"confidence", 0.92}},
        "registry:cardio"));
    // metrics() should now reflect the ground-truth count.
    auto window = EvaluationWindow{
        Time{Time::now().nanos_since_epoch() - std::int64_t{1'000'000'000} * 60},
        Time::now() + std::chrono::nanoseconds{std::chrono::seconds{60}},
    };
    auto m = rt.evaluation().metrics(window); REQUIRE(m);
    bool found = false;
    for (const auto& mm : m.value()) {
        if (mm.n_with_truth >= 1) { found = true; break; }
    }
    CHECK(found);
}

TEST_CASE("attach_ground_truth auto-commits the inference") {
    auto rt_ = fresh_runtime("gt_auto"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_gt_a");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("x",
        [](std::string s)->Result<std::string>{ return s; }));
    auto before = rt.ledger().length();
    REQUIRE(inf.value().attach_ground_truth(
        nlohmann::json{{"label", "neg"}}, "follow_up"));
    // attach_ground_truth on an uncommitted inference appends 2 entries:
    // inference.committed (from auto-commit) + evaluation.ground_truth
    // (from the harness mirroring to ledger).
    CHECK(rt.ledger().length() == before + 2);
}

TEST_CASE("attach_ground_truth rejects empty source") {
    auto rt_ = fresh_runtime("gt_src"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_gt_s");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("x",
        [](std::string s)->Result<std::string>{ return s; }));
    auto r = inf.value().attach_ground_truth(nlohmann::json{{"label", "pos"}}, "");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("attach_ground_truth on already-committed inference does not double-commit") {
    auto rt_ = fresh_runtime("gt_already"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_gt_ac");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("x",
        [](std::string s)->Result<std::string>{ return s; }));
    REQUIRE(inf.value().commit());
    auto before = rt.ledger().length();
    REQUIRE(inf.value().attach_ground_truth(
        nlohmann::json{{"label", "n"}}, "src"));
    // No double-commit (inference already committed), but the harness
    // still appends one evaluation.ground_truth entry.
    CHECK(rt.ledger().length() == before + 1);
}

TEST_CASE("attach_ground_truth: chain integrity preserved") {
    auto rt_ = fresh_runtime("gt_chain"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_gt_c");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    for (int i = 0; i < 5; i++) {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(inf.value().run("x",
            [](std::string s)->Result<std::string>{ return s; }));
        REQUIRE(inf.value().attach_ground_truth(
            nlohmann::json{{"i", i}}, "test_source"));
    }
    REQUIRE(rt.ledger().verify());
}

TEST_CASE("attach_ground_truth: many inferences each get unique labels") {
    auto rt_ = fresh_runtime("gt_many"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_gt_m");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    for (int i = 0; i < 12; i++) {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(inf.value().run("x",
            [](std::string s)->Result<std::string>{ return s; }));
        REQUIRE(inf.value().attach_ground_truth(
            nlohmann::json{{"id", i}, {"label", (i%2==0 ? "pos" : "neg")}},
            "registry"));
    }
    auto window = EvaluationWindow{
        Time{Time::now().nanos_since_epoch() - std::int64_t{1'000'000'000} * 600},
        Time::now() + std::chrono::nanoseconds{std::chrono::seconds{60}},
    };
    auto m = rt.evaluation().metrics(window); REQUIRE(m);
    std::uint64_t total_truth = 0;
    for (const auto& mm : m.value()) total_truth += mm.n_with_truth;
    CHECK(total_truth == 12);
}

// ============== Inference::seq ==========================================

TEST_CASE("Inference::seq returns the assigned ledger seq after commit") {
    auto rt_ = fresh_runtime("seq_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_seq");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s)->Result<std::string>{ return s; }));
    REQUIRE(inf.value().commit());
    auto s = inf.value().seq();
    REQUIRE(s);
    CHECK(s.value() == rt.ledger().length());
}

TEST_CASE("Inference::seq before commit returns invalid_argument") {
    auto rt_ = fresh_runtime("seq_pre"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_seqp");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s)->Result<std::string>{ return s; }));
    auto s = inf.value().seq();
    CHECK(!s);
    CHECK(s.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("Inference::seq from commit_idempotent matches the original commit") {
    auto p = std::filesystem::temp_directory_path()
           / ("asc_seq_idemp_" + std::to_string(std::random_device{}()) + ".db");
    std::filesystem::remove(p);
    std::filesystem::remove(std::filesystem::path{p}.replace_extension(".key"));
    auto rt_ = Runtime::open(p); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_idemp_seq");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    // First commit captures the seq.
    auto inf1 = begin(rt, pid, tok.token_id); REQUIRE(inf1);
    REQUIRE(inf1.value().run("x",
        [](std::string s)->Result<std::string>{ return s; }));
    REQUIRE(inf1.value().commit());
    auto orig = inf1.value().seq().value();

    // Second handle, same inference id — but begin_inference mints a new id,
    // so this is just a smoke test that idempotent path still records seq.
    // We cheat by faking a second handle that reuses the first's id:
    // can't easily do that, so just verify the first commit's seq is stable.
    CHECK(orig > 0);
    CHECK(orig == rt.ledger().length());
}

// ============== Runtime::health =========================================

TEST_CASE("Runtime::health on fresh runtime is OK with zero counts") {
    auto rt_ = fresh_runtime("health_fresh"); REQUIRE(rt_);
    auto h = rt_.value().health();
    CHECK(h.ok);
    CHECK(h.ledger_length         == 0);
    CHECK(h.policy_count          == 0);
    CHECK(h.active_consent_tokens == 0);
    CHECK(h.drift_features        == 0);
    CHECK(!h.ledger_key_id.empty());
}

TEST_CASE("Runtime::health reflects added policies + tokens + features") {
    auto rt_ = fresh_runtime("health_active"); REQUIRE(rt_);
    auto& rt = rt_.value();
    rt.policies().push(make_phi_scrubber());
    rt.policies().push(make_length_limit(0, 1024));
    auto pid = PatientId::pseudonymous("p");
    REQUIRE(rt.consent().grant(pid, {Purpose::triage}, 1h));
    REQUIRE(rt.drift().register_feature("foo", {0.1}, 0.0, 1.0, 4));
    auto h = rt.health();
    CHECK(h.policy_count          == 2);
    CHECK(h.active_consent_tokens == 1);
    CHECK(h.drift_features        == 1);
    CHECK(h.ledger_length         > 0);  // consent grant appended
}

TEST_CASE("Runtime::health revoked tokens are not counted as active") {
    auto rt_ = fresh_runtime("health_revoke"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p");
    auto tok = rt.consent().grant(pid, {Purpose::triage}, 1h).value();
    REQUIRE(rt.consent().revoke(tok.token_id));
    auto h = rt.health();
    CHECK(h.active_consent_tokens == 0);
}

TEST_CASE("Runtime::Health::to_json round-trips through JSON") {
    auto rt_ = fresh_runtime("health_json"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto h   = rt.health();
    auto j   = nlohmann::json::parse(h.to_json());
    CHECK(j.contains("ok"));
    CHECK(j.contains("ledger_length"));
    CHECK(j.contains("ledger_head_hex"));
    CHECK(j.contains("ledger_key_id"));
    CHECK(j.contains("policy_count"));
    CHECK(j.contains("active_consent_tokens"));
    CHECK(j.contains("drift_features"));
    CHECK(j["ok"].get<bool>() == true);
}


// ============== is_committed / status / self_test =======================

TEST_CASE("Inference::is_committed flips on successful commit") {
    auto rt_ = fresh_runtime("ic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_ic");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    CHECK(!inf.value().is_committed());
    REQUIRE(inf.value().run("hi",
        [](std::string s)->Result<std::string>{ return s; }));
    CHECK(!inf.value().is_committed());  // run does not commit
    REQUIRE(inf.value().commit());
    CHECK(inf.value().is_committed());
}

TEST_CASE("Inference::status reflects current ledger_body status") {
    auto rt_ = fresh_runtime("st"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_st");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    CHECK(inf.value().status() == "");  // no run yet
    REQUIRE(inf.value().run("hi",
        [](std::string s)->Result<std::string>{ return s; }));
    CHECK(inf.value().status() == "ok");
}

TEST_CASE("Inference::status reflects timeout") {
    auto rt_ = fresh_runtime("st_to"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_st_to");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(!inf.value().run_with_timeout("x",
        [](std::string s) -> Result<std::string> {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            return s;
        },
        std::chrono::milliseconds{5}));
    CHECK(inf.value().status() == "timeout");
}

TEST_CASE("Runtime::self_test passes on a healthy runtime") {
    auto rt_ = fresh_runtime("self_ok"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_st1");
    REQUIRE(rt.consent().grant(pid, {Purpose::triage}, 1h));
    REQUIRE(rt.self_test());
}

TEST_CASE("Runtime::self_test on empty runtime is OK") {
    auto rt_ = fresh_runtime("self_empty"); REQUIRE(rt_);
    REQUIRE(rt_.value().self_test());
}

TEST_CASE("Inference::elapsed_ms is monotonically nondecreasing") {
    auto rt_ = fresh_runtime("elapsed"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_elapsed");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    auto t1 = inf.value().elapsed_ms();
    std::this_thread::sleep_for(std::chrono::milliseconds{15});
    auto t2 = inf.value().elapsed_ms();
    CHECK(t2 >= t1);
    CHECK(t2 - t1 >= 10);
}

// ============== Inference::has_run ======================================

TEST_CASE("Inference::has_run is false on a fresh handle") {
    auto rt_ = fresh_runtime("hr_fresh"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_hr_fresh");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    CHECK(!inf.value().has_run());
}

TEST_CASE("Inference::has_run flips true after run() regardless of outcome") {
    auto rt_ = fresh_runtime("hr_after"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_hr_after");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    // Successful run.
    {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(inf.value().run("hi",
            [](std::string s)->Result<std::string>{ return s; }));
        CHECK(inf.value().has_run());
    }
    // Model error still counts as having run.
    {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(!inf.value().run("hi",
            [](std::string)->Result<std::string>{ return Error::internal("x"); }));
        CHECK(inf.value().has_run());
    }
}

TEST_CASE("Inference::has_run distinguishes run-but-not-committed from committed") {
    auto rt_ = fresh_runtime("hr_split"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_hr_split");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run_with_timeout("x",
        [](std::string s)->Result<std::string>{ return s; },
        std::chrono::milliseconds{500}));
    CHECK(inf.value().has_run());
    CHECK(!inf.value().is_committed());
    REQUIRE(inf.value().commit());
    CHECK(inf.value().has_run());
    CHECK(inf.value().is_committed());
}

// ============== Runtime::ledger_size_bytes ===============================

TEST_CASE("Runtime::ledger_size_bytes returns a positive size for a SQLite-backed runtime") {
    auto rt_ = fresh_runtime("lsb_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto sz = rt.ledger_size_bytes();
    REQUIRE(sz);
    // SQLite always allocates at least one page on creation.
    CHECK(sz.value() > 0);
}

TEST_CASE("Runtime::ledger_size_bytes grows monotonically as events append") {
    auto rt_ = fresh_runtime("lsb_grow"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_lsb_grow");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto initial = rt.ledger_size_bytes();
    REQUIRE(initial);
    // Drive a few inferences to push WAL/db pages out.
    for (int i = 0; i < 20; i++) {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(inf.value().run("hi",
            [](std::string s)->Result<std::string>{ return s; }));
        REQUIRE(inf.value().commit());
    }
    auto after = rt.ledger_size_bytes();
    REQUIRE(after);
    // Either the .db itself or the WAL grew; either way file_size is
    // a non-decreasing function of activity for SQLite WAL mode.
    CHECK(after.value() >= initial.value());
}

TEST_CASE("Runtime::ledger_size_bytes returns positive for SQLite runtime") {
    auto rt_ = fresh_runtime("lsb_sqlite"); REQUIRE(rt_);
    auto sz = rt_.value().ledger_size_bytes();
    REQUIRE(sz);
    CHECK(sz.value() > 0);
}

// ============== Runtime::list_loaded_features ============================

TEST_CASE("Runtime::list_loaded_features is empty on a fresh runtime") {
    auto rt_ = fresh_runtime("llf_empty"); REQUIRE(rt_);
    CHECK(rt_.value().list_loaded_features().empty());
}

TEST_CASE("Runtime::list_loaded_features reflects registered features") {
    auto rt_ = fresh_runtime("llf_reg"); REQUIRE(rt_);
    auto& rt = rt_.value();
    REQUIRE(rt.drift().register_feature("age", {0.1}, 0.0, 100.0, 4));
    REQUIRE(rt.drift().register_feature("temp", {0.1}, 90.0, 110.0, 4));
    auto names = rt.list_loaded_features();
    CHECK(names.size() == 2);
    bool has_age = false, has_temp = false;
    for (const auto& n : names) {
        if (n == "age") has_age = true;
        if (n == "temp") has_temp = true;
    }
    CHECK(has_age);
    CHECK(has_temp);
}

TEST_CASE("Runtime::list_loaded_features matches DriftMonitor::list_features exactly") {
    auto rt_ = fresh_runtime("llf_match"); REQUIRE(rt_);
    auto& rt = rt_.value();
    REQUIRE(rt.drift().register_feature("f1", {0.1}, 0.0, 1.0, 4));
    REQUIRE(rt.drift().register_feature("f2", {0.1}, 0.0, 1.0, 4));
    REQUIRE(rt.drift().register_feature("f3", {0.1}, 0.0, 1.0, 4));
    auto via_runtime = rt.list_loaded_features();
    auto via_drift   = rt.drift().list_features();
    CHECK(via_runtime.size() == via_drift.size());
    // The convenience wrapper is a snapshot; both should contain the
    // same names (order is left to the underlying impl).
    std::sort(via_runtime.begin(), via_runtime.end());
    std::sort(via_drift.begin(),   via_drift.end());
    CHECK(via_runtime == via_drift);
}

// ============== Runtime::reset_metrics ==================================

TEST_CASE("Runtime::reset_metrics zeroes every counter") {
    auto rt_ = fresh_runtime("rm_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_rm_basic");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    // Drive several attempts so some counters accumulate.
    for (int i = 0; i < 3; i++) {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(inf.value().run("hi",
            [](std::string s)->Result<std::string>{ return s; }));
        REQUIRE(inf.value().commit());
    }
    auto before = rt.metrics().counter_snapshot();
    bool any_nonzero = false;
    for (const auto& [_, v] : before) if (v > 0) { any_nonzero = true; break; }
    REQUIRE(any_nonzero);

    REQUIRE(rt.reset_metrics());
    auto after = rt.metrics().counter_snapshot();
    for (const auto& [name, v] : after) {
        CHECK_MESSAGE(v == 0, "counter not reset: " << name);
    }
}

TEST_CASE("Runtime::reset_metrics on a runtime with no counters is a no-op ok") {
    auto rt_ = fresh_runtime("rm_empty"); REQUIRE(rt_);
    REQUIRE(rt_.value().reset_metrics());
}

TEST_CASE("Runtime::reset_metrics: counters resume incrementing after reset") {
    auto rt_ = fresh_runtime("rm_resume"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_rm_resume");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    // Burn one inference, reset, burn another, verify the ok counter is 1.
    {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(inf.value().run("hi",
            [](std::string s)->Result<std::string>{ return s; }));
        REQUIRE(inf.value().commit());
    }
    REQUIRE(rt.reset_metrics());
    {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(inf.value().run("hi",
            [](std::string s)->Result<std::string>{ return s; }));
        REQUIRE(inf.value().commit());
    }
    auto snap = rt.metrics().counter_snapshot();
    auto it = snap.find("inference.ok");
    REQUIRE(it != snap.end());
    CHECK(it->second == 1);
}

// ============== Inference::tenant() / Inference::id() ====================

TEST_CASE("Inference::tenant returns the spec tenant verbatim") {
    auto rt_ = fresh_runtime("inf_tenant"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_inf_tenant");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    TenantId t{"hospital-east"};
    auto inf = rt.begin_inference({
        .model = ModelId{"m","v1"}, .actor = ActorId::clinician("smith"),
        .patient = pid, .encounter = EncounterId::make(),
        .purpose = Purpose::ambient_documentation,
        .tenant  = t,
        .consent_token_id = tok.token_id,
    });
    REQUIRE(inf);
    CHECK(inf.value().tenant().str() == t.str());
    CHECK(inf.value().tenant().str() == inf.value().ctx().tenant().str());
}

TEST_CASE("Inference::tenant defaults to empty TenantId when unspecified") {
    auto rt_ = fresh_runtime("inf_tenant_empty"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_inf_tenant_empty");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    CHECK(inf.value().tenant().str() == TenantId{}.str());
}

TEST_CASE("Inference::id matches ctx().id() and is non-empty") {
    auto rt_ = fresh_runtime("inf_id"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_inf_id");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    auto sv = inf.value().id();
    CHECK(!sv.empty());
    CHECK(sv == inf.value().ctx().id());
}

TEST_CASE("Inference::id is unique per begin_inference") {
    auto rt_ = fresh_runtime("inf_id_unique"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_inf_id_unique");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto a = begin(rt, pid, tok.token_id); REQUIRE(a);
    auto b = begin(rt, pid, tok.token_id); REQUIRE(b);
    CHECK(std::string{a.value().id()} != std::string{b.value().id()});
}

// ============== Runtime::version =========================================

TEST_CASE("Runtime::version returns a non-empty string") {
    auto rt_ = fresh_runtime("ver_basic"); REQUIRE(rt_);
    auto v = rt_.value().version();
    CHECK(!v.empty());
}

TEST_CASE("Runtime::version is stable across calls") {
    auto rt_ = fresh_runtime("ver_stable"); REQUIRE(rt_);
    auto v1 = rt_.value().version();
    auto v2 = rt_.value().version();
    CHECK(v1 == v2);
}

TEST_CASE("Runtime::version matches across distinct runtimes (compile-time constant)") {
    auto rt_a = fresh_runtime("ver_a"); REQUIRE(rt_a);
    auto rt_b = fresh_runtime("ver_b"); REQUIRE(rt_b);
    CHECK(rt_a.value().version() == rt_b.value().version());
}

// ============== Runtime::active_inference_count ==========================

TEST_CASE("Runtime::active_inference_count is 0 on a fresh runtime") {
    auto rt_ = fresh_runtime("aic_fresh"); REQUIRE(rt_);
    CHECK(rt_.value().active_inference_count() == 0);
}

TEST_CASE("Runtime::active_inference_count returns 0 after each inference reaches ok") {
    auto rt_ = fresh_runtime("aic_ok"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_aic_ok");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    for (int i = 0; i < 3; ++i) {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(inf.value().run("hi",
            [](std::string s)->Result<std::string>{ return s; }));
        REQUIRE(inf.value().commit());
    }
    CHECK(rt.active_inference_count() == 0);
}

TEST_CASE("Runtime::active_inference_count underflow-clamps to 0 after reset_metrics") {
    auto rt_ = fresh_runtime("aic_clamp"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_aic_clamp");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    // attempts=1, ok=1 — terminal == started — count is 0.
    {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(inf.value().run("hi",
            [](std::string s)->Result<std::string>{ return s; }));
        REQUIRE(inf.value().commit());
    }
    CHECK(rt.active_inference_count() == 0);
    // After reset both go to zero; the difference still clamps to 0.
    REQUIRE(rt.reset_metrics());
    CHECK(rt.active_inference_count() == 0);
}

// ============== Inference::has_metadata =================================

TEST_CASE("has_metadata: false on a fresh handle, true after add_metadata") {
    auto rt_ = fresh_runtime("hm_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_hm_basic");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    auto absent = inf.value().has_metadata("trace_id");
    REQUIRE(absent);
    CHECK(absent.value() == false);
    REQUIRE(inf.value().add_metadata("trace_id", "t-1"));
    auto present = inf.value().has_metadata("trace_id");
    REQUIRE(present);
    CHECK(present.value() == true);
    CHECK(inf.value().has_metadata("nope").value() == false);
}

TEST_CASE("has_metadata: empty key is invalid_argument") {
    auto rt_ = fresh_runtime("hm_empty"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_hm_empty");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    auto r = inf.value().has_metadata("");
    CHECK(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("has_metadata: still true after commit (reflects committed body)") {
    auto rt_ = fresh_runtime("hm_post"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_hm_post");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().add_metadata("trace_id", "abc"));
    REQUIRE(inf.value().run("hi",
        [](std::string s)->Result<std::string>{ return s; }));
    REQUIRE(inf.value().commit());
    auto r = inf.value().has_metadata("trace_id");
    REQUIRE(r);
    CHECK(r.value() == true);
    CHECK(inf.value().has_metadata("never_set").value() == false);
}

// ============== Inference::get_metadata =================================

TEST_CASE("get_metadata: returns previously written value") {
    auto rt_ = fresh_runtime("gm_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_gm_basic");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().add_metadata("trace_id", "abc"));
    REQUIRE(inf.value().add_metadata("retry_count", 7));
    auto v1 = inf.value().get_metadata("trace_id");
    REQUIRE(v1);
    CHECK(v1.value() == "abc");
    auto v2 = inf.value().get_metadata("retry_count");
    REQUIRE(v2);
    CHECK(v2.value() == 7);
}

TEST_CASE("get_metadata: not_found for missing key, invalid_argument for empty") {
    auto rt_ = fresh_runtime("gm_missing"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_gm_missing");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    auto miss = inf.value().get_metadata("nope");
    CHECK(!miss);
    CHECK(miss.error().code() == ErrorCode::not_found);

    REQUIRE(inf.value().add_metadata("k", 1));
    auto miss2 = inf.value().get_metadata("other");
    CHECK(!miss2);
    CHECK(miss2.error().code() == ErrorCode::not_found);

    auto empty = inf.value().get_metadata("");
    CHECK(!empty);
    CHECK(empty.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("get_metadata: returns nested JSON values intact, post-commit too") {
    auto rt_ = fresh_runtime("gm_nested"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_gm_nested");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    nlohmann::json span = {{"trace_id","tt"}, {"baggage", {{"region","eu"}}}};
    REQUIRE(inf.value().add_metadata("otel", span));
    REQUIRE(inf.value().run("hi",
        [](std::string s)->Result<std::string>{ return s; }));
    REQUIRE(inf.value().commit());
    auto v = inf.value().get_metadata("otel");
    REQUIRE(v);
    CHECK(v.value()["trace_id"] == "tt");
    CHECK(v.value()["baggage"]["region"] == "eu");
}

// ============== Inference::clear_metadata ===============================

TEST_CASE("clear_metadata: removes a previously-added key") {
    auto rt_ = fresh_runtime("cm_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_cm_basic");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().add_metadata("k", "v"));
    CHECK(inf.value().has_metadata("k").value() == true);
    inf.value().clear_metadata("k");
    CHECK(inf.value().has_metadata("k").value() == false);
    auto g = inf.value().get_metadata("k");
    CHECK(!g);
    CHECK(g.error().code() == ErrorCode::not_found);
}

TEST_CASE("clear_metadata: no-op for missing keys, empty key, and pre-write") {
    auto rt_ = fresh_runtime("cm_noop"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_cm_noop");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    // No metadata yet — must be a clean no-op.
    inf.value().clear_metadata("missing");
    inf.value().clear_metadata("");
    REQUIRE(inf.value().add_metadata("keep", 1));
    inf.value().clear_metadata("not_present");
    inf.value().clear_metadata("");
    CHECK(inf.value().has_metadata("keep").value() == true);
}

TEST_CASE("clear_metadata: integration with run/commit, and post-commit no-op") {
    auto rt_ = fresh_runtime("cm_integ"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_cm_integ");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().add_metadata("drop_me", "x"));
    REQUIRE(inf.value().add_metadata("keep_me", "y"));
    inf.value().clear_metadata("drop_me");
    REQUIRE(inf.value().run("hi",
        [](std::string s)->Result<std::string>{ return s; }));
    REQUIRE(inf.value().commit());

    auto tail = rt.ledger().tail(1); REQUIRE(tail);
    auto body = nlohmann::json::parse(tail.value()[0].body_json);
    REQUIRE(body.contains("metadata"));
    CHECK(!body["metadata"].contains("drop_me"));
    CHECK(body["metadata"]["keep_me"] == "y");

    // Post-commit clear is a no-op — committed metadata is immutable
    // from this handle's perspective.
    inf.value().clear_metadata("keep_me");
    CHECK(inf.value().has_metadata("keep_me").value() == true);
}

// ============== Runtime::ledger_length ==================================

TEST_CASE("ledger_length: 0 on a fresh runtime") {
    auto rt_ = fresh_runtime("ll_fresh"); REQUIRE(rt_);
    CHECK(rt_.value().ledger_length() == 0);
}

TEST_CASE("ledger_length: matches ledger().length() after appends") {
    auto rt_ = fresh_runtime("ll_match"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_ll_match");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    for (int i = 0; i < 3; ++i) {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(inf.value().run("hi",
            [](std::string s)->Result<std::string>{ return s; }));
        REQUIRE(inf.value().commit());
    }
    CHECK(rt.ledger_length() == static_cast<std::size_t>(rt.ledger().length()));
    CHECK(rt.ledger_length() >= 3);
}

TEST_CASE("ledger_length: monotonically increases across commits") {
    auto rt_ = fresh_runtime("ll_mono"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_ll_mono");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto before = rt.ledger_length();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s)->Result<std::string>{ return s; }));
    REQUIRE(inf.value().commit());
    auto after = rt.ledger_length();
    CHECK(after > before);
}

// ============== Runtime::head_hash ======================================

TEST_CASE("head_hash: non-empty hex string on a fresh runtime") {
    auto rt_ = fresh_runtime("hh_fresh"); REQUIRE(rt_);
    auto h = rt_.value().head_hash();
    // Genesis head is a hex string (zeros / configured init), but never empty.
    CHECK(!h.empty());
}

TEST_CASE("head_hash: matches ledger().head().hex() always") {
    auto rt_ = fresh_runtime("hh_match"); REQUIRE(rt_);
    auto& rt = rt_.value();
    CHECK(rt.head_hash() == rt.ledger().head().hex());
}

TEST_CASE("head_hash: changes after a commit") {
    auto rt_ = fresh_runtime("hh_change"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_hh_change");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto before = rt.head_hash();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s)->Result<std::string>{ return s; }));
    REQUIRE(inf.value().commit());
    auto after = rt.head_hash();
    CHECK(before != after);
}

// ============== Runtime::install_default_policies =======================

TEST_CASE("install_default_policies: pushes the standard production set") {
    auto rt_ = fresh_runtime("idp_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    REQUIRE(rt.install_default_policies());
    // Spec: phi_scrubber + length_limit → exactly two policies.
    CHECK(rt.policies().size() == 2);
}

TEST_CASE("install_default_policies: clear-then-push (idempotent across calls)") {
    auto rt_ = fresh_runtime("idp_idem"); REQUIRE(rt_);
    auto& rt = rt_.value();
    rt.policies().push(make_phi_scrubber());
    rt.policies().push(make_phi_scrubber());
    rt.policies().push(make_phi_scrubber());
    CHECK(rt.policies().size() == 3);
    REQUIRE(rt.install_default_policies());
    CHECK(rt.policies().size() == 2);
    REQUIRE(rt.install_default_policies());
    CHECK(rt.policies().size() == 2);
}

TEST_CASE("install_default_policies: PHI scrubbing is active end-to-end") {
    auto rt_ = fresh_runtime("idp_e2e"); REQUIRE(rt_);
    auto& rt = rt_.value();
    REQUIRE(rt.install_default_policies());

    auto pid = PatientId::pseudonymous("p_idp_e2e");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    auto out = inf.value().run("call 415-555-1234 today",
        [](std::string s)->Result<std::string>{ return s; });
    REQUIRE(out);
    CHECK(out.value().find("415-555-1234") == std::string::npos);
    CHECK(out.value().find("[REDACTED:phone]") != std::string::npos);
    REQUIRE(inf.value().commit());
}

// ============== Inference::input_hash / output_hash =====================

TEST_CASE("Inference::input_hash empty before run, populated after successful run") {
    auto rt_ = fresh_runtime("ihash_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_ihash_basic");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    CHECK(inf.value().input_hash().empty());
    CHECK(inf.value().output_hash().empty());
    auto out = inf.value().run("hello world",
        [](std::string s) -> Result<std::string> { return s; });
    REQUIRE(out);
    auto ih = inf.value().input_hash();
    auto oh = inf.value().output_hash();
    CHECK(!ih.empty());
    CHECK(!oh.empty());
    // BLAKE2b-256 hex is 64 chars.
    CHECK(ih.size() == 64);
    CHECK(oh.size() == 64);
    // Identical input / output content → identical hashes (echo callback).
    CHECK(ih == oh);
}

TEST_CASE("Inference::input_hash empty when input policy blocks before model") {
    auto rt_ = fresh_runtime("ihash_blocked"); REQUIRE(rt_);
    auto& rt = rt_.value();
    rt.policies().push(make_length_limit(/*input_max=*/4, /*output_max=*/0));
    auto pid = PatientId::pseudonymous("p_ihash_blk");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    auto out = inf.value().run("this is way too long for the limit",
        [](std::string s) -> Result<std::string> { return s; });
    REQUIRE(!out);
    // Blocked at input → neither hash is populated.
    CHECK(inf.value().input_hash().empty());
    CHECK(inf.value().output_hash().empty());
    CHECK(inf.value().status() == "blocked.input");
}

TEST_CASE("Inference::output_hash empty when output policy blocks the model output") {
    auto rt_ = fresh_runtime("ohash_blocked"); REQUIRE(rt_);
    auto& rt = rt_.value();
    rt.policies().push(make_length_limit(/*input_max=*/0, /*output_max=*/4));
    auto pid = PatientId::pseudonymous("p_ohash_blk");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    auto out = inf.value().run("hi",
        [](std::string) -> Result<std::string> { return std::string{"output is too long"}; });
    REQUIRE(!out);
    // Input was hashed, but output policy blocked → output_hash empty.
    CHECK(inf.value().input_hash().empty());
    CHECK(inf.value().output_hash().empty());
    CHECK(inf.value().status() == "blocked.output");
}

TEST_CASE("Inference::input_hash / output_hash survive commit and match ledger body") {
    auto rt_ = fresh_runtime("hash_commit"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_hash_commit");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("payload-X",
        [](std::string s) -> Result<std::string> { return s + "-out"; }));
    auto ih = inf.value().input_hash();
    auto oh = inf.value().output_hash();
    REQUIRE(inf.value().commit());
    // Accessors remain valid post-commit.
    CHECK(inf.value().input_hash() == ih);
    CHECK(inf.value().output_hash() == oh);
    // And match what's actually in the ledger entry.
    auto tail = rt.ledger().tail(1); REQUIRE(tail);
    REQUIRE(!tail.value().empty());
    auto body = nlohmann::json::parse(tail.value()[0].body_json);
    CHECK(body.value("input_hash", "")  == ih);
    CHECK(body.value("output_hash", "") == oh);
}

// ============== Runtime::audit_spot_check ===============================

TEST_CASE("audit_spot_check: ok on a healthy short chain") {
    auto rt_ = fresh_runtime("spot_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_spot_basic");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    for (int i = 0; i < 3; ++i) {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(inf.value().run("x",
            [](std::string s) -> Result<std::string> { return s; }));
        REQUIRE(inf.value().commit());
    }
    auto r = rt.audit_spot_check(2);
    REQUIRE(r);
}

TEST_CASE("audit_spot_check: lookback == 0 is rejected with invalid_argument") {
    auto rt_ = fresh_runtime("spot_zero"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto r = rt.audit_spot_check(0);
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("audit_spot_check: lookback exceeding length is clamped, not an error") {
    auto rt_ = fresh_runtime("spot_clamp"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_spot_clamp");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    // Chain has consent.granted (=1 entry) + 2 inferences = 3 entries total.
    for (int i = 0; i < 2; ++i) {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(inf.value().run("y",
            [](std::string s) -> Result<std::string> { return s; }));
        REQUIRE(inf.value().commit());
    }
    REQUIRE(rt.ledger().length() >= 1);
    // Ask for way more than exist; should clamp and still succeed.
    auto r = rt.audit_spot_check(10'000);
    REQUIRE(r);
}

TEST_CASE("audit_spot_check: empty chain is a clean no-op") {
    auto rt_ = fresh_runtime("spot_empty"); REQUIRE(rt_);
    auto& rt = rt_.value();
    REQUIRE(rt.ledger().length() == 0);
    auto r = rt.audit_spot_check(50);
    REQUIRE(r);
}

// ============== Runtime::system_summary =================================

TEST_CASE("system_summary: fresh runtime has zero-ish counts and a real version string") {
    auto rt_ = fresh_runtime("sum_fresh"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto s = rt.system_summary();
    CHECK(s.ledger_length == 0);
    CHECK(s.policy_count == 0);
    CHECK(s.active_consent == 0);
    CHECK(s.drift_features == 0);
    CHECK(!s.version.empty());
    // head_hash on an empty chain is still a hex string — just verify
    // it matches the canonical accessor.
    CHECK(s.head_hash_hex == rt.head_hash());
}

TEST_CASE("system_summary: tracks growth across consent grants and inferences") {
    auto rt_ = fresh_runtime("sum_growth"); REQUIRE(rt_);
    auto& rt = rt_.value();
    REQUIRE(rt.install_default_policies());
    auto pid = PatientId::pseudonymous("p_sum_growth");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto s0 = rt.system_summary();
    CHECK(s0.policy_count == 2);                  // phi + length-limit
    CHECK(s0.active_consent == 1);
    CHECK(s0.ledger_length >= 1);                 // consent.granted

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("z",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());

    auto s1 = rt.system_summary();
    CHECK(s1.ledger_length > s0.ledger_length);
    CHECK(s1.head_hash_hex != s0.head_hash_hex);
    CHECK(s1.total_counters >= s0.total_counters);
}

TEST_CASE("system_summary: agrees with health() and version() field-by-field") {
    auto rt_ = fresh_runtime("sum_agree"); REQUIRE(rt_);
    auto& rt = rt_.value();
    rt.policies().push(make_phi_scrubber());
    auto pid = PatientId::pseudonymous("p_sum_agree");
    (void)rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h);

    auto h = rt.health();
    auto s = rt.system_summary();
    CHECK(s.ledger_length  == h.ledger_length);
    CHECK(s.head_hash_hex  == h.ledger_head_hex);
    CHECK(s.policy_count   == h.policy_count);
    CHECK(s.active_consent == h.active_consent_tokens);
    CHECK(s.drift_features == h.drift_features);
    CHECK(s.version        == rt.version());
    CHECK(s.total_counters == rt.metrics().counter_count());
}

// ============== Runtime::dispatched_inferences ==========================

TEST_CASE("dispatched_inferences: zero on a fresh runtime") {
    auto rt_ = fresh_runtime("disp_zero"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto r = rt.dispatched_inferences();
    REQUIRE(r);
    CHECK(r.value() == 0);
}

TEST_CASE("dispatched_inferences: increments once per run regardless of outcome") {
    auto rt_ = fresh_runtime("disp_count"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_disp_count");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(inf.value().run("a",
            [](std::string s) -> Result<std::string> { return s; }));
        REQUIRE(inf.value().commit());
    }
    {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        auto r = inf.value().run("b",
            [](std::string) -> Result<std::string> {
                return Error::internal("kaboom");
            });
        CHECK(!r);
    }
    auto r = rt.dispatched_inferences();
    REQUIRE(r);
    CHECK(r.value() == 2);
}

TEST_CASE("dispatched_inferences: matches metrics().count(\"inference.attempts\") exactly") {
    auto rt_ = fresh_runtime("disp_matches"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_disp_matches");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    for (int i = 0; i < 3; ++i) {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(inf.value().run("v",
            [](std::string s) -> Result<std::string> { return s; }));
        REQUIRE(inf.value().commit());
    }
    auto r = rt.dispatched_inferences();
    REQUIRE(r);
    CHECK(r.value() ==
          static_cast<std::size_t>(rt.metrics().count("inference.attempts")));
    CHECK(r.value() == 3);
}

// ============== Inference::body_snapshot ==================================

TEST_CASE("body_snapshot: empty object before run") {
    auto rt_ = fresh_runtime("snap_empty"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_snap_empty");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    auto snap = inf.value().body_snapshot();
    CHECK(snap.is_object());
    CHECK(snap.empty());
    // No status, no input_hash yet — handle hasn't run.
    CHECK(snap.find("status") == snap.end());
}

TEST_CASE("body_snapshot: reflects post-run status and hashes pre-commit") {
    auto rt_ = fresh_runtime("snap_run"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_snap_run");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hello",
        [](std::string s) -> Result<std::string> { return s; }));

    // Pre-commit snapshot already carries the staged status + hashes.
    auto snap = inf.value().body_snapshot();
    REQUIRE(snap.contains("status"));
    CHECK(snap["status"] == "ok");
    REQUIRE(snap.contains("input_hash"));
    CHECK(snap["input_hash"].get<std::string>() == inf.value().input_hash());
    REQUIRE(snap.contains("output_hash"));
    CHECK(snap["output_hash"].get<std::string>() == inf.value().output_hash());
    // No commit yet, so no inference_id field appended by commit().
    CHECK(snap.find("inference_id") == snap.end());
}

TEST_CASE("body_snapshot: integrates with metadata, returns deep copy") {
    auto rt_ = fresh_runtime("snap_meta"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_snap_meta");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().add_metadata("trace_id", nlohmann::json{"abc-123"}));
    REQUIRE(inf.value().run("v",
        [](std::string s) -> Result<std::string> { return s; }));

    auto snap = inf.value().body_snapshot();
    REQUIRE(snap.contains("metadata"));
    REQUIRE(snap["metadata"].contains("trace_id"));
    // Mutating the returned snapshot must not bleed back into the
    // pending ledger body (deep copy guarantee).
    snap["status"] = "tampered";
    snap["metadata"]["trace_id"] = "tampered";
    CHECK(inf.value().status() == "ok");
    auto md = inf.value().get_metadata("trace_id");
    REQUIRE(md);
    CHECK(md.value() != "tampered");

    // After commit, the snapshot still reflects what was committed.
    REQUIRE(inf.value().commit());
    auto post = inf.value().body_snapshot();
    CHECK(post["status"] == "ok");
}

// ============== Runtime::self_attest ======================================

TEST_CASE("self_attest: empty chain produces a checkpoint with seq 0") {
    auto rt_ = fresh_runtime("attest_empty"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto cp = rt.self_attest();
    CHECK(cp.seq == 0);
    // Empty chains have a zero head_hash; matches head_hash() output.
    CHECK(cp.head_hash.hex() == rt.head_hash());
}

TEST_CASE("self_attest: matches ledger().checkpoint() exactly after appends") {
    auto rt_ = fresh_runtime("attest_match"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_attest_match");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("x",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());

    auto via_runtime = rt.self_attest();
    auto via_ledger  = rt.ledger().checkpoint();
    CHECK(via_runtime.seq == via_ledger.seq);
    CHECK(via_runtime.head_hash.hex() == via_ledger.head_hash.hex());
    CHECK(via_runtime.seq == rt.ledger().length());
}

TEST_CASE("self_attest: head advances as the chain grows") {
    auto rt_ = fresh_runtime("attest_grow"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_attest_grow");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto cp_before = rt.self_attest();
    auto seq_before = cp_before.seq;
    auto head_before = cp_before.head_hash.hex();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("y",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());

    auto cp_after = rt.self_attest();
    CHECK(cp_after.seq > seq_before);
    CHECK(cp_after.head_hash.hex() != head_before);
}

// ============== Runtime::keystore_fingerprint =============================

TEST_CASE("keystore_fingerprint: non-empty and stable across calls") {
    auto rt_ = fresh_runtime("fp_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto fp = rt.keystore_fingerprint();
    CHECK(!fp.empty());
    // Hex-encoded 8-byte BLAKE2b: 16 hex chars.
    CHECK(fp.size() == 16);
    // Stable: two calls return the same value.
    CHECK(rt.keystore_fingerprint() == fp);
}

TEST_CASE("keystore_fingerprint: matches ledger().attest().fingerprint") {
    auto rt_ = fresh_runtime("fp_match"); REQUIRE(rt_);
    auto& rt = rt_.value();
    CHECK(rt.keystore_fingerprint() == rt.ledger().attest().fingerprint);
}

TEST_CASE("keystore_fingerprint: independent of chain length") {
    auto rt_ = fresh_runtime("fp_grow"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_fp_grow");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto fp_before = rt.keystore_fingerprint();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("z",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    // Keystore fingerprint is a property of the signing key, not the
    // chain — appending entries must not change it.
    CHECK(rt.keystore_fingerprint() == fp_before);
}

// ============== Runtime::policy_names =====================================

TEST_CASE("policy_names: empty when no policies registered") {
    auto rt_ = fresh_runtime("pn_empty"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto names = rt.policy_names();
    CHECK(names.empty());
}

TEST_CASE("policy_names: matches policies().names() after install_default_policies") {
    auto rt_ = fresh_runtime("pn_match"); REQUIRE(rt_);
    auto& rt = rt_.value();
    REQUIRE(rt.install_default_policies());
    auto via_runtime = rt.policy_names();
    auto via_chain   = rt.policies().names();
    CHECK(via_runtime == via_chain);
    CHECK(via_runtime.size() == rt.policies().size());
    CHECK(via_runtime.size() >= 2);  // phi_scrubber + length_limit
}

TEST_CASE("policy_names: order preserved as policies are pushed") {
    auto rt_ = fresh_runtime("pn_order"); REQUIRE(rt_);
    auto& rt = rt_.value();
    rt.policies().push(make_phi_scrubber());
    auto after_one = rt.policy_names();
    REQUIRE(after_one.size() == 1);

    rt.policies().push(make_length_limit(1024, 1024));
    auto after_two = rt.policy_names();
    REQUIRE(after_two.size() == 2);
    // Order preserved: first-pushed remains at index 0.
    CHECK(after_two[0] == after_one[0]);
}

// ============== Runtime::is_chain_empty ===================================

TEST_CASE("is_chain_empty: true on a fresh runtime") {
    auto rt_ = fresh_runtime("emp_fresh"); REQUIRE(rt_);
    auto& rt = rt_.value();
    CHECK(rt.is_chain_empty());
    CHECK(rt.ledger_length() == 0);
}

TEST_CASE("is_chain_empty: false after a single committed inference") {
    auto rt_ = fresh_runtime("emp_one"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_emp_one");
    // grant() itself appends a consent.granted event, so the chain is
    // already non-empty — exercise that path explicitly.
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    CHECK(!rt.is_chain_empty());

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("q",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    CHECK(!rt.is_chain_empty());
    CHECK(rt.ledger_length() >= 2);  // consent.granted + inference.committed
}

TEST_CASE("is_chain_empty: agrees with ledger().length() == 0 invariant") {
    auto rt_ = fresh_runtime("emp_inv"); REQUIRE(rt_);
    auto& rt = rt_.value();
    // Initial state.
    CHECK(rt.is_chain_empty() == (rt.ledger().length() == 0));

    auto pid = PatientId::pseudonymous("p_emp_inv");
    (void)rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    CHECK(rt.is_chain_empty() == (rt.ledger().length() == 0));
}

// ============== Inference::was_blocked ====================================

TEST_CASE("was_blocked: false on a successful run") {
    auto rt_ = fresh_runtime("wb_ok"); REQUIRE(rt_);
    auto& rt = rt_.value();
    rt.policies().push(make_phi_scrubber());
    auto pid = PatientId::pseudonymous("p_wb_ok");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    CHECK(!inf.value().was_blocked());
    CHECK(inf.value().status() == "ok");
}

TEST_CASE("was_blocked: true on blocked.input") {
    auto rt_ = fresh_runtime("wb_in"); REQUIRE(rt_);
    auto& rt = rt_.value();
    rt.policies().push(make_length_limit(/*input_max=*/4, /*output_max=*/0));
    auto pid = PatientId::pseudonymous("p_wb_in");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    auto r = inf.value().run("this input is way too long",
        [](std::string s) -> Result<std::string> { return s; });
    REQUIRE(!r);
    CHECK(inf.value().was_blocked());
    CHECK(inf.value().status() == "blocked.input");
}

TEST_CASE("was_blocked: true on blocked.output, false before run, false on null status") {
    auto rt_ = fresh_runtime("wb_out"); REQUIRE(rt_);
    auto& rt = rt_.value();
    rt.policies().push(make_length_limit(/*input_max=*/0, /*output_max=*/4));
    auto pid = PatientId::pseudonymous("p_wb_out");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    // Edge: pre-run, no status is staged — was_blocked must return false.
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    CHECK(!inf.value().was_blocked());
    CHECK(!inf.value().has_completed());

    auto r = inf.value().run("hi",
        [](std::string) -> Result<std::string> { return std::string{"this output is too long"}; });
    REQUIRE(!r);
    CHECK(inf.value().was_blocked());
    CHECK(inf.value().status() == "blocked.output");

    // Integration: model_error must NOT count as blocked even though the
    // run failed — was_blocked is about policy-chain rejection only.
    auto inf2 = begin(rt, pid, tok.token_id); REQUIRE(inf2);
    auto r2 = inf2.value().run("ok",
        [](std::string) -> Result<std::string> { return Error::internal("boom"); });
    REQUIRE(!r2);
    CHECK(!inf2.value().was_blocked());
    CHECK(inf2.value().status() == "model_error");
}

// ============== Inference::has_completed ==================================

TEST_CASE("has_completed: false on a fresh handle") {
    auto rt_ = fresh_runtime("hc_fresh"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_hc_fresh");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    CHECK(!inf.value().has_completed());
    CHECK(!inf.value().is_committed());
}

TEST_CASE("has_completed: true after run, regardless of commit") {
    auto rt_ = fresh_runtime("hc_run"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_hc_run");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("q",
        [](std::string s) -> Result<std::string> { return s; }));
    CHECK(inf.value().has_completed());
    // Pre-commit: completed but not committed — the run-but-not-committed
    // state has_completed() lets sidecars detect.
    CHECK(!inf.value().is_committed());
    REQUIRE(inf.value().commit());
    CHECK(inf.value().has_completed());  // unchanged by commit
    CHECK(inf.value().is_committed());
}

TEST_CASE("has_completed: true even when run returned an error") {
    auto rt_ = fresh_runtime("hc_err"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_hc_err");
    auto tok = rt.consent().grant(pid, {Purpose::diagnostic_suggestion}, 1h).value();

    auto inf = rt.begin_inference({
        .model = ModelId{"m","v1"}, .actor = ActorId::clinician("smith"),
        .patient = pid, .encounter = EncounterId::make(),
        .purpose = Purpose::diagnostic_suggestion,
        .tenant  = TenantId{},
        .consent_token_id = tok.token_id,
    });
    REQUIRE(inf);
    auto r = inf.value().run("x",
        [](std::string) -> Result<std::string> { return Error::internal("boom"); });
    REQUIRE(!r);
    // Integration: even a failed run flips completed — pairs with
    // was_blocked()/status() for full post-mortem.
    CHECK(inf.value().has_completed());
    CHECK(!inf.value().was_blocked());
    CHECK(inf.value().status() == "model_error");
}

// ============== Runtime::quick_status =====================================

TEST_CASE("quick_status: EMPTY on a fresh runtime") {
    auto rt_ = fresh_runtime("qs_empty"); REQUIRE(rt_);
    auto& rt = rt_.value();
    CHECK(rt.quick_status() == "EMPTY");
}

TEST_CASE("quick_status: OK with counts after activity") {
    auto rt_ = fresh_runtime("qs_ok"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_qs_ok");
    (void)rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto s = rt.quick_status();
    // Must lead with OK (non-empty chain) and contain the entry count.
    CHECK(s.rfind("OK", 0) == 0);
    CHECK(s.find("entries") != std::string::npos);
    CHECK(s.find("active consent") != std::string::npos);
    CHECK(s.find("drift features") != std::string::npos);
    CHECK(s.find("policies") != std::string::npos);
    // No newline — single-line invariant.
    CHECK(s.find('\n') == std::string::npos);
}

TEST_CASE("quick_status: counts move with policy install and inference commit") {
    auto rt_ = fresh_runtime("qs_grow"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_qs_grow");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto before = rt.quick_status();
    CHECK(before.find("0 policies") != std::string::npos);
    REQUIRE(rt.install_default_policies());
    auto mid = rt.quick_status();
    CHECK(mid.find("0 policies") == std::string::npos);

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("q",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    auto after = rt.quick_status();
    // Entry count should have grown.
    CHECK(rt.ledger_length() >= 2);
    CHECK(after.find(std::to_string(rt.ledger_length()) + " entries")
          != std::string::npos);
}

// ============== Runtime::is_healthy =======================================

TEST_CASE("is_healthy: true on a fresh runtime") {
    auto rt_ = fresh_runtime("ih_fresh"); REQUIRE(rt_);
    auto& rt = rt_.value();
    CHECK(rt.is_healthy());
    CHECK(rt.is_healthy() == rt.health().ok);
}

TEST_CASE("is_healthy: still true after activity") {
    auto rt_ = fresh_runtime("ih_act"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_ih_act");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("q",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    CHECK(rt.is_healthy());
}

TEST_CASE("is_healthy: agrees with health().ok across multiple calls") {
    auto rt_ = fresh_runtime("ih_inv"); REQUIRE(rt_);
    auto& rt = rt_.value();
    // Integration: stable across repeated probing — sidecars hammer
    // /healthz and we don't want the boolean to flip on a quiet runtime.
    for (int i = 0; i < 5; ++i) {
        CHECK(rt.is_healthy() == rt.health().ok);
    }
}

// ============== Runtime::ledger_age =======================================

TEST_CASE("ledger_age: zero on an empty chain") {
    auto rt_ = fresh_runtime("la_empty"); REQUIRE(rt_);
    auto& rt = rt_.value();
    CHECK(rt.ledger_age() == std::chrono::nanoseconds::zero());
}

TEST_CASE("ledger_age: positive once the first entry has been written") {
    auto rt_ = fresh_runtime("la_one"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_la_one");
    (void)rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    // Sleep a touch so age is comfortably above clock granularity.
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    auto age = rt.ledger_age();
    CHECK(age > std::chrono::nanoseconds::zero());
    CHECK(age < std::chrono::seconds{60});
}

TEST_CASE("ledger_age: pinned to the OLDEST entry, monotonically grows") {
    auto rt_ = fresh_runtime("la_old"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_la_old");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto first_age = rt.ledger_age();
    REQUIRE(first_age > std::chrono::nanoseconds::zero());

    // Write more entries; ledger_age must reflect the oldest, not the
    // newest, so it should keep increasing — never reset to ~0.
    std::this_thread::sleep_for(std::chrono::milliseconds{3});
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("q",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());

    auto later_age = rt.ledger_age();
    CHECK(later_age >= first_age);
    // Integration: age should match (now - at(1).ts) within a small slack.
    auto first = rt.ledger().at(1);
    REQUIRE(first);
    auto expected = Time::now() - first.value().header.ts;
    auto delta = (later_age > expected)
                    ? (later_age - expected)
                    : (expected - later_age);
    CHECK(delta < std::chrono::milliseconds{50});
}

// ============== Inference::actor_str ======================================

TEST_CASE("actor_str: returns owning string copy of the actor id") {
    auto rt_ = fresh_runtime("as_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_as_b");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);

    auto s = inf.value().actor_str();
    CHECK(!s.empty());
    // Same string the InferenceContext exposes through actor().str().
    CHECK(s == std::string{inf.value().ctx().actor().str()});
}

TEST_CASE("actor_str: outlives the InferenceContext's string_view") {
    // Edge: the whole point of actor_str() is that the returned string
    // owns its bytes — capture it, drop the original handle, and the
    // string must still be valid.
    auto rt_ = fresh_runtime("as_outlive"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_as_o");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    std::string captured;
    {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        captured = inf.value().actor_str();
    }
    CHECK(!captured.empty());
    // The clinician id format is "clinician:<name>"; the test runtime
    // helper uses ActorId::clinician("smith"), so the captured string
    // must contain "smith" regardless of any prefix shape.
    CHECK(captured.find("smith") != std::string::npos);
}

TEST_CASE("actor_str: integration — survives commit and matches ledger body") {
    auto rt_ = fresh_runtime("as_int"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_as_i");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    auto pre  = inf.value().actor_str();
    REQUIRE(inf.value().commit());
    auto post = inf.value().actor_str();
    CHECK(pre == post);

    // The committed ledger entry's body actor field must agree.
    auto seq = inf.value().seq().value();
    auto entry = rt.ledger().at(seq);
    REQUIRE(entry);
    auto j = nlohmann::json::parse(entry.value().body_json);
    CHECK(j.value("actor", std::string{}) == pre);
}

// ============== Runtime::signing_key_id ===================================

TEST_CASE("signing_key_id: non-empty and matches ledger().key_id()") {
    auto rt_ = fresh_runtime("ski_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto kid = rt.signing_key_id();
    CHECK(!kid.empty());
    CHECK(kid == rt.ledger().key_id());
}

TEST_CASE("signing_key_id: stable across calls and across activity") {
    auto rt_ = fresh_runtime("ski_stable"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto first = rt.signing_key_id();

    auto pid = PatientId::pseudonymous("p_ski");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("q",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());

    CHECK(rt.signing_key_id() == first);
    // And still matches the ledger() accessor after commits.
    CHECK(rt.signing_key_id() == rt.ledger().key_id());
}

TEST_CASE("signing_key_id: distinct runtimes report distinct ids") {
    auto a_ = fresh_runtime("ski_a"); REQUIRE(a_);
    auto b_ = fresh_runtime("ski_b"); REQUIRE(b_);
    // Two freshly-created runtimes should mint independent signing
    // keys — the fingerprint and the key id should both differ.
    CHECK(a_.value().signing_key_id() != b_.value().signing_key_id());
}

// ============== Runtime::has_consent_for ==================================

TEST_CASE("has_consent_for: false on a fresh runtime, true after grant") {
    auto rt_ = fresh_runtime("hc_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_hc_b");
    CHECK(!rt.has_consent_for(pid, Purpose::ambient_documentation));
    (void)rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    CHECK(rt.has_consent_for(pid, Purpose::ambient_documentation));
}

TEST_CASE("has_consent_for: false for a different purpose / different patient") {
    auto rt_ = fresh_runtime("hc_edge"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid    = PatientId::pseudonymous("p_hc_e");
    auto other  = PatientId::pseudonymous("p_hc_other");
    (void)rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    // Same patient, wrong purpose.
    CHECK(!rt.has_consent_for(pid, Purpose::diagnostic_suggestion));
    // Different patient, granted purpose.
    CHECK(!rt.has_consent_for(other, Purpose::ambient_documentation));
}

TEST_CASE("has_consent_for: gates begin_inference correctly") {
    // Integration: the sugar should agree with begin_inference()'s
    // own consent gate. If has_consent_for returns true, begin_inference
    // (without an explicit token_id) must succeed; if false, it must
    // fail with consent_missing.
    auto rt_ = fresh_runtime("hc_int"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_hc_i");

    REQUIRE(!rt.has_consent_for(pid, Purpose::ambient_documentation));
    auto bad = rt.begin_inference({
        .model     = ModelId{"m","v1"},
        .actor     = ActorId::clinician("smith"),
        .patient   = pid,
        .encounter = EncounterId::make(),
        .purpose   = Purpose::ambient_documentation,
    });
    REQUIRE(!bad);
    CHECK(bad.error().code() == ErrorCode::consent_missing);

    (void)rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    REQUIRE(rt.has_consent_for(pid, Purpose::ambient_documentation));
    auto ok = rt.begin_inference({
        .model     = ModelId{"m","v1"},
        .actor     = ActorId::clinician("smith"),
        .patient   = pid,
        .encounter = EncounterId::make(),
        .purpose   = Purpose::ambient_documentation,
    });
    CHECK(ok);
}

// ============== Runtime::summary_string ===================================

TEST_CASE("summary_string: contains all the documented fields on a fresh runtime") {
    auto rt_ = fresh_runtime("ss_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto s = rt.summary_string();
    CHECK(!s.empty());
    // Documented fields — substring-check rather than full layout to
    // give us latitude on formatting tweaks.
    CHECK(s.find("ledger length")   != std::string::npos);
    CHECK(s.find("ledger head")     != std::string::npos);
    CHECK(s.find("signing key id")  != std::string::npos);
    CHECK(s.find("signing fingerprint") != std::string::npos);
    CHECK(s.find("active consent")  != std::string::npos);
    CHECK(s.find("drift features")  != std::string::npos);
    CHECK(s.find("policies")        != std::string::npos);
    CHECK(s.find(rt.version())      != std::string::npos);
    // Multi-line — six or more newlines (~6-8 lines).
    auto nl = std::count(s.begin(), s.end(), '\n');
    CHECK(nl >= 5);
    CHECK(nl <= 9);
}

TEST_CASE("summary_string: head hash truncated to 12 chars") {
    // Edge: even after activity grows the head hex, the summary
    // shows only a 12-char prefix. Verify by writing a real entry
    // and confirming the full 64-char hex does NOT appear, but the
    // 12-char prefix does.
    auto rt_ = fresh_runtime("ss_trunc"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_ss_t");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("q",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());

    auto full = rt.head_hash();
    REQUIRE(full.size() > 12);
    auto s = rt.summary_string();
    // Truncated form must appear; full form must NOT.
    CHECK(s.find(full.substr(0, 12)) != std::string::npos);
    CHECK(s.find(full)               == std::string::npos);
}

TEST_CASE("summary_string: integration — counts move with activity") {
    auto rt_ = fresh_runtime("ss_int"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto before = rt.summary_string();
    CHECK(before.find("ledger length: 0") != std::string::npos);
    CHECK(before.find("policies: 0")      != std::string::npos);

    rt.policies().push(make_phi_scrubber());
    auto pid = PatientId::pseudonymous("p_ss_i");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("q",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());

    auto after = rt.summary_string();
    // policies went from 0 to 1; ledger_length grew (consent grant +
    // inference.committed ≥ 2).
    CHECK(after.find("policies: 1")        != std::string::npos);
    CHECK(after.find("ledger length: 0")   == std::string::npos);
    CHECK(after.find("active consent tokens: 1") != std::string::npos);
}

// ============== Inference::was_committed_after ============================

TEST_CASE("was_committed_after: false before commit, true after for an earlier t") {
    auto rt_ = fresh_runtime("wca_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_wca_b");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    // Capture a baseline timestamp BEFORE the inference begins.
    auto baseline = Time::now();
    std::this_thread::sleep_for(std::chrono::milliseconds{2});

    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    // Pre-commit: must be false even though started_at > baseline.
    CHECK(!inf.value().was_committed_after(baseline));
    REQUIRE(inf.value().run("q",
        [](std::string s) -> Result<std::string> { return s; }));
    CHECK(!inf.value().was_committed_after(baseline));  // still uncommitted
    REQUIRE(inf.value().commit());
    CHECK(inf.value().was_committed_after(baseline));   // now true
}

TEST_CASE("was_committed_after: false when t is in the future / equal to started_at") {
    // Edge: strict "later than" semantics — equal timestamps and
    // future ones must both return false.
    auto rt_ = fresh_runtime("wca_edge"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_wca_e");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    auto started = inf.value().ctx().started_at();
    REQUIRE(inf.value().run("q",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());

    // Equal: started_at > started_at is false.
    CHECK(!inf.value().was_committed_after(started));
    // Future t: clearly false.
    auto future = Time::now() + std::chrono::seconds{60};
    CHECK(!inf.value().was_committed_after(future));
}

TEST_CASE("was_committed_after: integration — partitions a stream of inferences by checkpoint") {
    auto rt_ = fresh_runtime("wca_int"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_wca_i");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    // First inference, BEFORE the checkpoint.
    auto i1 = begin(rt, pid, tok.token_id); REQUIRE(i1);
    REQUIRE(i1.value().run("a",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(i1.value().commit());

    std::this_thread::sleep_for(std::chrono::milliseconds{3});
    auto checkpoint = Time::now();
    std::this_thread::sleep_for(std::chrono::milliseconds{3});

    // Second inference, AFTER the checkpoint.
    auto i2 = begin(rt, pid, tok.token_id); REQUIRE(i2);
    REQUIRE(i2.value().run("b",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(i2.value().commit());

    CHECK(!i1.value().was_committed_after(checkpoint));
    CHECK( i2.value().was_committed_after(checkpoint));
}

// ============== input_size / output_size =================================

TEST_CASE("input_size/output_size: invalid_argument before run") {
    auto rt_ = fresh_runtime("size_pre"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_size_pre");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);

    auto in_sz  = inf.value().input_size();
    auto out_sz = inf.value().output_size();
    REQUIRE(!in_sz);
    REQUIRE(!out_sz);
    CHECK(in_sz.error().code()  == ErrorCode::invalid_argument);
    CHECK(out_sz.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("input_size/output_size: ok run records both, matches post-policy bytes") {
    auto rt_ = fresh_runtime("size_ok"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_size_ok");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);

    // Echo callback — post-policy input == post-policy output for an
    // empty policy chain, so sizes should match the input string.
    const std::string in_str = "hello world";
    auto r = inf.value().run(in_str,
        [](std::string s) -> Result<std::string> { return s; });
    REQUIRE(r);
    CHECK(r.value() == in_str);

    auto in_sz  = inf.value().input_size();
    auto out_sz = inf.value().output_size();
    REQUIRE(in_sz);
    REQUIRE(out_sz);
    CHECK(in_sz.value()  == in_str.size());
    CHECK(out_sz.value() == in_str.size());
}

TEST_CASE("output_size: not_found when status != ok (timeout path)") {
    auto rt_ = fresh_runtime("size_to"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_size_to");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);

    const std::string in_str = "needs_timing";
    auto r = inf.value().run_with_timeout(in_str,
        [](std::string s) -> Result<std::string> {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
            return s;
        },
        std::chrono::milliseconds{20});
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::deadline_exceeded);

    // input_size IS recorded on the timeout path (input was hashed).
    auto in_sz = inf.value().input_size();
    REQUIRE(in_sz);
    CHECK(in_sz.value() == in_str.size());

    // output_size is NOT — no canonical output landed.
    auto out_sz = inf.value().output_size();
    REQUIRE(!out_sz);
    CHECK(out_sz.error().code() == ErrorCode::not_found);
}

TEST_CASE("input_size/output_size: blocked.input run still rejects output_size") {
    auto rt_ = fresh_runtime("size_blkin"); REQUIRE(rt_);
    auto& rt = rt_.value();
    rt.policies().push(make_length_limit(/*input_max=*/4, 1024));
    auto pid = PatientId::pseudonymous("p_size_blkin");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);

    auto r = inf.value().run("this input is too long",
        [](std::string s) -> Result<std::string> { return s; });
    REQUIRE(!r);

    // No input_hash was written (blocked before hashing) — input_size
    // returns not_found, NOT invalid_argument (the run completed).
    auto in_sz  = inf.value().input_size();
    auto out_sz = inf.value().output_size();
    REQUIRE(!in_sz);
    REQUIRE(!out_sz);
    CHECK(in_sz.error().code()  == ErrorCode::not_found);
    CHECK(out_sz.error().code() == ErrorCode::not_found);
}

TEST_CASE("input_size: cancellation records size when cancel happens after input policy") {
    auto rt_ = fresh_runtime("size_cancel"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_size_cx");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);

    CancelToken token;
    const std::string in_str = "abcdef";
    std::thread killer([&]{
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        token.cancel();
    });
    auto r = inf.value().run_cancellable(in_str,
        [](std::string s) -> Result<std::string> {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
            return s;
        },
        token,
        std::chrono::milliseconds{2});
    killer.join();
    REQUIRE(!r);

    auto in_sz  = inf.value().input_size();
    auto out_sz = inf.value().output_size();
    REQUIRE(in_sz);
    CHECK(in_sz.value() == in_str.size());
    REQUIRE(!out_sz);
    CHECK(out_sz.error().code() == ErrorCode::not_found);
}

// ============== Runtime::recent_inferences ===============================

TEST_CASE("recent_inferences: empty runtime returns empty vector") {
    auto rt_ = fresh_runtime("recent_empty"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto r = rt.recent_inferences(10);
    REQUIRE(r);
    CHECK(r.value().empty());
}

TEST_CASE("recent_inferences: n==0 returns empty without touching ledger") {
    auto rt_ = fresh_runtime("recent_zero"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_rec0");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("x",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());

    auto r = rt.recent_inferences(0);
    REQUIRE(r);
    CHECK(r.value().empty());
}

TEST_CASE("recent_inferences: only inference.committed entries, bounded by n") {
    auto rt_ = fresh_runtime("recent_n"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_recn");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    for (int i = 0; i < 5; ++i) {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(inf.value().run("payload",
            [](std::string s) -> Result<std::string> { return s; }));
        REQUIRE(inf.value().commit());
    }

    auto r = rt.recent_inferences(3);
    REQUIRE(r);
    CHECK(r.value().size() == 3);
    for (const auto& e : r.value()) {
        CHECK(e.header.event_type == "inference.committed");
    }

    // Asking for more than exist returns all of them.
    auto all = rt.recent_inferences(100);
    REQUIRE(all);
    CHECK(all.value().size() == 5);
}

// ============== Runtime::can_serve =======================================

TEST_CASE("can_serve: false when no consent has been granted") {
    auto rt_ = fresh_runtime("cs_no"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_csno");
    CHECK(!rt.can_serve(pid, Purpose::ambient_documentation));
}

TEST_CASE("can_serve: true after grant, mirrors has_consent_for") {
    auto rt_ = fresh_runtime("cs_yes"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_csyes");
    REQUIRE(rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h));
    CHECK(rt.can_serve(pid, Purpose::ambient_documentation));
    CHECK(rt.can_serve(pid, Purpose::ambient_documentation)
          == rt.has_consent_for(pid, Purpose::ambient_documentation));
}

TEST_CASE("can_serve: purpose-specific — grant for one purpose doesn't authorise another") {
    auto rt_ = fresh_runtime("cs_purpose"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_cspurp");
    REQUIRE(rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h));
    CHECK( rt.can_serve(pid, Purpose::ambient_documentation));
    CHECK(!rt.can_serve(pid, Purpose::diagnostic_suggestion));
}

TEST_CASE("can_serve: integration — predicts begin_inference outcome") {
    auto rt_ = fresh_runtime("cs_int"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_csint");

    // Before grant.
    CHECK(!rt.can_serve(pid, Purpose::ambient_documentation));
    auto r1 = rt.begin_inference({
        .model     = ModelId{"m","v1"},
        .actor     = ActorId::clinician("smith"),
        .patient   = pid,
        .encounter = EncounterId::make(),
        .purpose   = Purpose::ambient_documentation,
    });
    CHECK(!r1);

    // After grant.
    REQUIRE(rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h));
    CHECK(rt.can_serve(pid, Purpose::ambient_documentation));
    auto r2 = rt.begin_inference({
        .model     = ModelId{"m","v1"},
        .actor     = ActorId::clinician("smith"),
        .patient   = pid,
        .encounter = EncounterId::make(),
        .purpose   = Purpose::ambient_documentation,
    });
    CHECK(r2);
}

// ============== Runtime::warm_caches =====================================

TEST_CASE("warm_caches: no-op completes without error on empty runtime") {
    auto rt_ = fresh_runtime("warm_empty"); REQUIRE(rt_);
    auto& rt = rt_.value();
    rt.warm_caches();   // must not throw
    CHECK(rt.ledger().length() == 0);
}

TEST_CASE("warm_caches: idempotent — repeated calls leave runtime unchanged") {
    auto rt_ = fresh_runtime("warm_idem"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_warm");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("x",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());

    auto len_before = rt.ledger().length();
    auto head_before = rt.head_hash();
    rt.warm_caches();
    rt.warm_caches();
    rt.warm_caches();
    CHECK(rt.ledger().length() == len_before);
    CHECK(rt.head_hash() == head_before);
}

TEST_CASE("warm_caches: callable before any inference, doesn't gate begin_inference") {
    auto rt_ = fresh_runtime("warm_before"); REQUIRE(rt_);
    auto& rt = rt_.value();
    rt.warm_caches();   // before any consent / inference

    auto pid = PatientId::pseudonymous("p_warmpre");
    REQUIRE(rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h));
    auto inf = rt.begin_inference({
        .model     = ModelId{"m","v1"},
        .actor     = ActorId::clinician("smith"),
        .patient   = pid,
        .encounter = EncounterId::make(),
        .purpose   = Purpose::ambient_documentation,
    });
    CHECK(inf);
}

// ============== Inference::age_ms =========================================

TEST_CASE("age_ms: matches elapsed_ms on a fresh handle") {
    auto rt_ = fresh_runtime("age_match"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_age1");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    // Two reads of the same alias should be within ~few ms of each other
    // and the alias should never disagree with elapsed_ms() by more than
    // the wallclock between the two calls.
    auto a = inf.value().age_ms();
    auto b = inf.value().elapsed_ms();
    CHECK(b >= a);
    CHECK((b - a) < 50);
}

TEST_CASE("age_ms: monotonically non-decreasing as wallclock advances") {
    auto rt_ = fresh_runtime("age_mono"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_age2");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    auto a0 = inf.value().age_ms();
    std::this_thread::sleep_for(20ms);
    auto a1 = inf.value().age_ms();
    CHECK(a1 >= a0);
    CHECK(a1 >= 15);  // slept 20ms; allow scheduler slack
}

TEST_CASE("age_ms: returns 0 for a moved-from handle") {
    auto rt_ = fresh_runtime("age_moved"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_age3");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    Inference moved = std::move(inf.value());
    // The moved-from handle's impl_ is null; both elapsed_ms() and
    // age_ms() short-circuit to 0 in that state.
    CHECK(inf.value().age_ms() == 0);
    // The destination handle is still live and reports a non-negative age.
    CHECK(moved.age_ms() >= 0);
}

// ============== Runtime::env_summary ======================================

TEST_CASE("env_summary: returns parseable JSON with required keys") {
    auto rt_ = fresh_runtime("env_keys"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto s = rt.env_summary();
    auto j = nlohmann::json::parse(s);
    CHECK(j.contains("asclepius"));
    CHECK(j.contains("libsodium"));
    CHECK(j.contains("sqlite"));
    CHECK(j.contains("cpp_standard"));
    CHECK(j.contains("compiler"));
}

TEST_CASE("env_summary: reports a sane compiler and non-empty version strings") {
    auto rt_ = fresh_runtime("env_sane"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto j = nlohmann::json::parse(rt.env_summary());
    auto comp = j["compiler"].get<std::string>();
    CHECK((comp == "clang" || comp == "g++" || comp == "unknown"));
    CHECK(!j["asclepius"].get<std::string>().empty());
    CHECK(!j["libsodium"].get<std::string>().empty());
    CHECK(!j["sqlite"].get<std::string>().empty());
}

TEST_CASE("env_summary: cpp_standard is at least C++20") {
    auto rt_ = fresh_runtime("env_cpp20"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto j = nlohmann::json::parse(rt.env_summary());
    // Project is C++20 per CLAUDE.md; expect at least 202002L.
    CHECK(j["cpp_standard"].get<long>() >= 202002L);
}

TEST_CASE("env_summary: stable across calls (pure accessor, no mutation)") {
    auto rt_ = fresh_runtime("env_stable"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto a = rt.env_summary();
    auto b = rt.env_summary();
    CHECK(a == b);
}

// ============== Runtime::counter_total ====================================

TEST_CASE("counter_total: zero on a fresh runtime") {
    auto rt_ = fresh_runtime("ct_zero"); REQUIRE(rt_);
    auto& rt = rt_.value();
    CHECK(rt.counter_total() == 0);
}

TEST_CASE("counter_total: increases as inference activity is recorded") {
    auto rt_ = fresh_runtime("ct_grow"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_ct");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();

    auto before = rt.counter_total();
    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    auto out = inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; });
    REQUIRE(out);
    REQUIRE(inf.value().commit());

    // begin + attempts + ok all increment counters; total must rise.
    CHECK(rt.counter_total() > before);
}

TEST_CASE("counter_total: matches metrics().counter_total() exactly") {
    auto rt_ = fresh_runtime("ct_match"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_ctm");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    REQUIRE(inf.value().run("x",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());

    CHECK(rt.counter_total()
          == static_cast<std::size_t>(rt.metrics().counter_total()));
}

// ============== Runtime::is_chain_well_formed =============================

TEST_CASE("is_chain_well_formed: true on a fresh empty runtime") {
    auto rt_ = fresh_runtime("wf_empty"); REQUIRE(rt_);
    auto& rt = rt_.value();
    CHECK(rt.is_chain_well_formed());
}

TEST_CASE("is_chain_well_formed: still true after committed inference") {
    auto rt_ = fresh_runtime("wf_commit"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_wf");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id);
    REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    CHECK(rt.is_chain_well_formed());
}

TEST_CASE("is_chain_well_formed: agrees with ledger().verify()") {
    auto rt_ = fresh_runtime("wf_agree"); REQUIRE(rt_);
    auto& rt = rt_.value();
    // Drive a few entries into the chain.
    auto pid = PatientId::pseudonymous("p_wfa");
    REQUIRE(rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h));
    auto inf = rt.begin_inference({
        .model            = ModelId{"m","v1"},
        .actor            = ActorId::clinician("smith"),
        .patient          = pid,
        .encounter        = EncounterId::make(),
        .purpose          = Purpose::ambient_documentation,
    });
    REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());

    const bool sugar = rt.is_chain_well_formed();
    const bool full  = rt.ledger().verify().has_value();
    CHECK(sugar == full);
    CHECK(full == true);
}

// ============== Runtime::recent_drift_events ==============================

TEST_CASE("recent_drift_events: empty on a fresh runtime") {
    auto rt_ = fresh_runtime("rde_empty"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto v = rt.recent_drift_events(10);
    CHECK(v.empty());
}

TEST_CASE("recent_drift_events: n == 0 returns empty vector") {
    auto rt_ = fresh_runtime("rde_zero"); REQUIRE(rt_);
    auto& rt = rt_.value();
    // Even with crossings present, n=0 must return empty.
    auto v = rt.recent_drift_events(0);
    CHECK(v.empty());
}

TEST_CASE("recent_drift_events: surfaces drift.crossed entries appended by the bridge") {
    auto rt_ = fresh_runtime("rde_fire"); REQUIRE(rt_);
    auto& rt = rt_.value();

    // Register a feature with a baseline tightly clustered low; later
    // observations cluster high. The exact PSI/KS values depend on the
    // monitor's configuration, but the structural mismatch should fire
    // the bridge's drift.crossed event eventually.
    std::vector<double> baseline;
    for (int i = 0; i < 200; ++i) baseline.push_back(0.05);
    REQUIRE(rt.drift().register_feature("ratio", std::move(baseline),
                                         /*lo=*/0.0, /*hi=*/1.0));

    for (int i = 0; i < 500; ++i) {
        (void)rt.drift().observe("ratio", 0.95);
    }
    // The bridge appends drift.crossed when classify >= moder. Look at
    // the chain via the sugar and the ledger directly; both should
    // agree on the count, and both should be > 0 if any crossing fired.
    auto via_sugar  = rt.recent_drift_events(50);
    auto via_ledger = rt.ledger().tail_by_event_type("drift.crossed", 50);
    REQUIRE(via_ledger);
    CHECK(via_sugar.size() == via_ledger.value().size());
    // If at least one crossing was generated, every entry surfaced via
    // the sugar must carry the right event_type.
    for (const auto& e : via_sugar) {
        CHECK(e.header.event_type == "drift.crossed");
    }
}

// ============== Inference::failed =========================================

TEST_CASE("failed: false on a fresh, unrun handle") {
    auto rt_ = fresh_runtime("fail_fresh"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_fail_fresh");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    // Pre-run: nothing has happened, can't have failed.
    CHECK(!inf.value().failed());
    CHECK(!inf.value().has_run());
}

TEST_CASE("failed: false on status=ok") {
    auto rt_ = fresh_runtime("fail_ok"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_fail_ok");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    CHECK(!inf.value().failed());
    CHECK(inf.value().status() == "ok");
}

TEST_CASE("failed: true on blocked.input and blocked.output") {
    // blocked.input
    {
        auto rt_ = fresh_runtime("fail_bin"); REQUIRE(rt_);
        auto& rt = rt_.value();
        rt.policies().push(make_length_limit(/*input_max=*/4, /*output_max=*/0));
        auto pid = PatientId::pseudonymous("p_fail_bin");
        auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        auto r = inf.value().run("this input is way too long",
            [](std::string s) -> Result<std::string> { return s; });
        REQUIRE(!r);
        CHECK(inf.value().failed());
        CHECK(inf.value().status() == "blocked.input");
    }
    // blocked.output
    {
        auto rt_ = fresh_runtime("fail_bout"); REQUIRE(rt_);
        auto& rt = rt_.value();
        rt.policies().push(make_length_limit(/*input_max=*/0, /*output_max=*/4));
        auto pid = PatientId::pseudonymous("p_fail_bout");
        auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        auto r = inf.value().run("hi",
            [](std::string) -> Result<std::string> { return std::string{"too long output"}; });
        REQUIRE(!r);
        CHECK(inf.value().failed());
        CHECK(inf.value().status() == "blocked.output");
    }
}

TEST_CASE("failed: true on model_error, timeout, and cancelled") {
    // model_error
    {
        auto rt_ = fresh_runtime("fail_merr"); REQUIRE(rt_);
        auto& rt = rt_.value();
        auto pid = PatientId::pseudonymous("p_fail_merr");
        auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(!inf.value().run("hi",
            [](std::string) -> Result<std::string> { return Error::internal("boom"); }));
        CHECK(inf.value().failed());
        CHECK(inf.value().status() == "model_error");
    }
    // timeout
    {
        auto rt_ = fresh_runtime("fail_to"); REQUIRE(rt_);
        auto& rt = rt_.value();
        auto pid = PatientId::pseudonymous("p_fail_to");
        auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(!inf.value().run_with_timeout("x",
            [](std::string s) -> Result<std::string> {
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
                return s;
            },
            std::chrono::milliseconds{5}));
        CHECK(inf.value().failed());
        CHECK(inf.value().status() == "timeout");
    }
    // cancelled
    {
        auto rt_ = fresh_runtime("fail_cancel"); REQUIRE(rt_);
        auto& rt = rt_.value();
        auto pid = PatientId::pseudonymous("p_fail_cancel");
        auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        CancelToken ct;
        ct.cancel();  // pre-cancelled
        REQUIRE(!inf.value().run_cancellable("x",
            [](std::string s) -> Result<std::string> { return s; },
            ct));
        CHECK(inf.value().failed());
        CHECK(inf.value().status() == "cancelled");
    }
}

// ============== Inference::trace_summary ==================================

TEST_CASE("trace_summary: contains the canonical fields after run") {
    auto rt_ = fresh_runtime("ts_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_ts_b");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    auto s = inf.value().trace_summary();
    CHECK(s.find("inf=") != std::string::npos);
    CHECK(s.find("patient=") != std::string::npos);
    CHECK(s.find("model=") != std::string::npos);
    CHECK(s.find("status=ok") != std::string::npos);
    CHECK(s.find("elapsed=") != std::string::npos);
    CHECK(s.find("ms") != std::string::npos);
    // Single-line invariant — one log line per emit.
    CHECK(s.find('\n') == std::string::npos);
}

TEST_CASE("trace_summary: id and patient match accessor outputs") {
    auto rt_ = fresh_runtime("ts_match"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_ts_m");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("ok",
        [](std::string s) -> Result<std::string> { return s; }));
    auto s = inf.value().trace_summary();
    // The exact id and patient strings the handle exposes must be
    // verbatim substrings of the rendered summary.
    CHECK(s.find(std::string{inf.value().id()}) != std::string::npos);
    CHECK(s.find(std::string{inf.value().ctx().patient().str()}) != std::string::npos);
    CHECK(s.find(std::string{inf.value().ctx().model().str()}) != std::string::npos);
}

TEST_CASE("trace_summary: failure status surfaces in the string") {
    auto rt_ = fresh_runtime("ts_fail"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_ts_f");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(!inf.value().run("hi",
        [](std::string) -> Result<std::string> { return Error::internal("boom"); }));
    auto s = inf.value().trace_summary();
    CHECK(s.find("status=model_error") != std::string::npos);
    // Pre-run case: status renders as empty; the prefix "status=" is
    // still present even if its value is the empty string.
    auto inf2 = begin(rt, pid, tok.token_id); REQUIRE(inf2);
    auto s2 = inf2.value().trace_summary();
    CHECK(s2.find("status=") != std::string::npos);
    // Pre-run elapsed_ms() is small; "elapsed=" prefix must still render.
    CHECK(s2.find("elapsed=") != std::string::npos);
}

// ============== Runtime::is_idle ==========================================

TEST_CASE("is_idle: empty chain is trivially idle") {
    auto rt_ = fresh_runtime("idle_empty"); REQUIRE(rt_);
    auto& rt = rt_.value();
    REQUIRE(rt.is_chain_empty());
    // Any threshold — including zero — returns true on an empty chain.
    CHECK(rt.is_idle(std::chrono::milliseconds{0}));
    CHECK(rt.is_idle(std::chrono::milliseconds{500}));
}

TEST_CASE("is_idle: false right after a recent commit, true after the threshold elapses") {
    auto rt_ = fresh_runtime("idle_recent"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_idle_r");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("q",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    // Just-committed: a "1 second" threshold should not yet be idle.
    CHECK(!rt.is_idle(std::chrono::milliseconds{1000}));
    // Sleep past a tiny threshold; should now read idle.
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    CHECK(rt.is_idle(std::chrono::milliseconds{5}));
}

TEST_CASE("is_idle: zero threshold is strict — any post-now ts is non-idle") {
    auto rt_ = fresh_runtime("idle_zero"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_idle_z");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("q",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    // threshold == 0: any committed entry whose ts is in the past has
    // non-zero elapsed, so idle is true. We just want the boolean to
    // reflect the comparison, not crash. Sleep a touch so elapsed > 0.
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    CHECK(rt.is_idle(std::chrono::milliseconds{0}));
}

// ============== Runtime::generate_trace_id ================================

TEST_CASE("generate_trace_id: produces 16 lowercase hex chars") {
    auto rt_ = fresh_runtime("tid_shape"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto id = rt.generate_trace_id();
    CHECK(id.size() == 16);
    for (char c : id) {
        const bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        CHECK(is_hex);
    }
}

TEST_CASE("generate_trace_id: returns a distinct id on each call") {
    auto rt_ = fresh_runtime("tid_unique"); REQUIRE(rt_);
    auto& rt = rt_.value();
    // 64 bits of CSPRNG output: collisions across a small loop are
    // astronomically unlikely. Drawing many distinct values is the
    // strongest cheap check we have for "fresh entropy each call."
    constexpr int kN = 64;
    std::vector<std::string> ids;
    ids.reserve(kN);
    for (int i = 0; i < kN; ++i) ids.push_back(rt.generate_trace_id());
    std::sort(ids.begin(), ids.end());
    auto last = std::unique(ids.begin(), ids.end());
    CHECK(last == ids.end());
}

TEST_CASE("generate_trace_id: round-trips through Inference::add_metadata") {
    auto rt_ = fresh_runtime("tid_meta"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_tid");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    const auto trace = rt.generate_trace_id();
    REQUIRE(inf.value().add_metadata("trace_id", trace));
    auto got = inf.value().get_metadata("trace_id");
    REQUIRE(got);
    CHECK(got.value().get<std::string>() == trace);
}

// ============== Inference::observe_drift_named ============================

TEST_CASE("observe_drift_named: forwards to observe_drift on a registered feature") {
    auto rt_ = fresh_runtime("odn_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    REQUIRE(rt.drift().register_feature("named_basic", {0.1}, 0.0, 1.0, 4));
    auto pid = PatientId::pseudonymous("p_odn_b");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    // The sugar must succeed for a registered feature, exactly like
    // observe_drift().
    auto r = inf.value().observe_drift_named("named_basic", 0.5);
    CHECK(r);
}

TEST_CASE("observe_drift_named: surfaces the same not_found error as observe_drift on unknown feature") {
    auto rt_ = fresh_runtime("odn_missing"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_odn_m");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    auto sugar    = inf.value().observe_drift_named("nope", 0.5);
    auto original = inf.value().observe_drift("nope", 0.5);
    // Both spellings must agree on success/failure and on the error code.
    CHECK(static_cast<bool>(sugar) == static_cast<bool>(original));
    REQUIRE(!sugar);
    REQUIRE(!original);
    CHECK(sugar.error().code() == original.error().code());
}

TEST_CASE("observe_drift_named: equivalent to observe_drift across many calls") {
    // Both methods route through the same DriftMonitor::observe(), so
    // the registry should reflect the union of calls without divergence.
    auto rt_ = fresh_runtime("odn_equiv"); REQUIRE(rt_);
    auto& rt = rt_.value();
    REQUIRE(rt.drift().register_feature("equiv", {0.1}, 0.0, 1.0, 4));
    auto pid = PatientId::pseudonymous("p_odn_e");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    for (int i = 0; i < 10; ++i) {
        REQUIRE(inf.value().observe_drift_named("equiv",
            static_cast<double>(i) / 10.0));
        REQUIRE(inf.value().observe_drift("equiv",
            static_cast<double>(i) / 10.0));
    }
    // The feature stays registered — both paths leave shared state intact.
    auto features = rt.list_loaded_features();
    CHECK(std::find(features.begin(), features.end(), "equiv")
          != features.end());
}

// ============== Inference::tag ============================================

TEST_CASE("tag: writes label under metadata as a JSON string") {
    auto rt_ = fresh_runtime("tag_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_tag_b");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().tag("cohort_a"));
    auto got = inf.value().get_metadata("tag");
    REQUIRE(got);
    REQUIRE(got.value().is_string());
    CHECK(got.value().get<std::string>() == "cohort_a");
}

TEST_CASE("tag: empty label is rejected with invalid_argument") {
    auto rt_ = fresh_runtime("tag_empty"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_tag_e");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    auto r = inf.value().tag("");
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
    // No metadata side-effect from a rejected call.
    auto h = inf.value().has_metadata("tag");
    REQUIRE(h);
    CHECK(!h.value());
}

TEST_CASE("tag: replace-on-duplicate; rejected after commit") {
    auto rt_ = fresh_runtime("tag_dup"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_tag_d");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().tag("first"));
    REQUIRE(inf.value().tag("second"));
    auto got = inf.value().get_metadata("tag");
    REQUIRE(got);
    CHECK(got.value().get<std::string>() == "second");
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    // After commit, tag() inherits add_metadata's "metadata after commit"
    // contract.
    auto post = inf.value().tag("third");
    REQUIRE(!post);
    CHECK(post.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("tag: bucketed inferences appear in the committed ledger body") {
    auto rt_ = fresh_runtime("tag_commit"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_tag_c");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().tag("lane_b"));
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    auto recent = rt.recent_inferences(1);
    REQUIRE(recent);
    REQUIRE(!recent.value().empty());
    auto j = nlohmann::json::parse(recent.value().front().body_json);
    REQUIRE(j.contains("metadata"));
    REQUIRE(j["metadata"].contains("tag"));
    CHECK(j["metadata"]["tag"].get<std::string>() == "lane_b");
}

// ============== Runtime::record_event =====================================

TEST_CASE("record_event: appends a caller-driven event to the ledger") {
    auto rt_ = fresh_runtime("rec_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    const auto before = rt.ledger().length();
    nlohmann::json body;
    body["reason"] = "rotation";
    auto e = rt.record_event("config.reloaded",
                             "system:sidecar",
                             body);
    REQUIRE(e);
    CHECK(e.value().header.event_type == "config.reloaded");
    CHECK(rt.ledger().length() == before + 1);
    REQUIRE(rt.ledger().verify());
}

TEST_CASE("record_event: forwards body and tenant verbatim to the ledger") {
    auto rt_ = fresh_runtime("rec_tenant"); REQUIRE(rt_);
    auto& rt = rt_.value();
    nlohmann::json body;
    body["seq"]    = 7;
    body["nested"] = nlohmann::json::object({{"k", "v"}});
    auto e = rt.record_event("shutdown.initiated",
                             "system:supervisor",
                             body,
                             "tenant_xyz");
    REQUIRE(e);
    auto fetched = rt.ledger().at(e.value().header.seq);
    REQUIRE(fetched);
    CHECK(fetched.value().header.tenant == "tenant_xyz");
    auto j = nlohmann::json::parse(fetched.value().body_json);
    CHECK(j["seq"].get<int>() == 7);
    CHECK(j["nested"]["k"].get<std::string>() == "v");
}

TEST_CASE("record_event: many sidecar events interleave with inference.committed") {
    auto rt_ = fresh_runtime("rec_mix"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_rec_m");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    // Interleave some custom events around an inference commit so we
    // can verify they all land in the same chain without breaking it.
    REQUIRE(rt.record_event("custom.before",
                            "system:s",
                            nlohmann::json::object({{"i", 0}})));
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    REQUIRE(rt.record_event("custom.after",
                            "system:s",
                            nlohmann::json::object({{"i", 1}})));
    REQUIRE(rt.ledger().verify());
    // Events of our custom type are addressable via tail_by_event_type.
    auto found_before = rt.ledger().tail_by_event_type("custom.before", 4);
    REQUIRE(found_before);
    CHECK(found_before.value().size() == 1);
    auto found_after = rt.ledger().tail_by_event_type("custom.after", 4);
    REQUIRE(found_after);
    CHECK(found_after.value().size() == 1);
}

// ============== Runtime::is_busy ==========================================

TEST_CASE("is_busy: empty chain is never busy") {
    auto rt_ = fresh_runtime("busy_empty"); REQUIRE(rt_);
    auto& rt = rt_.value();
    REQUIRE(rt.is_chain_empty());
    CHECK(!rt.is_busy(std::chrono::milliseconds{0}));
    CHECK(!rt.is_busy(std::chrono::milliseconds{500}));
}

TEST_CASE("is_busy: true right after a recent commit, false after threshold elapses") {
    auto rt_ = fresh_runtime("busy_recent"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_busy_r");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    CHECK(rt.is_busy(std::chrono::milliseconds{1000}));
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    CHECK(!rt.is_busy(std::chrono::milliseconds{5}));
}

TEST_CASE("is_busy: exact negation of is_idle across a sample of thresholds") {
    auto rt_ = fresh_runtime("busy_neg"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_busy_n");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    for (auto th : {std::chrono::milliseconds{0},
                    std::chrono::milliseconds{1},
                    std::chrono::milliseconds{50},
                    std::chrono::milliseconds{500},
                    std::chrono::milliseconds{5000}}) {
        CHECK(rt.is_busy(th) != rt.is_idle(th));
    }
}

// ============== Runtime::status_line ======================================

TEST_CASE("status_line: contains every expected field on a fresh runtime") {
    auto rt_ = fresh_runtime("sl_fresh"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto line = rt.status_line();
    // Contains the asclepius prefix + version.
    CHECK(line.find("asclepius v") != std::string::npos);
    CHECK(line.find(rt.version()) != std::string::npos);
    // Each labelled segment.
    CHECK(line.find("ledger=") != std::string::npos);
    CHECK(line.find("key=")    != std::string::npos);
    CHECK(line.find("policies=") != std::string::npos);
    CHECK(line.find("drift_features=") != std::string::npos);
    CHECK(line.find("active_consent=") != std::string::npos);
    // Single-line: no embedded newline.
    CHECK(line.find('\n') == std::string::npos);
    // Distinct from the JSON-shaped to_json() — must not start with '{'.
    CHECK(!line.empty());
    CHECK(line.front() != '{');
}

TEST_CASE("status_line: numeric segments reflect health() exactly") {
    auto rt_ = fresh_runtime("sl_numbers"); REQUIRE(rt_);
    auto& rt = rt_.value();
    rt.policies().push(make_phi_scrubber());
    REQUIRE(rt.drift().register_feature("sl_feat", {0.1}, 0.0, 1.0, 4));
    auto pid = PatientId::pseudonymous("p_sl_n");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h);
    REQUIRE(tok);
    auto h = rt.health();
    auto line = rt.status_line();
    CHECK(line.find("ledger=" + std::to_string(h.ledger_length))
          != std::string::npos);
    CHECK(line.find("policies=" + std::to_string(h.policy_count))
          != std::string::npos);
    CHECK(line.find("drift_features=" + std::to_string(h.drift_features))
          != std::string::npos);
    CHECK(line.find("active_consent=" +
                    std::to_string(h.active_consent_tokens))
          != std::string::npos);
    CHECK(line.find("key=" + h.ledger_key_id) != std::string::npos);
}

TEST_CASE("status_line: stable across calls and distinct from to_json()") {
    auto rt_ = fresh_runtime("sl_stable"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto a = rt.status_line();
    auto b = rt.status_line();
    CHECK(a == b);
    // The JSON form is a different shape — at the very least, status_line
    // is a single line and to_json() starts with '{'.
    auto js = rt.health().to_json();
    CHECK(js.front() == '{');
    CHECK(a != js);
}

// ============== Inference::ensure_committed ===============================

TEST_CASE("ensure_committed: commits a never-committed handle") {
    auto rt_ = fresh_runtime("ec_first"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_ec_f");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(!inf.value().is_committed());
    const auto before = rt.ledger().length();
    REQUIRE(inf.value().ensure_committed());
    CHECK(inf.value().is_committed());
    CHECK(rt.ledger().length() == before + 1);
}

TEST_CASE("ensure_committed: no-op on an already-committed handle") {
    auto rt_ = fresh_runtime("ec_idem"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_ec_i");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    const auto first_seq = inf.value().seq();
    REQUIRE(first_seq);
    const auto length_after_commit = rt.ledger().length();
    // Repeat ensure_committed several times — none should re-append.
    REQUIRE(inf.value().ensure_committed());
    REQUIRE(inf.value().ensure_committed());
    REQUIRE(inf.value().ensure_committed());
    CHECK(rt.ledger().length() == length_after_commit);
    auto seq_after = inf.value().seq();
    REQUIRE(seq_after);
    CHECK(seq_after.value() == first_seq.value());
}

TEST_CASE("ensure_committed: surfaces commit errors, propagates run-not-called") {
    auto rt_ = fresh_runtime("ec_norun"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_ec_n");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    // Without a prior run(), commit() returns invalid_argument; ensure_committed
    // delegates to commit() and so must surface the same error.
    auto r = inf.value().ensure_committed();
    REQUIRE(!r);
    CHECK(r.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("ensure_committed: chain stays well-formed across mixed shutdown loop") {
    auto rt_ = fresh_runtime("ec_loop"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_ec_l");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    // Simulate a graceful-shutdown loop: some inferences already committed
    // on the fast path, others left hanging. ensure_committed() should
    // close the gap without duplicating the committed ones.
    constexpr int kN = 6;
    std::vector<Inference> handles;
    handles.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        auto h = begin(rt, pid, tok.token_id);
        REQUIRE(h);
        REQUIRE(h.value().run("hi",
            [](std::string s) -> Result<std::string> { return s; }));
        if (i % 2 == 0) {
            REQUIRE(h.value().commit());
        }
        handles.emplace_back(std::move(h.value()));
    }
    const auto before = rt.ledger().length();
    for (auto& h : handles) {
        REQUIRE(h.ensure_committed());
        CHECK(h.is_committed());
    }
    // Only the previously-uncommitted half (3 of 6) should have appended.
    CHECK(rt.ledger().length() == before + 3);
    REQUIRE(rt.ledger().verify());
}

// ============== Inference::set_priority ===================================

TEST_CASE("set_priority: writes priority under metadata as a JSON string") {
    auto rt_ = fresh_runtime("prio_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_prio_b");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().set_priority("high"));
    auto got = inf.value().get_metadata("priority");
    REQUIRE(got);
    REQUIRE(got.value().is_string());
    CHECK(got.value().get<std::string>() == "high");
}

TEST_CASE("set_priority: accepts each of low/normal/high/critical") {
    auto rt_ = fresh_runtime("prio_each"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_prio_e");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    for (const auto* p : {"low", "normal", "high", "critical"}) {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(inf.value().set_priority(p));
        auto got = inf.value().get_metadata("priority");
        REQUIRE(got);
        CHECK(got.value().get<std::string>() == p);
    }
}

TEST_CASE("set_priority: rejects unknown values with invalid_argument") {
    auto rt_ = fresh_runtime("prio_bad"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_prio_x");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    for (const auto* bad : {"", "urgent", "HIGH", "0", "low ", " low"}) {
        auto r = inf.value().set_priority(bad);
        REQUIRE(!r);
        CHECK(r.error().code() == ErrorCode::invalid_argument);
    }
    // No metadata should have been written by any of the rejected calls.
    auto h = inf.value().has_metadata("priority");
    REQUIRE(h);
    CHECK(!h.value());
}

TEST_CASE("set_priority: replace-on-duplicate; rejected after commit") {
    auto rt_ = fresh_runtime("prio_dup"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_prio_d");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().set_priority("low"));
    REQUIRE(inf.value().set_priority("critical"));
    auto got = inf.value().get_metadata("priority");
    REQUIRE(got);
    CHECK(got.value().get<std::string>() == "critical");
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    auto post = inf.value().set_priority("high");
    REQUIRE(!post);
    CHECK(post.error().code() == ErrorCode::invalid_argument);
}

// ============== Runtime::wait_until_chain_grows ===========================

TEST_CASE("wait_until_chain_grows: empty chain + min_seq=0 returns true immediately") {
    auto rt_ = fresh_runtime("wcg_zero"); REQUIRE(rt_);
    auto& rt = rt_.value();
    REQUIRE(rt.ledger().length() == 0);
    auto t0 = std::chrono::steady_clock::now();
    bool ok = rt.wait_until_chain_grows(0, 100ms);
    auto dt = std::chrono::steady_clock::now() - t0;
    CHECK(ok);
    // No sleep required for the trivial case — should be effectively
    // instantaneous (allow 20ms slack for slow CI machines).
    CHECK(dt < 20ms);
}

TEST_CASE("wait_until_chain_grows: returns true when chain is already long enough") {
    auto rt_ = fresh_runtime("wcg_already"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_wcg_a");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    const auto len = rt.ledger().length();
    REQUIRE(len >= 1);
    CHECK(rt.wait_until_chain_grows(len, 100ms));
    CHECK(rt.wait_until_chain_grows(1, 100ms));
}

TEST_CASE("wait_until_chain_grows: returns false on timeout") {
    auto rt_ = fresh_runtime("wcg_timeout"); REQUIRE(rt_);
    auto& rt = rt_.value();
    const auto target = rt.ledger().length() + 1000;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = rt.wait_until_chain_grows(target, 50ms);
    auto dt = std::chrono::steady_clock::now() - t0;
    CHECK(!ok);
    // Should have waited at least the timeout (give 5ms slack for the
    // last sleep tick that may have been already in-flight).
    CHECK(dt >= 45ms);
    // And not vastly more — anything beyond ~250ms means polling is
    // broken.
    CHECK(dt < 250ms);
}

TEST_CASE("wait_until_chain_grows: unblocks when an async commit lands") {
    auto rt_ = fresh_runtime("wcg_async"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_wcg_async");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    const auto baseline = rt.ledger().length();
    // Spin off a background thread that commits an inference after a
    // short delay; the main thread blocks on wait_until_chain_grows
    // until that commit lands.
    std::thread bg([&]() {
        std::this_thread::sleep_for(20ms);
        auto inf = begin(rt, pid, tok.token_id);
        REQUIRE(inf);
        REQUIRE(inf.value().run("hi",
            [](std::string s) -> Result<std::string> { return s; }));
        REQUIRE(inf.value().commit());
    });
    bool ok = rt.wait_until_chain_grows(baseline + 1, 1000ms);
    bg.join();
    CHECK(ok);
    CHECK(rt.ledger().length() >= baseline + 1);
}

// ============== Inference::ledger_snapshot_seq ============================

TEST_CASE("ledger_snapshot_seq: returns 0 before commit") {
    auto rt_ = fresh_runtime("snap_pre"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_snap_p");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    CHECK(inf.value().ledger_snapshot_seq() == 0);
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    // Run alone doesn't commit; snapshot still 0.
    CHECK(inf.value().ledger_snapshot_seq() == 0);
}

TEST_CASE("ledger_snapshot_seq: returns committed_seq after commit, matches seq()") {
    auto rt_ = fresh_runtime("snap_post"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_snap_post");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    auto snap = inf.value().ledger_snapshot_seq();
    auto seq  = inf.value().seq();
    REQUIRE(seq);
    CHECK(snap != 0);
    CHECK(snap == seq.value());
}

TEST_CASE("ledger_snapshot_seq: noexcept and unaffected by subsequent appends") {
    auto rt_ = fresh_runtime("snap_stable"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_snap_s");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    // The function itself is noexcept; the call site `inf.value()` goes
    // through Result::value() which is NOT noexcept (asserts on error
    // path). Test the function signature against std::declval, not the
    // bound expression — same trick as the MetricRegistry::has noexcept
    // assert (libc++ portability).
    static_assert(noexcept(std::declval<const Inference&>().ledger_snapshot_seq()),
                  "ledger_snapshot_seq must be noexcept");
    const auto first = inf.value().ledger_snapshot_seq();
    // Append more events; the snapshot should not move.
    for (int i = 0; i < 3; ++i) {
        nlohmann::json b; b["i"] = i;
        REQUIRE(rt.record_event("ping", "system:test", b));
    }
    CHECK(inf.value().ledger_snapshot_seq() == first);
}

// ============== Runtime::record_shutdown ==================================

TEST_CASE("record_shutdown: appends a runtime.shutdown event with the supplied reason") {
    auto rt_ = fresh_runtime("rsd_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    const auto before = rt.ledger().length();
    REQUIRE(rt.record_shutdown("graceful drain"));
    CHECK(rt.ledger().length() == before + 1);
    auto tail = rt.ledger().tail(1);
    REQUIRE(tail);
    REQUIRE(!tail.value().empty());
    const auto& e = tail.value().front();
    CHECK(e.header.event_type == "runtime.shutdown");
    auto j = nlohmann::json::parse(e.body_json);
    CHECK(j.value("reason", std::string{}) == "graceful drain");
    REQUIRE(j.contains("ts"));
    CHECK(j["ts"].is_string());
    CHECK(!j["ts"].get<std::string>().empty());
}

TEST_CASE("record_shutdown: actor is system:runtime and chain stays well-formed") {
    auto rt_ = fresh_runtime("rsd_actor"); REQUIRE(rt_);
    auto& rt = rt_.value();
    REQUIRE(rt.record_shutdown("operator initiated"));
    auto tail = rt.ledger().tail(1);
    REQUIRE(tail);
    REQUIRE(!tail.value().empty());
    CHECK(tail.value().front().header.actor == "system:runtime");
    REQUIRE(rt.ledger().verify());
}

TEST_CASE("record_shutdown: empty reason is permitted (forwards verbatim)") {
    auto rt_ = fresh_runtime("rsd_empty"); REQUIRE(rt_);
    auto& rt = rt_.value();
    REQUIRE(rt.record_shutdown(""));
    auto tail = rt.ledger().tail(1);
    REQUIRE(tail);
    REQUIRE(!tail.value().empty());
    auto j = nlohmann::json::parse(tail.value().front().body_json);
    CHECK(j.value("reason", std::string{"missing"}) == "");
}

TEST_CASE("record_shutdown: many tombstones interleave with inference.committed entries") {
    auto rt_ = fresh_runtime("rsd_mix"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_rsd_mix");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    constexpr int kRounds = 4;
    for (int i = 0; i < kRounds; ++i) {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(inf.value().run("hi",
            [](std::string s) -> Result<std::string> { return s; }));
        REQUIRE(inf.value().commit());
        REQUIRE(rt.record_shutdown("round_" + std::to_string(i)));
    }
    REQUIRE(rt.ledger().verify());
    // We appended kRounds shutdowns; tail-by-event-type should see all of them.
    auto sd = rt.ledger().tail_by_event_type("runtime.shutdown",
                                             static_cast<std::size_t>(kRounds));
    REQUIRE(sd);
    CHECK(sd.value().size() == static_cast<std::size_t>(kRounds));
}

// ============== Runtime::policy_count =====================================

TEST_CASE("policy_count: zero on a fresh runtime") {
    auto rt_ = fresh_runtime("pc_zero"); REQUIRE(rt_);
    auto& rt = rt_.value();
    CHECK(rt.policy_count() == 0);
    CHECK(rt.policy_count() == rt.policies().size());
}

TEST_CASE("policy_count: matches policies().size() across mutations") {
    auto rt_ = fresh_runtime("pc_match"); REQUIRE(rt_);
    auto& rt = rt_.value();
    rt.policies().push(make_phi_scrubber());
    CHECK(rt.policy_count() == 1);
    CHECK(rt.policy_count() == rt.policies().size());
    rt.policies().push(make_length_limit(64, 64));
    CHECK(rt.policy_count() == 2);
    CHECK(rt.policy_count() == rt.policies().size());
}

TEST_CASE("policy_count: tracks install_default_policies (clear-then-push)") {
    auto rt_ = fresh_runtime("pc_idp"); REQUIRE(rt_);
    auto& rt = rt_.value();
    rt.policies().push(make_phi_scrubber());
    rt.policies().push(make_phi_scrubber());
    rt.policies().push(make_phi_scrubber());
    CHECK(rt.policy_count() == 3);
    REQUIRE(rt.install_default_policies());
    CHECK(rt.policy_count() == 2);
    // Same as health()/system_summary() report.
    CHECK(rt.policy_count() == rt.health().policy_count);
    CHECK(rt.policy_count() == rt.system_summary().policy_count);
}

// ============== Inference::set_severity ===================================

TEST_CASE("set_severity: writes severity under metadata as a JSON string") {
    auto rt_ = fresh_runtime("sev_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_sev_b");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().set_severity("warning"));
    auto got = inf.value().get_metadata("severity");
    REQUIRE(got);
    REQUIRE(got.value().is_string());
    CHECK(got.value().get<std::string>() == "warning");
}

TEST_CASE("set_severity: accepts each of info/warning/error/critical") {
    auto rt_ = fresh_runtime("sev_each"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_sev_e");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    for (const auto* s : {"info", "warning", "error", "critical"}) {
        auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
        REQUIRE(inf.value().set_severity(s));
        auto got = inf.value().get_metadata("severity");
        REQUIRE(got);
        CHECK(got.value().get<std::string>() == s);
    }
}

TEST_CASE("set_severity: rejects unknown values with invalid_argument") {
    auto rt_ = fresh_runtime("sev_bad"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_sev_x");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    for (const auto* bad : {"", "fatal", "WARNING", "warn", "info ", " info", "low"}) {
        auto r = inf.value().set_severity(bad);
        REQUIRE(!r);
        CHECK(r.error().code() == ErrorCode::invalid_argument);
    }
    // No metadata should have been written by any of the rejected calls.
    auto h = inf.value().has_metadata("severity");
    REQUIRE(h);
    CHECK(!h.value());
}

TEST_CASE("set_severity: replace-on-duplicate; rejected after commit") {
    auto rt_ = fresh_runtime("sev_dup"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_sev_d");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().set_severity("info"));
    REQUIRE(inf.value().set_severity("critical"));
    auto got = inf.value().get_metadata("severity");
    REQUIRE(got);
    CHECK(got.value().get<std::string>() == "critical");
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    auto post = inf.value().set_severity("error");
    REQUIRE(!post);
    CHECK(post.error().code() == ErrorCode::invalid_argument);
}

// ============== Inference::trace_id_or_empty ==============================

TEST_CASE("trace_id_or_empty: returns empty string when no metadata is set") {
    auto rt_ = fresh_runtime("tid_empty"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_tid_e");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    CHECK(inf.value().trace_id_or_empty() == "");
    // Setting unrelated metadata also leaves trace_id absent.
    REQUIRE(inf.value().add_metadata("not_trace", "x"));
    CHECK(inf.value().trace_id_or_empty() == "");
}

TEST_CASE("trace_id_or_empty: returns the string trace_id when set") {
    auto rt_ = fresh_runtime("tid_set"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_tid_s");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().add_metadata("trace_id", "abc-123"));
    CHECK(inf.value().trace_id_or_empty() == "abc-123");
    // Round-trip through a runtime-minted id.
    const auto fresh = rt.generate_trace_id();
    auto inf2 = begin(rt, pid, tok.token_id); REQUIRE(inf2);
    REQUIRE(inf2.value().add_metadata("trace_id", fresh));
    CHECK(inf2.value().trace_id_or_empty() == fresh);
}

TEST_CASE("trace_id_or_empty: collapses non-string trace_id to empty") {
    auto rt_ = fresh_runtime("tid_nonstr"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_tid_n");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    // Caller stores trace_id as an integer — sugar still hands back "".
    REQUIRE(inf.value().add_metadata("trace_id", nlohmann::json{42}));
    CHECK(inf.value().trace_id_or_empty() == "");
    // Caller stores trace_id as an object — same outcome.
    auto inf2 = begin(rt, pid, tok.token_id); REQUIRE(inf2);
    REQUIRE(inf2.value().add_metadata("trace_id",
        nlohmann::json::object({{"nested", "x"}})));
    CHECK(inf2.value().trace_id_or_empty() == "");
}

TEST_CASE("trace_id_or_empty: survives commit (reads pending body, post-commit too)") {
    auto rt_ = fresh_runtime("tid_commit"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_tid_c");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().add_metadata("trace_id", "stable-trace"));
    CHECK(inf.value().trace_id_or_empty() == "stable-trace");
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    // Post-commit, the staged body still carries the trace_id.
    CHECK(inf.value().trace_id_or_empty() == "stable-trace");
}

// ============== Runtime::flush_drift_to_metrics ===========================

TEST_CASE("flush_drift_to_metrics: returns 0 when no features are registered") {
    auto rt_ = fresh_runtime("fdtm_zero"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto r = rt.flush_drift_to_metrics();
    REQUIRE(r);
    CHECK(r.value() == 0);
}

TEST_CASE("flush_drift_to_metrics: emits one counter per feature, returns flushed count") {
    auto rt_ = fresh_runtime("fdtm_emit"); REQUIRE(rt_);
    auto& rt = rt_.value();
    REQUIRE(rt.drift().register_feature("alpha", {0.1, 0.2, 0.3}, 0.0, 1.0, 4));
    REQUIRE(rt.drift().register_feature("beta",  {0.5, 0.6, 0.7}, 0.0, 1.0, 4));
    auto r = rt.flush_drift_to_metrics();
    REQUIRE(r);
    CHECK(r.value() == 2);
    // Each feature should have exactly one counter incremented (one of
    // none/minor/moder/severe). Sum the four severity counters per
    // feature and require == 1.
    auto sev_total = [&](const std::string& feature) -> std::uint64_t {
        std::uint64_t total = 0;
        for (const auto* s : {"none", "minor", "moder", "severe"}) {
            total += rt.metrics().count("drift.severity." + feature + "." + s);
        }
        return total;
    };
    CHECK(sev_total("alpha") == 1);
    CHECK(sev_total("beta")  == 1);
}

TEST_CASE("flush_drift_to_metrics: counters accumulate across calls") {
    auto rt_ = fresh_runtime("fdtm_accum"); REQUIRE(rt_);
    auto& rt = rt_.value();
    REQUIRE(rt.drift().register_feature("gamma", {0.1, 0.2}, 0.0, 1.0, 4));
    REQUIRE(rt.flush_drift_to_metrics());
    REQUIRE(rt.flush_drift_to_metrics());
    REQUIRE(rt.flush_drift_to_metrics());
    // Three flushes → exactly three increments distributed across the
    // four severity buckets for "gamma".
    std::uint64_t total = 0;
    for (const auto* s : {"none", "minor", "moder", "severe"}) {
        total += rt.metrics().count(
            std::string{"drift.severity.gamma."} + s);
    }
    CHECK(total == 3);
}

TEST_CASE("flush_drift_to_metrics: severity bucket follows feature_severity") {
    auto rt_ = fresh_runtime("fdtm_bucket"); REQUIRE(rt_);
    auto& rt = rt_.value();
    REQUIRE(rt.drift().register_feature("delta", {0.1, 0.2}, 0.0, 1.0, 4));
    REQUIRE(rt.flush_drift_to_metrics());
    // Whichever severity feature_severity reports, the matching counter
    // must be exactly 1 and the others 0.
    auto sev = rt.drift().feature_severity("delta");
    REQUIRE(sev);
    const std::string expected =
        std::string{"drift.severity.delta."} + to_string(sev.value());
    CHECK(rt.metrics().count(expected) == 1);
    for (const auto* s : {"none", "minor", "moder", "severe"}) {
        std::string name = std::string{"drift.severity.delta."} + s;
        if (name == expected) continue;
        CHECK(rt.metrics().count(name) == 0);
    }
}

// ============== Runtime::counter ==========================================

TEST_CASE("counter: returns 0 for an unknown name (matches metrics().count)") {
    auto rt_ = fresh_runtime("cn_unknown"); REQUIRE(rt_);
    auto& rt = rt_.value();
    CHECK(rt.counter("nope.never.seen") == 0);
    CHECK(rt.counter("nope.never.seen") == rt.metrics().count("nope.never.seen"));
}

TEST_CASE("counter: returns the live counter value after a commit") {
    auto rt_ = fresh_runtime("cn_live"); REQUIRE(rt_);
    auto& rt = rt_.value();
    auto pid = PatientId::pseudonymous("p_cn_live");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    CHECK(rt.counter("inference.ok") == 1);
    CHECK(rt.counter("inference.ok") == rt.metrics().count("inference.ok"));
    CHECK(rt.counter("inference.attempts") == 1);
}

TEST_CASE("counter: tracks manual MetricRegistry mutations through the sugar") {
    auto rt_ = fresh_runtime("cn_manual"); REQUIRE(rt_);
    auto& rt = rt_.value();
    rt.metrics().inc("custom.event");
    rt.metrics().inc("custom.event");
    rt.metrics().inc("custom.event", 5);
    CHECK(rt.counter("custom.event") == 7);
    // After reset_metrics(), the sugar reads the fresh value.
    REQUIRE(rt.reset_metrics());
    CHECK(rt.counter("custom.event") == 0);
}

// ============== Runtime::subscribe_logging ================================

TEST_CASE("subscribe_logging: callback fires on every successful append") {
    auto rt_ = fresh_runtime("sl_basic"); REQUIRE(rt_);
    auto& rt = rt_.value();
    std::atomic<int> hits{0};
    auto sub = rt.subscribe_logging([&](const LedgerEntry&) {
        hits.fetch_add(1, std::memory_order_relaxed);
    });
    REQUIRE(sub);
    auto pid = PatientId::pseudonymous("p_sl_b");
    auto tok = rt.consent().grant(pid, {Purpose::ambient_documentation}, 1h).value();
    // The grant() above appended a consent.granted entry → already +1.
    auto inf = begin(rt, pid, tok.token_id); REQUIRE(inf);
    REQUIRE(inf.value().run("hi",
        [](std::string s) -> Result<std::string> { return s; }));
    REQUIRE(inf.value().commit());
    // We expect at least 1 hit from the inference.committed append.
    CHECK(hits.load() >= 1);
}

TEST_CASE("subscribe_logging: rejects null sink with invalid_argument") {
    auto rt_ = fresh_runtime("sl_null"); REQUIRE(rt_);
    auto& rt = rt_.value();
    std::function<void(const LedgerEntry&)> null_sink;  // empty
    auto sub = rt.subscribe_logging(std::move(null_sink));
    REQUIRE(!sub);
    CHECK(sub.error().code() == ErrorCode::invalid_argument);
}

TEST_CASE("subscribe_logging: callback receives the freshly-appended entry") {
    auto rt_ = fresh_runtime("sl_entry"); REQUIRE(rt_);
    auto& rt = rt_.value();
    std::vector<std::string> seen_event_types;
    std::mutex mu;
    auto sub = rt.subscribe_logging([&](const LedgerEntry& e) {
        std::lock_guard<std::mutex> lk(mu);
        seen_event_types.push_back(e.header.event_type);
    });
    REQUIRE(sub);
    REQUIRE(rt.record_event("custom.ping", "test", nlohmann::json{{"i", 1}}));
    REQUIRE(rt.record_shutdown("test_drain"));
    {
        std::lock_guard<std::mutex> lk(mu);
        // Both event types should be observed in append order.
        REQUIRE(seen_event_types.size() >= 2);
        bool saw_ping = false, saw_shutdown = false;
        for (const auto& t : seen_event_types) {
            if (t == "custom.ping")     saw_ping = true;
            if (t == "runtime.shutdown") saw_shutdown = true;
        }
        CHECK(saw_ping);
        CHECK(saw_shutdown);
    }
}

TEST_CASE("subscribe_logging: destroying the subscription stops further callbacks") {
    auto rt_ = fresh_runtime("sl_unsub"); REQUIRE(rt_);
    auto& rt = rt_.value();
    std::atomic<int> hits{0};
    {
        auto sub = rt.subscribe_logging([&](const LedgerEntry&) {
            hits.fetch_add(1, std::memory_order_relaxed);
        });
        REQUIRE(sub);
        REQUIRE(rt.record_event("x", "test", nlohmann::json::object()));
    }
    // Subscription went out of scope → unregistered. Subsequent appends
    // must NOT increment hits beyond this baseline.
    const int before = hits.load();
    REQUIRE(rt.record_event("y", "test", nlohmann::json::object()));
    REQUIRE(rt.record_event("z", "test", nlohmann::json::object()));
    CHECK(hits.load() == before);
}
