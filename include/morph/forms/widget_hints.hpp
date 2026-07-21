// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/widget_hints.hpp
/// @brief Typed wrappers that carry rendering-control intent for a form field.
///
/// `morph::forms::Multiline` and `morph::forms::Ranged<Min, Max, Step>` are
/// thin field-type wrappers in the same family as `morph::forms::Choice`
/// (choice.hpp): each carries a rendering *control* preference in the C++
/// type itself — `Multiline` says "edit me as a text area", `Ranged` says
/// "edit me as a slider with these bounds" — and `morph::forms::schemaJson`
/// surfaces that preference as the `x-widget` (and, for `Ranged`, `x-min` /
/// `x-max` / `x-step`) schema annotation(s). See docs/spec/forms/widget_hints.md
/// for the full design.
///
/// Neither wrapper changes what travels on the wire: the `glz::meta`
/// specialisations below reflect each type's bare payload directly, exactly
/// as `Choice` does, so `Multiline` serialises as a plain JSON string and
/// `Ranged` as a nullable number.

#include <concepts>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace morph::forms {

/// @brief A string field edited as a multi-line text area.
///
/// Wire form: a plain JSON string (via the `glz::meta` specialisation below),
/// exactly like an unwrapped `std::string` member — `Multiline` changes only
/// the *rendering intent*, never what travels. Because the payload is a bare
/// `std::string` (not a `std::optional`), `Multiline` does **not** define
/// `hasValue()`: like a plain `std::string` member, it is always considered
/// engaged by `allRequiredEngaged` (see docs/spec/forms/forms.md, "Empty state").
struct Multiline {
    /// @brief The text content.
    std::string value;

    /// @brief Constructs the empty string.
    constexpr Multiline() noexcept = default;

    /// @brief Engages with @p text.
    /// @param text The initial text content.
    Multiline(std::string text) noexcept(std::is_nothrow_move_constructible_v<std::string>) : value{std::move(text)} {}

    /// @brief The preferred control id for this field.
    /// @return `"textarea"`.
    [[nodiscard]] static constexpr std::string_view widget() noexcept { return "textarea"; }

    /// @brief Equality on the text content.
    /// @param other Multiline to compare against.
    /// @return `true` when both hold the same text.
    [[nodiscard]] constexpr bool operator==(const Multiline& other) const = default;
};

/// @brief A bounded numeric field edited as a slider.
/// @tparam Min  Inclusive lower bound of the control track.
/// @tparam Max  Inclusive upper bound of the control track (must be > `Min`).
/// @tparam Step Track increment (default `1`; must be > 0). Must be the same
///              arithmetic type as `Min`/`Max` — a floating-point `Ranged`
///              must name `Step` explicitly (the default `1` is `int`).
template <auto Min, auto Max, auto Step = 1>
    requires(std::is_arithmetic_v<decltype(Min)> && std::same_as<decltype(Min), decltype(Max)> &&
             std::same_as<decltype(Min), decltype(Step)>)
struct Ranged {
    static_assert(Min < Max, "Ranged: Min must be less than Max");
    static_assert(Step > decltype(Step){0}, "Ranged: Step must be strictly positive");

    /// @brief The payload; `std::nullopt` means "not entered".
    std::optional<decltype(Min)> value;

    /// @brief Constructs the empty state.
    constexpr Ranged() noexcept = default;

    /// @brief Engages with @p selected.
    /// @param selected The selected value.
    constexpr Ranged(decltype(Min) selected) noexcept : value{selected} {}

    /// @brief Adopts an optional payload as-is.
    /// @param payload Engaged or empty payload.
    constexpr Ranged(std::optional<decltype(Min)> payload) noexcept : value{payload} {}

    /// @brief Whether a value has been entered.
    /// @return `true` if the payload is engaged.
    [[nodiscard]] constexpr bool hasValue() const noexcept { return value.has_value(); }

    /// @brief Unchecked access to the engaged value (UB when empty, exactly
    ///        like `std::optional::operator*`).
    /// @return The engaged value.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] constexpr decltype(Min) operator*() const noexcept { return *value; }

    /// @brief The slider's inclusive lower bound.
    /// @return `Min`.
    [[nodiscard]] static constexpr auto min() noexcept { return Min; }

    /// @brief The slider's inclusive upper bound.
    /// @return `Max`.
    [[nodiscard]] static constexpr auto max() noexcept { return Max; }

    /// @brief The slider's track increment.
    /// @return `Step`.
    [[nodiscard]] static constexpr auto step() noexcept { return Step; }

    /// @brief The preferred control id for this field.
    /// @return `"slider"`.
    [[nodiscard]] static constexpr std::string_view widget() noexcept { return "slider"; }

    /// @brief Equality on the payload; empty equals only empty.
    /// @param other Ranged to compare against.
    /// @return `true` when both are empty or both hold equal values.
    [[nodiscard]] constexpr bool operator==(const Ranged& other) const = default;
};

}  // namespace morph::forms

/// @brief On the wire a Multiline is a plain string — the widget hint lives
///        in the C++ type and the generated schema only.
template <>
struct glz::meta<morph::forms::Multiline> {
    static constexpr auto value = &morph::forms::Multiline::value;
    static constexpr std::string_view name = "Multiline";
};

/// @brief On the wire a Ranged is its nullable underlying value — the slider
///        bounds live in the C++ type and the generated schema only.
/// @tparam Min  Inclusive lower bound.
/// @tparam Max  Inclusive upper bound.
/// @tparam Step Track increment.
template <auto Min, auto Max, auto Step>
struct glz::meta<morph::forms::Ranged<Min, Max, Step>> {
    static constexpr auto value = &morph::forms::Ranged<Min, Max, Step>::value;
    static constexpr std::string_view name = "Ranged";
};
