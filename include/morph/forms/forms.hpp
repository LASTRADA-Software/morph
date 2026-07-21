// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/forms.hpp
/// @brief JSON-Forms-style schema generation for morph actions.
///
/// Given an action type `A` (a plain aggregate, as registered with
/// `BRIDGE_REGISTER_ACTION`), this header produces a standard JSON Schema a
/// client can render a form from at runtime — the "describe" half of an
/// auto-generated GUI. It builds on glaze's `write_json_schema` (which
/// already contributes types, `$defs`, per-field metadata declared via
/// `glz::json_schema<A>`, and the `ExtUnits` stamped by
/// `morph::units::Quantity`) and closes the two gaps glaze leaves open:
///
/// - **`required`** — glaze's schema writer emits no `required` array at
///   all. `schemaJson<A>()` derives one: a member is *required* unless it is
///   a `std::optional<...>` or its name is listed in the action's opt-out
///   list (see below).
/// - **`x-decimalPlaces`** — for `Quantity` members, the unit's default
///   decimal count from `UnitTraits`, so a client knows the input step
///   without hardcoding unit knowledge.
/// - **`x-order`** — the member's declaration index on every property, so a
///   renderer can lay fields out in declaration order (JSON object key order
///   is not reliable once schemas pass through DOMs/maps).
/// - **`x-unitAlternatives`** — for `Quantity` members whose unit system
///   declares convertible units (`UnitTraits<E>::relations`): an array of
///   `{id, display, decimals, num, den}` entries, where `num/den` is the
///   exact alternative-to-canonical ratio. Renderers offer a unit selector
///   and recalculate entered values exactly on switch; payloads always carry
///   the canonical unit.
/// - **`x-optionsAction` / `x-optionValue` / `x-optionLabel`** — for
///   `morph::forms::Choice` members: which registered action serves the
///   options, and which result-row fields carry the submitted value and the
///   display label. Renderers turn these into combo boxes populated by
///   executing the named action.
///
/// `morph::time::Timestamp` members need no extension keys: their schema
/// carries the standard `"format": "date-time"` annotation.
///
/// @par Declaring optional fields
/// Required is the default. An action opts individual fields out with a
/// static member list:
/// @code{.cpp}
/// struct RecordMeasurement {
///     std::int64_t sampleId = 0;
///     Density density{};
///     Moisture moisture{};   // may stay empty
///
///     static constexpr std::array optionalFields{std::string_view{"moisture"}};
///     [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
/// };
/// @endcode
///
/// @par Readiness helper
/// `allRequiredEngaged(action)` returns `true` when every *required*
/// empty-capable member (`Quantity`, `Choice`, `Timestamp` — anything with a
/// `hasValue()`) is engaged. Wire it up as the action's `validate()`
/// (the existing `ActionValidator` machinery picks that method up
/// automatically) so the same declaration drives the schema's `required`
/// array, the client-side submit gate, and the fielded-action readiness
/// check. Non-quantity members are not checked — a plain `int64_t` cannot
/// express "not filled in"; use a `Quantity` (or a custom `validate()`) when
/// that distinction matters.

#include <cctype>
#include <cstddef>
#include <glaze/glaze.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../util/quantity.hpp"
#include "choice.hpp"

namespace morph::forms {

/// @brief Per-field presentation overrides: label, help, placeholder,
///        read-only, hidden (docs/spec/forms/forms.md, "Field metadata").
///
/// An action opts in with a `static constexpr std::array<FieldMeta, N>`
/// (or, for the `describe<>()` sugar, a `static const` array defined
/// out-of-line — see `describe()`'s documentation) named `fieldMetadata`,
/// mirroring the existing `optionalFields` convention. Every member other
/// than `field` defaults to "not declared": an empty `label`/`help`/
/// `placeholder` means "infer the title, omit the rest"; `readOnly`/`hidden`
/// default to `false`. `mergeSchemaExtras` looks up the entry (if any)
/// matching each reflected member by wire key and patches the property node;
/// an entry naming a field that does not exist on the action is ignored.
struct FieldMeta {
    /// @brief Wire key of the member this entry describes.
    std::string_view field;
    /// @brief Display label; empty infers a title-cased name from `field`.
    std::string_view label{};
    /// @brief Help text; empty omits `description`.
    std::string_view help{};
    /// @brief In-control placeholder hint; empty omits `x-placeholder`.
    std::string_view placeholder{};
    /// @brief Reserved widget-selection override slot. Storage only: its
    ///        semantics and the `x-widget` key it will emit belong to
    ///        `docs/planned/gui_widget_hints.md`; this header never reads it.
    std::string_view widget{};
    /// @brief Displayed but not editable when `true`; emits `x-readonly`.
    bool readOnly{false};
    /// @brief Not shown at all when `true`; emits `x-hidden`. The field still
    ///        travels in the payload (see `docs/spec/forms/forms.md`,
    ///        "Field metadata is not a security control").
    bool hidden{false};

    /// @brief Returns a copy with `placeholder` set to @p text.
    /// @param text The placeholder hint.
    /// @return The updated descriptor.
    [[nodiscard]] constexpr FieldMeta withPlaceholder(std::string_view text) const noexcept {
        FieldMeta copy = *this;
        copy.placeholder = text;
        return copy;
    }

    /// @brief Returns a copy with `readOnly` set to `true`.
    /// @return The updated descriptor.
    [[nodiscard]] constexpr FieldMeta withReadOnly() const noexcept {
        FieldMeta copy = *this;
        copy.readOnly = true;
        return copy;
    }

    /// @brief Returns a copy with `hidden` set to `true`.
    /// @return The updated descriptor.
    [[nodiscard]] constexpr FieldMeta withHidden() const noexcept {
        FieldMeta copy = *this;
        copy.hidden = true;
        return copy;
    }
};

/// @brief Concept: a field type with an internal empty state (`Quantity`,
///        `Choice`, `Timestamp`, or any user type exposing `hasValue()`).
///
/// `hasValue()` must be `noexcept`: `allRequiredEngaged` is `noexcept` and calls
/// it on every member, so a throwing `hasValue()` would cross a `noexcept`
/// boundary and call `std::terminate`. Requiring it here turns that into a
/// compile-time rejection instead — a user field with a throwing `hasValue()`
/// simply does not satisfy the concept.
template <typename T>
concept EmptyCapableField = requires(const T& field) {
    { field.hasValue() } noexcept -> std::convertible_to<bool>;
};

namespace detail {

/// @brief Trait: is `T` a `std::optional<...>` (and therefore never required)?
template <typename T>
struct IsStdOptional : std::false_type {};

template <typename T>
struct IsStdOptional<std::optional<T>> : std::true_type {};

template <typename T>
inline constexpr bool isStdOptional = IsStdOptional<std::remove_cvref_t<T>>::value;

/// @brief Concept: action declares a `static constexpr` iterable
///        `optionalFields` list of field names.
template <typename A>
concept HasOptionalFields = requires {
    std::begin(A::optionalFields);
    std::end(A::optionalFields);
};

/// @brief Returns `true` when @p fieldName appears in `A::optionalFields`.
template <typename A>
[[nodiscard]] constexpr bool declaredOptional(std::string_view fieldName) noexcept {
    if constexpr (HasOptionalFields<A>) {
        for (std::string_view const candidate : A::optionalFields) {
            if (candidate == fieldName) {
                return true;
            }
        }
    } else {
        static_cast<void>(fieldName);
    }
    return false;
}

/// @brief Concept: action declares a `static constexpr`/`static const`
///        iterable `fieldMetadata` list of `FieldMeta` entries.
template <typename A>
concept HasFieldMetadata = requires {
    std::begin(A::fieldMetadata);
    std::end(A::fieldMetadata);
};

/// @brief Returns the `FieldMeta` entry naming @p fieldName in
///        `A::fieldMetadata`, or `nullptr` if @p A declares no such list or
///        no entry names @p fieldName.
template <typename A>
[[nodiscard]] const FieldMeta* findFieldMeta(std::string_view fieldName) noexcept {
    if constexpr (HasFieldMetadata<A>) {
        for (auto const& candidate : A::fieldMetadata) {
            if (candidate.field == fieldName) {
                return &candidate;
            }
        }
    } else {
        static_cast<void>(fieldName);
    }
    return nullptr;
}

/// @brief Splits @p fieldName on camelCase/underscore boundaries and
///        title-cases each word (`dryMassPct` -> `"Dry Mass Pct"`,
///        `sample_id` -> `"Sample Id"`, `notes` -> `"Notes"`).
inline std::string inferTitle(std::string_view fieldName) {
    std::string result;
    bool startOfWord = true;
    for (std::size_t i = 0; i < fieldName.size(); ++i) {
        char const c = fieldName[i];
        if (c == '_') {
            startOfWord = true;
            continue;
        }
        bool const isUpper = std::isupper(static_cast<unsigned char>(c)) != 0;
        bool const prevUpper = i > 0 && std::isupper(static_cast<unsigned char>(fieldName[i - 1])) != 0;
        if (i > 0 && isUpper && !prevUpper) {
            startOfWord = true;
        }
        if (startOfWord && !result.empty()) {
            result += ' ';
        }
        result += startOfWord ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                              : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        startOfWord = false;
    }
    return result;
}

/// @brief Invokes `visitor.operator()<I>(name, member)` for every reflected
///        member of @p action (glaze pure reflection).
template <typename A, typename Visitor>
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward, cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
// — member-tie iteration
constexpr void forEachNamedMember(A&& action, Visitor&& visitor) {
    using Plain = std::remove_cvref_t<A>;
    constexpr auto memberCount = glz::reflect<Plain>::size;
    auto memberTie = glz::to_tie(action);
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — index bounded by
        // reflect::size
        (visitor.template operator()<I>(glz::reflect<Plain>::keys[I], glz::get_member(action, get<I>(memberTie))),
         ...);
    }(std::make_index_sequence<memberCount>{});
}

/// @brief The DOM post-merge behind `schemaJson`: adds the derived `required`
///        array, `x-order`, and `x-decimalPlaces` to a glaze-produced schema.
///
/// Separated from `schemaJson` so the fallback path (malformed input passes
/// through unchanged) is directly testable.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM requires operator[]
template <typename A>
[[nodiscard]] std::string mergeSchemaExtras(std::string rawSchema) {
    // u64 number mode: the schema carries int64/uint64 bounds in $defs, which
    // the default double-only DOM would silently round.
    glz::generic_u64 dom{};
    if (glz::read_json(dom, rawSchema)) {
        return rawSchema;
    }

    glz::generic_u64::array_t requiredNames{};
    A probe{};
    forEachNamedMember(probe, [&]<std::size_t I>(std::string_view name, const auto& member) {
        using Member = std::remove_cvref_t<decltype(member)>;
        static_cast<void>(member);
        const bool isOptional = isStdOptional<Member> || declaredOptional<A>(name);
        if (!isOptional) {
            requiredNames.emplace_back(std::string{name});
        }
        auto& property = dom["properties"][std::string{name}];
        property["x-order"] = std::uint64_t{I};

        // Label/help/placeholder/read-only/hidden: an explicit FieldMeta
        // entry overrides the inferred title and adds the rest; absent, every
        // field still gets an inferred title and nothing else (Field
        // metadata is additive/optional per gui_overview.md's versioning
        // stance — a renderer that ignores these keys shows the raw wire key
        // as the caption, no helper/placeholder text, every field editable
        // and visible, exactly as before this feature).
        const FieldMeta* fieldMeta = findFieldMeta<A>(name);
        std::string_view const declaredLabel = fieldMeta != nullptr ? fieldMeta->label : std::string_view{};
        property["title"] = declaredLabel.empty() ? inferTitle(name) : std::string{declaredLabel};
        if (fieldMeta != nullptr) {
            if (!fieldMeta->help.empty()) {
                property["description"] = std::string{fieldMeta->help};
            }
            if (!fieldMeta->placeholder.empty()) {
                property["x-placeholder"] = std::string{fieldMeta->placeholder};
            }
            if (fieldMeta->readOnly) {
                property["x-readonly"] = true;
            }
            if (fieldMeta->hidden) {
                property["x-hidden"] = true;
            }
        }

        if constexpr (units::isQuantity<Member>) {
            // The field's *declared* precision: the unit default unless the
            // field's type overrides it (Quantity<Unit::m3, 4>).
            property["x-decimalPlaces"] = std::uint64_t{Member::declaredDecimals};

            // Convertible display/entry units with their exact ratios.
            auto const alternatives = Member::unitAlternatives();
            if (!alternatives.empty()) {
                glz::generic_u64::array_t list{};
                for (auto const& alternative : alternatives) {
                    auto const meta =
                        units::UnitTraits<std::remove_const_t<decltype(Member::unit)>>::meta(alternative.unit);
                    glz::generic_u64 entry{};
                    entry["id"] = std::string{meta.id};
                    entry["display"] = std::string{meta.display};
                    entry["decimals"] = std::uint64_t{meta.defaultDecimals};
                    entry["num"] = alternative.num;
                    entry["den"] = alternative.den;
                    list.emplace_back(std::move(entry));
                }
                property["x-unitAlternatives"] = list;
            }
        }
        if constexpr (isChoice<Member>) {
            // Which action serves the options, and which result-row fields
            // carry the submitted value / display label.
            property["x-optionsAction"] = std::string{Member::optionsAction()};
            property["x-optionValue"] = std::string{Member::valueField()};
            property["x-optionLabel"] = std::string{Member::labelField()};
        }
    });
    // Always assign — an explicit empty array beats leaving whatever the
    // schema writer may have emitted (or omitted) for `required`.
    dom["required"] = requiredNames;

    // value_or without a move: the copy is irrelevant (schemaJson memoises),
    // and keeping the fallback branch inside glaze's expected avoids an
    // untestable line here (write_json of a DOM we just built cannot fail).
    return glz::write_json(dom).value_or(rawSchema);
}
// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

}  // namespace detail

/// @brief Retags every `Quantity` member of @p action to its **declared**
///        precision, so a stored value matches the precision the schema
///        advertises via `x-decimalPlaces`.
///
/// A wire payload carries each `Quantity` with its own runtime `dp`, which a
/// client may set to anything. Left alone, the field is stored at the client's
/// `dp`, silently contradicting the schema's `x-decimalPlaces` (which is the
/// field's compile-time *declared* precision, `Quantity<U, Dec>::declaredDecimals`).
/// Calling this on the decode path — right after `ActionTraits<A>::fromJson` and
/// before dispatch — re-tags each `Quantity` to `declaredPrecision()` so the two
/// agree. `atDeclaredPrecision()` only changes the value's precision tag (an
/// exact `Rational` re-rounding to the declared decimals); an empty `Quantity`
/// is left empty. Non-`Quantity` members are untouched.
///
/// This is the enforcement half of the `x-decimalPlaces` contract: the schema
/// advertises the declared precision and the dispatch path now stores at that
/// precision, rather than honouring whatever `dp` the client sent.
/// @tparam A     Action type (a reflectable aggregate).
/// @param action Draft action whose `Quantity` members are retagged in place.
template <typename A>
constexpr void reconcileDeclaredPrecision(A& action) {
    using Plain = std::remove_cvref_t<A>;
    // Only actions glaze can reflect member-by-member (aggregates, or types with
    // a `glz::meta`) can be walked here. Actions with hand-written codecs and no
    // reflectable shape — and there is nothing to retag on them anyway — fall
    // through as a no-op so this stays safe to call for *every* registered
    // action from the dispatch path, not only form actions.
    if constexpr (glz::reflectable<Plain> || glz::glaze_object_t<Plain>) {
        constexpr auto memberCount = glz::reflect<Plain>::size;
        auto memberTie = glz::to_tie(action);
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            [[maybe_unused]] auto retag = [&]<std::size_t Idx>() {
                auto& member = glz::get_member(action, get<Idx>(memberTie));
                using Member = std::remove_cvref_t<decltype(member)>;
                if constexpr (units::isQuantity<Member>) {
                    member = member.atDeclaredPrecision();
                }
            };
            (retag.template operator()<I>(), ...);
        }(std::make_index_sequence<memberCount>{});
    } else {
        static_cast<void>(action);
    }
}

/// @brief Whether every required empty-capable member of @p action is
///        engaged (has a value).
///
/// Empty-capable covers `Quantity`, `Choice`, `Timestamp`, and any user type
/// satisfying `EmptyCapableField`. Required means: not a `std::optional<...>`
/// member and not listed in `A::optionalFields`. Intended as the body of the
/// action's `validate()`.
/// @tparam A     Action type (a reflectable aggregate).
/// @param action Draft whose fields are checked.
/// @return `true` when no required empty-capable field is empty.
template <typename A>
[[nodiscard]] constexpr bool allRequiredEngaged(const A& action) noexcept {
    bool allEngaged = true;
    detail::forEachNamedMember(action, [&]<std::size_t I>(std::string_view name, const auto& member) {
        using Member = std::remove_cvref_t<decltype(member)>;
        if constexpr (EmptyCapableField<Member>) {
            if (!detail::declaredOptional<A>(name) && !member.hasValue()) {
                allEngaged = false;
            }
        } else {
            static_cast<void>(name);
            static_cast<void>(member);
        }
    });
    return allEngaged;
}

/// @brief Generates the JSON Schema for action type @p A, ready for a
///        client-side form renderer.
///
/// glaze's `write_json_schema<A>()` output, post-processed with:
///   - a top-level `required` array (see file docs for the rule),
///   - `x-decimalPlaces` on every `Quantity` property (the unit's default),
///   - `x-order` (declaration index) on every property.
///
/// The result is fixed per type, so it is computed once and cached. On any
/// internal failure the unmerged glaze schema (or an empty string if even
/// that failed) is returned rather than throwing — schema generation is a
/// description facility, never worth crashing a server over.
/// @tparam A Action type (a reflectable aggregate).
/// @return The merged schema JSON.
template <typename A>
[[nodiscard]] std::string schemaJson() {
    static const std::string cached =
        detail::mergeSchemaExtras<A>(glz::write_json_schema<A>().value_or(std::string{}));
    return cached;
}

}  // namespace morph::forms
