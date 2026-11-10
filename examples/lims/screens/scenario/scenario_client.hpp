// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <exception>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/session/session.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

/// @file
/// How the scenario writes its records: as a person, through the models' actions.

namespace lims::screens {

/// @brief A bridge over an in-process backend that carries one person's session, so each scenario record is written
///        by the person the scenario names and is checked, journalled and announced like any dispatched action.
class ScenarioClient {
public:
    /// @brief A client acting as @p principal.
    /// @param principal The person's login.
    explicit ScenarioClient(std::string_view principal)
        : _bridge{std::make_unique<morph::backend::LocalBackend>(_workers), _owner} {
        morph::session::Context session;
        session.principal = std::string{principal};
        _bridge.setDefaultSession(std::move(session));
    }

    /// @brief Runs @p action on a model and waits for its answer.
    /// @tparam Model The model type.
    /// @tparam Action The action type.
    /// @param action The action.
    /// @return The action's result.
    /// @throws The action's own failure, or `std::runtime_error` when it does not finish within 30 s.
    template <typename Model, typename Action>
    typename morph::model::ActionTraits<Action>::Result call(Action action) {
        using Result = morph::model::ActionTraits<Action>::Result;
        morph::bridge::BridgeHandler<Model> handler{_bridge, &_owner};
        std::optional<Result> answer;
        std::exception_ptr failure;
        bool done = false;
        handler.execute(std::move(action))
            .then([&answer, &done](Result result) {
                answer = std::move(result);
                done = true;
            })
            .onError([&failure, &done](std::exception_ptr error) {
                failure = std::move(error);
                done = true;
            });
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
        while (!done && std::chrono::steady_clock::now() < deadline) {
            _owner.runFor(std::chrono::milliseconds{10});
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (!answer) {
            throw std::runtime_error{"lims scenario: an action did not finish within 30 s"};
        }
        return std::move(*answer);
    }

private:
    morph::exec::ThreadPoolExecutor _workers{1};
    morph::exec::MainThreadExecutor _owner;
    morph::bridge::Bridge _bridge;
};

}  // namespace lims::screens
