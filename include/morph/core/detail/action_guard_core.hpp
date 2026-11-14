// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <concepts>
#include <exception>

/// @file
/// The declarations every execution site needs to run an action's guard. `registry.hpp` includes this header, so it
/// depends on nothing there. See `docs/spec/core/action_guards.md`.

namespace morph::model {

/// @brief Whether @p Action declares a guard: a public member alias `guard` naming the guard type.
///
/// Read off the action's own definition, so every translation unit that names the action gives the same answer.
template <typename Action>
concept DeclaresGuard = requires { typename Action::guard; };

/// @brief Whether @p Model declares `void onGuardRefused(Action const&, std::exception const&)`, which every
///        execution site and `executeChecked` call after the action's guard refuses it.
template <typename Model, typename Action>
concept GuardRefusalObserver = requires(Model& model, Action const& action, std::exception const& refusal) {
    { model.onGuardRefused(action, refusal) } -> std::same_as<void>;
};

namespace detail {

/// @brief Whether @p Guard checks @p Action on its payload alone.
template <typename Guard, typename Action>
concept GuardChecksAction = requires(Action const& action) {
    { Guard::check(action) } -> std::same_as<void>;
};

/// @brief Whether @p Guard checks @p Action against an instance of @p Model.
template <typename Guard, typename Model, typename Action>
concept GuardChecksInModel = requires(Model const& model, Action const& action) {
    { Guard::check(model, action) } -> std::same_as<void>;
};

/// @brief Runs @p Action's guard on @p action in the form it declares; nothing for an unguarded action.
/// @tparam Model The model type the action is executed on.
/// @tparam Action The action type.
/// @param model The instance the action is addressed to.
/// @param action The decoded, recomputed, validated action.
template <typename Model, typename Action>
void guardAction(Model const& model, Action const& action) {
    if constexpr (DeclaresGuard<Action>) {
        using Guard = typename Action::guard;
        // A hard error, never a false: an execution site compiled without the guard's definition must not compile,
        // and the form checks below are only stable once the type is complete.
        // NOLINTNEXTLINE(bugprone-sizeof-expression): sizeof is the completeness test here, not a size.
        static_assert(sizeof(Guard) > 0, "an action's guard must be complete where its execution sites are compiled");
        constexpr bool payloadForm = GuardChecksAction<Guard, Action>;
        constexpr bool modelForm = GuardChecksInModel<Guard, Model, Action>;
        static_assert(payloadForm != modelForm,
                      "an action's guard provides exactly one callable check for this model: "
                      "check(Action const&) or check(Model const&, Action const&)");
        if constexpr (modelForm) {
            Guard::check(model, action);
        } else {
            Guard::check(action);
        }
    } else {
        static_cast<void>(model);
        static_cast<void>(action);
    }
}

}  // namespace detail
}  // namespace morph::model
