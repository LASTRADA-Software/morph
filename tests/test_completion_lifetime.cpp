// SPDX-License-Identifier: Apache-2.0
//
// morph::async::HasLifetime and Completion's lifetime-bound overloads.

#include <morph/core/completion.hpp>
#include <morph/core/lifetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>

#include "test_support.hpp"

using SyncExecutor = morph::testing::InlineExecutor;
using morph::async::Completion;
using morph::async::HasLifetime;

namespace {

/// @brief A receiver that writes through `this` from its callback -- the
///        shape that produced #137's real use-after-free.
class Receiver : public HasLifetime {
  public:
    int received = -1;

    /// @brief Call counters held *outside* the receiver, so "did the callback
    ///        body run?" stays observable after the receiver is gone.
    ///
    /// Asserting only that resolving a completion does not throw would pass
    /// with no guard at all -- writing through a destroyed object is undefined
    /// behaviour, not an exception. Copying these `shared_ptr`s into the
    /// lambda means the counter survives the receiver, so a suppressed
    /// callback is directly observable and no test here relies on UB being
    /// detected.
    std::shared_ptr<int> thenCalls = std::make_shared<int>(0);
    std::shared_ptr<int> errorCalls = std::make_shared<int>(0);

    void observe(Completion<int>& completion) {
        completion.then(this, [this, calls = thenCalls](int value) {
                       ++*calls;
                       received = value;
                   })
            .onError(this, [this, calls = errorCalls](const std::exception_ptr&) {
                ++*calls;
                received = -2;
            });
    }
};

}  // namespace

TEST_CASE("A lifetime-bound then() fires while the receiver is alive", "[completion][lifetime]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    Completion<int> comp{state, &exec};

    Receiver receiver;
    receiver.observe(comp);
    state->setValue(42);

    CHECK(receiver.received == 42);
    CHECK(*receiver.thenCalls == 1);
    CHECK(*receiver.errorCalls == 0);
}

TEST_CASE("A lifetime-bound then() does not fire after the receiver is destroyed",
          "[completion][lifetime]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    Completion<int> comp{state, &exec};

    std::shared_ptr<int> calls;
    {
        Receiver receiver;
        receiver.observe(comp);
        calls = receiver.thenCalls;
        // Receiver dies here with the completion still unresolved -- exactly
        // the window a posted reply lands in.
    }

    state->setValue(42);

    // The callback body never ran. Without the guard it would have run and
    // written through a destroyed object.
    CHECK(*calls == 0);
}

TEST_CASE("A lifetime-bound onError() does not fire after the receiver is destroyed",
          "[completion][lifetime]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    Completion<int> comp{state, &exec};

    std::shared_ptr<int> calls;
    {
        Receiver receiver;
        receiver.observe(comp);
        calls = receiver.errorCalls;
    }

    state->setException(std::make_exception_ptr(std::runtime_error{"boom"}));
    CHECK(*calls == 0);
}

TEST_CASE("The guard is per-receiver, not global", "[completion][lifetime]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    Completion<int> comp{state, &exec};

    Receiver survivor;
    std::shared_ptr<int> doomedCalls;
    {
        Receiver doomed;
        doomed.observe(comp);
        doomedCalls = doomed.thenCalls;
        survivor.observe(comp);
    }
    state->setValue(7);

    // The surviving receiver still gets its callback: one receiver's death
    // must not silence another's handler on the same completion.
    CHECK(survivor.received == 7);
    CHECK(*survivor.thenCalls == 1);
    CHECK(*doomedCalls == 0);
}

TEST_CASE("thenDetached still fires with no receiver to guard", "[completion][lifetime]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    Completion<int> comp{state, &exec};

    int received = -1;
    comp.thenDetached([&](int value) { received = value; });
    state->setValue(5);

    CHECK(received == 5);
}

TEST_CASE("A moved-from receiver gets a fresh token rather than sharing one",
          "[completion][lifetime]") {
    // HasLifetime's copy/move give the new object its own token: a token is
    // an identity, not a value. Sharing one would make a copy's death silence
    // the original's callbacks.
    Receiver first;
    const auto firstToken = first.lifetimeToken();
    Receiver second{std::move(first)};

    CHECK_FALSE(second.lifetimeToken().expired());
    CHECK_FALSE(firstToken.expired());  // `first` is moved-from but still alive
    CHECK(second.lifetimeToken().lock() != firstToken.lock());
}
