// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/choice.hpp
/// @brief Fields whose value is picked from a list served by another action.
///
/// A `morph::forms::Choice<T, "ListPayees">` member declares, in the type:
/// *this field is not free input — its options are the result of executing
/// the named action.* Renderers turn it into a combo box: they execute the
/// options action (over the same wire the form actions use), read the result
/// rows, and offer `labelField` as the display text while submitting
/// `valueField` as the payload.
///
/// @code{.cpp}
/// struct SampleInfo { std::int64_t id = 0; std::string name; };
/// struct SampleList { std::vector<SampleInfo> samples; };
/// struct ListSamples {};   // registered like any other action (a pure query)
///
/// struct RecordMeasurement {
///     morph::forms::Choice<std::int64_t, "ListSamples"> sampleId;  // combo box
///     ...
/// };
/// @endcode
///
/// In the generated schema the declaration surfaces as
/// `x-optionsAction` / `x-optionValue` / `x-optionLabel` on the property
/// (added by `morph::forms::schemaJson`), so a client knows *which action to
/// call* and *which result fields to use* without hardcoding anything.
///
/// A `Choice` can also declare a `DependsOn` pack naming sibling fields whose
/// current values parameterise the options action — a cascading picklist
/// (e.g. the list of cities depends on the selected country). The options
/// action then receives `{name: value, ...}` instead of an empty body, and
/// the schema additionally carries `x-optionsDependsOn`. See
/// `optionsDependsOn()` below; a `Choice` with no `DependsOn` is unaffected.
///
/// On the wire a `Choice` is its nullable underlying value (`T`); the
/// options metadata never travels with payloads. Like `Quantity` and
/// `Timestamp`, the blank state ("nothing selected") lives inside, and a
/// non-optional `Choice` member is *required* by the `morph::forms` rules.

#include <array>
#include <cstddef>
#include <glaze/glaze.hpp>
#include <optional>
#include <string_view>
#include <type_traits>

#include "../attributes.hpp"
#include "../detail/fixed_string.hpp"

namespace morph::forms {

/// @brief A structural, NTTP-capable compile-time string (for naming the
///        options action and its value/label result fields in the type).
///
/// An alias for the shared `morph::detail::FixedString` — the same underlying
/// type the units layer's `NamedQuantity` uses — so there is one definition, not
/// two look-alikes. Kept as a public `morph::forms::` name for source
/// compatibility.
/// @tparam N Storage size including the terminating null.
template <std::size_t N>
using FixedString = ::morph::detail::FixedString<N>;

/// @brief An optionally-empty value selected from options served by another
///        action.
///
/// @tparam T             Underlying value type submitted on the wire (e.g.
///                       `std::int64_t` for ids, `std::string` for codes).
/// @tparam OptionsAction Type id of the registered action whose result
///                       provides the options. Executed with an empty body
///                       when `DependsOn` is empty (the common case);
///                       otherwise with a body built from the current
///                       values of the named sibling fields.
/// @tparam ValueField    Field of each result row submitted as the value.
/// @tparam LabelField    Field of each result row shown to the user.
/// @tparam DependsOn     Wire (JSON) field names of sibling fields in the
///                       same action whose current values parameterise the
///                       options action — a cascading picklist. Empty by
///                       default, which makes the options action
///                       independent (today's behavior, unchanged).
template <typename T, FixedString OptionsAction, FixedString ValueField = "id", FixedString LabelField = "name",
          FixedString... DependsOn>
struct Choice {
    /// @brief The payload; `std::nullopt` means "nothing selected".
    std::optional<T> value;

    /// @brief Constructs the empty state.
    constexpr Choice() noexcept = default;

    /// @brief Engages with @p selected.
    /// @param selected The selected value.
    constexpr Choice(T selected) noexcept(std::is_nothrow_move_constructible_v<T>) : value{std::move(selected)} {}

    /// @brief Adopts an optional payload as-is.
    /// @param payload Engaged or empty payload.
    constexpr Choice(std::optional<T> payload) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value{std::move(payload)} {}

    /// @brief Type id of the action that serves the options.
    /// @return The action name declared in the field's type.
    [[nodiscard]] static constexpr std::string_view optionsAction() noexcept { return OptionsAction.view(); }

    /// @brief Result-row field submitted as the value.
    /// @return The field name declared in the field's type.
    [[nodiscard]] static constexpr std::string_view valueField() noexcept { return ValueField.view(); }

    /// @brief Result-row field displayed to the user.
    /// @return The field name declared in the field's type.
    [[nodiscard]] static constexpr std::string_view labelField() noexcept { return LabelField.view(); }

    /// @brief Wire field names of sibling fields whose current values
    ///        parameterise the options action (a cascading picklist).
    /// @return The declared `DependsOn` names, in declaration order; empty
    ///         for an independent `Choice` (the default).
    [[nodiscard]] static constexpr std::array<std::string_view, sizeof...(DependsOn)> optionsDependsOn() noexcept {
        return {DependsOn.view()...};
    }

    /// @brief Whether a value has been selected.
    /// @return `true` if the payload is engaged.
    [[nodiscard]] constexpr bool hasValue() const noexcept { return value.has_value(); }

    /// @brief Unchecked access to the selected value (UB when empty, exactly
    ///        like `std::optional` — the unchecked contract is the point).
    /// @return The selected value — a reference into this `Choice`, valid only
    ///         for as long as it is.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] constexpr const T& operator*() const noexcept MORPH_LIFETIMEBOUND { return *value; }

    /// @brief Equality on the payload; empty equals only empty.
    /// @param other Choice to compare against.
    /// @return `true` when both are empty or both hold equal values.
    [[nodiscard]] constexpr bool operator==(const Choice& other) const noexcept = default;
};

namespace detail {

/// @brief Trait: is `T` some `Choice<...>`?
template <typename T>
struct IsChoice : std::false_type {};

template <typename T, FixedString OptionsAction, FixedString ValueField, FixedString LabelField,
          FixedString... DependsOn>
struct IsChoice<Choice<T, OptionsAction, ValueField, LabelField, DependsOn...>> : std::true_type {};

}  // namespace detail

/// @brief `true` when `T` (cvref-stripped) is a `morph::forms::Choice`.
template <typename T>
inline constexpr bool isChoice = detail::IsChoice<std::remove_cvref_t<T>>::value;

}  // namespace morph::forms

/// @brief On the wire a Choice is its nullable underlying value — the options
///        metadata lives in the C++ type and in generated schemas only.
template <typename T, morph::forms::FixedString OptionsAction, morph::forms::FixedString ValueField,
          morph::forms::FixedString LabelField, morph::forms::FixedString... DependsOn>
struct glz::meta<morph::forms::Choice<T, OptionsAction, ValueField, LabelField, DependsOn...>> {
    static constexpr auto value = &morph::forms::Choice<T, OptionsAction, ValueField, LabelField, DependsOn...>::value;
    static constexpr std::string_view name = "Choice";
};
