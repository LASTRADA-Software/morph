// SPDX-License-Identifier: Apache-2.0
//
// Coverage for the client-side execute deadline (examples/LADDER.md's
// "Framework prerequisites" #2): Bridge::setExecuteDeadline races the real
// reply against a client-owned timeout, so a frame silently dropped by
// QtWebSocketServerConfig::messagesPerSecond, or a genuinely hung server,
// no longer blocks the calling Completion forever.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "test_support.hpp"

namespace {

struct DeadlineCount {
    int x = 0;
};

struct DeadlineModel {
    int execute(const DeadlineCount& a) { return a.x; }
};

// A backend whose execute() never resolves its Completion, simulating a frame
// the server dropped -- no reply, ever, on this path -- or a hung server.
class NeverRepliesBackend : public morph::backend::detail::IBackend {
public:
    morph::exec::detail::ModelId registerModel(
        const std::string&, std::function<std::unique_ptr<morph::model::detail::IModelHolder>()>) override {
        return morph::exec::detail::ModelId{1};
    }
    void deregisterModel(morph::exec::detail::ModelId) override {}
    morph::async::Completion<std::shared_ptr<void>> execute(morph::exec::detail::ModelId,
                                                            morph::backend::detail::ActionCall,
                                                            morph::exec::IExecutor* cbExec) override {
        auto state = std::make_shared<morph::async::detail::CompletionState<std::shared_ptr<void>>>();
        ++liveCompletions;
        return morph::async::Completion<std::shared_ptr<void>>{state, cbExec};
        // `state` is intentionally dropped here with no setValue/setException
        // ever called -- the Completion this returns never settles on its
        // own, matching a dropped frame or a server that never replies.
    }
    void notifyBackendChanged() override {}
    // Deliberately a no-op: a real backend resolves its outstanding states
    // here, which is exactly the "something eventually settles it" behaviour
    // these tests must not rely on.
    void cancelPending(const std::exception_ptr&) override {}

    std::atomic<int> liveCompletions{0};
};

// A backend that holds every state it hands out and only settles it when the
// test says so -- a server whose reply arrives *after* the client already gave
// up. Lets the "a late real reply is silently discarded" guarantee be asserted
// deterministically rather than by racing wall-clock timers.
class LateReplyBackend : public morph::backend::detail::IBackend {
public:
    morph::exec::detail::ModelId registerModel(
        const std::string&, std::function<std::unique_ptr<morph::model::detail::IModelHolder>()>) override {
        return morph::exec::detail::ModelId{1};
    }
    void deregisterModel(morph::exec::detail::ModelId) override {}
    morph::async::Completion<std::shared_ptr<void>> execute(morph::exec::detail::ModelId,
                                                            morph::backend::detail::ActionCall,
                                                            morph::exec::IExecutor* cbExec) override {
        auto state = std::make_shared<morph::async::detail::CompletionState<std::shared_ptr<void>>>();
        {
            std::scoped_lock const lock{_mtx};
            _pending.push_back(state);
        }
        return morph::async::Completion<std::shared_ptr<void>>{state, cbExec};
    }
    void notifyBackendChanged() override {}
    void cancelPending(const std::exception_ptr&) override {}

    /// Settles every outstanding request with @p value, as a server reply that
    /// finally turned up would.
    void replyLate(int value) {
        std::vector<std::shared_ptr<morph::async::detail::CompletionState<std::shared_ptr<void>>>> pending;
        {
            std::scoped_lock const lock{_mtx};
            pending.swap(_pending);
        }
        for (auto& state : pending) {
            state->setValue(std::static_pointer_cast<void>(std::make_shared<int>(value)));
        }
    }

private:
    std::mutex _mtx;
    std::vector<std::shared_ptr<morph::async::detail::CompletionState<std::shared_ptr<void>>>> _pending;
};

}  // namespace

template <>
struct morph::model::ActionTraits<DeadlineCount> {
    using Result = int;
    static constexpr std::string_view typeId() { return "Deadline_Count"; }
    static std::string toJson(const DeadlineCount& a) { return R"({"x":)" + std::to_string(a.x) + "}"; }
    static DeadlineCount fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& r) { return std::to_string(r); }
    static int resultFromJson(std::string_view s) { return std::stoi(std::string{s}); }
};
template <>
struct morph::model::ModelTraits<DeadlineModel> {
    static constexpr std::string_view typeId() { return "Deadline_Model"; }
};

TEST_CASE("Bridge::setExecuteDeadline(0) (the default) never fires -- a call that never replies "
          "stays pending, matching pre-existing behavior",
          "[core][bridge][client-deadline]") {
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<NeverRepliesBackend>()};
    CHECK(bridge.executeDeadline() == std::chrono::milliseconds{0});
    morph::bridge::BridgeHandler<DeadlineModel> handler{bridge, &exec};

    bool resolved = false;
    handler.execute(DeadlineCount{.x = 1})
        .then([&resolved](int) { resolved = true; })
        .onError([&resolved](const std::exception_ptr&) { resolved = true; });
    exec.runFor(std::chrono::milliseconds{200});
    CHECK_FALSE(resolved);
}

TEST_CASE("Bridge::setExecuteDeadline fires ClientTimeoutError when no reply arrives in time",
          "[core][bridge][client-deadline]") {
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<NeverRepliesBackend>()};
    bridge.setExecuteDeadline(std::chrono::milliseconds{50});
    CHECK(bridge.executeDeadline() == std::chrono::milliseconds{50});
    morph::bridge::BridgeHandler<DeadlineModel> handler{bridge, &exec};

    bool failed = false;
    bool threwClientTimeout = false;
    handler.execute(DeadlineCount{.x = 1}).onError([&](const std::exception_ptr& err) {
        failed = true;
        try {
            std::rethrow_exception(err);
        } catch (const morph::backend::ClientTimeoutError&) {
            threwClientTimeout = true;
        } catch (...) {
        }
    });
    // Poll rather than a single runFor(): the deadline fires on the
    // TimeoutScheduler's own background thread, which posts to `exec` --
    // give it real wall-clock slack, matching this codebase's other
    // cross-thread test patterns.
    for (int i = 0; i < 50 && !failed; ++i) {
        exec.runFor(std::chrono::milliseconds{20});
    }
    REQUIRE(failed);
    CHECK(threwClientTimeout);
}

TEST_CASE("A deadline that is cancelled by a real, on-time reply does not also fire",
          "[core][bridge][client-deadline]") {
    // Uses the ordinary in-process LocalBackend, which always replies quickly.
    // Exercises the disarm path (the `.then`/`.onError` cancel-before-settle
    // lines) and pins the happy path: enabling a deadline must not perturb a
    // call that replies in time. What a *missing* disarm would look like from
    // the outside is covered by the next test instead -- because
    // CompletionState is first-result-wins, a deadline that fires after an
    // on-time reply is a no-op, so it cannot be observed here.
    morph::exec::ThreadPoolExecutor workerPool{2};
    morph::exec::MainThreadExecutor guiExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(workerPool)};
    bridge.setExecuteDeadline(std::chrono::milliseconds{2000});  // generous; must not fire
    morph::bridge::BridgeHandler<DeadlineModel> handler{bridge, &guiExec};

    int result = -1;
    bool failed = false;
    handler.execute(DeadlineCount{.x = 7})
        .then([&result](int r) { result = r; })
        .onError([&failed](const std::exception_ptr&) { failed = true; });
    for (int i = 0; i < 50 && result == -1 && !failed; ++i) {
        guiExec.runFor(std::chrono::milliseconds{10});
    }
    CHECK(result == 7);
    CHECK_FALSE(failed);
    // If the disarm did not work, the 2000ms deadline would still be pending on
    // the scheduler's background thread when the Bridge goes out of scope here.
    // That must not hang the test process: ~TimeoutScheduler drops pending
    // entries without firing them and joins its thread unconditionally, so a
    // leaked entry costs nothing at teardown. Noted rather than asserted --
    // there is no public handle to observe it through.
}

TEST_CASE("A real reply that arrives after the deadline already fired is silently discarded",
          "[core][bridge][client-deadline]") {
    // The idempotency half of the race documented in docs/spec/core/completion.md:
    // once the deadline resolved the Completion with ClientTimeoutError, the
    // server's eventual reply must not resurrect it with a value. Driven
    // explicitly by the test rather than by wall-clock luck.
    morph::exec::MainThreadExecutor exec;
    auto backendOwner = std::make_unique<LateReplyBackend>();
    auto* const backend = backendOwner.get();
    morph::bridge::Bridge bridge{std::move(backendOwner)};
    bridge.setExecuteDeadline(std::chrono::milliseconds{50});
    morph::bridge::BridgeHandler<DeadlineModel> handler{bridge, &exec};

    int settleCount = 0;
    int value = -1;
    bool threwClientTimeout = false;
    handler.execute(DeadlineCount{.x = 1})
        .then([&](int r) {
            ++settleCount;
            value = r;
        })
        .onError([&](const std::exception_ptr& err) {
            ++settleCount;
            try {
                std::rethrow_exception(err);
            } catch (const morph::backend::ClientTimeoutError&) {
                threwClientTimeout = true;
            } catch (...) {
            }
        });

    for (int i = 0; i < 50 && settleCount == 0; ++i) {
        exec.runFor(std::chrono::milliseconds{20});
    }
    REQUIRE(settleCount == 1);
    REQUIRE(threwClientTimeout);

    // The server finally replies. Nothing may change.
    backend->replyLate(99);
    exec.runFor(std::chrono::milliseconds{200});
    CHECK(settleCount == 1);
    CHECK(value == -1);
}
