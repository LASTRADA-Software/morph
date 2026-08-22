// SPDX-License-Identifier: Apache-2.0
//
// QtExecutor's teardown contract: a task still queued when the executor is
// destroyed is dropped rather than delivered against freed memory.
//
// The second case is a regression test for a use-after-free that segfaulted
// plain, uninstrumented builds 5 runs out of 5 -- it is not a
// sanitizer-only diagnostic.

#include <QCoreApplication>
#include <QEventLoop>

#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/qt/qt_executor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {

/// @brief A `Completion` and the `Promise` that settles it, kept together so a
///        chain link can be captured by one `shared_ptr`.
template <typename T>
struct Pending {
    morph::async::Completion<T> completion;
    typename morph::async::Completion<T>::Promise promise;

    explicit Pending(std::pair<morph::async::Completion<T>, typename morph::async::Completion<T>::Promise> pair)
        : completion{std::move(pair.first)}, promise{std::move(pair.second)} {}
};

}  // namespace

TEST_CASE("QtExecutor drops a task still queued when it is destroyed", "[qt][executor][teardown]") {
    // Held outside the executor so the assertion survives its destruction.
    auto ran = std::make_shared<int>(0);

    {
        morph::qt::QtExecutor executor;
        executor.post([ran] { ++*ran; });
        // Destroyed with the event still on the Qt queue, undelivered.
    }

    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    // Not merely "did not crash": the body demonstrably never ran.
    CHECK(*ran == 0);
}

TEST_CASE("QtExecutor still runs a task posted and delivered while it is alive",
          "[qt][executor][teardown]") {
    // The guard must not be so eager that it drops ordinary work.
    morph::qt::QtExecutor executor;
    auto ran = std::make_shared<int>(0);
    executor.post([ran] { ++*ran; });

    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    CHECK(*ran == 1);
}

TEST_CASE("A nested Completion chain outliving its QtExecutor does not use it after free",
          "[qt][executor][teardown]") {
    // The real shape of the bug. Bridge::executeVia chains three Completions
    // per action, each settled from *inside* the previous one's delivered
    // callback. A caller waiting only on its own top-level completion sees
    // "done" while an intermediate post is still queued -- and that post's
    // body calls post() again for the next link.
    //
    // Joining the worker pool proves every post() *happened*; it says nothing
    // about whether the resulting event was ever *delivered*. Before the
    // guard, pumping after teardown drove that body into a freed executor and
    // segfaulted an uninstrumented build.
    using morph::async::Completion;

    auto workerPool = std::make_unique<morph::exec::ThreadPoolExecutor>(4);
    auto qtExecutor = std::make_unique<morph::qt::QtExecutor>();

    std::atomic<int> outstanding{1};
    auto lastLinkRan = std::make_shared<int>(0);

    auto outer = std::make_shared<Pending<int>>(Completion<int>::makeSettleable(qtExecutor.get()));
    auto inner = std::make_shared<Pending<int>>(Completion<int>::makeSettleable(qtExecutor.get()));
    auto caller = std::make_shared<Pending<int>>(Completion<int>::makeSettleable(qtExecutor.get()));

    caller->completion.onError([lastLinkRan](const std::exception_ptr&) { ++*lastLinkRan; });
    inner->completion.onError([inner, caller](const std::exception_ptr&) {
        caller->promise.reject(std::make_exception_ptr(std::runtime_error{"second"}));
    });
    outer->completion.then([&outstanding, inner](int) {
        inner->promise.reject(std::make_exception_ptr(std::runtime_error{"first"}));
        // Caller-visible "done" -- while inner's own post is still queued.
        --outstanding;
    });

    workerPool->post([outer] { outer->promise.resolve(42); });

    // Unbudgeted processEvents(), deliberately: it returns as soon as the
    // currently-queued events are handled, so the loop exits the instant the
    // caller sees "done" and the *nested* posts are still queued. A budgeted
    // pump (processEvents(AllEvents, 10)) drains the whole chain before
    // teardown and the window this test exists for never opens -- which is
    // how the first draft of this test passed for the wrong reason.
    for (int spin = 0; spin < 5000 && outstanding.load() != 0; ++spin) {
        QCoreApplication::processEvents();
    }
    REQUIRE(outstanding.load() == 0);

    workerPool.reset();   // joins: every post() so far has happened
    qtExecutor.reset();   // freed with an undelivered post still queued

    // Reaching the end of this test *is* the assertion: before the guard,
    // pumping here drove the queued callback's body into a freed executor and
    // the process died with SIGSEGV, taking the whole binary with it.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    INFO("last chain link ran: " << *lastLinkRan);
    SUCCEED("pumped past executor teardown without a use-after-free");
}
