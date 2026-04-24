// SPDX-License-Identifier: Apache-2.0

#include <morph/completion.hpp>
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <string>


struct SyncExecutor : morph::exec::IExecutor {
    void post(std::function<void()> fn) override { fn(); }
};

// ── setException branches ─────────────────────────────────────────────────────

TEST_CASE("CompletionState: setException with no onErr handler is a no-op on callback", "[completion]") {
    // Covers the false arm of `if (onErr)` in setException.
    // onErrAttached must be set manually to suppress the orphan destructor log.
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    state->onErrAttached = true;
    REQUIRE_NOTHROW(state->setException(std::make_exception_ptr(std::runtime_error{"x"})));
    REQUIRE(state->error != nullptr);
    REQUIRE(state->ready);
}

TEST_CASE("CompletionState: setException with no executor does not post callback", "[completion]") {
    // Covers `callback != nullptr && cbExec != nullptr` false arm (cbExec is null).
    // onErr is set so callback would be non-null, but cbExec is nullptr so no post.
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    state->cbExec = nullptr;
    bool fired = false;
    state->onErr = [&](const std::exception_ptr&) { fired = true; };
    state->setException(std::make_exception_ptr(std::runtime_error{"no-exec"}));
    REQUIRE_FALSE(fired);
    REQUIRE(state->ready);
}

// ── attachThen with null executor ─────────────────────────────────────────────

TEST_CASE("CompletionState: attachThen fires immediately but cbExec null does not post", "[completion]") {
    // Covers `fireNow != nullptr && cbExec == nullptr` false arm in attachThen.
    // State is already ready with a value; attaching a then handler would normally
    // fire immediately, but with no executor the callback is silently dropped.
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    state->cbExec = nullptr;
    state->setValue(5);  // ready, no executor set yet

    bool fired = false;
    state->attachThen([&](int) { fired = true; });
    REQUIRE_FALSE(fired);
}

// ── attachOnError with null executor ─────────────────────────────────────────

TEST_CASE("CompletionState: attachOnError fires immediately but cbExec null does not post", "[completion]") {
    // Covers `fireNow != nullptr && cbExec == nullptr` false arm in attachOnError.
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    state->cbExec = nullptr;
    state->onErrAttached = true;
    state->setException(std::make_exception_ptr(std::runtime_error{"err"}));

    bool fired = false;
    state->attachOnError([&](const std::exception_ptr&) { fired = true; });
    REQUIRE_FALSE(fired);
}

// ── ~CompletionState destructor branches ─────────────────────────────────────

TEST_CASE("CompletionState: destructor with !ready exits early, no crash", "[completion]") {
    // Covers the `!ready` true arm of the early-return guard in ~CompletionState.
    {
        auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
        // Never set ready — destructor must exit silently.
    }
    REQUIRE(true);
}

TEST_CASE("CompletionState: destructor with value set and no error exits early", "[completion]") {
    // Covers the `!error` true arm of the early-return guard.
    {
        auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
        state->setValue(1);  // ready=true, error=null -> early return
    }
    REQUIRE(true);
}

TEST_CASE("CompletionState: orphan destructor logs std::exception", "[completion]") {
    // Covers the `catch (const std::exception&)` branch in ~CompletionState.
    // ready=true, error set to std::exception, onErrAttached=false -> falls through
    // to the rethrow, caught by catch(const std::exception&), logs it, no crash.
    {
        auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
        state->ready = true;
        try {
            throw std::runtime_error{"orphan std exception"};
        } catch (...) {
            state->error = std::current_exception();
        }
        // onErrAttached stays false — will log "[orphan] unhandled exception: ..."
    }
    REQUIRE(true);
}
