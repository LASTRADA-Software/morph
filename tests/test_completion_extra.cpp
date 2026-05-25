// SPDX-License-Identifier: Apache-2.0

#include <morph/completion.hpp>
#include <morph/logger.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <stdexcept>
#include <string>

#include "test_support.hpp"

using SyncExecutor = morph::testing::InlineExecutor;
using LogGuard = morph::log::ScopedLoggerOverride;

namespace {
struct NotAStdExceptionExtra {};
}  // namespace

// ── CompletionState edge branches ─────────────────────────────────────────────

TEST_CASE("CompletionState: setValue with no callback attached does not crash", "[completion]") {
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    state->setValue(5);
    REQUIRE(state->value.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(*state->value == 5);
}

TEST_CASE("CompletionState: setException with no callback attached does not crash", "[completion]") {
    // Must attach onError to suppress orphan log
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    state->attachOnError([](const std::exception_ptr&) {});
    state->setException(std::make_exception_ptr(std::runtime_error{"silent"}));
    REQUIRE(state->error != nullptr);
}

TEST_CASE("CompletionState: attachThen when already errored does not fire handler", "[completion]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    state->cbExec = &exec;
    state->attachOnError([](const std::exception_ptr&) {});
    state->setException(std::make_exception_ptr(std::runtime_error{"err"}));

    bool thenFired = false;
    state->attachThen([&](int) { thenFired = true; });
    REQUIRE_FALSE(thenFired);
}

TEST_CASE("CompletionState: attachOnError when already resolved with value does not fire", "[completion]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    state->cbExec = &exec;
    state->setValue(99);

    bool errFired = false;
    state->attachOnError([&](const std::exception_ptr&) { errFired = true; });
    REQUIRE_FALSE(errFired);
}

TEST_CASE("CompletionState: attachOnError when already errored fires immediately", "[completion]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    state->cbExec = &exec;
    state->setException(std::make_exception_ptr(std::runtime_error{"late"}));

    bool errFired = false;
    state->attachOnError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::runtime_error& ex) {
            errFired = (std::string{ex.what()} == "late");
        }
    });
    REQUIRE(errFired);
}

TEST_CASE("CompletionState: setValue with no executor does not post callback", "[completion]") {
    // cbExec == nullptr — callback must not be invoked
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    state->cbExec = nullptr;

    bool fired = false;
    state->onOk = [&](int) { fired = true; };
    state->setValue(3);
    REQUIRE_FALSE(fired);
}

TEST_CASE("CompletionState: orphan destructor logs unknown (non-std) exception", "[completion]") {
    // Throw a non-std::exception type; destructor must not crash
    struct WeirdError {};
    {
        auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
        // Manually inject a non-std::exception exception_ptr
        try {
            throw WeirdError{};
        } catch (...) {
            state->error = std::current_exception();
            state->ready = true;
            // onErrAttached stays false — will hit the unknown exception branch in dtor
        }
        // state destroyed here → dtor logs "[orphan] unhandled unknown exception"
    }
    REQUIRE(true);  // just verify no crash/terminate
}

// ── morph::async::Completion public API edge branches ───────────────────────────────────────

TEST_CASE("morph::async::Completion: default-constructed null state, then/onError are no-ops", "[completion]") {
    morph::async::Completion<int> comp;
    bool thenFired = false;
    bool errFired = false;
    comp.then([&](int) { thenFired = true; }).onError([&](const std::exception_ptr&) { errFired = true; });
    REQUIRE_FALSE(thenFired);
    REQUIRE_FALSE(errFired);
    REQUIRE(comp.state() == nullptr);
}

TEST_CASE("morph::async::Completion: move semantics transfer state", "[completion]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> original{state, &exec};
    morph::async::Completion<int> moved = std::move(original);

    REQUIRE(moved.state() == state);
    auto movedFromState = original.state();  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    REQUIRE(movedFromState == nullptr);

    int received = -1;
    moved.then([&](int val) { received = val; });
    state->setValue(77);
    REQUIRE(received == 77);
}

TEST_CASE("morph::async::Completion: on_error fires when error set after handler attached", "[completion]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &exec};

    std::string msg;
    comp.onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::runtime_error& ex) {
            msg = ex.what();
        }
    });

    state->setException(std::make_exception_ptr(std::runtime_error{"delayed"}));
    REQUIRE(msg == "delayed");
}

// ── Per-instantiation coverage for non-int Completion specialisations ────────
//
// The bridge/backend layers instantiate Completion/CompletionState with
// std::string, double, and std::shared_ptr<void>. Codecov's coverage view
// processes the LCOV file per-instantiation, so any branch arm exercised only
// for `int` shows up as partial for the other three template specialisations.
// `exerciseAllBranches<T>` walks every branch in completion.hpp once per type,
// so the merged LCOV has both arms taken for every instantiation.

namespace {

// Each helper exercises one cluster of branches and stays well under the
// cognitive-complexity threshold; together they cover every branch in
// completion.hpp for the instantiation `T`.

template <typename T>
void exerciseSetValueBranches(const T& sampleValue, morph::exec::IExecutor& exec) {
    using namespace morph::async;
    // setValue with no onOk attached AND cbExec nullptr → False arms of `if (onOk)`
    // and `callback != nullptr && cbExec != nullptr`.
    auto stateNoCallback = std::make_shared<detail::CompletionState<T>>();
    stateNoCallback->setValue(T{sampleValue});
    REQUIRE(stateNoCallback->ready);

    // attachThen first, then setValue → onOk True arm, cbExec True arm.
    auto stateAttachFirst = std::make_shared<detail::CompletionState<T>>();
    stateAttachFirst->cbExec = &exec;
    bool fired = false;
    stateAttachFirst->attachThen([&](const T&) { fired = true; });
    stateAttachFirst->setValue(T{sampleValue});
    REQUIRE(fired);

    // onOk attached but cbExec nullptr → cbExec null short-circuit arm in line 43.
    auto stateNoExec = std::make_shared<detail::CompletionState<T>>();
    stateNoExec->onOk = [](const T&) {};
    stateNoExec->setValue(T{sampleValue});
    REQUIRE(stateNoExec->ready);
}

template <typename T>
void exerciseAttachThenBranches(const T& sampleValue, morph::exec::IExecutor& exec) {
    using namespace morph::async;
    // attachThen on already-ready state with cbExec → ready&&value True arm
    // and `fireNow != nullptr && cbExec != nullptr` True arm.
    auto stateReady = std::make_shared<detail::CompletionState<T>>();
    stateReady->cbExec = &exec;
    stateReady->setValue(T{sampleValue});
    bool fired = false;
    stateReady->attachThen([&](const T&) { fired = true; });
    REQUIRE(fired);

    // attachThen on already-ready state with cbExec null → fireNow non-null,
    // cbExec null short-circuit arm at line 75.
    auto stateReadyNoExec = std::make_shared<detail::CompletionState<T>>();
    stateReadyNoExec->setValue(T{sampleValue});
    stateReadyNoExec->attachThen([](const T&) {});
}

template <typename T>
void exerciseSetExceptionBranches(morph::exec::IExecutor& exec) {
    using namespace morph::async;
    // setException with no onErr attached → False arm of `if (onErr)` and
    // `cbExec != nullptr`. onErrAttached pre-set to suppress orphan log.
    auto stateNoHandler = std::make_shared<detail::CompletionState<T>>();
    stateNoHandler->onErrAttached = true;
    stateNoHandler->setException(std::make_exception_ptr(std::runtime_error{"no-handler"}));
    REQUIRE(stateNoHandler->error != nullptr);

    // attachOnError then setException → onErr True arm, cbExec True arm.
    auto stateAttachFirst = std::make_shared<detail::CompletionState<T>>();
    stateAttachFirst->cbExec = &exec;
    bool fired = false;
    stateAttachFirst->attachOnError([&](const std::exception_ptr&) { fired = true; });
    stateAttachFirst->setException(std::make_exception_ptr(std::runtime_error{"err"}));
    REQUIRE(fired);

    // onErr set but cbExec nullptr → cbExec null short-circuit arm in line 60.
    auto stateNoExec = std::make_shared<detail::CompletionState<T>>();
    stateNoExec->onErr = [](const std::exception_ptr&) {};
    stateNoExec->setException(std::make_exception_ptr(std::runtime_error{"err"}));
}

template <typename T>
void exerciseAttachOnErrorBranches(morph::exec::IExecutor& exec) {
    using namespace morph::async;
    // attachOnError on already-errored state with cbExec set → ready&&error
    // True arm and line 91 True arm.
    auto stateErrored = std::make_shared<detail::CompletionState<T>>();
    stateErrored->cbExec = &exec;
    stateErrored->onErrAttached = true;
    stateErrored->setException(std::make_exception_ptr(std::runtime_error{"err"}));
    bool fired = false;
    stateErrored->attachOnError([&](const std::exception_ptr&) { fired = true; });
    REQUIRE(fired);

    // attachOnError on already-errored state with cbExec null → cbExec null
    // arm at line 91.
    auto stateErroredNoExec = std::make_shared<detail::CompletionState<T>>();
    stateErroredNoExec->onErrAttached = true;
    stateErroredNoExec->setException(std::make_exception_ptr(std::runtime_error{"err"}));
    stateErroredNoExec->attachOnError([](const std::exception_ptr&) {});
}

template <typename T>
void exerciseOrphanDestructorBranches() {
    using namespace morph::async;
    struct LocalNonStdError {};

    // Orphan destructor with std::exception → catch (const std::exception&).
    auto stateStd = std::make_shared<detail::CompletionState<T>>();
    stateStd->ready = true;
    try {
        throw std::runtime_error{"orphan"};
    } catch (...) {
        stateStd->error = std::current_exception();
    }

    // Orphan destructor with non-std exception → catch (...).
    auto stateNonStd = std::make_shared<detail::CompletionState<T>>();
    stateNonStd->ready = true;
    try {
        throw LocalNonStdError{};
    } catch (...) {
        stateNonStd->error = std::current_exception();
    }
}

template <typename T>
void exerciseCompletionWrapperBranches(const T& sampleValue, morph::exec::IExecutor& exec) {
    using namespace morph::async;

    // Default ctor + null-state ctor → False arm of `_state != nullptr` at
    // lines 142, 164, 179.
    Completion<T> empty;
    empty.then([](const T&) {}).onError([](const std::exception_ptr&) {});
    REQUIRE(empty.state() == nullptr);

    std::shared_ptr<detail::CompletionState<T>> nullState;
    Completion<T> nullCtor{nullState, &exec};
    REQUIRE(nullCtor.state() == nullptr);

    // Real-state ctor + then/onError → True arms.
    auto state = std::make_shared<detail::CompletionState<T>>();
    Completion<T> comp{state, &exec};
    comp.then([](const T&) {}).onError([](const std::exception_ptr&) {});
    state->setValue(T{sampleValue});
}

template <typename T>
void exerciseThrowingSinkOrphanBranches() {
    using namespace morph::async;
    LogGuard guard;
    morph::log::setLogger([](morph::log::LogLevel, std::string_view) {
        throw std::runtime_error{"sink-fail"};
    });

    // Inner catch (...) {} after the std::exception branch (lines 104-105).
    auto stateStd = std::make_shared<detail::CompletionState<T>>();
    stateStd->ready = true;
    try {
        throw std::runtime_error{"orphan-throwing-sink"};
    } catch (...) {
        stateStd->error = std::current_exception();
    }

    // Inner catch (...) {} after the unknown-exception branch (lines 109-110).
    auto stateNonStd = std::make_shared<detail::CompletionState<T>>();
    stateNonStd->ready = true;
    try {
        throw NotAStdExceptionExtra{};
    } catch (...) {
        stateNonStd->error = std::current_exception();
    }
}

template <typename T>
void exerciseAllBranches(const T& sampleValue) {
    SyncExecutor exec;
    exerciseSetValueBranches<T>(sampleValue, exec);
    exerciseAttachThenBranches<T>(sampleValue, exec);
    exerciseSetExceptionBranches<T>(exec);
    exerciseAttachOnErrorBranches<T>(exec);
    exerciseOrphanDestructorBranches<T>();
    exerciseCompletionWrapperBranches<T>(sampleValue, exec);
    exerciseThrowingSinkOrphanBranches<T>();
}

}  // namespace

TEST_CASE("CompletionState<int>: every branch and orphan path", "[completion]") {
    exerciseAllBranches<int>(123);
}

TEST_CASE("CompletionState<string>: every branch and orphan path", "[completion]") {
    exerciseAllBranches<std::string>("hello");
}

TEST_CASE("CompletionState<double>: every branch and orphan path", "[completion]") {
    exerciseAllBranches<double>(3.14);
}

TEST_CASE("CompletionState<shared_ptr<void>>: every branch and orphan path", "[completion]") {
    exerciseAllBranches<std::shared_ptr<void>>(std::static_pointer_cast<void>(std::make_shared<int>(42)));
}
