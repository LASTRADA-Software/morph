// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/flows.hpp
/// @brief Multi-step wizards: an ordered sequence of registered actions that
///        share a "flow draft" spanning the whole sequence.
///
/// A `morph::flows::Wizard` names an ordered list of already-registered
/// action types (see `BRIDGE_REGISTER_ACTION`, registry.hpp) plus, per step,
/// a human title and an optional `Bind` prefill declaration mapping that
/// step's field name to a `"<PriorAction>.<field>"` path into an earlier
/// step's submitted draft or result. `wizardSchemaJson<W>()` emits the `w-*`
/// document a renderer consumes to build a stepper;
/// `morph::flows::FlowSession<Model, Steps...>` (see the `set<>`/`advance`/
/// `back` API added alongside it) is the typed C++ sequencer that fires each
/// step through the ordinary `BridgeHandler<Model>::set<>` / `subscribe<>`
/// path (bridge.hpp) and tracks the resolved values a caller (or a renderer)
/// reads to prefill a later step.
///
/// This is purely additive metadata and sequencing over the existing
/// dispatch path: no new wire format, no new execution mode. See
/// docs/spec/forms/workflows_navigation.md.

#include <cstddef>
#include <glaze/glaze.hpp>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "../core/bridge.hpp"
#include "forms.hpp"

namespace morph::flows {

/// @brief One `field -> "<PriorAction>.<field>"` prefill binding declared on
///        a wizard step.
/// @tparam Field The step action's field name to prefill.
/// @tparam Path  Source path, `"<PriorAction>.<field>"`, into an earlier
///               step's captured values (see `FlowSession::resolved`).
template <morph::forms::FixedString Field, morph::forms::FixedString Path>
struct Bind {
    /// @brief The step action's field name this binding fills.
    /// @return The declared field name.
    [[nodiscard]] static constexpr std::string_view field() noexcept { return Field.view(); }

    /// @brief The source path into an earlier step's captured values.
    /// @return The declared `"<PriorAction>.<field>"` path.
    [[nodiscard]] static constexpr std::string_view path() noexcept { return Path.view(); }
};

/// @brief One step of a `Wizard`: a registered action, a display title, and
///        zero or more `Bind` prefill declarations.
/// @tparam Action Registered action type (`BRIDGE_REGISTER_ACTION`) this step fires.
/// @tparam Title  Human title for the step (breadcrumb / header).
/// @tparam Binds  Zero or more `Bind<Field, Path>` prefill declarations.
template <typename Action, morph::forms::FixedString Title, typename... Binds>
struct WizardStep {
    /// @brief The step's action type.
    using action = Action;

    /// @brief Tuple of this step's `Bind<...>` prefill declarations (possibly empty).
    using binds = std::tuple<Binds...>;

    /// @brief The step's display title.
    /// @return The declared title.
    [[nodiscard]] static constexpr std::string_view title() noexcept { return Title.view(); }
};

/// @brief An ordered sequence of `WizardStep`s sharing one flow.
/// @tparam Title Human title for the whole flow.
/// @tparam Steps One or more `WizardStep<Action, Title, Binds...>` types, in order.
template <morph::forms::FixedString Title, typename... Steps>
struct Wizard {
    /// @brief Tuple of this wizard's ordered `WizardStep<...>` types.
    using steps = std::tuple<Steps...>;

    /// @brief The wizard's display title.
    /// @return The declared title.
    [[nodiscard]] static constexpr std::string_view title() noexcept { return Title.view(); }
};

/// @brief Traits specialisation mapping a `Wizard` type to its string type-id.
///
/// Specialise via `BRIDGE_REGISTER_WIZARD` rather than by hand. The default is
/// a forward declaration — using it without a specialisation is an
/// incomplete-type error.
/// @tparam W Concrete `Wizard<...>` type.
template <typename W>
struct WizardTraits;  // forward — specialise or use BRIDGE_REGISTER_WIZARD

namespace detail {

/// @brief Invokes `visitor.template operator()<std::tuple_element_t<I, Tuple>, I>()`
///        for every element of @p Tuple, in order.
/// @tparam Tuple   A `std::tuple<...>` type (only its element types/arity are used).
/// @tparam Visitor Callable with a `template<typename Element, std::size_t I> operator()()`.
/// @param visitor Callable invoked once per tuple element.
template <typename Tuple, typename Visitor>
constexpr void forEachTupleElement(Visitor&& visitor) {
    []<std::size_t... I>(std::index_sequence<I...>, Visitor&& innerVisitor) {
        (innerVisitor.template operator()<std::tuple_element_t<I, Tuple>, I>(), ...);
    }(std::make_index_sequence<std::tuple_size_v<Tuple>>{}, std::forward<Visitor>(visitor));
}

/// @brief Invokes `visitor.template operator()<Step>()` for the pack element
///        of `Steps...` at runtime position @p index. A no-op when
///        `index >= sizeof...(Steps)`.
/// @tparam Steps   The pack to index into.
/// @tparam Visitor Callable with a `template<typename Step> operator()()`.
/// @param index   0-based position to visit.
/// @param visitor Callable invoked for the step at @p index.
template <typename... Steps, typename Visitor>
constexpr void forStep(std::size_t index, Visitor&& visitor) {
    std::size_t i = 0;
    (void)((i++ == index ? (visitor.template operator()<Steps>(), true) : false) || ...);
}

/// @brief Trait: `true` when every type in `Ts...` is pairwise distinct.
/// @tparam Ts Types to check for pairwise distinctness.
template <typename... Ts>
struct AllDistinct : std::true_type {};

/// @brief Recursive case: `T` distinct from every type in `Rest...`, and `Rest...` pairwise distinct.
/// @tparam T    The type being checked against `Rest...`.
/// @tparam Rest The remaining types.
template <typename T, typename... Rest>
struct AllDistinct<T, Rest...> : std::bool_constant<(!std::is_same_v<T, Rest> && ...) && AllDistinct<Rest...>::value> {
};

}  // namespace detail

/// @brief Generates the `w-*` JSON document for wizard type @p W.
///
/// Emits `w-title` and an ordered `w-steps` array; each step carries `action`
/// (the step's registered action type-id), `title`, and — only when the step
/// declares at least one `Bind` — a `prefill` object mapping field name to
/// `"<PriorAction>.<field>"` path. See docs/spec/forms/workflows_navigation.md
/// for the full key vocabulary.
/// @tparam W Concrete `Wizard<Title, Steps...>` type.
/// @return The wizard's JSON document. Empty string only if glaze's own JSON
///         writer fails on the assembled DOM (schema generation never throws).
template <typename W>
[[nodiscard]] std::string wizardSchemaJson() {
    glz::generic_u64 dom{};
    dom["w-title"] = std::string{W::title()};

    glz::generic_u64::array_t steps{};
    detail::forEachTupleElement<typename W::steps>([&]<typename StepT, std::size_t I>() {
        static_cast<void>(I);
        glz::generic_u64 step{};
        step["action"] = std::string{::morph::model::ActionTraits<typename StepT::action>::typeId()};
        step["title"] = std::string{StepT::title()};
        if constexpr (std::tuple_size_v<typename StepT::binds> != 0) {
            auto& prefillNode = step["prefill"];
            detail::forEachTupleElement<typename StepT::binds>([&]<typename BindT, std::size_t J>() {
                static_cast<void>(J);
                prefillNode[std::string{BindT::field()}] = std::string{BindT::path()};
            });
        }
        steps.emplace_back(std::move(step));
    });
    dom["w-steps"] = steps;

    return glz::write_json(dom).value_or(std::string{});
}

}  // namespace morph::flows

/// @brief Specialises `morph::flows::WizardTraits<W>` with the string type-id @p NAME.
///
/// Metadata only: unlike `BRIDGE_REGISTER_ACTION`, this performs no
/// static-init registration into any dispatch registry — a wizard is never
/// itself executed, only its steps' already-registered actions are (see
/// docs/spec/forms/workflows_navigation.md).
/// @param W    Concrete `morph::flows::Wizard<...>` type.
/// @param NAME String literal used as the wizard's type-id.
#define BRIDGE_REGISTER_WIZARD(W, NAME)                                      \
    template <>                                                              \
    struct morph::flows::WizardTraits<W> {                                   \
        static constexpr std::string_view typeId() noexcept { return NAME; } \
    };
