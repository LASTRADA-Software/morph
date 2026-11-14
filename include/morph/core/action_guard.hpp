// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <string_view>
#include <utility>

#include "../forms/forms.hpp"
#include "detail/action_guard_core.hpp"
#include "registry.hpp"

/// @file
/// Action guards: a check an action declares with `using guard = ...;`, which the framework runs on the decoded
/// action on every execution site, after the validator and before `Model::execute`. This header adds the calls an
/// application makes; the execution sites need only `detail/action_guard_core.hpp`. See
/// `docs/spec/core/action_guards.md`.

namespace morph::model {

/// @brief Runs @p action on @p model as a dispatch would: recompute, the validator, the guard, then `execute`.
///
/// For a caller that holds a model directly — a unit test of a guard's outcome, server-side code running a model in
/// process. A plain `model.execute(action)` runs no gate. There is no holder, so nothing is journaled and the guard is
/// never skipped; a refusal reaches `model.onGuardRefused` when the model declares it.
/// @tparam Model The model type; registered, so its id names a `ValidationError`.
/// @tparam Action The action type; its handler is an ordinary one.
/// @param model The model.
/// @param action The action, by value: recomputing its computed fields changes it.
/// @return What `execute` returned.
/// @throws ValidationError if the recomputed action fails its validator.
/// @throws std::exception whatever the guard, `onGuardRefused` or `execute` throws.
template <typename Model, typename Action>
auto executeChecked(Model& model, Action action) {
    static_assert(!isTaskHandler<decltype(model.execute(action))>,
                  "executeChecked runs ordinary handlers only: a Task would outlive the action it refers to");
    ::morph::forms::recomputeAll(action);
    if (!ActionValidator<Action>::ready(action)) {
        throw ValidationError{ModelTraits<Model>::typeId(), ActionTraits<Action>::typeId()};
    }
    if constexpr (DeclaresGuard<Action>) {
        try {
            detail::guardAction(std::as_const(model), action);
        } catch (const std::exception& refusal) {
            if constexpr (GuardRefusalObserver<Model, Action>) {
                model.onGuardRefused(action, refusal);
            }
            throw;
        }
    }
    return model.execute(action);
}

/// @brief Whether the action registered as (@p modelType, @p actionType) in the default dispatcher — where
///        `BRIDGE_REGISTER_ACTION` registers — declares a guard.
/// @param modelType The model's registered id.
/// @param actionType The action's registered id.
/// @return `ActionDispatcher::isGuarded` on `detail::defaultDispatcher()`.
[[nodiscard]] inline bool isGuarded(std::string_view modelType, std::string_view actionType) {
    return detail::defaultDispatcher().isGuarded(modelType, actionType);
}

}  // namespace morph::model
