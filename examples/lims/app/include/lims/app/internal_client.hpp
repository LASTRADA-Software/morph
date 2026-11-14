// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/owner_strand.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <optional>
#include <string>
#include <string_view>

/// @file
/// The server's own client: background work dispatches through the same `RemoteServer` a socket client reaches,
/// under a service principal whose token the server mints, so it is authorized, journaled and fanned out like a
/// user's call (`examples/LADDER.md`, "Background jobs").

namespace lims::app {

/// @brief A `Bridge` over `SimulatedRemoteBackend` into a server, acting as one service principal.
///
/// The bridge belongs to a one-thread executor of its own, where every call is issued and settled, so `call` may
/// block any other thread — a relay pass, a schedule tick — without blocking the thread that delivers its answer.
/// `call` must not be made on that executor; a Task handler awaits `dispatch` instead.
class InternalClient {
public:
    /// @brief A client of @p server acting as @p principal.
    /// @param server The server. Borrowed: it must outlive this client.
    /// @param secret The token secret the server's authorizer verifies with.
    /// @param principal A principal in the reserved `system:` namespace.
    /// @throws ValidationError if @p principal is not a service principal.
    InternalClient(::morph::backend::RemoteServer& server, std::string_view secret, std::string principal);

    /// @brief Releases the bridge on its own executor. Every `dispatch` must have settled.
    ~InternalClient();
    InternalClient(InternalClient const&) = delete;
    InternalClient& operator=(InternalClient const&) = delete;
    InternalClient(InternalClient&&) = delete;
    InternalClient& operator=(InternalClient&&) = delete;

    /// @brief Dispatches @p action on a fresh instance of @p Model.
    /// @tparam Model The model.
    /// @tparam Action The action.
    /// @param action The action.
    /// @return Its result, settled on this client's executor; `co_await` it from a Task handler.
    template <typename Model, typename Action>
    [[nodiscard]] ::morph::async::Completion<typename ::morph::model::ActionTraits<Action>::Result> dispatch(
        Action action);

    /// @brief Dispatches @p action on a fresh instance of @p Model and waits for its result.
    /// @tparam Model The model.
    /// @tparam Action The action.
    /// @param action The action.
    /// @return Its result.
    /// @throws std::exception the server's refusal or the action's failure.
    template <typename Model, typename Action>
    typename ::morph::model::ActionTraits<Action>::Result call(Action action);

    /// @brief The principal it acts as.
    /// @return It.
    [[nodiscard]] std::string const& principal() const noexcept { return _principal; }

private:
    /// @brief Runs @p task on the bridge's executor and waits for it.
    void onOwner(std::function<void()> const& task);

    std::string _principal;
    // A strand over the one-thread pool: the bridge's owner must be serial, which a pool reports it is not.
    ::morph::exec::ThreadPoolExecutor _pool{1};
    ::morph::exec::OwnerStrand _owner{_pool};
    std::optional<::morph::bridge::Bridge> _bridge;
};

template <typename Model, typename Action>
::morph::async::Completion<typename ::morph::model::ActionTraits<Action>::Result> InternalClient::dispatch(
    Action action) {
    using Result = ::morph::model::ActionTraits<Action>::Result;
    auto [completion, promise] = ::morph::async::Completion<Result>::makeSettleable(&_owner);
    auto settle = std::make_shared<typename ::morph::async::Completion<Result>::Promise>(std::move(promise));
    _owner.post([this, settle, action = std::move(action)] mutable {
        try {
            // The handler is kept alive by its call's callbacks: execute() returns before the server has run the
            // action, and a handler destroyed here would deregister its instance under the pending call.
            auto handler = std::make_shared<::morph::bridge::BridgeHandler<Model>>(*_bridge, &_owner);
            handler->execute(std::move(action))
                .then([settle, handler](Result result) { settle->resolve(std::move(result)); })
                .onError([settle, handler](std::exception_ptr const& error) { settle->reject(error); });
        } catch (...) {
            settle->reject(std::current_exception());
        }
    });
    return std::move(completion);
}

template <typename Model, typename Action>
typename ::morph::model::ActionTraits<Action>::Result InternalClient::call(Action action) {
    using Result = ::morph::model::ActionTraits<Action>::Result;
    auto settled = std::make_shared<std::promise<Result>>();
    auto answer = settled->get_future();
    _owner.post([this, settled, action = std::move(action)] mutable {
        try {
            auto handler = std::make_shared<::morph::bridge::BridgeHandler<Model>>(*_bridge, &_owner);
            handler->execute(std::move(action))
                .then([settled, handler](Result result) { settled->set_value(std::move(result)); })
                .onError([settled, handler](std::exception_ptr const& error) { settled->set_exception(error); });
        } catch (...) {
            settled->set_exception(std::current_exception());
        }
    });
    return answer.get();
}

}  // namespace lims::app
