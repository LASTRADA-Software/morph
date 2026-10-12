// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../core/bridge.hpp"
#include "../../core/completion.hpp"
#include "../../core/executor.hpp"
#include "form_session.hpp"

/// @file
/// @brief Submitters and choice fetchers bound to an application's own `BridgeHandler`s.
///
/// Specified in `docs/spec/forms/engine.md`, "Submitters".

namespace morph::forms {

namespace detail::engine {

/// @brief Executes an action through one handler, or declines when the handler does not serve it.
using JsonRoute =
    std::function<std::optional<async::Completion<std::string>>(std::string_view actionType, std::string const& body)>;

/// @brief The route through @p handler.
/// @tparam Model The handler's model.
/// @tparam Sharing The handler's sharing policy.
/// @param handler The handler; it must outlive the route.
/// @return A route that serves exactly the actions @p handler's model registers.
template <class Model, class Sharing>
[[nodiscard]] JsonRoute routeThrough(bridge::BridgeHandler<Model, Sharing>& handler) {
    return [&handler](std::string_view actionType,
                      std::string const& body) -> std::optional<async::Completion<std::string>> {
        if (!handler.servesAction(actionType)) {
            return std::nullopt;
        }
        return handler.executeJson(actionType, body);
    };
}

/// @brief Runs @p body through the first route that serves @p actionType.
/// @param routes The routes, in priority order.
/// @param callbacks Where a refusal is delivered.
/// @param actionType The registered action id.
/// @param body The JSON body.
/// @return The route's completion, or one failed with `std::invalid_argument` when no route serves the action.
[[nodiscard]] inline async::Completion<std::string> routeFirst(std::vector<JsonRoute> const& routes,
                                                               exec::IExecutor& callbacks, std::string_view actionType,
                                                               std::string const& body) {
    for (JsonRoute const& route : routes) {
        if (std::optional<async::Completion<std::string>> completion = route(actionType, body)) {
            return std::move(*completion);
        }
    }
    auto [failed, promise] = async::Completion<std::string>::makeSettleable(&callbacks);
    promise.reject(std::make_exception_ptr(
        std::invalid_argument{"no handler serves the action \"" + std::string{actionType} + "\""}));
    return std::move(failed);
}

}  // namespace detail::engine

/// @brief A `Submitter` that sends each action through the first of @p handlers whose model registers it.
///
/// Use it when a form's submissions must reach the model instance the rest of the screen uses — an in-memory
/// model, or a shared instance another handler attached. `bridgeSubmitter` keeps handlers of its own instead.
/// @tparam Handlers `bridge::BridgeHandler<M, S>` types.
/// @param callbacks The executor a refusal is delivered on: the handlers' GUI executor.
/// @param handlers The handlers, in priority order; they must outlive the returned submitter.
/// @return The submitter.
template <class... Handlers>
    requires(sizeof...(Handlers) > 0)
[[nodiscard]] Submitter handlerSubmitter(exec::IExecutor& callbacks, Handlers&... handlers) {
    std::vector<detail::engine::JsonRoute> routes{detail::engine::routeThrough(handlers)...};
    return [routes = std::move(routes), &callbacks](std::string_view actionType, std::string bodyJson) {
        return detail::engine::routeFirst(routes, callbacks, actionType, bodyJson);
    };
}

/// @brief A `ChoiceFetcher` that runs each options action through the first of @p handlers that serves it.
/// @tparam Handlers `bridge::BridgeHandler<M, S>` types.
/// @param callbacks The executor a refusal is delivered on: the handlers' GUI executor.
/// @param handlers The handlers, in priority order; they must outlive the returned fetcher.
/// @return The fetcher.
template <class... Handlers>
    requires(sizeof...(Handlers) > 0)
[[nodiscard]] ChoiceFetcher handlerChoiceFetcher(exec::IExecutor& callbacks, Handlers&... handlers) {
    std::vector<detail::engine::JsonRoute> routes{detail::engine::routeThrough(handlers)...};
    return [routes = std::move(routes), &callbacks](std::string_view optionsAction, std::string bodyJson) {
        return detail::engine::routeFirst(routes, callbacks, optionsAction, bodyJson);
    };
}

}  // namespace morph::forms
