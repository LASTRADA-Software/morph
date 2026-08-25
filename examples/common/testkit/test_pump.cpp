// SPDX-License-Identifier: Apache-2.0
#include <QCoreApplication>
#include <QTimer>
#include <catch2/catch_test_macros.hpp>
#include <morph/qt/qt_executor.hpp>
#include <stdexcept>

#include "testkit/pump.hpp"

TEST_CASE("pumpUntil returns true once the predicate flips", "[ladder][testkit][pump]") {
    REQUIRE(QCoreApplication::instance() != nullptr);
    bool flag = false;
    QTimer::singleShot(20, [&] { flag = true; });
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return flag; }, std::chrono::milliseconds{500}));
}

TEST_CASE("pumpUntil returns false on timeout without hanging", "[ladder][testkit][pump]") {
    REQUIRE_FALSE(morph::ladder::testkit::pumpUntil([] { return false; }, std::chrono::milliseconds{50}));
}

// deadlineScale() itself reads MORPH_LADDER_DEADLINE_MS behind a `static
// const` guard that runs exactly once per *process* — no test in this shared
// binary can ever be first to observe a particular env value, since some
// earlier test has always already forced the "unset" path. computeDeadlineScale
// takes the raw env value as a parameter instead, so it's directly testable
// without a process boundary — see pump.hpp's comment on it for the full
// rationale.
TEST_CASE("computeDeadlineScale is 1.0 when MORPH_LADDER_DEADLINE_MS is unset", "[ladder][testkit][pump]") {
    REQUIRE(morph::ladder::testkit::detail::computeDeadlineScale(nullptr) == 1.0);
}

TEST_CASE("computeDeadlineScale interprets its argument as a new 5000ms baseline", "[ladder][testkit][pump]") {
    REQUIRE(morph::ladder::testkit::detail::computeDeadlineScale("2500") == 0.5);
    REQUIRE(morph::ladder::testkit::detail::computeDeadlineScale("5000") == 1.0);
    REQUIRE(morph::ladder::testkit::detail::computeDeadlineScale("10000") == 2.0);
}

TEST_CASE("computeDeadlineScale is 1.0 for an unparseable value, not a crash", "[ladder][testkit][pump]") {
    REQUIRE(morph::ladder::testkit::detail::computeDeadlineScale("not-a-number") == 1.0);
    REQUIRE(morph::ladder::testkit::detail::computeDeadlineScale("") == 1.0);
}

// morph::async::Completion<T> is consumer-facing only (then()/onError()); it has
// no resolve()/fail() of its own. The producer side is Completion<T>::
// makeSettleable(execPtr) (issue #55's public "settleable promise" seam,
// docs/spec/core/completion.md), which returns a {Completion<T>, Promise}
// pair sharing one state -- the Promise exposes resolve()/reject() without
// ever naming morph::async::detail::CompletionState<T>. Here we use
// morph::qt::QtExecutor (already linked in via morph::qt) as the executor,
// since it delivers callbacks through the Qt event loop exactly as
// pumpUntil expects to pump them.

TEST_CASE("awaitQt resolves a Completion<T> and returns its value", "[ladder][testkit][pump]") {
    morph::qt::QtExecutor executor;
    auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&executor);
    auto sharedPromise = std::make_shared<morph::async::Completion<int>::Promise>(std::move(promise));
    QTimer::singleShot(10, [sharedPromise] { sharedPromise->resolve(42); });
    REQUIRE(morph::ladder::testkit::awaitQt(std::move(completion)) == 42);
}

TEST_CASE("awaitQt rethrows the completion's error", "[ladder][testkit][pump]") {
    morph::qt::QtExecutor executor;
    auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&executor);
    auto sharedPromise = std::make_shared<morph::async::Completion<int>::Promise>(std::move(promise));
    QTimer::singleShot(10, [sharedPromise] {
        try {
            throw std::runtime_error("boom");
        } catch (...) {
            sharedPromise->reject(std::current_exception());
        }
    });
    REQUIRE_THROWS_AS(morph::ladder::testkit::awaitQt(std::move(completion)), std::runtime_error);
}

// Regression test for a stack-use-after-scope bug: awaitQt's original
// implementation captured its `value`/`error` locals *by reference* in the
// then()/onError() handlers. Those handlers are stored on the completion's
// backing CompletionState, which can outlive awaitQt's stack frame — e.g.
// when awaitQt times out and throws while the underlying operation is still
// pending. Here `state` (the CompletionState) is kept alive by this test
// past the awaitQt call, exactly as an unrelated pending-call map elsewhere
// would keep it alive in production. Resolving it *after* awaitQt has
// already thrown and unwound exercises the late-callback path: with the old
// by-reference capture this write lands on destroyed stack memory (a
// stack-use-after-scope, reliably flagged by ASan even when it doesn't
// crash outright in a plain build); with the fix (heap state behind a
// shared_ptr captured by value) it lands on harmless, still-valid, orphaned
// heap memory. This test cannot assert on the corrupted value directly —
// its value is proving the process doesn't crash/corrupt under a sanitizer.
TEST_CASE("awaitQt timeout does not leave dangling references for a late-firing callback", "[ladder][testkit][pump]") {
    morph::qt::QtExecutor executor;
    auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&executor);
    auto sharedPromise = std::make_shared<morph::async::Completion<int>::Promise>(std::move(promise));

    // Nothing ever resolves this completion before the deadline, so awaitQt
    // times out and throws while its then()/onError() handlers are still
    // attached to the shared state behind `sharedPromise`.
    REQUIRE_THROWS_AS(morph::ladder::testkit::awaitQt(std::move(completion), std::chrono::milliseconds{50}),
                      std::runtime_error);

    // awaitQt's frame is gone, but `sharedPromise` (held here, as a backend's
    // pending-call map would hold the underlying state) is still alive and
    // its paired Completion still holds the handlers awaitQt installed.
    // Resolve it now and pump so the posted callback actually runs.
    sharedPromise->resolve(42);
    // Deliberately discarded: the predicate is `false` by construction, so
    // this is "pump for 50ms", not a wait — the timeout *is* the point.
    (void)morph::ladder::testkit::pumpUntil([] { return false; }, std::chrono::milliseconds{50});

    SUCCEED("late resolution after awaitQt's timeout did not crash or corrupt memory");
}
