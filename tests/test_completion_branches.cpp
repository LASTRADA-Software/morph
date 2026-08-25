// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/core/logger.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

#include "test_support.hpp"

using SyncExecutor = morph::testing::InlineExecutor;

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
    state->onErr.push_back([&](const std::exception_ptr&) { fired = true; });
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

TEST_CASE("CompletionState: an orphan destructor survives a throwing log sink", "[completion][logger]") {
    // The motivating case for morph#158. ~CompletionState is implicitly
    // noexcept and logs the abandoned exception, so before the logging layer
    // became noexcept a sink that threw here meant std::terminate -- which the
    // destructor worked around with a local try/catch(...) and a NOLINT. That
    // workaround is gone; this is what replaces it.
    //
    // A test that merely destroyed the state would pass whether or not the
    // guarantee holds, since a terminate() would abort the run rather than
    // fail an assertion. So it also checks that the record was *counted*,
    // which proves the sink was actually reached and actually threw.
    morph::log::ScopedLoggerOverride const guard;
    morph::log::setLogLevel(morph::log::LogLevel::debug);
    int sinkCalls = 0;
    morph::log::setLogger([&](morph::log::LogLevel, std::string_view) {
        ++sinkCalls;
        throw std::runtime_error{"sink throws during teardown"};
    });

    const auto droppedBefore = morph::log::droppedLogRecords();
    {
        auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
        state->ready = true;
        try {
            throw std::runtime_error{"orphaned"};
        } catch (...) {
            state->error = std::current_exception();
        }
        state->onErrAttached = false;
    }  // ~CompletionState logs here, into a sink that throws

    REQUIRE(sinkCalls == 1);
    REQUIRE(morph::log::droppedLogRecords() == droppedBefore + 1);
}
