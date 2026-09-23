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
/// `morph::units::Quantity`) and closes the gaps glaze leaves open. The list
/// below is the main surface, not an exhaustive enumeration of every key
/// emitted:
///
/// - **`required`** — under the options `schemaJson<A>()` uses, glaze derives no
///   `required` entries from member types: `glz::requires_key` only returns
///   `true` for a member when `meta<T>::requires_key` says so or when
///   `Opts.error_on_missing_keys` is set, and morph sets neither. (A type
///   declaring `meta<V>::required`, and a tagged variant's discriminator, do
///   still get one.)
///   `schemaJson<A>()` always writes its own, overwriting whatever the schema
///   writer did or did not produce: a member is *required* unless it is a
///   `std::optional<...>` or its name is listed in the action's opt-out list
///   (see below).
/// - **`x-decimalPlaces`** — for `Quantity` members, the field's *declared*
///   precision (`Quantity<U, Dec>::declaredDecimals`, which defaults to the
///   unit's `UnitTraits` default but is overridable per member), so a client
///   knows the input step without hardcoding unit knowledge.
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
/// - **`x-optionsDependsOn`** — for a `Choice` whose options are
///   parameterised by sibling field values (`Choice`'s `DependsOn` pack): the
///   wire names of those sibling fields. Renderers send `{name: value, ...}`
///   as the options-action request body instead of an empty one, and
///   re-fetch when a named field changes. Omitted when the `Choice` declares
///   no dependency.
/// - **`x-layout` / `x-group` / `x-section` / `x-colspan`** — for actions
///   declaring a `static constexpr` `formLayout` and/or `fieldSpans`
///   (`morph::forms::FieldGroup` / `FieldSpan`, `forms/layout.hpp`): visual
///   structure (sections, tabs, an accordion) over the flat field list, and
///   per-field grid column spans. Absent either declaration, none of these
///   keys are emitted and a renderer lays fields out exactly as it does
///   today.
/// - **`x-widget`** — for a field whose type declares a `noexcept static
///   constexpr widget()` (`Multiline`, `Ranged` — widget_hints.hpp), or any
///   field named in a `fieldMetadata`-shaped override: the renderer's
///   preferred control id (`"textarea"`, `"slider"`, `"radio"`, …).
/// - **`x-min` / `x-max` / `x-step`** — for a field whose type additionally
///   declares `min()`/`max()`/`step()` (the `Ranged` shape): the slider's
///   control-track bounds and increment — advisory, not a validation bound.
/// - **`x-rules`** — for an action declaring a `static constexpr formRules`
///   (`morph::forms::ruleList(...)`): a closed, typed cross-field rule
///   vocabulary (`requiredWhen`, comparisons, membership, presentation, and
///   the compound `andOf`/`orOf`/`notOf` conditions that nest a condition
///   tree to any depth) evaluated identically by the schema, the client, and
///   the server. See `morph::forms::allRulesSatisfied` below and
///   docs/spec/forms/forms.md.
/// - **`minimum` / `maximum` / `multipleOf`** — for a field whose
///   `fieldMetadata` entry declares them (`morph::forms::FieldMeta`): a
///   per-field scalar bound, stamped on the property node as the standard
///   JSON-Schema keys of those names. `multipleOf: 1` is how "whole number"
///   is spelled. `morph::forms::allFieldBoundsSatisfied` evaluates the same
///   declaration in C++, so an action's `validate()` enforces exactly what
///   the client was served. Bounds are per *field*, unlike
///   `UnitTraits::bounds`, which is per unit.
/// - **`x-computed` / `x-readonly`** — for a member listed as the destination
///   of an action's `computedFields` declaration: the field is derived from
///   sibling inputs (named in `x-computed.inputs`) and must not be rendered as
///   an editable control (`x-readonly: true`). See `morph::forms::computed`,
///   `morph::forms::computeList`, and `morph::forms::recomputeAll`.
///
/// `morph::time::Timestamp` members need no extension keys: their schema
/// carries the standard `"format": "date-time"` annotation.
///
/// **Nested aggregates (recursive, cycle-safe).** A member whose type is
/// itself a reflectable aggregate — a plain nested struct, or
/// `std::vector<Sub>` — gets its own members annotated too: `x-order`,
/// title/`FieldMeta`, `required`, and the `Quantity`/`Choice`/widget/
/// ranged-bounds rules above, applied against the nested type's own
/// reflection. Unlike the top level, this recurses into the type graph, to
/// whatever depth it has: there is no depth limit, and a self- or
/// mutually-referential type is described rather than rejected — glaze emits
/// a finite `$ref`-cyclic schema for it, and the runtime `$defs` visited set
/// walks that once. Computed fields/`formLayout`/`fieldSpans`/`formRules`
/// remain top-level-only regardless of depth. See docs/spec/forms/forms.md,
/// "Nested aggregates (recursive, cycle-safe)", and
/// `detail::annotateNestedAggregateRef`.
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
/// @par Declaring computed fields
/// A derived, read-only field is declared with a `static constexpr` map from a
/// destination member to its declared input members and a pure derivation:
/// @code{.cpp}
/// struct LineItem {
///     Quantity<Units, 2> qty;
///     Quantity<Units, 2> price;
///     Quantity<Units, 2> total;  // computed -- not user-entered
///
///     // A generic (auto) lambda parameter: this initializer runs while
///     // LineItem is still an incomplete type, so the body's member access
///     // must stay dependent until first use, after the class is complete.
///     static constexpr auto computedFields = morph::forms::computeList(
///         morph::forms::computed<&LineItem::total, &LineItem::qty, &LineItem::price>(
///             [](const auto& s) { return s.qty * s.price; }));
/// };
/// @endcode
/// `schemaJson<A>()` then emits `x-computed`/`x-readonly` on `total` and
/// excludes it from `required`; `recomputeAll<A>(action)` is the single
/// evaluator the reactive `set<>` path and every dispatch path call to
/// overwrite it authoritatively -- see `bridge.md`/`registry.md`.
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

#include <algorithm>
#include <array>
#include <cctype>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <limits>
#include <memory>
#include <morph/detail/fixed_string.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../util/quantity.hpp"
#include "../util/rational.hpp"
#include "choice.hpp"
#include "layout.hpp"

namespace morph::forms {

/// @brief Per-field presentation overrides and scalar bounds: label, help,
///        placeholder, read-only, hidden, `minimum`/`maximum`/`multipleOf`
///        (docs/spec/forms/forms.md, "Field metadata").
///
/// An action opts in with a `static constexpr std::array<FieldMeta, N>`
/// (or, for the `describe<>()` sugar, a `static const` array defined
/// out-of-line — see `describe()`'s documentation) named `fieldMetadata`,
/// mirroring the existing `optionalFields` convention. Every member other
/// than `field` defaults to "not declared": an empty `label`/`help`/
/// `placeholder` means "infer the title, omit the rest"; `readOnly`/`hidden`
/// default to `false`; a disengaged `minimum`/`maximum`/`multipleOf` emits
/// nothing and checks nothing. `mergeSchemaExtras` looks up the entry (if any)
/// matching each reflected member by wire key and patches the property node;
/// an entry naming a field that does not exist on the action is ignored.
///
/// @par Bounds are per **field**, and are not presentation
/// The three numeric members are the one part of this type that is *not*
/// presentation: `morph::forms::allFieldBoundsSatisfied` evaluates them as a
/// C++ predicate, so an action whose `validate()` calls it enforces the same
/// declaration the schema advertises — the "the DTO *is* the form definition"
/// rule (`examples/IMPLEMENTATION.md` rule 3) applied to a scalar bound, which
/// the `formRules` vocabulary cannot express because every comparison node
/// there takes two member pointers, never a literal. They are
/// keyed by field rather than by unit precisely so a floor declared for one
/// `Quantity` member does not constrain a sibling of the same type —
/// `UnitTraits::bounds` is per-unit and cannot make that distinction.
struct FieldMeta {
    /// @brief Wire key of the member this entry describes.
    std::string_view field;
    /// @brief Display label; empty infers a title-cased name from `field`.
    std::string_view label{};
    /// @brief Help text; empty omits `description`.
    std::string_view help{};
    /// @brief In-control placeholder hint; empty omits `x-placeholder`.
    std::string_view placeholder{};
    /// @brief Explicit message-key **stem** override for this field's i18n
    ///        catalog keys; empty means "derive the stem from the action's
    ///        `ActionTraits<A>::typeId()` and this field's wire key" (see
    ///        `morph::forms::i18n::fieldKey`, `forms/i18n.hpp`). A non-empty
    ///        stem replaces only the `<actionTypeId>.<wireField>` portion of
    ///        the key — the per-slot `.label` / `.help` / `.placeholder`
    ///        suffix is still appended on top of it (see
    ///        `morph::forms::i18n::explicitFieldKey`), so `"myKey"` expands
    ///        to `"myKey.label"` / `"myKey.help"` / `"myKey.placeholder"`,
    ///        never to a single complete key on its own. Emitted as
    ///        `x-i18nKey` only when non-empty.
    std::string_view i18nKey{};
    /// @brief Widget-selection override; empty means "use the field type's
    ///        own `widget()`, if any". A non-empty value replaces that
    ///        type-derived default and is emitted as `x-widget`
    ///        (docs/spec/forms/widget_hints.md). Read structurally by
    ///        `detail::widgetOverride`, not through this type by name — see
    ///        `detail::HasFieldMetadataWidgets`.
    std::string_view widget{};
    /// @brief Displayed but not editable when `true`; emits `x-readonly`.
    bool readOnly{false};
    /// @brief Not shown at all when `true`; emits `x-hidden`. The field still
    ///        travels in the payload (see `docs/spec/forms/forms.md`,
    ///        "Field metadata is not a security control").
    bool hidden{false};

    /// @brief Inclusive lower bound on the field's numeric value; emitted as
    ///        the standard JSON-Schema `minimum`. Disengaged means "no floor".
    ///
    /// For a `Quantity` member this bounds the **scalar value the field
    /// denotes**, in the canonical unit — not the `{num,den,dp}` object the
    /// member serialises as.
    std::optional<::morph::math::Rational> minimum{};

    /// @brief Inclusive upper bound on the field's numeric value; emitted as
    ///        the standard JSON-Schema `maximum`. Disengaged means "no
    ///        ceiling". Same canonical-unit reading as `minimum`.
    std::optional<::morph::math::Rational> maximum{};

    /// @brief The field's value must be an exact integer multiple of this;
    ///        emitted as the standard JSON-Schema `multipleOf`. Disengaged
    ///        means "any value".
    ///
    /// `multipleOf = 1` is how integrality is spelled — the constraint
    /// `Quantity` cannot carry in its type, since `Quantity<U, Dec>` requires
    /// `Dec >= 1` and therefore always represents tenths exactly.
    ///
    /// A non-positive value is **ignored** (neither emitted nor checked),
    /// matching JSON Schema, which requires `multipleOf` to be strictly
    /// positive: a zero divisor has no meaning and a negative one divides
    /// exactly the same set of values as its magnitude.
    std::optional<::morph::math::Rational> multipleOf{};

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

    /// @brief Returns a copy with `minimum` set to @p bound.
    /// @param bound Inclusive lower bound, in the field's canonical unit.
    /// @return The updated descriptor.
    [[nodiscard]] constexpr FieldMeta withMinimum(::morph::math::Rational bound) const noexcept {
        FieldMeta copy = *this;
        copy.minimum = bound;
        return copy;
    }

    /// @brief Returns a copy with `maximum` set to @p bound.
    /// @param bound Inclusive upper bound, in the field's canonical unit.
    /// @return The updated descriptor.
    [[nodiscard]] constexpr FieldMeta withMaximum(::morph::math::Rational bound) const noexcept {
        FieldMeta copy = *this;
        copy.maximum = bound;
        return copy;
    }

    /// @brief Returns a copy with `multipleOf` set to @p step.
    /// @param step Divisor the value must be an exact integer multiple of;
    ///             non-positive values are ignored (see the member).
    /// @return The updated descriptor.
    [[nodiscard]] constexpr FieldMeta withMultipleOf(::morph::math::Rational step) const noexcept {
        FieldMeta copy = *this;
        copy.multipleOf = step;
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
///
/// Constrained to `FieldMeta` elements specifically (not just "some iterable
/// named `fieldMetadata`"): `findFieldMeta` below hands back a `const
/// FieldMeta*`, so an action whose `fieldMetadata` holds a different element
/// shape must not satisfy this concept — that shape may still be a valid
/// *widget*-override source for `detail::HasFieldMetadataWidgets` /
/// `detail::widgetOverride` (structural, `.field`/`.widget` only), which is
/// deliberately independent of this concept.
template <typename A>
concept HasFieldMetadata = requires {
    { *std::begin(A::fieldMetadata) } -> std::convertible_to<const FieldMeta&>;
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

/// @brief Concept: a field type that declares its own preferred control id via
///        a `noexcept` `static constexpr widget()` — the shape `Multiline` and
///        `Ranged` (forms/widget_hints.hpp) expose; any user type may opt in
///        the same way.
template <typename T>
concept DeclaresWidget = requires {
    { std::remove_cvref_t<T>::widget() } noexcept -> std::convertible_to<std::string_view>;
};

/// @brief Concept: a field type that declares slider bounds via `noexcept`
///        `static constexpr min()` / `max()` / `step()` — the `Ranged` shape.
template <typename T>
concept DeclaresRangedBounds = requires {
    { std::remove_cvref_t<T>::min() } noexcept;
    { std::remove_cvref_t<T>::max() } noexcept;
    { std::remove_cvref_t<T>::step() } noexcept;
};

/// @brief Concept: `A` declares a `static constexpr` iterable `fieldMetadata`
///        (structural check only — the element type is not named here).
template <typename A>
concept HasFieldMetadataEntries = requires {
    std::begin(A::fieldMetadata);
    std::end(A::fieldMetadata);
};

/// @brief Concept: `A::fieldMetadata` entries additionally expose `.field` and
///        `.widget`, both convertible to `std::string_view` — the shape
///        `FieldMeta` (above) has. Checked structurally (duck-typed) so this
///        header never has to include or name that type by name in this
///        lookup: any descriptor array with the two members is honoured as a
///        widget-override source, regardless of which header declares it.
template <typename A>
concept HasFieldMetadataWidgets =
    HasFieldMetadataEntries<A> && requires(std::remove_cvref_t<decltype(*std::begin(A::fieldMetadata))> entry) {
        { entry.field } -> std::convertible_to<std::string_view>;
        { entry.widget } -> std::convertible_to<std::string_view>;
    };

/// @brief Returns the non-empty `widget` of the `A::fieldMetadata` entry whose
///        `field` equals @p fieldName, or an empty view when `A` declares no
///        `fieldMetadata` (or none of its entries name @p fieldName).
template <typename A>
[[nodiscard]] constexpr std::string_view widgetOverride(std::string_view fieldName) noexcept {
    if constexpr (HasFieldMetadataWidgets<A>) {
        for (auto const& entry : A::fieldMetadata) {
            if (std::string_view{entry.field} == fieldName) {
                return std::string_view{entry.widget};
            }
        }
    } else {
        static_cast<void>(fieldName);
    }
    return {};
}

/// @brief Invokes `visitor.operator()<I>(name, member)` for every reflected
///        member of @p action (glaze pure reflection).
template <typename A, typename Visitor>
// Neither forwarding reference is forwarded, and neither may be. `action` is
// bound by `glz::to_tie` into a tuple of references that outlives this line and
// is read member-by-member below; moving from it would leave the tie pointing
// at a moved-from object. `visitor` is invoked once per reflected member by the
// fold expression, so forwarding it would move from it on the first member and
// call a moved-from callable for every one after. Both are `&&` to preserve the
// argument's cv-qualification through the tie — a `const A&` must tie to const
// members — not to enable a move. The directive stays on one physical line
// deliberately; see the note at detail/fixed_string.hpp:48.
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
constexpr void forEachNamedMember(A&& action, Visitor&& visitor) {
    using Plain = std::remove_cvref_t<A>;
    constexpr auto memberCount = glz::reflect<Plain>::size;
    auto memberTie = glz::to_tie(action);
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        // `I` is a pack of `std::index_sequence<memberCount>`, i.e. every value in
        // [0, glz::reflect<Plain>::size), and `keys` is an array of exactly that
        // size — the index cannot be out of range by construction. The directive
        // stays on one physical line; see the note at detail/fixed_string.hpp:48.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        (visitor.template operator()<I>(glz::reflect<Plain>::keys[I], glz::get_member(action, get<I>(memberTie))),
         ...);
    }(std::make_index_sequence<memberCount>{});
}

/// @brief Extracts the containing class type from a pointer-to-member type.
template <typename T>
struct MemberPointerClass;

/// @brief Partial specialisation matching `Member Class::*`.
template <typename C, typename M>
struct MemberPointerClass<M C::*> {
    /// @brief The pointer-to-member's containing class.
    using type = C;
};

/// @brief Convenience alias for `MemberPointerClass<T>::type`.
template <typename T>
using MemberPointerClassT = typename MemberPointerClass<T>::type;

/// @brief Resolves the wire key name of @p MemberPtr by comparing member
///        addresses on a default-constructed probe of its containing type.
///
/// Deliberately **not** `constexpr`/`consteval`: glaze's `get_member` for
/// reflectable (pure-reflection) aggregates is not itself constexpr, so this
/// can only run at ordinary runtime — which is why `describe<>()` is a plain
/// function and why a `fieldMetadata` array built from it must be defined
/// out-of-line (see `describe()`'s documentation for why and how).
template <auto MemberPtr>
[[nodiscard]] std::string_view memberWireName() noexcept {
    using A = MemberPointerClassT<decltype(MemberPtr)>;
    A probe{};
    std::string_view found{};
    forEachNamedMember(probe, [&]<std::size_t I>(std::string_view name, auto& member) {
        using MemberT = std::remove_reference_t<decltype(member)>;
        using TargetT = std::remove_reference_t<decltype(probe.*MemberPtr)>;
        if constexpr (std::is_same_v<MemberT, TargetT>) {
            if (std::addressof(member) == std::addressof(probe.*MemberPtr)) {
                found = name;
            }
        }
    });
    return found;
}

/// @brief Deduces the action type a condition/rule node's `test(const A&)
/// const` ranges over, from its member-function-pointer type. Every
/// condition/rule node in this header (`Engaged`, `Equals`, `Greater`, …, and
/// the compound `And`/`Or`/`Not` nodes) exposes exactly this shape, so
/// `andOf`/`orOf`/`notOf` use it to recover `A` without requiring every leaf
/// node to name it as a separate member type.
/// @tparam TestMemberPtr Pointer-to-member-function type of `&Cond::test`.
template <typename TestMemberPtr>
struct ConditionActionTypeFromTest;

/// @brief Specialisation matching `bool (Cond::*)(const A&) const noexcept`.
/// @tparam Cond The condition/rule node type.
/// @tparam A    The deduced action type.
template <typename Cond, typename A>
struct ConditionActionTypeFromTest<bool (Cond::*)(const A&) const noexcept> {
    /// @brief The deduced action type.
    using type = A;
};

/// @brief The action type @p Cond's `test()` ranges over.
/// @tparam Cond A condition/rule node type (must expose `test(const A&) const noexcept`).
template <typename Cond>
using ConditionActionType = typename ConditionActionTypeFromTest<decltype(&Cond::test)>::type;

/// @brief The closed set of cross-field rule and condition kinds `x-rules`
/// carries in its `kind` field. One flat enum serves both top-level rules
/// (`RequiredWhen`, `Greater`, `ExactlyOneOf`, `VisibleWhen`, ...) and the
/// condition nodes nested inside a rule's `when` clause (`Engaged`,
/// `NotEngaged`, `Equals`, and the comparison kinds reused as booleans) —
/// see docs/spec/forms/forms.md's `x-rules` renderer-contract table.
enum class RuleKind : std::uint8_t {
    Engaged,
    NotEngaged,
    Equals,
    Greater,
    GreaterOrEqual,
    Less,
    LessOrEqual,
    RequiredWhen,
    ExactlyOneOf,
    AtLeastOneOf,
    MutuallyExclusive,
    VisibleWhen,
    ReadonlyWhen,
    And,
    Or,
    Not,
};

/// @brief The wire `"kind"` string for @p kind, exactly as documented in
/// forms.md's `x-rules` table.
[[nodiscard]] constexpr std::string_view ruleKindName(RuleKind kind) noexcept {
    switch (kind) {
        case RuleKind::Engaged:
            return "engaged";
        case RuleKind::NotEngaged:
            return "notEngaged";
        case RuleKind::Equals:
            return "equals";
        case RuleKind::Greater:
            return "greater";
        case RuleKind::GreaterOrEqual:
            return "greaterOrEqual";
        case RuleKind::Less:
            return "less";
        case RuleKind::LessOrEqual:
            return "lessOrEqual";
        case RuleKind::RequiredWhen:
            return "requiredWhen";
        case RuleKind::ExactlyOneOf:
            return "exactlyOneOf";
        case RuleKind::AtLeastOneOf:
            return "atLeastOneOf";
        case RuleKind::MutuallyExclusive:
            return "mutuallyExclusive";
        case RuleKind::VisibleWhen:
            return "visibleWhen";
        case RuleKind::ReadonlyWhen:
            return "readonlyWhen";
        case RuleKind::And:
            return "and";
        case RuleKind::Or:
            return "or";
        case RuleKind::Not:
            return "not";
        default:
            // Unreachable through any real code path: every rule/condition
            // node's `kind` member is a `static constexpr detail::RuleKind`
            // initialised from one of the enumerators above, and the switch
            // handles all of them explicitly. This arm only exists to satisfy
            // the compiler that the function returns on every enum value,
            // including one manufactured by an out-of-range `static_cast`.
            return "";
    }
}

/// @brief Whether @p value counts as "engaged" for rule purposes:
/// `hasValue()` for an `EmptyCapableField` (`Quantity`/`Choice`/`Timestamp`),
/// otherwise `has_value()` for a plain `std::optional<T>` — the rule
/// vocabulary treats both as "a field with an empty state", unlike
/// `allRequiredEngaged` (which only inspects `EmptyCapableField`; a plain
/// `std::optional<T>` exposes `has_value()`, not `hasValue()`, so it never
/// satisfies that concept — see forms.md's "two exclusions" note). Only ever
/// called on a type satisfying `EngageableField` (defined below), enforced
/// by every public factory that calls it.
template <typename T>
[[nodiscard]] constexpr bool isEngaged(const T& value) noexcept {
    if constexpr (::morph::forms::EmptyCapableField<T>) {
        return value.hasValue();
    } else {
        return value.has_value();
    }
}

/// @brief Resolves the wire (JSON) field name of @p field on `A`, the same
/// way `x-order` is derived (`mergeSchemaExtras`): a fresh probe instance is
/// walked with `forEachNamedMember`, matching by member address. `A` must be
/// default-constructible (already required by `schemaJson<A>()`). Returns an
/// empty string if @p field does not name a reflected member of `A` (should
/// not happen for a pointer-to-member of `A` itself; defensive only).
template <typename V, typename A>
[[nodiscard]] inline std::string resolveFieldName(V A::* field) {
    std::string found;
    A probe{};
    forEachNamedMember(probe, [&]<std::size_t I>(std::string_view name, const auto& member) {
        static_cast<void>(I);
        using Member = std::remove_cvref_t<decltype(member)>;
        if constexpr (std::is_same_v<Member, V>) {
            if (static_cast<const void*>(&member) == static_cast<const void*>(&(probe.*field))) {
                found = std::string{name};
            }
        }
    });
    return found;
}

/// @brief Constraint for the comparison rule/condition kinds (`greater`,
/// `greaterOrEqual`, `less`, `lessOrEqual`): an
/// `EmptyCapableField` whose engaged value (`operator*()`) is three-way
/// comparable to itself — satisfied by `Quantity` (dereferences to
/// `math::Rational`) and `morph::time::Timestamp` (dereferences to
/// `DateTime`), matching forms.md's "numeric / Timestamp" scope for
/// comparisons.
template <typename V>
concept ComparableField = ::morph::forms::EmptyCapableField<V> && requires(const V& value) {
    { *value <=> *value };
};

/// @brief The shared compile-time string used to capture an `equals` literal.
///
/// Aliased, not redefined: `morph::detail::FixedString` is the project's single
/// canonical NTTP-capable fixed string (the forms `Choice` layer and the units
/// layer already alias it too).
///
/// `equals(&A::code, "URGENT")` used to store its argument as a `std::string`,
/// which quietly bounded the documented
/// `static constexpr auto formRules = ruleList(...)` form to whatever fits the
/// standard library's small-string buffer — 15 characters on libstdc++. One
/// character more and the string allocates, so the rule node is no longer a
/// constant expression and the declaration fails with "refers to a result of
/// `operator new`". The limit is invisible in the source: the same code
/// compiles or does not depending only on how long the literal is, and on which
/// standard library is in use.
///
/// Holding the characters inline removes the allocation, so a literal of any
/// length works. Passing an explicit `std::string` still stores a `std::string`
/// (see `RuleLiteral`) and still cannot be `constexpr` when it allocates — that
/// is inherent to the type the caller chose, not something this can fix.
///
/// @tparam N Literal length including its trailing NUL.
template <std::size_t N>
using LiteralString = ::morph::detail::FixedString<N>;

/// @brief Trait: is @p T a `LiteralString`? `false` for every other type.
/// @tparam T Type to test.
template <typename T>
inline constexpr bool isLiteralString = false;

/// @brief `isLiteralString` specialization recognising `LiteralString<N>`,
///        where `N` is the recognised literal's length.
template <std::size_t N>
inline constexpr bool isLiteralString<LiteralString<N>> = true;

}  // namespace detail

/// @brief Broader than `EmptyCapableField`: also covers a plain
/// `std::optional<T>` member (e.g. `std::optional<std::string> email`),
/// which does **not** satisfy `EmptyCapableField` — it exposes
/// `has_value()`, not `hasValue()` (see forms.md's `allRequiredEngaged`
/// "two exclusions" note). The cross-field rule vocabulary's engagement
/// checks (`engaged`, `notEngaged`, `requiredWhen`, and the membership rules
/// `exactlyOneOf`/`atLeastOneOf`/`mutuallyExclusive`) accept either kind of
/// field, since docs/spec/forms/forms.md's worked example ranges an
/// `exactlyOneOf` over two plain
/// `std::optional<std::string>` fields.
template <typename T>
concept EngageableField = EmptyCapableField<T> || detail::isStdOptional<T>;

/// @brief Concept: @p Cond may be used as a **nested condition** — inside an
/// `andOf`/`orOf`/`notOf` tree, or as the `when` clause of a `requiredWhen` /
/// `visibleWhen` / `readonlyWhen` rule.
///
/// Every rule and condition node in this header exposes the same shape (`kind`,
/// `test(const A&) const noexcept`, `emitNode()`), which is what makes them
/// substitutable — and what made "has a `test()`" a useless admission test.
/// `VisibleWhen::test()` and `ReadonlyWhen::test()` return `true`
/// *unconditionally, by design*: they are presentation rules and never gate
/// submission, so a renderer reads their `when` clause rather than calling
/// them. Nested as a condition, such a node therefore contributes a constant
/// and can never influence the tree it sits in — `andOf(visibleWhen(…), c)`
/// silently collapses to `c`, having compiled and looking like it says
/// something. `RequiredWhen` is excluded for the neighbouring reason: it is a
/// rule *about* a condition, not a condition, and nesting one emits a
/// `"requiredWhen"` node in a `when` position that no renderer's condition
/// vocabulary has a case for.
///
/// So the marker is declared explicitly by the nodes whose `test()` is a
/// function of the action, rather than inferred from `isPresentation` — which
/// would admit `RequiredWhen` — or from the presence of `test()`, which admits
/// everything. Adding a node to the condition vocabulary is one line on the
/// node itself.
///
/// The membership rules (`exactlyOneOf` / `atLeastOneOf` / `mutuallyExclusive`)
/// *are* conditions by this rule: their `test()` really does range over the
/// action. A renderer that does not recognise them nested treats them as
/// "cannot evaluate" and defers to the server, which is the sanctioned
/// fallback (forms.md, "Renderer fallback") rather than a disagreement.
///
/// (`Cond` is the candidate node type. It is deliberately not documented with
/// a parameter-doc command: -Wdocumentation does not consider a concept a
/// template declaration, so such a command here is an error under
/// -Weverything -Werror, which is how the WASM ladder builds. `HasFormRules`
/// and `HasExplicitSubmit` below omit it for the same reason.)
template <typename Cond>
concept Condition = requires {
    { Cond::isCondition } -> std::convertible_to<bool>;
} && Cond::isCondition;

/// @brief Condition: `field` is engaged (has a value). One of the closed
/// condition kinds a `requiredWhen` / `visibleWhen` / `readonlyWhen` rule's
/// `when` clause accepts.
/// @tparam V Field member type (must satisfy `EngageableField`).
/// @tparam A Action type the field belongs to.
template <typename V, typename A>
struct Engaged {
    /// @brief Pointer to the member this condition inspects.
    V A::* field;
    /// @brief The wire `"kind"` this condition emits: `"engaged"`.
    static constexpr detail::RuleKind kind = detail::RuleKind::Engaged;
    /// @brief Usable as a nested condition: this node's `test()` is a
    /// function of the action, so composing it into an `and`/`or`/`not`
    /// tree changes what that tree evaluates to.
    static constexpr bool isCondition = true;

    /// @brief Evaluates the condition against @p action.
    /// @param action The action snapshot to inspect.
    /// @return `true` when the field is engaged.
    [[nodiscard]] constexpr bool test(const A& action) const noexcept { return detail::isEngaged(action.*field); }

    /// @brief Emits this condition's `x-rules` JSON node.
    /// @return `{"kind":"engaged","fields":["<wire name>"]}`.
    [[nodiscard]] glz::generic_u64 emitNode() const {
        glz::generic_u64 node{};
        node["kind"] = std::string{detail::ruleKindName(kind)};
        glz::generic_u64::array_t fields{};
        fields.emplace_back(detail::resolveFieldName(field));
        node["fields"] = fields;
        return node;
    }
};

/// @brief Builds an `Engaged<V, A>` condition testing whether @p field is
/// engaged.
/// @tparam V Field member type (deduced; must satisfy `EngageableField`).
/// @tparam A Action type (deduced).
/// @param field Pointer to the member to test.
/// @return The condition node.
template <typename V, typename A>
    requires EngageableField<V>
[[nodiscard]] constexpr auto engaged(V A::* field) {
    return Engaged<V, A>{field};
}

/// @brief Condition: `field` is **not** engaged. The complement of
/// `engaged`.
/// @tparam V Field member type (must satisfy `EngageableField`).
/// @tparam A Action type the field belongs to.
template <typename V, typename A>
struct NotEngaged {
    /// @brief Pointer to the member this condition inspects.
    V A::* field;
    /// @brief The wire `"kind"` this condition emits: `"notEngaged"`.
    static constexpr detail::RuleKind kind = detail::RuleKind::NotEngaged;
    /// @brief Usable as a nested condition: this node's `test()` is a
    /// function of the action, so composing it into an `and`/`or`/`not`
    /// tree changes what that tree evaluates to.
    static constexpr bool isCondition = true;

    /// @brief Evaluates the condition against @p action.
    /// @param action The action snapshot to inspect.
    /// @return `true` when the field is **not** engaged.
    [[nodiscard]] constexpr bool test(const A& action) const noexcept { return !detail::isEngaged(action.*field); }

    /// @brief Emits this condition's `x-rules` JSON node.
    /// @return `{"kind":"notEngaged","fields":["<wire name>"]}`.
    [[nodiscard]] glz::generic_u64 emitNode() const {
        glz::generic_u64 node{};
        node["kind"] = std::string{detail::ruleKindName(kind)};
        glz::generic_u64::array_t fields{};
        fields.emplace_back(detail::resolveFieldName(field));
        node["fields"] = fields;
        return node;
    }
};

/// @brief Builds a `NotEngaged<V, A>` condition testing whether @p field is
/// **not** engaged.
/// @tparam V Field member type (deduced; must satisfy `EngageableField`).
/// @tparam A Action type (deduced).
/// @param field Pointer to the member to test.
/// @return The condition node.
template <typename V, typename A>
    requires EngageableField<V>
[[nodiscard]] constexpr auto notEngaged(V A::* field) {
    return NotEngaged<V, A>{field};
}

/// @brief Rule/condition: `*lhs > *rhs` when both operands are engaged;
/// vacuously satisfied when either is unengaged (a form still being filled
/// in must not fail this comparison prematurely — see forms.md). Compares
/// the operands' **engaged values** (`operator*()`) directly — e.g. the
/// underlying `math::Rational` for `Quantity`, or `DateTime` for
/// `Timestamp` — never the field type's own (possibly throwing, for
/// `Quantity`) `operator<=>`.
/// @tparam V Field member type shared by both operands (must satisfy
///           `detail::ComparableField`).
/// @tparam A Action type both fields belong to.
template <typename V, typename A>
struct Greater {
    /// @brief Pointer to the left-hand member.
    V A::* lhs;
    /// @brief Pointer to the right-hand member.
    V A::* rhs;
    /// @brief The wire `"kind"` this node emits: `"greater"`.
    static constexpr detail::RuleKind kind = detail::RuleKind::Greater;
    /// @brief Usable as a nested condition: this node's `test()` is a
    /// function of the action, so composing it into an `and`/`or`/`not`
    /// tree changes what that tree evaluates to.
    static constexpr bool isCondition = true;
    /// @brief Validation rule (not presentation): participates in the gate.
    static constexpr bool isPresentation = false;

    /// @brief Evaluates the comparison against @p action.
    /// @param action The action snapshot to inspect.
    /// @return `true` when either operand is unengaged, or `*lhs > *rhs`.
    [[nodiscard]] constexpr bool test(const A& action) const noexcept {
        const auto& lv = action.*lhs;
        const auto& rv = action.*rhs;
        if (!lv.hasValue() || !rv.hasValue()) {
            return true;
        }
        return (*lv <=> *rv) == std::strong_ordering::greater;
    }

    /// @brief Emits this rule's `x-rules` JSON node.
    /// @return `{"kind":"greater","fields":["<lhs wire name>","<rhs wire name>"]}`.
    [[nodiscard]] glz::generic_u64 emitNode() const {
        glz::generic_u64 node{};
        node["kind"] = std::string{detail::ruleKindName(kind)};
        glz::generic_u64::array_t fields{};
        fields.emplace_back(detail::resolveFieldName(lhs));
        fields.emplace_back(detail::resolveFieldName(rhs));
        node["fields"] = fields;
        return node;
    }
};

/// @brief Builds a `Greater<V, A>` rule/condition: `*lhs > *rhs`.
/// @tparam V Field member type shared by both operands (deduced; must
///           satisfy `detail::ComparableField`).
/// @tparam A Action type (deduced).
/// @param lhs Pointer to the left-hand member.
/// @param rhs Pointer to the right-hand member.
/// @return The rule/condition node.
template <typename V, typename A>
    requires detail::ComparableField<V>
[[nodiscard]] constexpr auto greater(V A::* lhs, V A::* rhs) {
    return Greater<V, A>{lhs, rhs};
}

/// @brief Rule/condition: `*lhs >= *rhs` when both operands are engaged;
/// vacuously satisfied when either is unengaged. See `Greater` for the
/// exact-value / vacuous-operand rationale.
/// @tparam V Field member type shared by both operands (must satisfy
///           `detail::ComparableField`).
/// @tparam A Action type both fields belong to.
template <typename V, typename A>
struct GreaterOrEqual {
    /// @brief Pointer to the left-hand member.
    V A::* lhs;
    /// @brief Pointer to the right-hand member.
    V A::* rhs;
    /// @brief The wire `"kind"` this node emits: `"greaterOrEqual"`.
    static constexpr detail::RuleKind kind = detail::RuleKind::GreaterOrEqual;
    /// @brief Usable as a nested condition: this node's `test()` is a
    /// function of the action, so composing it into an `and`/`or`/`not`
    /// tree changes what that tree evaluates to.
    static constexpr bool isCondition = true;
    /// @brief Validation rule (not presentation): participates in the gate.
    static constexpr bool isPresentation = false;

    /// @brief Evaluates the comparison against @p action.
    /// @param action The action snapshot to inspect.
    /// @return `true` when either operand is unengaged, or `*lhs >= *rhs`.
    [[nodiscard]] constexpr bool test(const A& action) const noexcept {
        const auto& lv = action.*lhs;
        const auto& rv = action.*rhs;
        if (!lv.hasValue() || !rv.hasValue()) {
            return true;
        }
        return std::is_gteq(*lv <=> *rv);
    }

    /// @brief Emits this rule's `x-rules` JSON node.
    /// @return `{"kind":"greaterOrEqual","fields":["<lhs wire name>","<rhs wire name>"]}`.
    [[nodiscard]] glz::generic_u64 emitNode() const {
        glz::generic_u64 node{};
        node["kind"] = std::string{detail::ruleKindName(kind)};
        glz::generic_u64::array_t fields{};
        fields.emplace_back(detail::resolveFieldName(lhs));
        fields.emplace_back(detail::resolveFieldName(rhs));
        node["fields"] = fields;
        return node;
    }
};

/// @brief Builds a `GreaterOrEqual<V, A>` rule/condition: `*lhs >= *rhs`.
/// @tparam V Field member type shared by both operands (deduced; must
///           satisfy `detail::ComparableField`).
/// @tparam A Action type (deduced).
/// @param lhs Pointer to the left-hand member.
/// @param rhs Pointer to the right-hand member.
/// @return The rule/condition node.
template <typename V, typename A>
    requires detail::ComparableField<V>
[[nodiscard]] constexpr auto greaterOrEqual(V A::* lhs, V A::* rhs) {
    return GreaterOrEqual<V, A>{lhs, rhs};
}

/// @brief Rule/condition: `*lhs < *rhs` when both operands are engaged;
/// vacuously satisfied when either is unengaged. See `Greater` for the
/// exact-value / vacuous-operand rationale.
/// @tparam V Field member type shared by both operands (must satisfy
///           `detail::ComparableField`).
/// @tparam A Action type both fields belong to.
template <typename V, typename A>
struct Less {
    /// @brief Pointer to the left-hand member.
    V A::* lhs;
    /// @brief Pointer to the right-hand member.
    V A::* rhs;
    /// @brief The wire `"kind"` this node emits: `"less"`.
    static constexpr detail::RuleKind kind = detail::RuleKind::Less;
    /// @brief Usable as a nested condition: this node's `test()` is a
    /// function of the action, so composing it into an `and`/`or`/`not`
    /// tree changes what that tree evaluates to.
    static constexpr bool isCondition = true;
    /// @brief Validation rule (not presentation): participates in the gate.
    static constexpr bool isPresentation = false;

    /// @brief Evaluates the comparison against @p action.
    /// @param action The action snapshot to inspect.
    /// @return `true` when either operand is unengaged, or `*lhs < *rhs`.
    [[nodiscard]] constexpr bool test(const A& action) const noexcept {
        const auto& lv = action.*lhs;
        const auto& rv = action.*rhs;
        if (!lv.hasValue() || !rv.hasValue()) {
            return true;
        }
        return std::is_lt(*lv <=> *rv);
    }

    /// @brief Emits this rule's `x-rules` JSON node.
    /// @return `{"kind":"less","fields":["<lhs wire name>","<rhs wire name>"]}`.
    [[nodiscard]] glz::generic_u64 emitNode() const {
        glz::generic_u64 node{};
        node["kind"] = std::string{detail::ruleKindName(kind)};
        glz::generic_u64::array_t fields{};
        fields.emplace_back(detail::resolveFieldName(lhs));
        fields.emplace_back(detail::resolveFieldName(rhs));
        node["fields"] = fields;
        return node;
    }
};

/// @brief Builds a `Less<V, A>` rule/condition: `*lhs < *rhs`.
/// @tparam V Field member type shared by both operands (deduced; must
///           satisfy `detail::ComparableField`).
/// @tparam A Action type (deduced).
/// @param lhs Pointer to the left-hand member.
/// @param rhs Pointer to the right-hand member.
/// @return The rule/condition node.
template <typename V, typename A>
    requires detail::ComparableField<V>
[[nodiscard]] constexpr auto less(V A::* lhs, V A::* rhs) {
    return Less<V, A>{lhs, rhs};
}

/// @brief Rule/condition: `*lhs <= *rhs` when both operands are engaged;
/// vacuously satisfied when either is unengaged. See `Greater` for the
/// exact-value / vacuous-operand rationale.
/// @tparam V Field member type shared by both operands (must satisfy
///           `detail::ComparableField`).
/// @tparam A Action type both fields belong to.
template <typename V, typename A>
struct LessOrEqual {
    /// @brief Pointer to the left-hand member.
    V A::* lhs;
    /// @brief Pointer to the right-hand member.
    V A::* rhs;
    /// @brief The wire `"kind"` this node emits: `"lessOrEqual"`.
    static constexpr detail::RuleKind kind = detail::RuleKind::LessOrEqual;
    /// @brief Usable as a nested condition: this node's `test()` is a
    /// function of the action, so composing it into an `and`/`or`/`not`
    /// tree changes what that tree evaluates to.
    static constexpr bool isCondition = true;
    /// @brief Validation rule (not presentation): participates in the gate.
    static constexpr bool isPresentation = false;

    /// @brief Evaluates the comparison against @p action.
    /// @param action The action snapshot to inspect.
    /// @return `true` when either operand is unengaged, or `*lhs <= *rhs`.
    [[nodiscard]] constexpr bool test(const A& action) const noexcept {
        const auto& lv = action.*lhs;
        const auto& rv = action.*rhs;
        if (!lv.hasValue() || !rv.hasValue()) {
            return true;
        }
        return std::is_lteq(*lv <=> *rv);
    }

    /// @brief Emits this rule's `x-rules` JSON node.
    /// @return `{"kind":"lessOrEqual","fields":["<lhs wire name>","<rhs wire name>"]}`.
    [[nodiscard]] glz::generic_u64 emitNode() const {
        glz::generic_u64 node{};
        node["kind"] = std::string{detail::ruleKindName(kind)};
        glz::generic_u64::array_t fields{};
        fields.emplace_back(detail::resolveFieldName(lhs));
        fields.emplace_back(detail::resolveFieldName(rhs));
        node["fields"] = fields;
        return node;
    }
};

/// @brief Builds a `LessOrEqual<V, A>` rule/condition: `*lhs <= *rhs`.
/// @tparam V Field member type shared by both operands (deduced; must
///           satisfy `detail::ComparableField`).
/// @tparam A Action type (deduced).
/// @param lhs Pointer to the left-hand member.
/// @param rhs Pointer to the right-hand member.
/// @return The rule/condition node.
template <typename V, typename A>
    requires detail::ComparableField<V>
[[nodiscard]] constexpr auto lessOrEqual(V A::* lhs, V A::* rhs) {
    return LessOrEqual<V, A>{lhs, rhs};
}

/// @brief Constraint: literal types `equals(...)` accepts — the closed,
/// JSON-representable scalar set a field can hold. Restricting the literal
/// type keeps `equals` losslessly serialisable into `x-rules`'s `value` key
/// (see forms.md): a numeric literal is always the exact `math::Rational`,
/// never a `double`.
template <typename T>
concept RuleLiteral = std::same_as<T, std::int64_t> || std::same_as<T, bool> || std::same_as<T, std::string> ||
                      std::same_as<T, ::morph::math::Rational> || detail::isLiteralString<T>;

namespace detail {

/// @brief The value `Equals::test` actually compares: the engaged payload of an
///        `EngageableField`, or the member itself when it has no empty state.
/// @tparam V Field member type.
/// @param fieldValue The member to read.
/// @return A reference to the value the literal is compared against.
template <typename V>
[[nodiscard]] constexpr decltype(auto) equalsOperand(const V& fieldValue) noexcept {
    // The parentheses are load-bearing, not redundant: with `decltype(auto)`,
    // `return (e);` yields `decltype((e))`, which preserves an lvalue operand as
    // a reference while still returning a prvalue one by value. `Choice` and
    // `std::optional` dereference to a reference, `Ranged` to a prvalue, and
    // both have to work — dropping the parentheses would copy the first, and
    // binding the result to `const auto&` instead would dangle on the second.
    if constexpr (EngageableField<V>) {
        // Unevaluated in the concept below; `Equals::test` guards engagement.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access, readability-redundant-parentheses)
        return (*fieldValue);
    } else {
        // NOLINTNEXTLINE(readability-redundant-parentheses)
        return (fieldValue);
    }
}

}  // namespace detail

/// @brief Constraint: a field of type @p V can be compared against a literal of
///        type @p L at all.
///
/// Checked at the `equals(...)` call site rather than left to fail inside
/// `Equals::test`. `equals(&A::someQuantity, "URGENT")` produced 83 lines whose
/// first error was `no match for 'operator=='` deep inside this header, with no
/// mention of `equals` and nothing pointing at the caller's own `formRules`.
///
/// (@p V is the field member type and @p L the literal type. Neither is
/// documented with a parameter-doc command: on a concept that is an error
/// under -Weverything -Werror -- see the note on `Condition` above.)
template <typename V, typename L>
concept ComparableAgainstLiteral = requires(const V& fieldValue, const L& literal) {
    { detail::equalsOperand(fieldValue) == literal } -> std::convertible_to<bool>;
};

/// @brief The largest N such that *every* integer in `[0, N]` is exactly
///        representable as an IEEE-754 double: 2^53.
///
/// Not "the largest value a double holds exactly" — 2^60 is exact too. What
/// stops at 2^53 is the *contiguous* range: past it, consecutive integers start
/// sharing a representation, so an integer bound above it cannot be relied on to
/// survive `JSON.parse` intact and needs an exact companion the renderer can
/// read instead.
inline constexpr std::uint64_t kExactDoubleLimit = 9007199254740992ULL;

/// @brief Signed spelling of `kExactDoubleLimit`, for the negative bound.
inline constexpr std::int64_t kExactDoubleLimitSigned = 9007199254740992LL;

/// @brief Condition: `field`'s engaged value equals @p literal. An
/// unengaged field is **not** vacuously satisfied here (unlike the
/// comparison kinds) — a field with no value cannot equal anything, so
/// `equals` returns `false` while the field is unengaged.
/// @tparam V Field member type.
/// @tparam A Action type the field belongs to.
/// @tparam L Literal type (must satisfy `RuleLiteral`).
template <typename V, typename A, typename L>
struct Equals {
    /// @brief Pointer to the member this condition inspects.
    V A::* field;
    /// @brief The literal to compare the field's engaged value against.
    L literal;
    /// @brief The wire `"kind"` this condition emits: `"equals"`.
    static constexpr detail::RuleKind kind = detail::RuleKind::Equals;
    /// @brief Usable as a nested condition: this node's `test()` is a
    /// function of the action, so composing it into an `and`/`or`/`not`
    /// tree changes what that tree evaluates to.
    static constexpr bool isCondition = true;

    /// @brief Evaluates the condition against @p action.
    /// @param action The action snapshot to inspect.
    /// @return `true` when the field is engaged (or has no empty state) and
    ///         its value equals `literal`.
    [[nodiscard]] constexpr bool test(const A& action) const noexcept {
        const auto& fieldValue = action.*field;
        if constexpr (EngageableField<V>) {
            if (!detail::isEngaged(fieldValue)) {
                return false;
            }
            return static_cast<bool>(*fieldValue == literal);
        } else {
            return static_cast<bool>(fieldValue == literal);
        }
    }

    /// @brief Emits this condition's `x-rules` JSON node.
    /// @return `{"kind":"equals","fields":["<wire name>"],"value":...}`,
    ///         where `value` is `{"num":...,"den":...}` for a `Rational`
    ///         literal and the bare scalar otherwise.
    [[nodiscard]] glz::generic_u64 emitNode() const {
        glz::generic_u64 node{};
        node["kind"] = std::string{detail::ruleKindName(kind)};
        glz::generic_u64::array_t fields{};
        fields.emplace_back(detail::resolveFieldName(field));
        node["fields"] = fields;
        if constexpr (std::is_same_v<L, ::morph::math::Rational>) {
            glz::generic_u64 value{};
            value["num"] = literal.numerator;
            value["den"] = literal.denominator;
            node["value"] = value;
        } else if constexpr (detail::isLiteralString<L>) {
            // Serialises identically to a std::string literal — the inline
            // storage is a compile-time representation detail, not a wire one.
            // Glaze DOM builder — same shape as every sibling assignment here.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            node["value"] = std::string{literal.view()};
        } else {
            node["value"] = literal;
            // An integral literal beyond 2^53 does not survive the renderer's
            // JSON.parse: it arrives rounded, and an `equals` against it then
            // compares two values the schema kept distinct. Carry the exact
            // digits alongside, as `x-exactMinimum`/`x-exactMaximum` do for
            // bounds; a renderer that ignores `valueText` is unaffected.
            if constexpr (std::is_integral_v<L> && !std::is_same_v<L, bool>) {
                if (std::cmp_greater(literal, kExactDoubleLimit) || std::cmp_less(literal, -kExactDoubleLimitSigned)) {
                    // Glaze DOM builder — same shape as every sibling assignment here.
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                    node["valueText"] = std::to_string(literal);
                }
            }
        }
        return node;
    }
};

/// @brief Builds an `Equals<V, A, L>` condition: `field`'s engaged value
/// equals @p literal.
/// @tparam V Field member type (deduced).
/// @tparam A Action type (deduced).
/// @tparam L Literal type (deduced; must satisfy `RuleLiteral`).
/// @param field   Pointer to the member to test.
/// @param literal The value to compare against.
/// @return The condition node.
template <typename V, typename A, RuleLiteral L>
    requires ComparableAgainstLiteral<V, L>
[[nodiscard]] constexpr auto equals(V A::* field, L literal) {
    return Equals<V, A, L>{field, std::move(literal)};
}

/// @brief `equals` overload for a string-literal argument
/// (`equals(&A::code, "X")`), so callers do not have to spell
/// `std::string{"X"}` explicitly.
///
/// The literal is captured inline as a `detail::LiteralString`, not copied into a
/// `std::string`, so the resulting node stays a literal type and the documented
/// `static constexpr auto formRules = ruleList(...)` form works for a literal of
/// any length. Stored as a `std::string`, it only worked while the text fit the
/// standard library's small-string buffer — see `detail::LiteralString`.
/// Serialisation is unaffected: `emitNode()` emits the same JSON string either
/// way.
///
/// @tparam V Field member type (deduced).
/// @tparam A Action type (deduced).
/// @tparam N String literal length (deduced), including the trailing `'\0'`.
/// @param field   Pointer to the member to test.
/// @param literal The string literal to compare against.
/// @return The condition node, with the literal stored inline.
template <typename V, typename A, std::size_t N>
    requires ComparableAgainstLiteral<V, detail::LiteralString<N>>
[[nodiscard]] constexpr auto equals(V A::* field, const char (&literal)[N]) {
    return Equals<V, A, detail::LiteralString<N>>{field, detail::LiteralString<N>{literal}};
}

/// @brief Rule: `field` must be engaged whenever @p Cond holds; vacuously
/// satisfied (not required) while the condition does not hold.
/// @tparam V    Field member type (must satisfy `EngageableField`).
/// @tparam A    Action type the field belongs to.
/// @tparam Cond Condition node type (e.g. `Engaged<V2, A>`).
template <typename V, typename A, typename Cond>
struct RequiredWhen {
    /// @brief Pointer to the member this rule may require.
    V A::* field;
    /// @brief The condition that, when true, makes `field` required.
    Cond when;
    /// @brief The wire `"kind"` this rule emits: `"requiredWhen"`.
    static constexpr detail::RuleKind kind = detail::RuleKind::RequiredWhen;
    /// @brief Validation rule (not presentation): participates in the gate.
    static constexpr bool isPresentation = false;

    /// @brief Evaluates the rule against @p action.
    /// @param action The action snapshot to inspect.
    /// @return `true` when `when` does not hold, or `field` is engaged.
    [[nodiscard]] constexpr bool test(const A& action) const noexcept {
        if (!when.test(action)) {
            return true;
        }
        return detail::isEngaged(action.*field);
    }

    /// @brief Emits this rule's `x-rules` JSON node.
    /// @return `{"kind":"requiredWhen","fields":["<wire name>"],"when":{...}}`.
    [[nodiscard]] glz::generic_u64 emitNode() const {
        glz::generic_u64 node{};
        node["kind"] = std::string{detail::ruleKindName(kind)};
        glz::generic_u64::array_t fields{};
        fields.emplace_back(detail::resolveFieldName(field));
        node["fields"] = fields;
        node["when"] = when.emitNode();
        return node;
    }
};

/// @brief Builds a `RequiredWhen<V, A, Cond>` rule: @p field becomes
/// required exactly when @p when holds.
/// @tparam V    Field member type (deduced; must satisfy `EngageableField`).
/// @tparam A    Action type (deduced).
/// @tparam Cond Condition node type (deduced).
/// @param field Pointer to the member that becomes conditionally required.
/// @param when  The condition node (`engaged(...)`, `notEngaged(...)`, a
///              comparison, or `equals(...)`).
/// @return The rule node.
template <typename V, typename A, Condition Cond>
    requires EngageableField<V> && std::same_as<detail::ConditionActionType<Cond>, A>
[[nodiscard]] constexpr auto requiredWhen(V A::* field, Cond when) {
    return RequiredWhen<V, A, Cond>{field, when};
}

/// @brief Rule: exactly one of the listed fields is engaged.
/// @tparam A  Action type all fields belong to.
/// @tparam Vs Field member types, one per listed field (must each satisfy
///            `EngageableField`).
template <typename A, typename... Vs>
struct ExactlyOneOf {
    /// @brief Pointers to the member fields this rule ranges over.
    std::tuple<Vs A::*...> fields;
    /// @brief The wire `"kind"` this rule emits: `"exactlyOneOf"`.
    static constexpr detail::RuleKind kind = detail::RuleKind::ExactlyOneOf;
    /// @brief Usable as a nested condition: this node's `test()` is a
    /// function of the action, so composing it into an `and`/`or`/`not`
    /// tree changes what that tree evaluates to.
    static constexpr bool isCondition = true;
    /// @brief Validation rule (not presentation): participates in the gate.
    static constexpr bool isPresentation = false;

    /// @brief Evaluates the rule against @p action.
    /// @param action The action snapshot to inspect.
    /// @return `true` when exactly one listed field is engaged.
    [[nodiscard]] constexpr bool test(const A& action) const noexcept {
        int engagedCount = 0;
        std::apply([&](auto... field) { ((engagedCount += (detail::isEngaged(action.*field) ? 1 : 0)), ...); },
                   fields);
        return engagedCount == 1;
    }

    /// @brief Emits this rule's `x-rules` JSON node.
    /// @return `{"kind":"exactlyOneOf","fields":["<wire name>", ...]}`.
    [[nodiscard]] glz::generic_u64 emitNode() const {
        glz::generic_u64 node{};
        node["kind"] = std::string{detail::ruleKindName(kind)};
        glz::generic_u64::array_t names{};
        std::apply([&](auto... field) { (names.emplace_back(detail::resolveFieldName(field)), ...); }, fields);
        node["fields"] = names;
        return node;
    }
};

/// @brief Builds an `ExactlyOneOf<A, Vs...>` rule over @p fields.
/// @tparam A  Action type (deduced).
/// @tparam Vs Field member types (deduced; each must satisfy
///            `EngageableField`).
/// @param fields Pointers to the member fields, at least two.
/// @return The rule node.
template <typename A, typename... Vs>
    requires(EngageableField<Vs> && ...)
[[nodiscard]] constexpr auto exactlyOneOf(Vs A::*... fields) {
    return ExactlyOneOf<A, Vs...>{std::tuple<Vs A::*...>{fields...}};
}

/// @brief Rule: at least one of the listed fields is engaged.
/// @tparam A  Action type all fields belong to.
/// @tparam Vs Field member types, one per listed field (must each satisfy
///            `EngageableField`).
template <typename A, typename... Vs>
struct AtLeastOneOf {
    /// @brief Pointers to the member fields this rule ranges over.
    std::tuple<Vs A::*...> fields;
    /// @brief The wire `"kind"` this rule emits: `"atLeastOneOf"`.
    static constexpr detail::RuleKind kind = detail::RuleKind::AtLeastOneOf;
    /// @brief Usable as a nested condition: this node's `test()` is a
    /// function of the action, so composing it into an `and`/`or`/`not`
    /// tree changes what that tree evaluates to.
    static constexpr bool isCondition = true;
    /// @brief Validation rule (not presentation): participates in the gate.
    static constexpr bool isPresentation = false;

    /// @brief Evaluates the rule against @p action.
    /// @param action The action snapshot to inspect.
    /// @return `true` when at least one listed field is engaged.
    [[nodiscard]] constexpr bool test(const A& action) const noexcept {
        int engagedCount = 0;
        std::apply([&](auto... field) { ((engagedCount += (detail::isEngaged(action.*field) ? 1 : 0)), ...); },
                   fields);
        return engagedCount >= 1;
    }

    /// @brief Emits this rule's `x-rules` JSON node.
    /// @return `{"kind":"atLeastOneOf","fields":["<wire name>", ...]}`.
    [[nodiscard]] glz::generic_u64 emitNode() const {
        glz::generic_u64 node{};
        node["kind"] = std::string{detail::ruleKindName(kind)};
        glz::generic_u64::array_t names{};
        std::apply([&](auto... field) { (names.emplace_back(detail::resolveFieldName(field)), ...); }, fields);
        node["fields"] = names;
        return node;
    }
};

/// @brief Builds an `AtLeastOneOf<A, Vs...>` rule over @p fields.
/// @tparam A  Action type (deduced).
/// @tparam Vs Field member types (deduced; each must satisfy
///            `EngageableField`).
/// @param fields Pointers to the member fields, at least two.
/// @return The rule node.
template <typename A, typename... Vs>
    requires(EngageableField<Vs> && ...)
[[nodiscard]] constexpr auto atLeastOneOf(Vs A::*... fields) {
    return AtLeastOneOf<A, Vs...>{std::tuple<Vs A::*...>{fields...}};
}

/// @brief Rule: at most one of the listed fields is engaged.
/// @tparam A  Action type all fields belong to.
/// @tparam Vs Field member types, one per listed field (must each satisfy
///            `EngageableField`).
template <typename A, typename... Vs>
struct MutuallyExclusive {
    /// @brief Pointers to the member fields this rule ranges over.
    std::tuple<Vs A::*...> fields;
    /// @brief The wire `"kind"` this rule emits: `"mutuallyExclusive"`.
    static constexpr detail::RuleKind kind = detail::RuleKind::MutuallyExclusive;
    /// @brief Usable as a nested condition: this node's `test()` is a
    /// function of the action, so composing it into an `and`/`or`/`not`
    /// tree changes what that tree evaluates to.
    static constexpr bool isCondition = true;
    /// @brief Validation rule (not presentation): participates in the gate.
    static constexpr bool isPresentation = false;

    /// @brief Evaluates the rule against @p action.
    /// @param action The action snapshot to inspect.
    /// @return `true` when at most one listed field is engaged.
    [[nodiscard]] constexpr bool test(const A& action) const noexcept {
        int engagedCount = 0;
        std::apply([&](auto... field) { ((engagedCount += (detail::isEngaged(action.*field) ? 1 : 0)), ...); },
                   fields);
        return engagedCount <= 1;
    }

    /// @brief Emits this rule's `x-rules` JSON node.
    /// @return `{"kind":"mutuallyExclusive","fields":["<wire name>", ...]}`.
    [[nodiscard]] glz::generic_u64 emitNode() const {
        glz::generic_u64 node{};
        node["kind"] = std::string{detail::ruleKindName(kind)};
        glz::generic_u64::array_t names{};
        std::apply([&](auto... field) { (names.emplace_back(detail::resolveFieldName(field)), ...); }, fields);
        node["fields"] = names;
        return node;
    }
};

/// @brief Builds a `MutuallyExclusive<A, Vs...>` rule over @p fields.
/// @tparam A  Action type (deduced).
/// @tparam Vs Field member types (deduced; each must satisfy
///            `EngageableField`).
/// @param fields Pointers to the member fields, at least two.
/// @return The rule node.
template <typename A, typename... Vs>
    requires(EngageableField<Vs> && ...)
[[nodiscard]] constexpr auto mutuallyExclusive(Vs A::*... fields) {
    return MutuallyExclusive<A, Vs...>{std::tuple<Vs A::*...>{fields...}};
}

/// @brief Presentation rule: the listed field is shown only while @p Cond
/// holds. Never gates `allRulesSatisfied` — while hidden, the field's
/// current draft value still travels in the payload (hiding never clears
/// it), exactly like a static `x-hidden` field.
/// @tparam V    Field member type.
/// @tparam A    Action type the field belongs to.
/// @tparam Cond Condition node type.
template <typename V, typename A, typename Cond>
struct VisibleWhen {
    /// @brief Pointer to the member whose visibility this rule controls.
    V A::* field;
    /// @brief The condition that, when true, makes `field` visible.
    Cond when;
    /// @brief The wire `"kind"` this rule emits: `"visibleWhen"`.
    static constexpr detail::RuleKind kind = detail::RuleKind::VisibleWhen;
    /// @brief Presentation rule: never participates in the gate.
    static constexpr bool isPresentation = true;

    /// @brief Always `true`: presentation rules never gate submission. A
    /// renderer inspects `when` directly (not this method) to decide the
    /// field's visibility.
    /// @param action Unused (kept for interface uniformity with every other
    ///               rule node).
    /// @return `true`, unconditionally.
    [[nodiscard]] constexpr bool test(const A& action) const noexcept {
        static_cast<void>(action);
        return true;
    }

    /// @brief Emits this rule's `x-rules` JSON node.
    /// @return `{"kind":"visibleWhen","fields":["<wire name>"],"when":{...}}`.
    [[nodiscard]] glz::generic_u64 emitNode() const {
        glz::generic_u64 node{};
        node["kind"] = std::string{detail::ruleKindName(kind)};
        glz::generic_u64::array_t fields{};
        fields.emplace_back(detail::resolveFieldName(field));
        node["fields"] = fields;
        node["when"] = when.emitNode();
        return node;
    }
};

/// @brief Builds a `VisibleWhen<V, A, Cond>` presentation rule: @p field is
/// shown only while @p when holds.
/// @tparam V    Field member type (deduced).
/// @tparam A    Action type (deduced).
/// @tparam Cond Condition node type (deduced).
/// @param field Pointer to the member whose visibility is controlled.
/// @param when  The condition node.
/// @return The rule node.
template <typename V, typename A, Condition Cond>
    requires std::same_as<detail::ConditionActionType<Cond>, A>
[[nodiscard]] constexpr auto visibleWhen(V A::* field, Cond when) {
    return VisibleWhen<V, A, Cond>{field, when};
}

/// @brief Presentation rule: the listed field is editable only while
/// @p Cond does **not** hold (the field is read-only while `when` holds).
/// Never gates `allRulesSatisfied`, exactly like `VisibleWhen`.
/// @tparam V    Field member type.
/// @tparam A    Action type the field belongs to.
/// @tparam Cond Condition node type.
template <typename V, typename A, typename Cond>
struct ReadonlyWhen {
    /// @brief Pointer to the member whose editability this rule controls.
    V A::* field;
    /// @brief The condition that, when true, makes `field` read-only.
    Cond when;
    /// @brief The wire `"kind"` this rule emits: `"readonlyWhen"`.
    static constexpr detail::RuleKind kind = detail::RuleKind::ReadonlyWhen;
    /// @brief Presentation rule: never participates in the gate.
    static constexpr bool isPresentation = true;

    /// @brief Always `true`: presentation rules never gate submission. A
    /// renderer inspects `when` directly (not this method) to decide the
    /// field's editability.
    /// @param action Unused (kept for interface uniformity with every other
    ///               rule node).
    /// @return `true`, unconditionally.
    [[nodiscard]] constexpr bool test(const A& action) const noexcept {
        static_cast<void>(action);
        return true;
    }

    /// @brief Emits this rule's `x-rules` JSON node.
    /// @return `{"kind":"readonlyWhen","fields":["<wire name>"],"when":{...}}`.
    [[nodiscard]] glz::generic_u64 emitNode() const {
        glz::generic_u64 node{};
        node["kind"] = std::string{detail::ruleKindName(kind)};
        glz::generic_u64::array_t fields{};
        fields.emplace_back(detail::resolveFieldName(field));
        node["fields"] = fields;
        node["when"] = when.emitNode();
        return node;
    }
};

/// @brief Builds a `ReadonlyWhen<V, A, Cond>` presentation rule: @p field
/// is editable only while @p when does **not** hold.
/// @tparam V    Field member type (deduced).
/// @tparam A    Action type (deduced).
/// @tparam Cond Condition node type (deduced).
/// @param field Pointer to the member whose editability is controlled.
/// @param when  The condition node.
/// @return The rule node.
template <typename V, typename A, Condition Cond>
    requires std::same_as<detail::ConditionActionType<Cond>, A>
[[nodiscard]] constexpr auto readonlyWhen(V A::* field, Cond when) {
    return ReadonlyWhen<V, A, Cond>{field, when};
}

/// @brief Compound condition: all of `Conds...` hold. Nests to any depth —
/// each `Cond` may itself be a leaf (`Engaged`, `Equals`, a comparison, …) or
/// another `And`/`Or`/`Not`. Usable both as a nested `when` clause and
/// directly as a top-level `formRules` entry (it declares `isPresentation`
/// and `test()` exactly like every other validation rule), which is what
/// lets a single rule carry a compound condition tree instead of factoring
/// the composition into multiple single-condition rules.
/// @tparam A     Action type every nested condition ranges over.
/// @tparam Conds Nested condition node types, at least two.
template <typename A, typename... Conds>
struct And {
    /// @brief The nested conditions, in declaration order.
    std::tuple<Conds...> conditions;
    /// @brief The wire `"kind"` this node emits: `"and"`.
    static constexpr detail::RuleKind kind = detail::RuleKind::And;
    /// @brief Usable as a nested condition: this node's `test()` is a
    /// function of the action, so composing it into an `and`/`or`/`not`
    /// tree changes what that tree evaluates to.
    static constexpr bool isCondition = true;
    /// @brief Validation rule (not presentation): participates in the gate
    /// when used as a top-level `formRules` entry.
    static constexpr bool isPresentation = false;

    /// @brief Evaluates the condition against @p action.
    /// @param action The action snapshot to inspect.
    /// @return `true` when every nested condition holds.
    [[nodiscard]] constexpr bool test(const A& action) const noexcept {
        return std::apply([&](const auto&... cond) { return (cond.test(action) && ...); }, conditions);
    }

    /// @brief Emits this condition's `x-rules` JSON node.
    /// @return `{"kind":"and","conditions":[{...}, ...]}`.
    [[nodiscard]] glz::generic_u64 emitNode() const {
        glz::generic_u64 node{};
        node["kind"] = std::string{detail::ruleKindName(kind)};
        glz::generic_u64::array_t nested{};
        std::apply([&](const auto&... cond) { (nested.emplace_back(cond.emitNode()), ...); }, conditions);
        node["conditions"] = nested;
        return node;
    }
};

/// @brief Builds an `And<A, Conds...>` condition: every listed condition must
/// hold.
/// @tparam Cond0 First condition's node type (deduced); its action type `A`
///               is recovered from `test()` and shared by every other node.
/// @tparam Conds Remaining nested condition node types (deduced).
/// @param condition0  The first nested condition.
/// @param conditions  The remaining nested conditions, at least one more.
/// @return The compound condition node.
///
/// The action type is recovered from `Cond0` alone, so every other operand is
/// required to agree with it here rather than failing deep inside
/// `std::tuple`'s instantiation: `andOf(engaged(&C::x), engaged(&B::y))`
/// produced 153 lines whose first error was inside `<type_traits>` and which
/// named the caller's own `formRules` line only near the end.
template <Condition Cond0, Condition... Conds>
    requires(std::same_as<detail::ConditionActionType<Cond0>, detail::ConditionActionType<Conds>> && ...)
[[nodiscard]] constexpr auto andOf(Cond0 condition0, Conds... conditions) {
    static_assert(sizeof...(Conds) >= 1, "andOf: needs at least two conditions; one operand is the condition itself");
    return And<detail::ConditionActionType<Cond0>, Cond0, Conds...>{
        std::tuple<Cond0, Conds...>{std::move(condition0), std::move(conditions)...}};
}

/// @brief Compound condition: at least one of `Conds...` holds. Nests to any
/// depth, and is usable directly as a top-level `formRules` entry, exactly
/// like `And`.
/// @tparam A     Action type every nested condition ranges over.
/// @tparam Conds Nested condition node types, at least two.
template <typename A, typename... Conds>
struct Or {
    /// @brief The nested conditions, in declaration order.
    std::tuple<Conds...> conditions;
    /// @brief The wire `"kind"` this node emits: `"or"`.
    static constexpr detail::RuleKind kind = detail::RuleKind::Or;
    /// @brief Usable as a nested condition: this node's `test()` is a
    /// function of the action, so composing it into an `and`/`or`/`not`
    /// tree changes what that tree evaluates to.
    static constexpr bool isCondition = true;
    /// @brief Validation rule (not presentation): participates in the gate
    /// when used as a top-level `formRules` entry.
    static constexpr bool isPresentation = false;

    /// @brief Evaluates the condition against @p action.
    /// @param action The action snapshot to inspect.
    /// @return `true` when at least one nested condition holds.
    [[nodiscard]] constexpr bool test(const A& action) const noexcept {
        return std::apply([&](const auto&... cond) { return (cond.test(action) || ...); }, conditions);
    }

    /// @brief Emits this condition's `x-rules` JSON node.
    /// @return `{"kind":"or","conditions":[{...}, ...]}`.
    [[nodiscard]] glz::generic_u64 emitNode() const {
        glz::generic_u64 node{};
        node["kind"] = std::string{detail::ruleKindName(kind)};
        glz::generic_u64::array_t nested{};
        std::apply([&](const auto&... cond) { (nested.emplace_back(cond.emitNode()), ...); }, conditions);
        node["conditions"] = nested;
        return node;
    }
};

/// @brief Builds an `Or<A, Conds...>` condition: at least one listed
/// condition must hold.
/// @tparam Cond0 First condition's node type (deduced); its action type `A`
///               is recovered from `test()` and shared by every other node.
/// @tparam Conds Remaining nested condition node types (deduced).
/// @param condition0  The first nested condition.
/// @param conditions  The remaining nested conditions, at least one more.
/// @return The compound condition node.
/// Constrained exactly as `andOf` is, and for the same reasons.
template <Condition Cond0, Condition... Conds>
    requires(std::same_as<detail::ConditionActionType<Cond0>, detail::ConditionActionType<Conds>> && ...)
[[nodiscard]] constexpr auto orOf(Cond0 condition0, Conds... conditions) {
    static_assert(sizeof...(Conds) >= 1, "orOf: needs at least two conditions; one operand is the condition itself");
    return Or<detail::ConditionActionType<Cond0>, Cond0, Conds...>{
        std::tuple<Cond0, Conds...>{std::move(condition0), std::move(conditions)...}};
}

/// @brief Compound condition: the nested condition does **not** hold. Nests
/// to any depth, and is usable directly as a top-level `formRules` entry,
/// exactly like `And`/`Or`.
/// @tparam A    Action type the nested condition ranges over.
/// @tparam Cond Nested condition node type.
template <typename A, typename Cond>
struct Not {
    /// @brief The negated condition.
    Cond condition;
    /// @brief The wire `"kind"` this node emits: `"not"`.
    static constexpr detail::RuleKind kind = detail::RuleKind::Not;
    /// @brief Usable as a nested condition: this node's `test()` is a
    /// function of the action, so composing it into an `and`/`or`/`not`
    /// tree changes what that tree evaluates to.
    static constexpr bool isCondition = true;
    /// @brief Validation rule (not presentation): participates in the gate
    /// when used as a top-level `formRules` entry.
    static constexpr bool isPresentation = false;

    /// @brief Evaluates the condition against @p action.
    /// @param action The action snapshot to inspect.
    /// @return `true` when the nested condition does **not** hold.
    [[nodiscard]] constexpr bool test(const A& action) const noexcept { return !condition.test(action); }

    /// @brief Emits this condition's `x-rules` JSON node.
    /// @return `{"kind":"not","condition":{...}}`.
    [[nodiscard]] glz::generic_u64 emitNode() const {
        glz::generic_u64 node{};
        node["kind"] = std::string{detail::ruleKindName(kind)};
        node["condition"] = condition.emitNode();
        return node;
    }
};

/// @brief Builds a `Not<A, Cond>` condition: negates @p condition.
/// @tparam Cond Nested condition node type (deduced); its action type `A` is
///              recovered from `test()`.
/// @param condition The condition to negate.
/// @return The compound condition node.
template <Condition Cond>
[[nodiscard]] constexpr auto notOf(Cond condition) {
    return Not<detail::ConditionActionType<Cond>, Cond>{std::move(condition)};
}

/// @brief Composed list of an action's declared cross-field rules — the
/// value of `A::formRules`. Built by `ruleList(...)`; never constructed
/// directly.
/// @tparam Rules Rule node types, in declaration order.
template <typename... Rules>
struct RuleList {
    /// @brief The declared rules, in declaration order.
    std::tuple<Rules...> rules;
};

/// @brief Composes @p rules into the `RuleList` an action assigns to its
/// `static constexpr formRules` member.
/// @tparam Rules Rule node types (deduced).
/// @param rules The rule nodes, in the order they should be evaluated and
///              emitted.
/// @return The composed `RuleList<Rules...>`.
template <typename... Rules>
[[nodiscard]] constexpr auto ruleList(Rules... rules) {
    return RuleList<Rules...>{std::tuple<Rules...>{std::move(rules)...}};
}

/// @brief Concept: action `A` declares a `static constexpr` `formRules`
/// member (a `RuleList<...>`). Mirrors `detail::HasOptionalFields`.
template <typename A>
concept HasFormRules = requires { A::formRules; };

/// @brief Concept: action `A` declares a `static constexpr bool
///        explicitSubmit`, opting its form out of auto-submit-on-validity.
///
/// Opt-in, like `formLayout`/`fieldSpans`/`formRules`: an action that says
/// nothing keeps the renderer's default (submit as soon as the form is valid),
/// so adding this changes no existing schema. Declaring it `true` emits
/// `x-submitMode: "explicit"`; declaring it `false` emits nothing, which is
/// the same as not declaring it at all.
///
/// A side-effectful action wants this. Auto-submit fires on *every* keystroke
/// that leaves the form valid, so a `CreatePaste` bound directly to a live
/// controller stores one paste per typed character.
///
/// (The template parameter is intentionally left undocumented: clang's
/// -Wdocumentation does not consider a concept a template declaration, so a
/// parameter-doc command here is an error under -Weverything -Werror, which is
/// how the WASM ladder builds. `HasFormRules` above omits it for the same
/// reason. Note the command cannot even be *named* in this comment -- clang
/// parses it inside backticks too.)
template <typename A>
concept HasExplicitSubmit = requires {
    { A::explicitSubmit } -> std::convertible_to<bool>;
};

namespace detail {

/// @brief Evaluates @p rule against @p action, skipping presentation rules
/// (`VisibleWhen` / `ReadonlyWhen`) by construction —
/// they never gate.
template <typename Rule, typename A>
[[nodiscard]] constexpr bool evaluateGatingRule(const Rule& rule, const A& action) noexcept {
    if constexpr (Rule::isPresentation) {
        static_cast<void>(rule);
        static_cast<void>(action);
        return true;
    } else {
        return rule.test(action);
    }
}

}  // namespace detail

/// @brief Whether every **validation** rule in `A::formRules` holds for
/// @p action. Presentation rules (`visibleWhen` / `readonlyWhen`) are
/// skipped — they can never fail this check. Returns `true` unconditionally
/// for an action with no `formRules` (safe to call from every action's
/// `validate()` regardless of whether it declares rules).
/// @tparam A Action type.
/// @param action Action snapshot to check.
/// @return `true` when every validation rule holds (or there are none).
template <typename A>
[[nodiscard]] constexpr bool allRulesSatisfied(const A& action) noexcept {
    if constexpr (HasFormRules<A>) {
        return std::apply([&](const auto&... rule) { return (detail::evaluateGatingRule(rule, action) && ...); },
                          A::formRules.rules);
    } else {
        static_cast<void>(action);
        return true;
    }
}

/// @brief Thrown by `schemaJson<A>()` when `A`'s declared `formRules`
///        contradict `A`'s own derived `required` array so completely that no
///        submission could satisfy both.
///
/// The one contradiction that is decidable from the two halves alone: a rule
/// that **caps** how many of the fields it ranges over may be engaged at once
/// (`exactlyOneOf`, `mutuallyExclusive` — never `atLeastOneOf`, which is a
/// floor and can always be met by engaging everything) ranging over two or
/// more fields that `required` also demands. `required` says "fill in both";
/// the rule says "at most one"; the form is dead on arrival.
///
/// This is a **programming error in the action's declaration**, not a runtime
/// condition — hence `std::logic_error`, and hence a throw rather than the
/// silent tolerance schema generation extends to a `formLayout` entry naming
/// an unknown field. The fix is to name the rule's fields in
/// `A::optionalFields` (the rule is then the only gate on them) or to drop
/// the rule.
struct UnsatisfiableFormError : std::logic_error {
    /// @brief Constructs the error with a message naming the action type, the
    ///        offending rule kind, and the fields that are both required and
    ///        capped by it.
    /// @param actionName    The action type's name (`glz::name_v<A>`).
    /// @param ruleKind      The offending rule's wire `"kind"` string, e.g.
    ///                      `"exactlyOneOf"`.
    /// @param requiredFields Comma-separated wire names of the fields that are
    ///                      in `required` *and* ranged over by the rule.
    UnsatisfiableFormError(std::string_view actionName, std::string_view ruleKind, std::string_view requiredFields)
        : std::logic_error("morph::forms: unsatisfiable form for action '" + std::string{actionName} + "': rule '" +
                           std::string{ruleKind} +
                           "' caps how many of its fields may be engaged, but these are also "
                           "in the schema's required array: " +
                           std::string{requiredFields} +
                           ". No submission can satisfy both. List them in the action's optionalFields, or drop the "
                           "rule.") {}
};

namespace detail {

/// @brief One computed-field declaration: binds a destination member to the
///        input members it derives from and a pure derivation function.
///
/// Built by `morph::forms::computed<Dst, Inputs...>(fn)`; never named directly
/// by user code. `Dst` and `Inputs...` are pointer-to-data-member NTTPs (so a
/// renamed or deleted field is a compile error); `Fn` is the deduced callable
/// type of the pure derivation `fn(const A&) -> ValueOfDst`.
/// @tparam Dst    Pointer-to-data-member of the derived (destination) field.
/// @tparam Fn     Deduced callable type of the derivation function.
/// @tparam Inputs Pointer-to-data-members of the fields the derivation reads.
template <auto Dst, typename Fn, auto... Inputs>
struct ComputedField {
    /// @brief The pure derivation: `ValueOfDst(const A&)`.
    Fn fn;
};

/// @brief Concept: action declares a `static constexpr computedFields` member
///        (a `ComputeList` built by `morph::forms::computeList(...)`).
template <typename A>
concept HasComputedFields = requires { A::computedFields; };

/// @brief Ordered collection of `ComputedField` declarations for one action type.
///
/// Built by `morph::forms::computeList(...)`; never named directly by user code.
/// @tparam Fields Deduced `ComputedField<...>` types, one per declared entry.
template <typename... Fields>
struct ComputeList {
    /// @brief The declarations, in declaration order.
    std::tuple<Fields...> fields;
};

/// @brief Whether @p memberAddr is the destination address of @p field.
/// @tparam A      Action type (a reflectable aggregate).
/// @tparam Dst    Pointer-to-data-member of @p field's destination.
/// @tparam Fn     Callable type of @p field's derivation function.
/// @tparam Inputs Pointer-to-data-members of @p field's declared inputs.
/// @param action     The action instance @p memberAddr was taken from.
/// @param memberAddr Address of the member being tested.
/// @param field      The computed-field declaration to test against.
/// @return `true` if `memberAddr == std::addressof(action.*Dst)`.
template <typename A, auto Dst, typename Fn, auto... Inputs>
[[nodiscard]] constexpr bool isDestinationOf(const A& action, const void* memberAddr,
                                             const ComputedField<Dst, Fn, Inputs...>& field) noexcept {
    static_cast<void>(field);
    return memberAddr == static_cast<const void*>(std::addressof(action.*Dst));
}

/// @brief Whether @p memberAddr is the address of the destination member of
///        any entry in `A::computedFields`.
///
/// Used by `allRequiredEngaged` to exclude computed destinations from
/// required-ness the same way the schema's `required` array excludes them
/// (see `mergeSchemaExtras`). Compares addresses (not names) because the
/// caller already has a live member reference from `forEachNamedMember`, and
/// `Dst` gives a member reference on the *same* action instance via
/// `action.*Dst`.
/// @tparam A Action type (a reflectable aggregate).
/// @param action     The action instance @p memberAddr was taken from.
/// @param memberAddr Address of the member being tested.
/// @return `true` if @p memberAddr is a computed destination; always `false`
///         when `A` declares no `computedFields`.
template <typename A>
[[nodiscard]] constexpr bool isComputedDestinationMember(const A& action, const void* memberAddr) noexcept {
    if constexpr (HasComputedFields<A>) {
        bool found = false;
        std::apply([&](const auto&... field) { ((found = found || isDestinationOf(action, memberAddr, field)), ...); },
                   action.computedFields.fields);
        return found;
    } else {
        static_cast<void>(action);
        static_cast<void>(memberAddr);
        return false;
    }
}

/// @brief Evaluates one `ComputedField` against @p action, writing the result
///        into the destination member in place.
///
/// If every declared input is engaged -- or is not itself empty-capable, in
/// which case it is always considered engaged, mirroring `allRequiredEngaged`'s
/// treatment of non-empty-capable members -- the destination member is
/// overwritten with `field.fn(action)`. For a `Quantity` destination the
/// result is first converted to the destination's own type (same unit, the
/// destination's own `DeclaredDecimals`) and then **rounded** to that type's
/// declared precision (`Quantity::atDeclaredPrecision()`), so the stored value
/// matches the field's advertised `x-decimalPlaces` regardless of what
/// declared precision `Fn`'s return type happened to carry -- and regardless of
/// how many decimals the derivation itself produced (a product of two 2-decimal
/// operands is exact to 4). If any declared input is unengaged, the destination
/// is instead reset to its default-constructed (empty, for
/// `Quantity`/`Choice`/`Timestamp`) value rather than computed from a missing
/// operand.
/// @tparam A      Action type (a reflectable aggregate).
/// @tparam Dst    Pointer-to-data-member of the destination field.
/// @tparam Fn     Callable type of the derivation function.
/// @tparam Inputs Pointer-to-data-members of the declared input fields.
/// @param action Draft action whose destination member is overwritten in place.
/// @param field  The declaration being evaluated.
template <typename A, auto Dst, typename Fn, auto... Inputs>
constexpr void recomputeOne(A& action, const ComputedField<Dst, Fn, Inputs...>& field) {
    bool allEngaged = true;
    [[maybe_unused]] auto checkInput = [&]<auto InputPtr>() {
        using InputMember = std::remove_cvref_t<decltype(action.*InputPtr)>;
        if constexpr (EmptyCapableField<InputMember>) {
            if (!(action.*InputPtr).hasValue()) {
                allEngaged = false;
            }
        }
    };
    (checkInput.template operator()<Inputs>(), ...);

    using DstMember = std::remove_cvref_t<decltype(action.*Dst)>;
    if (!allEngaged) {
        action.*Dst = DstMember{};
        return;
    }
    auto result = field.fn(action);
    if constexpr (units::isQuantity<DstMember>) {
        DstMember const converted = result;
        action.*Dst = converted.atDeclaredPrecision();
    } else {
        action.*Dst = result;
    }
}

/// @brief Resolves the wire (JSON) field name of a pointer-to-member by
///        locating the reflected member of @p probe whose address matches
///        `probe.*memberPtr`.
///
/// Translates the compile-time pointer-to-member NTTPs a `ComputedField`
/// carries into the wire field names `x-computed` reports -- the same names
/// `x-order`/`required` already key on.
/// @tparam A         Action type (a reflectable aggregate).
/// @tparam MemberPtr  Deduced pointer-to-data-member type.
/// @param probe     A default-constructed instance of @p A.
/// @param memberPtr Pointer-to-data-member of @p A to resolve.
/// @return The member's reflected name (never empty in practice: @p memberPtr
///         always names a member of @p A).
template <typename A, typename MemberPtr>
[[nodiscard]] std::string_view resolveMemberName(const A& probe, MemberPtr memberPtr) {
    std::string_view result;
    forEachNamedMember(probe, [&]<std::size_t I>(std::string_view name, const auto& member) {
        static_cast<void>(I);
        if (static_cast<const void*>(std::addressof(member)) ==
            static_cast<const void*>(std::addressof(probe.*memberPtr))) {
            result = name;
        }
    });
    return result;
}

/// @brief Records one `ComputedField`'s destination -> ordered input wire
///        names into @p out, resolved against @p probe.
/// @tparam A      Action type (a reflectable aggregate).
/// @tparam Dst    Pointer-to-data-member of the destination field.
/// @tparam Fn     Callable type of the derivation function.
/// @tparam Inputs Pointer-to-data-members of the declared input fields.
/// @param probe Default-constructed instance of @p A used purely for name resolution.
/// @param field The declaration to record (its `fn` is not invoked here).
/// @param out   Map from destination wire name to its ordered input wire names.
template <typename A, auto Dst, typename Fn, auto... Inputs>
void collectComputedInputs(const A& probe, const ComputedField<Dst, Fn, Inputs...>& field,
                           std::unordered_map<std::string_view, std::vector<std::string_view>>& out) {
    static_cast<void>(field);
    out.emplace(resolveMemberName(probe, Dst), std::vector<std::string_view>{resolveMemberName(probe, Inputs)...});
}

/// @brief Trait: is `T` a `std::vector<...>`? Exposes the element type as
///        `ValueType` (`void` when `T` is not a vector).
template <typename T>
struct IsStdVector : std::false_type {
    using ValueType = void;
};

template <typename T, typename Alloc>
struct IsStdVector<std::vector<T, Alloc>> : std::true_type {
    using ValueType = T;
};

/// @brief Concept: `T` is glaze-reflectable as a JSON object -- the same test
///        that decides whether glaze emits a member into `$defs`/`$ref`
///        rather than inline. Shared by the nested-aggregate recursion below
///        and `reconcileDeclaredPrecision` elsewhere.
template <typename T>
concept ReflectableAggregate = glz::reflectable<T> || glz::glaze_object_t<T>;

/// @brief Concept: an integral member whose every value is exactly
///        representable as a `math::Rational` numerator, so a declared
///        `FieldMeta` bound can be *checked* against it in C++.
///
/// That is every signed integer type, plus every unsigned one narrower than
/// `std::int64_t`. A `std::uint64_t` member is deliberately excluded: half its
/// range has no `Rational` spelling, and silently checking the other half
/// would be a bound that holds for some values of a field and not others.
/// Such a member still *carries* its declared bound into the served schema —
/// only the C++ predicate stands aside (docs/spec/forms/forms.md, "Per-field
/// scalar bounds").
template <typename T>
concept BoundCheckableInteger =
    std::integral<T> && !std::same_as<T, bool> && (std::signed_integral<T> || std::numeric_limits<T>::digits < 64);

/// @brief Writes one declared bound onto a property node under @p key.
///
/// An integral bound is emitted as an integer, not as a `double`: that is what
/// glaze's own `minimum`/`maximum` are, so `annotateExactNumericBounds` then
/// gives a bound beyond 2^53 the same `x-exactMinimum`/`x-exactMaximum`
/// companion it gives a compiled one, with nothing here to special-case. A
/// non-integral bound has no exact JSON-number spelling and is emitted as its
/// quotient — the renderer's live gate is an approximation either way, and
/// `allFieldBoundsSatisfied` is the exact check (docs/spec/forms/forms.md,
/// "Per-instance constraints" makes the same trade for `x-minimum`).
/// @param property Property node to annotate in place.
/// @param key      Schema key to write: `"minimum"`, `"maximum"` or `"multipleOf"`.
/// @param bound    The declared bound.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- glaze DOM requires operator[]
inline void emitDeclaredBound(glz::generic_u64& property, const std::string& key,
                              const ::morph::math::Rational& bound) {
    if (bound.isInteger()) {
        property[key] = bound.numerator;
    } else {
        property[key] = static_cast<double>(bound.numerator) / static_cast<double>(bound.denominator);
    }
}
// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

/// @brief Whether @p meta declares any of the three numeric bounds.
/// @param meta The field descriptor to inspect.
/// @return `true` when at least one of `minimum`/`maximum`/`multipleOf` is engaged.
[[nodiscard]] constexpr bool declaresAnyBound(const FieldMeta& meta) noexcept {
    return meta.minimum.has_value() || meta.maximum.has_value() || meta.multipleOf.has_value();
}

/// @brief Whether a `multipleOf` declaration is usable — JSON Schema requires
///        it to be strictly positive, and a zero divisor has no meaning.
/// @param meta The field descriptor to inspect.
/// @return `true` when `multipleOf` is engaged and greater than zero.
[[nodiscard]] constexpr bool hasUsableMultipleOf(const FieldMeta& meta) noexcept {
    return meta.multipleOf.has_value() && !meta.multipleOf->isZero() && !meta.multipleOf->isNegative();
}

/// @brief Stamps whichever of `minimum`/`maximum`/`multipleOf` @p meta
///        declares onto @p property.
///
/// A free function rather than three more branches inside
/// `annotateBasicMemberProperty`, which is already long enough that
/// clang-tidy's cognitive-complexity check is the next thing to fire (the same
/// reasoning `annotateSubmitMode` was split out for).
/// @param property Property node to annotate in place.
/// @param meta     The field's declared metadata.
inline void annotateDeclaredBounds(glz::generic_u64& property, const FieldMeta& meta) {
    if (meta.minimum.has_value()) {
        emitDeclaredBound(property, "minimum", *meta.minimum);
    }
    if (meta.maximum.has_value()) {
        emitDeclaredBound(property, "maximum", *meta.maximum);
    }
    if (hasUsableMultipleOf(meta)) {
        emitDeclaredBound(property, "multipleOf", *meta.multipleOf);
    }
}

/// @brief Whether @p value satisfies every bound @p meta declares.
///
/// The comparisons run on the exact `math::Rational`, never on a `double`, so
/// the C++ verdict is the one the schema's rounded numbers only approximate.
/// `multipleOf` uses `checkedDiv` rather than `dividedBy`: a saturated
/// quotient comes back from the latter as a *successful* `±INT64_MAX/1`, which
/// is an integer and would be read as "yes, a multiple". An unrepresentable
/// quotient is not a multiple this function can vouch for, so it is refused.
/// @param meta  The field's declared metadata.
/// @param value The field's current value, in its canonical unit.
/// @return `true` when the value is within `[minimum, maximum]` and an exact
///         integer multiple of `multipleOf`, for whichever of the three are declared.
[[nodiscard]] constexpr bool satisfiesDeclaredBounds(const FieldMeta& meta,
                                                     const ::morph::math::Rational& value) noexcept {
    if (meta.minimum.has_value() && std::is_lt(value <=> *meta.minimum)) {
        return false;
    }
    if (meta.maximum.has_value() && std::is_gt(value <=> *meta.maximum)) {
        return false;
    }
    if (hasUsableMultipleOf(meta)) {
        auto const quotient = ::morph::math::checkedDiv(value, *meta.multipleOf);
        if (!quotient.has_value() || !quotient->isInteger()) {
            return false;
        }
    }
    return true;
}

/// @brief Applies the title/`FieldMeta`/`Quantity`/`Choice`/widget/
///        ranged-bounds annotations to one property node. Shared by
///        `mergeSchemaExtras`'s top-level pass and `annotateNestedAggregate`
///        below (the nested-aggregate recursion, to whatever depth the type
///        graph has) so both apply identical per-member rules -- this is the
///        single implementation of those rules; neither caller duplicates it.
///
/// Deliberately excludes computed-field annotations (`x-computed`/
/// `x-readonly`) and `x-order`: computed fields are not supported inside a
/// nested aggregate (see `annotateNestedAggregate`), and `x-order`'s source index
/// differs by caller, so each caller sets it itself.
/// @tparam Owner  The type declaring @p name (drives `FieldMeta`/widget-override lookup).
/// @tparam Member The static type of the member itself (drives type-driven annotations).
/// @param property DOM node for this one property; annotations are merged in, not replacing.
/// @param name     Wire (JSON) name of the member, for `FieldMeta`/widget-override lookup.
template <typename Owner, typename Member>
void annotateBasicMemberProperty(glz::generic_u64& property, std::string_view name) {
    // A plain `enum class` with no `glz::meta`/`glz::enumerate` fails glaze's
    // `glaze_enum_t` and falls through `to_json_schema<T>`'s final branch: a
    // six-way wildcard (`{"type": ["number","string","boolean","object",
    // "array","null"]}`) that states nothing. The shipped Qt/QML `DynamicForm`
    // then fires two mutually exclusive kind flags on it (`isBoolean` and
    // `isArray` both true) and draws a checkbox whose payload is a JSON array
    // of the string "false", reporting the form `ready` for a value nobody
    // chose. `glz::glaze_enum_t` is the same trait glaze's own
    // enum schema specialisation gates on, so this fires exactly when that
    // specialisation would not have been reached -- a `static_assert` whose
    // condition depends on @p Member, so it only fires for the specific
    // offending member type, not every call.
    if constexpr (std::is_enum_v<Member>) {
        static_assert(glz::glaze_enum_t<Member>,
                      "morph::forms: an enum class rendered as a form field must declare a glz::meta with "
                      "glz::enumerate -- without one, schemaJson<A>() cannot describe its closed set of "
                      "values (glaze emits a six-way wildcard type instead), and the shipped DynamicForm "
                      "renders that wildcard as a checkbox reporting the form ready for a value the user "
                      "never chose. See docs/spec/forms/forms.md, \"Closed sets\", and examples/"
                      "IMPLEMENTATION.md rule 3.");
    }
    const FieldMeta* fieldMeta = findFieldMeta<Owner>(name);
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
        if (!fieldMeta->i18nKey.empty()) {
            property["x-i18nKey"] = std::string{fieldMeta->i18nKey};
        }
        annotateDeclaredBounds(property, *fieldMeta);
    }

    if constexpr (units::isQuantity<Member>) {
        property["x-decimalPlaces"] = std::uint64_t{Member::declaredDecimals};
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
        property["x-optionsAction"] = std::string{Member::optionsAction()};
        property["x-optionValue"] = std::string{Member::valueField()};
        property["x-optionLabel"] = std::string{Member::labelField()};
        if constexpr (!Member::optionsDependsOn().empty()) {
            glz::generic_u64::array_t dependsOn{};
            for (auto const& parentName : Member::optionsDependsOn()) {
                dependsOn.emplace_back(std::string{parentName});
            }
            property["x-optionsDependsOn"] = dependsOn;
        }
    }

    std::string_view widgetHint{};
    if constexpr (DeclaresWidget<Member>) {
        widgetHint = Member::widget();
    }
    if constexpr (HasFieldMetadataWidgets<Owner>) {
        if (auto const overrideWidget = widgetOverride<Owner>(name); !overrideWidget.empty()) {
            widgetHint = overrideWidget;
        }
    }
    if (!widgetHint.empty()) {
        property["x-widget"] = std::string{widgetHint};
    }
    if constexpr (DeclaresRangedBounds<Member>) {
        using Bound = std::remove_cvref_t<decltype(Member::min())>;
        if constexpr (std::floating_point<Bound>) {
            property["x-min"] = static_cast<double>(Member::min());
            property["x-max"] = static_cast<double>(Member::max());
            property["x-step"] = static_cast<double>(Member::step());
        } else {
            property["x-min"] = static_cast<std::int64_t>(Member::min());
            property["x-max"] = static_cast<std::int64_t>(Member::max());
            property["x-step"] = static_cast<std::int64_t>(Member::step());
        }
    }
}

/// @brief The member of @p node named @p key, or `nullptr` when there is none.
///
/// The checked read over a `glz::generic_u64` object node, and morph's own
/// because glaze has no such thing. `generic_json::at(key)` is defined as
/// `{ return operator[](key); }` for *both* overloads (glaze v7.4.0,
/// `glaze/json/generic.hpp:320` and `:322`), and the non-const `operator[]`
/// it forwards to **inserts** a default-constructed member for a missing key
/// (`generic.hpp:201-211`). So on the mutating DOM walks below, `at()` is not
/// a bounds-safe alternative to `operator[]` -- it is the same function, and
/// on a missing key it turns a read into a write to the schema being emitted.
/// The const `operator[]` does check, by calling `glaze_error("Key not
/// found.")`, i.e. by throwing. Which of the two a call gets is decided by the
/// constness of the DOM, not by the spelling, which is why the remedy
/// `cppcoreguidelines-pro-bounds-avoid-unchecked-container-access` suggests
/// cannot be adopted mechanically here.
///
/// This returns a pointer instead: absence is a value the caller branches on,
/// a read never grows the document, and nothing throws. It also replaces the
/// `contains(key)` + `operator[](key)` pair every read site used to spell,
/// which probed the same map twice for one answer.
///
/// Only *reads* belong here. A site that means to create the member --
/// `property["x-order"] = ...` building the schema -- wants `operator[]`'s
/// insert and keeps it; converting one of those to a null check would change
/// behaviour, not make it safe. See `docs/spec/forms/forms.md`, "Reading the
/// DOM with `findMember`".
///
/// The returned pointer is into @p node's own object storage and is
/// invalidated by any insertion into @p node, exactly as the reference
/// `operator[]` returns is.
///
/// @tparam Node `glz::generic_u64` or `const glz::generic_u64`, deduced from
///               the argument, so a const DOM yields a const member and no
///               `const_cast` is needed to offer both.
/// @param node  The node to read. A node that is not an object -- including a
///               null one -- has no members and yields `nullptr`, rather than
///               being turned into an object as `operator[]` would.
/// @param key   Wire name of the member to find.
/// @return A pointer to the member, or `nullptr` when @p node is not an object
///         or holds no such key.
template <typename Node>
    requires std::same_as<std::remove_const_t<Node>, glz::generic_u64>
[[nodiscard]] Node* findMember(Node& node, std::string_view key) {
    auto* const object = node.template get_if<glz::generic_u64::object_t>();
    if (object == nullptr) {
        return nullptr;
    }
    auto* const entry = object->find(key);
    return (entry == object->end()) ? nullptr : &entry->second;
}

/// @brief The `$defs` keys already annotated during one `mergeSchemaExtras`
///        call, so a shared nested aggregate is annotated once instead of once
///        per route to it.
///
/// A nested aggregate's annotations are a function of its own type alone — no
/// per-route input reaches `annotateNestedAggregate` — so a `$defs` entry
/// reachable by several routes used to be rewritten with byte-identical content
/// once per route. This records which entries are done; the second and later
/// arrivals return immediately. Keys are owned `std::string`s rather than views
/// into the DOM: the DOM's object storage reallocates on insert, and a view into
/// a key it had moved would be read long after.
using NestedDefsVisited = std::unordered_set<std::string>;

/// @brief The whole schema DOM, as a type distinct from a node inside it.
///
/// Each of the three mutually recursive walkers below takes both the whole DOM
/// (to resolve a `$ref` against `$defs`) and one node within it (the thing to
/// annotate). While both were plain `glz::generic_u64&` they were adjacent
/// parameters of identical type, so transposing them at a call site compiled
/// silently and annotated against the wrong root — a defect with no diagnostic
/// of any kind. `bugprone-easily-swappable-parameters` flags exactly that
/// shape, and it flagged this one. Giving the DOM its own type turns the
/// transposition into a compile error, which is the actual remedy rather than
/// a suppression of the warning about it.
///
/// This is a reference wrapper: it owns nothing, is passed by value, and must
/// not outlive the DOM it names. The handle is held as a pointer rather than a
/// reference member so that the type stays assignable (and so that it does not
/// itself trip `cppcoreguidelines-avoid-const-or-ref-data-members`); it is
/// constructed from a reference and is therefore never null.
class SchemaDomRef {
public:
    /// @brief Wraps a DOM root.
    /// @param dom The DOM root to refer to. Must outlive this handle.
    explicit SchemaDomRef(glz::generic_u64& dom) noexcept : _dom(&dom) {}

    /// @brief The DOM root this refers to.
    /// @return A reference to the wrapped DOM root; never null.
    [[nodiscard]] glz::generic_u64& value() const noexcept { return *_dom; }

private:
    glz::generic_u64* _dom;
};

// annotateNestedAggregate, annotateNestedAggregateRef, and
// recurseIntoNestedAggregateIfAny are mutually recursive (each nested
// aggregate found while annotating one may itself contain another), so all
// three need forward declarations before any of their bodies can reference
// the others.
template <typename Sub>
void annotateNestedAggregate(SchemaDomRef dom, glz::generic_u64& node, NestedDefsVisited& visited);

template <typename Sub>
void annotateNestedAggregateRef(SchemaDomRef dom, glz::generic_u64& propertyOrItems, NestedDefsVisited& visited);

template <typename Member>
void recurseIntoNestedAggregateIfAny(SchemaDomRef dom, glz::generic_u64& property, NestedDefsVisited& visited);

/// @brief Recurses into @p property's own object schema if @p Member (or, for
///        `std::vector<Sub>`, its element type) is itself a
///        `ReflectableAggregate` -- the single decision point shared by
///        `mergeSchemaExtras`'s top-level loop and `annotateNestedAggregate`'s
///        own loop, so there is exactly one implementation of it.
///
/// **This function template carries no varying template argument, and that is
/// the whole termination argument.** `recurseIntoNestedAggregateIfAny<Member>`
/// reaches `annotateNestedAggregate<Sub>`, which reaches
/// `recurseIntoNestedAggregateIfAny<Member'>` for `Sub`'s own members. Every
/// specialisation is keyed on a *type* alone, the reachable type set of any
/// program is finite, and a specialisation already on the instantiation stack
/// is not instantiated again -- so a self-referential type
/// (`struct Node { std::vector<Node> children; };`) or a mutual reference
/// between two types costs one instantiation per type and stops, rather than
/// recursing forever (measured: see `docs/spec/forms/forms.md`, "Nested
/// aggregates (recursive, cycle-safe)").
///
/// Carrying *nothing* through the instantiation is what buys that, and the two
/// obvious alternatives both cost more. An ancestor *type list* makes every
/// distinct root-to-member route through the type graph its own instantiation,
/// so a domain model that is a DAG rather than a tree -- an `Address` under
/// both a `Customer` and a `Supplier`, a `Money` everywhere -- costs one
/// instantiation per route, and route count grows exponentially in the graph's
/// size (measured: a fixture whose route count is Fibonacci(n) reaches 86 s at
/// 2,584 routes). A depth counter cuts that to one instantiation per (type,
/// depth) pair, at the price of a depth cap and a `static_assert` that rejects
/// every cyclic type. Carrying nothing gives one instantiation per *type*, no
/// cap, and no rejection. The runtime recursion is stopped by @p visited, not
/// by the type system.
///
/// morph therefore imposes no depth limit, but the *compiler* does, and MSVC's
/// is low: 15 levels of nested aggregate initialisation inside an instantiated
/// template, past which `A probe{}` in `mergeSchemaExtras` is
/// `fatal error C1054`. See `docs/spec/forms/forms.md`, "Nesting depth in
/// practice", for the measurement on all three toolchains.
/// @tparam Member The static type of the member `annotateBasicMemberProperty`
///                 was just applied to.
/// @param dom      The whole schema DOM, wrapped (so a `$ref`'s `$defs` entry can be found);
///                  see `SchemaDomRef`.
/// @param property The property node for this member (or, for `std::vector<Sub>`,
///                  the property whose `"items"` node is the one to check).
/// @param visited  `$defs` keys already annotated on this `mergeSchemaExtras`
///                  call; see `NestedDefsVisited`.
template <typename Member>
void recurseIntoNestedAggregateIfAny(SchemaDomRef dom, glz::generic_u64& property, NestedDefsVisited& visited) {
    if constexpr (ReflectableAggregate<Member>) {
        annotateNestedAggregateRef<Member>(dom, property, visited);
    } else if constexpr (IsStdVector<Member>::value && ReflectableAggregate<typename IsStdVector<Member>::ValueType>) {
        using ItemType = typename IsStdVector<Member>::ValueType;
        if (auto* const items = findMember(property, "items")) {
            // A read, so it goes through findMember: a `std::vector<Sub>`
            // property glaze emitted without an `items` node is left alone
            // rather than given an empty one. The `contains` + `operator[]`
            // pair this replaces probed the same map twice and needed a
            // standing suppression to say why the subscript was safe.
            annotateNestedAggregateRef<ItemType>(dom, *items, visited);
        }
    }
}

/// @brief Annotates @p node -- the object-schema DOM node for a
///        nested-aggregate member -- applying `required` and
///        `annotateBasicMemberProperty`'s rules to its own properties, then
///        recursing into any of *its* members that are themselves nested
///        aggregates (see `recurseIntoNestedAggregateIfAny`), to whatever
///        depth the type graph actually has.
///
/// @p node is @e which DOM node depends on how many places in the whole
/// schema reference `Sub`: glaze **inlines** the object schema directly into
/// the referencing property when `Sub` is used exactly once (so @p node
/// *is* that property node), but **deduplicates** via `$defs`/`$ref` when
/// `Sub` is used two or more times (so @p node is the shared `$defs` entry,
/// resolved by the caller). Both forms have the identical `{"properties":
/// {...}}` shape this function needs, so one implementation handles both --
/// see the call site in `mergeSchemaExtras` for how @p node is resolved.
///
/// Computed fields, `formLayout`/`fieldSpans`, and `formRules` stay
/// top-level-only regardless of depth; a nested `Sub` declaring any of those
/// has no effect here.
///
/// For a `Sub` that is reachable from itself, this is entered once: the
/// caller's @p visited set records the `$defs` key on first arrival, and a
/// cyclic type is always in `$defs` (glaze inlines only a type used exactly
/// once in the whole schema, which a self-reference is not).
///
/// @tparam Sub   Nested aggregate type (default-constructible, glaze-reflectable
///                -- the same requirements the top-level action type already has).
/// @param dom  The whole schema DOM, wrapped (so a deeper `$ref`'s `$defs` entry can
///              be found); see `SchemaDomRef`.
/// @param node The object-schema DOM node to annotate in place (see above).
/// @param visited `$defs` keys already annotated on this `mergeSchemaExtras`
///                 call; see `NestedDefsVisited`.
template <typename Sub>
void annotateNestedAggregate(SchemaDomRef dom, glz::generic_u64& node, NestedDefsVisited& visited) {
    Sub probe{};
    glz::generic_u64::array_t requiredNames{};
    forEachNamedMember(probe, [&]<std::size_t I>(std::string_view name, const auto& member) {
        using Member = std::remove_cvref_t<decltype(member)>;
        if (!(isStdOptional<Member> || declaredOptional<Sub>(name))) {
            requiredNames.emplace_back(std::string{name});
        }
        auto& property = node["properties"][std::string{name}];
        property["x-order"] = std::uint64_t{I};
        annotateBasicMemberProperty<Sub, Member>(property, name);
        recurseIntoNestedAggregateIfAny<Member>(dom, property, visited);
    });
    // Idempotent if two members (or two actions sharing this schema call)
    // resolve to the same $defs entry: re-deriving the identical required
    // array is harmless. `visited` now stops the shared-$defs case from
    // arriving here twice in the first place, but the property is what makes
    // that skip safe, so it is stated rather than relied on silently.
    node["required"] = requiredNames;
}

/// @brief Resolves the object-schema DOM node for a nested-aggregate member,
///        given the property (or array `items`) node glaze wrote for it, and
///        annotates it via `annotateNestedAggregate<Sub>`.
///
/// Handles both forms `Sub` can take in the schema (see
/// `annotateNestedAggregate`'s doc comment): a `$ref` into `$defs` (`Sub` used
/// 2+ times somewhere in the schema) resolves to that shared def; anything
/// else is assumed to be the inlined object schema itself (`Sub` used exactly
/// once). A property that is neither -- glaze emitted something other than an
/// object schema for a type this function's caller already confirmed is a
/// `ReflectableAggregate` -- is left untouched rather than guessed at.
///
/// The `$defs` form is also where @p visited earns its keep: one shared entry
/// can be `$ref`'d from many properties, and every one of them used to rewrite
/// it with byte-identical content. The first arrival annotates it and records
/// the key; later arrivals return without touching the DOM.
/// @tparam Sub          Nested aggregate type, as `annotateNestedAggregate` requires.
/// @param dom           The whole schema DOM, wrapped (so a `$ref`'s `$defs` entry can
///                       be found); see `SchemaDomRef`.
/// @param propertyOrItems The property node itself (single nested member) or its
///                        array `items` node (`std::vector<Sub>` member).
/// @param visited       `$defs` keys already annotated on this `mergeSchemaExtras`
///                       call; see `NestedDefsVisited`.
template <typename Sub>
void annotateNestedAggregateRef(SchemaDomRef dom, glz::generic_u64& propertyOrItems, NestedDefsVisited& visited) {
    constexpr std::string_view kDefsPrefix = "#/$defs/";
    if (auto* const refNode = findMember(propertyOrItems, "$ref")) {
        if (auto const* ref = refNode->get_if<std::string>()) {
            if (std::string_view{*ref}.starts_with(kDefsPrefix)) {
                auto const key = std::string{ref->substr(kDefsPrefix.size())};
                // Checked, not indexed-and-hope: glz::generic_u64's object
                // storage reallocates on insert, so indexing a missing key
                // here would both fabricate a bogus empty $defs entry AND --
                // now that annotateNestedAggregate recurses -- risk dangling
                // a `node` reference an enclosing frame still holds into this
                // same $defs map. Well-formed glaze output never names a
                // $defs key that doesn't exist, so this only changes behavior
                // for malformed input, which is left untouched instead.
                // findMember is what states that in the type system rather
                // than in a comment beside a subscript.
                auto* const defs = findMember(dom.value(), "$defs");
                auto* const entry = (defs == nullptr) ? nullptr : findMember(*defs, key);
                // `visited.insert(...).second` is the first-arrival test: it
                // reports whether this call is the one that inserted the key,
                // so exactly one of the routes that reach a shared $defs entry
                // annotates it. It is evaluated last, so a key that fails
                // the lookup above is never recorded as done.
                if (entry != nullptr && visited.insert(key).second) {
                    annotateNestedAggregate<Sub>(dom, *entry, visited);
                }
            }
        }
        return;
    }
    // The inlined form needs no `visited` check: glaze inlines exactly when the
    // type is referenced once in the whole schema, so this node has one route.
    if (propertyOrItems.contains("properties")) {
        annotateNestedAggregate<Sub>(dom, propertyOrItems, visited);
    }
}

/// @brief Whether an emitted rule's wire `"kind"` *caps* how many of the
///        fields it ranges over may be engaged at once.
///
/// This is the single place the capping kinds are named. A future rule kind
/// with a ceiling ("at most two of these") joins this list and is then covered
/// by `rejectUnsatisfiableRules` with no other change.
///
/// `atLeastOneOf` is deliberately absent: it is a *floor*, not a ceiling, and
/// is satisfied by engaging every field it names — so it can never contradict
/// `required`, and rejecting it would be a false positive. `requiredWhen` is
/// likewise absent: it only ever *adds* required-ness.
/// @param kind The rule node's emitted `"kind"` string.
/// @return `true` for `"exactlyOneOf"` and `"mutuallyExclusive"`.
[[nodiscard]] inline bool capsEngagedCount(std::string_view kind) noexcept {
    return kind == ruleKindName(RuleKind::ExactlyOneOf) || kind == ruleKindName(RuleKind::MutuallyExclusive);
}

/// @brief One typed member of an emitted rule node, or `nullptr`.
///
/// Folds the find / end-compare / `get_if` triple into one call, which is what
/// keeps `findUnsatisfiableConjunct` below down to the branching its own
/// argument needs.
/// @tparam T   Expected value type of the member.
/// @param node The rule/condition node object.
/// @param key  Wire name of the member to read.
/// @return Pointer to the member's value, or `nullptr` when it is absent or
///         holds another type.
template <typename T>
[[nodiscard]] inline const T* ruleNodeMember(const glz::generic_u64::object_t& node, std::string_view key) {
    auto const* const entry = node.find(key);
    return (entry == node.end()) ? nullptr : entry->second.template get_if<T>();
}

/// @brief The names in @p fields that @p requiredNames also demands, rendered
///        as one comma-separated list — but only when there are **two or
///        more**.
///
/// One required field inside a capping rule is satisfiable (engage that one,
/// leave the rest empty), so fewer than two is reported as "no contradiction"
/// rather than as an empty list.
/// @param fields        The rule node's `fields` array.
/// @param requiredNames Wire names of every member that landed in `required`.
/// @return The offending names in declaration order, or `std::nullopt`.
[[nodiscard]] inline std::optional<std::string> requiredFieldsNamedBy(
    const glz::generic_u64::array_t& fields, const std::vector<std::string_view>& requiredNames) {
    std::string offenders{};
    std::size_t offenderCount = 0;
    for (auto const& fieldNode : fields) {
        auto const* fieldName = fieldNode.get_if<std::string>();
        // `std::ranges::find`, not `std::ranges::contains`: the latter is C++23
        // (P2302) and is absent from the libc++ the emscripten toolchain ships,
        // so it broke all three WASM TUs that include this header while
        // compiling fine under the libstdc++ the GCC leg uses. `ranges::find`
        // is C++20 and available on every toolchain this project builds on.
        if (fieldName == nullptr ||
            std::ranges::find(requiredNames, std::string_view{*fieldName}) == requiredNames.end()) {
            continue;
        }
        if (offenderCount > 0) {
            offenders += ", ";
        }
        ++offenderCount;
        offenders += *fieldName;
    }
    return (offenderCount >= 2) ? std::optional{std::move(offenders)} : std::nullopt;
}

/// @brief The first capping node in a **conjunction** of emitted rule nodes
///        that ranges over two or more fields `required` also demands.
///
/// Recurses into the `conditions` of any `and` node, because a conjunction of
/// conjunctions is one conjunction — see `rejectUnsatisfiableRules` for why
/// `or` and `not` are deliberately left alone.
///
/// @param nodes         Rule/condition nodes that must *all* hold.
/// @param requiredNames Wire names of every member that landed in `required`.
/// @return The offending node's `kind` and its comma-separated offending field
///         names, or `std::nullopt` when the conjunction is satisfiable.
// NOLINTNEXTLINE(misc-no-recursion) -- an `and` node's conditions are themselves a conjunction, so descending is the whole point
[[nodiscard]] inline std::optional<std::pair<std::string, std::string>> findUnsatisfiableConjunct(
    const glz::generic_u64::array_t& nodes, const std::vector<std::string_view>& requiredNames) {
    for (auto const& entry : nodes) {
        auto const* node = entry.get_if<glz::generic_u64::object_t>();
        if (node == nullptr) {
            continue;
        }
        auto const* kind = ruleNodeMember<std::string>(*node, "kind");
        if (kind == nullptr) {
            continue;
        }

        if (*kind == ruleKindName(RuleKind::And)) {
            auto const* nested = ruleNodeMember<glz::generic_u64::array_t>(*node, "conditions");
            if (auto offender = (nested == nullptr) ? std::nullopt : findUnsatisfiableConjunct(*nested, requiredNames);
                offender.has_value()) {
                return offender;
            }
            continue;
        }

        if (!capsEngagedCount(*kind)) {
            continue;
        }
        auto const* fields = ruleNodeMember<glz::generic_u64::array_t>(*node, "fields");
        if (fields == nullptr) {
            continue;
        }
        if (auto offenders = requiredFieldsNamedBy(*fields, requiredNames); offenders.has_value()) {
            return std::pair{*kind, *std::move(offenders)};
        }
    }
    return std::nullopt;
}

/// @brief Throws when any emitted rule node caps engagement over two or more
///        fields that the emitted `required` array also demands.
///
/// Called from `mergeSchemaExtras`, which is the one place both halves are in
/// hand: `required` is derived from field required-ness and `x-rules` from
/// `A::formRules`, entirely independently, so until here neither half can see
/// that it contradicts the other. Checking the *emitted* nodes (rather than
/// the compile-time rule tuple) means the check reads exactly what a renderer
/// would read, and keys off the same wire names `required` does.
///
/// **Why this throws when the rest of schema generation never does.** A
/// `formLayout` entry naming a field the action does not have is silently
/// ignored, and that is right: the author loses one layout hint and still
/// gets a working form. This is different in kind — the result is a form
/// *nobody can submit*, on any client, with no error naming the reason. The
/// failure is already certain at generation time and belongs to the author's
/// own build, so it is raised there rather than left to surface as a user who
/// cannot press Save.
///
/// One required field inside a capping rule is fine and is left alone: the
/// author engages that one and leaves the rest empty, which satisfies both
/// `exactlyOneOf` and `mutuallyExclusive`. Only two or more conflict.
///
/// Note that `std::optional` members can never appear here: `isStdOptional`
/// keeps them out of `required` on sight. The reachable case is an
/// `EmptyCapableField` (a `Quantity`, a `Choice`, a strong id) — required by
/// default, and rangeable by a membership rule.
/// **The check descends through `and`, and only through `and`.** It reads its
/// argument as a *conjunction* — every element has to hold — which is what
/// makes a contradiction in any one element a contradiction of the whole. The
/// top-level `x-rules` array is such a conjunction (`allRulesSatisfied` folds
/// it with `&&`) and so are an `and` node's `conditions`, so
/// `ruleList(andOf(exactlyOneOf(&A::a, &A::b), engaged(&A::c)))` is exactly as
/// unsubmittable as the direct `ruleList(exactlyOneOf(&A::a, &A::b))`, so it is
/// rejected too. Descending matters because `and`/`or`/`not` emit
/// `conditions`/`condition` rather than `fields`: a loop that skipped any node
/// without a `fields` key would let the wrapped spelling through. The identical
/// contradiction being a hard build failure in one spelling and a silently
/// unsubmittable form in the other is worse than not checking at all, since the
/// check's existence is what an author trusts.
///
/// `or` and `not` are **not** descended, and that is not an omission:
///
/// - Under `or`, a contradictory operand only makes that branch dead. The
///   other branch still satisfies the rule, so rejecting would be a false
///   positive — and a false positive here is a hard build failure on a form
///   that works.
/// - Under `not`, the contradiction inverts into a requirement. With `a` and
///   `b` both required, `notOf(exactlyOneOf(&A::a, &A::b))` asks for *not*
///   exactly one of them engaged, which engaging both — precisely what
///   `required` already demands — satisfies.
///
/// @tparam A Action type (a reflectable aggregate), used only to name the
///           offending type in the diagnostic.
/// @param xRules       The rule nodes just emitted from `A::formRules`.
/// @param requiredNames Wire names of every member that landed in `required`.
/// @throws UnsatisfiableFormError naming the first offending rule.
template <typename A>
void rejectUnsatisfiableRules(const glz::generic_u64::array_t& xRules,
                              const std::vector<std::string_view>& requiredNames) {
    if (auto const offender = findUnsatisfiableConjunct(xRules, requiredNames); offender.has_value()) {
        throw UnsatisfiableFormError{glz::name_v<A>, offender->first, offender->second};
    }
}

/// @brief Which declared bound `annotateExactBound` is to give an exact
///        companion -- and so both schema keys it touches.
///
/// One enumerator rather than the `(key, textKey)` pair of adjacent
/// `const std::string&`s this used to take. Those were transposable at a call
/// site with no diagnostic of any kind -- `annotateExactBound(node,
/// "x-exactMinimum", "minimum")` compiles and writes the bound into the
/// companion -- and `bugprone-easily-swappable-parameters` reported them the
/// moment the body stopped using the two in the same way (the read now goes
/// through `findMember`, the write still through `operator[]`). This is the
/// remedy `SchemaDomRef` above applies to the same defect shape: make the
/// transposition a compile error rather than suppress the warning about it.
/// It also leaves exactly one place where `minimum` is paired with
/// `x-exactMinimum`.
enum class ExactBoundKind : std::uint8_t {
    Minimum,
    Maximum,
};

/// @brief Adds an exact decimal-string companion for one numeric bound, when
///        the bound is too large for a double to hold exactly.
///
/// `minimum`/`maximum` are standard JSON-Schema vocabulary stamped by glaze,
/// and `mergeSchemaExtras` reads the schema in u64 number mode precisely so
/// they are not rounded on the C++ side. They are rounded anyway the moment a
/// renderer does `JSON.parse(controller.schemasJson)`, which every shipped app
/// does -- `INT64_MAX` becomes `9223372036854775808`, and a client-side gate
/// comparing against it then admits `INT64_MAX + 1` as "not greater". The
/// exact digits travel as a string, which `JSON.parse` cannot round.
///
/// Emitted only above `kExactDoubleLimit`: an ordinary bound loses nothing to a
/// double, so schemas that do not need this are byte-for-byte unchanged.
///
/// @param node Schema node to annotate in place (a property or a `$defs` entry).
/// @param kind Which declared bound to give a companion; see `ExactBoundKind`.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- the two writes below intend the insert
inline void annotateExactBound(glz::generic_u64& node, ExactBoundKind kind) {
    bool const isMinimum = (kind == ExactBoundKind::Minimum);
    std::string_view const key = isMinimum ? "minimum" : "maximum";
    std::string_view const textKey = isMinimum ? "x-exactMinimum" : "x-exactMaximum";
    // A read, so it is checked: findMember yields nullptr for a node with no
    // such bound instead of fabricating a null one. The two
    // `node[textKey] =` writes further down are the opposite case -- the
    // companion key is *meant* to be created -- and keep `operator[]`, which
    // is what the suppression above is still for.
    auto const* const bound = findMember(node, key);
    if (bound == nullptr) {
        return;
    }
    // std::cmp_* rather than a cast: the two bounds arrive in different
    // signednesses and the limit is unsigned, so a cast would be the very
    // sign-mismatch this comparison exists to get right.
    //
    // `bound` is read out into `value` before either write: inserting
    // `textKey` reallocates `node`'s object storage and invalidates it.
    if (bound->template holds<std::uint64_t>()) {
        auto const value = bound->template get<std::uint64_t>();
        if (std::cmp_greater(value, kExactDoubleLimit)) {
            node[textKey] = std::to_string(value);
        }
    } else if (bound->template holds<std::int64_t>()) {
        auto const value = bound->template get<std::int64_t>();
        if (std::cmp_greater(value, kExactDoubleLimit) || std::cmp_less(value, -kExactDoubleLimitSigned)) {
            node[textKey] = std::to_string(value);
        }
    }
}

/// @brief Walks a schema DOM, adding `x-exactMinimum`/`x-exactMaximum` wherever a
///        bound is too large for a double.
///
/// Recursive over the whole document rather than over `properties` alone,
/// because the bounds that actually matter live in `$defs`: a `std::int64_t`
/// member is emitted as a `$ref` to `$defs/int64_t`, and that definition is
/// where `minimum`/`maximum` sit.
///
/// @param node Node to walk; objects and arrays recurse, scalars are left alone.
// NOLINTNEXTLINE(misc-no-recursion) -- walking a JSON tree is inherently recursive
inline void annotateExactNumericBounds(glz::generic_u64& node) {
    if (node.is_object()) {
        annotateExactBound(node, ExactBoundKind::Minimum);
        annotateExactBound(node, ExactBoundKind::Maximum);
        for (auto& [childKey, child] : node.get_object()) {
            annotateExactNumericBounds(child);
        }
    } else if (node.is_array()) {
        for (auto& child : node.get_array()) {
            annotateExactNumericBounds(child);
        }
    }
}
// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

/// @brief Stamps `x-submitMode` on @p dom when `A` opts out of auto-submit.
///
/// A top-level key sourced from a declaration on the action type, exactly as
/// `x-layout` is (docs/spec/forms/forms.md, "Explicit submit mode"). Emitted
/// only for `explicitSubmit = true`, so an action that declares nothing -- or
/// declares it `false` -- keeps the renderer's auto-submit default and its
/// schema is byte-for-byte unchanged.
///
/// A free function rather than a few lines inside `mergeSchemaExtras`: that
/// function is already at the edge of clang-tidy's cognitive-complexity
/// threshold, and every `if constexpr` added inline pushes it further.
/// @tparam A Action type whose schema is being annotated.
/// @param dom Schema DOM to stamp in place.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- glaze DOM requires operator[]
template <typename A>
void annotateSubmitMode(glz::generic_u64& dom) {
    if constexpr (HasExplicitSubmit<A>) {
        if constexpr (A::explicitSubmit) {
            dom["x-submitMode"] = "explicit";
        }
    }
}
// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

/// @brief The DOM post-merge behind `schemaJson`: adds the derived `required`
///        array, `x-order`, `x-decimalPlaces`, and (for actions declaring
///        `computedFields`) `x-computed`/`x-readonly` to a glaze-produced schema.
///
/// Separated from `schemaJson` so the fallback path (malformed input passes
/// through unchanged) is directly testable.
///
/// Also the one place `required` and `x-rules` are both in hand, and so the
/// one place their mutual contradiction is visible: see
/// `rejectUnsatisfiableRules`, the sole path by which schema generation
/// throws.
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
    // Wire keys of every reflected member, in declaration order — reused
    // below to silently ignore a formLayout/fieldSpans entry that names a
    // field the action does not actually have (a tolerated unknown field
    // still yields a working form, so it is never worth throwing over --
    // unlike the one case rejectUnsatisfiableRules catches below).
    std::vector<std::string_view> memberNames{};
    // The subset of `memberNames` that lands in `required`, kept as wire names
    // so `rejectUnsatisfiableRules` can match the emitted rule nodes' `fields`
    // against it without re-reading the DOM array it just built.
    std::vector<std::string_view> requiredMemberNames{};
    // The line MSVC reports as `fatal error C1054: compiler limit:
    // initializers nested too deeply` when A roots a chain of nested
    // aggregates more than 14 levels deep. That is cl's limit, not morph's
    // (measured: docs/spec/forms/forms.md, "Nesting depth in practice"); the
    // diagnostic names neither A nor the nesting, so the pointer lives here.
    A probe{};

    // Shared across the whole member walk, not per member: a `$defs` entry two
    // different members reach is the same entry, and annotating it once is the
    // point (see NestedDefsVisited). Stays empty for an action with no nested
    // aggregate member, which is the common case.
    NestedDefsVisited nestedVisited{};

    // Computed-field destination names -> their declared input wire names,
    // resolved once against the probe. Empty when A declares no
    // computedFields (the common case), so every lookup against it below is
    // then trivially false -- no schema change for actions that don't opt in.
    std::unordered_map<std::string_view, std::vector<std::string_view>> computedInputs{};
    if constexpr (HasComputedFields<A>) {
        std::apply([&](const auto&... field) { (collectComputedInputs(probe, field, computedInputs), ...); },
                   A::computedFields.fields);
    }

    forEachNamedMember(probe, [&]<std::size_t I>(std::string_view name, const auto& member) {
        using Member = std::remove_cvref_t<decltype(member)>;
        static_cast<void>(member);
        memberNames.push_back(name);
        const bool isComputed = computedInputs.contains(name);
        // A computed field is derived, not user-entered: exclude it from
        // `required` the same way an opted-out or std::optional field is.
        const bool isOptional = isStdOptional<Member> || declaredOptional<A>(name) || isComputed;
        if (!isOptional) {
            requiredNames.emplace_back(std::string{name});
            requiredMemberNames.push_back(name);
        }
        auto& property = dom["properties"][std::string{name}];
        property["x-order"] = std::uint64_t{I};
        if (isComputed) {
            property["x-readonly"] = true;
            glz::generic_u64 computedMeta{};
            glz::generic_u64::array_t inputsList{};
            for (auto const& inputName : computedInputs.at(name)) {
                inputsList.emplace_back(std::string{inputName});
            }
            computedMeta["inputs"] = inputsList;
            property["x-computed"] = computedMeta;
        }

        // Label/title/FieldMeta/Quantity/Choice/widget/ranged-bounds: shared
        // with the nested-aggregate recursion's per-member pass so both apply
        // identical rules (see annotateBasicMemberProperty's doc comment).
        annotateBasicMemberProperty<A, Member>(property, name);

        // Nested aggregates (recursive, cycle-safe -- docs/spec/forms/forms.md,
        // "Nested aggregates (recursive, cycle-safe)"): a member whose type
        // is itself a reflectable aggregate gets an object schema from glaze --
        // either inlined directly into this property (the type is used exactly
        // once in the whole schema) or shared via `$defs`/`$ref` (used 2+
        // times). `recurseIntoNestedAggregateIfAny` resolves whichever form it
        // is and recurses so that object schema's own members get
        // `x-order`/`required`/title/Quantity/Choice/widget annotations too,
        // however deep the type graph goes -- there is no depth limit, and a
        // cyclic type is described rather than rejected (see that function's
        // doc comment). `nestedVisited` is what stops the walk. Purely
        // additive: an action with no nested aggregate member has nothing here
        // to trigger on, so its schema is byte-for-byte unchanged.
        recurseIntoNestedAggregateIfAny<Member>(SchemaDomRef{dom}, property, nestedVisited);
    });
    // Always assign — an explicit empty array beats leaving whatever the
    // schema writer may have emitted (or omitted) for `required`.
    dom["required"] = requiredNames;

    // Layout & grouping (docs/spec/forms/forms.md, "Layout & grouping"):
    // purely additive over the required/x-order pass above; a no-op unless
    // the action declares a static constexpr `formLayout`.
    if constexpr (detail::HasFormLayout<A>) {
        glz::generic_u64::array_t groupsJson{};
        // wire key -> 0-based index into A::formLayout; a field claimed by
        // two groups keeps the first (declaration order wins, silently —
        // the author still gets a working form, so this is not worth
        // throwing over).
        std::vector<std::pair<std::string_view, std::size_t>> sectionOf{};
        std::size_t groupIndex = 0;
        for (auto const& group : A::formLayout) {
            glz::generic_u64::array_t fieldsJson{};
            for (std::string_view fieldName : group.fields) {
                bool const isMember =
                    std::find(memberNames.begin(), memberNames.end(), fieldName) != memberNames.end();
                if (!isMember) {
                    continue;  // names a field the action does not have: ignored, never thrown
                }
                bool alreadyPlaced = false;
                for (auto const& placed : sectionOf) {
                    if (placed.first == fieldName) {
                        alreadyPlaced = true;
                        break;
                    }
                }
                if (alreadyPlaced) {
                    continue;  // first group to claim a field wins
                }
                fieldsJson.emplace_back(std::string{fieldName});
                sectionOf.emplace_back(fieldName, groupIndex);
            }
            glz::generic_u64 groupJson{};
            groupJson["title"] = std::string{group.title};
            groupJson["kind"] = std::string{groupKindName(group.kind)};
            groupJson["fields"] = fieldsJson;
            groupsJson.emplace_back(std::move(groupJson));
            ++groupIndex;
        }
        dom["x-layout"]["groups"] = groupsJson;

        for (auto const& placed : sectionOf) {
            auto& property = dom["properties"][std::string{placed.first}];
            property["x-group"] = std::string{A::formLayout[placed.second].title};
            property["x-section"] = std::uint64_t{placed.second};
        }
    }

    // Column spans (docs/spec/forms/forms.md, "Layout & grouping"): a no-op
    // unless the action declares a static constexpr `fieldSpans`.
    if constexpr (detail::HasFieldSpans<A>) {
        for (auto const& span : A::fieldSpans) {
            if (span.colspan <= 1) {
                continue;  // 1 is the default width; nothing to advertise
            }
            bool const isMember = std::find(memberNames.begin(), memberNames.end(), span.field) != memberNames.end();
            if (!isMember) {
                continue;  // names a field the action does not have: ignored, never thrown
            }
            auto& property = dom["properties"][std::string{span.field}];
            property["x-colspan"] = span.colspan;
        }
    }

    // Cross-field rules (docs/spec/forms/forms.md's `x-rules`): emitted only
    // when the action declares `formRules`, so an unannotated action's
    // schema is byte-identical to before this feature existed. Walks
    // whatever rule node types A::formRules holds -- every node type past
    // and future exposes the same emitNode() -> glz::generic_u64 shape, so
    // this loop needs no changes as new rule kinds are added. (Named
    // emitNode(), not emit(), because Qt's <QObject> headers `#define emit`
    // as an empty macro -- a bare `emit()` silently vanishes and fails to
    // parse in any translation unit that includes both Qt and this header,
    // e.g. examples/forms/gui_qml/FormsController.cpp.)
    if constexpr (HasFormRules<A>) {
        glz::generic_u64::array_t xRules{};
        std::apply([&](const auto&... rule) { (xRules.emplace_back(rule.emitNode()), ...); }, A::formRules.rules);
        // The one point where the two independently derived halves are both
        // in hand -- see rejectUnsatisfiableRules for why this is the one
        // author mistake schema generation refuses to tolerate silently.
        rejectUnsatisfiableRules<A>(xRules, requiredMemberNames);
        dom["x-rules"] = xRules;
    }

    annotateSubmitMode<A>(dom);

    // Exact companions for any bound a double cannot hold. Last, so it also
    // covers nodes added by the passes above.
    annotateExactNumericBounds(dom);

    // value_or without a move: the copy is irrelevant (schemaJson memoises),
    // and keeping the fallback branch inside glaze's expected avoids an
    // untestable line here (write_json of a DOM we just built cannot fail).
    return glz::write_json(dom).value_or(rawSchema);
}
// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

}  // namespace detail

/// @brief Builds one computed-field declaration binding a destination member
///        to its declared input members and a pure derivation function.
///
/// @code{.cpp}
/// static constexpr auto computedFields = morph::forms::computeList(
///     morph::forms::computed<&LineItem::total, &LineItem::qty, &LineItem::price>(
///         [](const auto& s) { return s.qty * s.price; }));  // auto: LineItem is incomplete here
/// @endcode
///
/// @tparam Dst    Pointer-to-data-member of the derived (destination) field.
/// @tparam Inputs Pointer-to-data-members of the fields the derivation reads,
///                in declaration order.
/// @tparam Fn     Deduced callable type: `ValueOfDst(const A&)`.
/// @param fn Pure function computing the destination value from the action.
///           Must have no side effects and read nothing beyond @p fn's own
///           argument -- the framework cannot check this; it is the author's
///           contract.
/// @return A `detail::ComputedField<Dst, Fn, Inputs...>` value.
template <auto Dst, auto... Inputs, typename Fn>
[[nodiscard]] consteval auto computed(Fn fn) noexcept {
    return detail::ComputedField<Dst, Fn, Inputs...>{fn};
}

/// @brief Builds a `ComputeList` from one or more `computed(...)` declarations.
///
/// Assign the result to a `static constexpr auto computedFields` member on the
/// action type; `recomputeAll<A>` and `schemaJson<A>()` detect it via the
/// `detail::HasComputedFields<A>` concept.
/// @tparam Fields Deduced `detail::ComputedField<...>` types.
/// @param fields The computed-field declarations, in declaration order.
/// @return A `detail::ComputeList<Fields...>` value.
template <typename... Fields>
[[nodiscard]] consteval auto computeList(Fields... fields) noexcept {
    return detail::ComputeList<Fields...>{std::tuple<Fields...>{fields...}};
}

/// @brief Recomputes every entry of `A::computedFields` in place on @p action.
///
/// A no-op for actions with no `computedFields` declaration -- backward
/// compatible with every existing action type. For an action that does
/// declare `computedFields`, every entry is evaluated in declaration order via
/// `detail::recomputeOne` (see that function for the per-entry semantics:
/// empty-input propagation and declared-precision retagging). Called from the
/// reactive `set<>` path (`bridge.hpp`, live/non-authoritative) and from every
/// dispatch site (`bridge.hpp`, `registry.hpp`, authoritative) so the value
/// the client displays and the value the server stores are derived from the
/// identical function over identically-reconciled inputs.
/// @tparam A     Action type (a reflectable aggregate).
/// @param action Draft action whose computed members are overwritten in place.
template <typename A>
constexpr void recomputeAll(A& action) {
    if constexpr (detail::HasComputedFields<A>) {
        std::apply([&](const auto&... field) { (detail::recomputeOne(action, field), ...); },
                   A::computedFields.fields);
    } else {
        static_cast<void>(action);
    }
}

/// @brief Builds a `FieldMeta` for the member named by @p MemberPtr, so the
///        wire key is never restated as a string.
///
/// @warning Because this resolves @p MemberPtr via runtime reflection on a
/// probe instance of its *own* containing type, a `fieldMetadata` array built
/// from `describe<>()` cannot be a single in-class `static constexpr`
/// initializer (the type is still incomplete at that point, and glaze's
/// reflection for it is not `constexpr` either — see
/// docs/spec/forms/forms.md, "deriving the field name from the member", for the
/// two compile errors this produces). Declare the member in the class
/// and define it just after the closing brace instead:
/// @code{.cpp}
/// struct RecordMeasurement {
///     Choice<std::int64_t, "ListSamples"> sampleId;
///     Density density{};
///     Moisture moisture{};
///
///     static const std::array<morph::forms::FieldMeta, 2> fieldMetadata;
/// };
/// inline const std::array<morph::forms::FieldMeta, 2> RecordMeasurement::fieldMetadata{
///     morph::forms::describe<&RecordMeasurement::sampleId>("Sample", "Which logged sample…"),
///     morph::forms::describe<&RecordMeasurement::moisture>().withReadOnly(),
/// };
/// @endcode
/// The plain `FieldMeta{.field = "sampleId", ...}` literal form has no such
/// restriction and stays a single in-class `static constexpr` array.
/// @tparam MemberPtr Pointer to the member, e.g. `&RecordMeasurement::sampleId`.
/// @param label Display label; empty infers one from the member name.
/// @param help  Help text; empty omits `description`.
/// @return A `FieldMeta` naming @p MemberPtr's wire key, with @p label and @p help set.
template <auto MemberPtr>
[[nodiscard]] FieldMeta describe(std::string_view label = {}, std::string_view help = {}) noexcept {
    return FieldMeta{.field = detail::memberWireName<MemberPtr>(), .label = label, .help = help};
}

/// @brief Rounds every `Quantity` member of @p action to its **declared**
///        precision, so the stored value matches the precision the schema
///        advertises via `x-decimalPlaces`.
///
/// A wire payload carries each `Quantity` with its own runtime `dp`, which a
/// client may set to anything. Left alone, the field is stored at the client's
/// `dp`, silently contradicting the schema's `x-decimalPlaces` (which is the
/// field's compile-time *declared* precision, `Quantity<U, Dec>::declaredDecimals`).
/// Calling this on the decode path — right after `ActionTraits<A>::fromJson` and
/// before dispatch — rounds each `Quantity` to `declaredPrecision()` so the two
/// agree. `atDeclaredPrecision()` performs an **exact `Rational` re-rounding**
/// (half away from zero, the rule the decimal formatter uses), so the value a
/// handler stores is the value the form displays — not a finer one hidden behind
/// a coarser tag. An empty `Quantity` is left empty; non-`Quantity` members are
/// untouched.
///
/// This is the enforcement half of the `x-decimalPlaces` contract: the schema
/// advertises the declared precision and the dispatch path stores at that
/// precision, rather than honouring whatever `dp` the client sent. Precision
/// beyond the declared amount is therefore **discarded, not hidden** — that is
/// the point, and it is why the operation normalises rather than rejects: the
/// same call also lands on server-derived `Quantity` values (see `recomputeOne`),
/// which routinely carry more decimals than the destination field declares.
/// @tparam A     Action type (a reflectable aggregate).
/// @param action Draft action whose `Quantity` members are rounded in place.
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

/// @brief Thrown when a decoded action has a `Quantity` field whose engaged
/// value falls outside its unit's declared bounds (`UnitTraits<E>::bounds`).
///
/// Distinct from `morph::model::ValidationError`: this is a **decode-level**
/// rejection — a wire payload that violates a physical/unit constraint baked
/// into the field's type, caught before an action's own `validate()` (a
/// business-rule check) ever runs. See docs/spec/forms/forms.md, "Pre-decode
/// wire validation — `checkQuantityBounds`".
struct QuantityDecodeError : std::runtime_error {
    /// @brief Constructs the error with a message naming the offending field.
    /// @param fieldName The wire (JSON) name of the out-of-bounds field.
    explicit QuantityDecodeError(std::string_view fieldName)
        : std::runtime_error("quantity field out of declared bounds: " + std::string{fieldName}) {}
};

/// @brief Checks every `Quantity` member of @p action against its unit's
///        declared bounds (`morph::units::Quantity::withinDeclaredBounds`,
///        driven by the optional `UnitTraits<E>::bounds(E)` customisation
///        point).
///
/// This is the **pre-decode wire validation seam**: called on the decode path
/// — right after `ActionTraits<A>::fromJson` and `reconcileDeclaredPrecision`,
/// before `recomputeAll`/`ActionValidator<A>::ready` — so a wire payload
/// carrying a value outside a field's declared physical/unit bounds (e.g. a
/// percentage above 100, a mass below zero) is rejected uniformly at the
/// framework level, before an action's own `validate()` (a business-rule
/// check, not a decode-level one) ever runs. No-op — always returns
/// `std::nullopt` — for actions with no `Quantity` members, or whose
/// `Quantity` members' units declare no `bounds()`: zero behaviour change,
/// backward compatible, exactly like `reconcileDeclaredPrecision`.
/// @tparam A     Action type (a reflectable aggregate).
/// @param action Decoded action to check.
/// @return The wire name of the first out-of-bounds `Quantity` member
///         encountered (in declaration order), or `std::nullopt` when every
///         `Quantity` member is within its declared bounds (or the unit
///         declares none).
template <typename A>
[[nodiscard]] inline std::optional<std::string> checkQuantityBounds(const A& action) {
    using Plain = std::remove_cvref_t<A>;
    std::optional<std::string> offender;
    if constexpr (glz::reflectable<Plain> || glz::glaze_object_t<Plain>) {
        detail::forEachNamedMember(action, [&]<std::size_t I>(std::string_view name, const auto& member) {
            static_cast<void>(I);
            if (offender.has_value()) {
                return;
            }
            using Member = std::remove_cvref_t<decltype(member)>;
            if constexpr (units::isQuantity<Member>) {
                if (!member.withinDeclaredBounds()) {
                    offender = std::string{name};
                }
            }
        });
    } else {
        static_cast<void>(action);
    }
    return offender;
}

/// @brief Runs `checkQuantityBounds<A>(action)` and throws `QuantityDecodeError`
/// naming the first out-of-bounds field, if any. The throwing counterpart used
/// directly on the decode path (registry.hpp/bridge.hpp call sites); a caller
/// that wants the field name without an exception uses `checkQuantityBounds`
/// itself.
/// @tparam A     Action type (a reflectable aggregate).
/// @param action Decoded action to check.
/// @throws QuantityDecodeError if any `Quantity` member is outside its unit's
///         declared bounds.
template <typename A>
inline void enforceQuantityBounds(const A& action) {
    if (auto offender = checkQuantityBounds(action); offender.has_value()) {
        throw QuantityDecodeError{*offender};
    }
}

/// @brief Whether every required empty-capable member of @p action is
///        engaged (has a value).
///
/// Empty-capable covers `Quantity`, `Choice`, `Timestamp`, and any user type
/// satisfying `EmptyCapableField`. Required means: not a `std::optional<...>`
/// member, not listed in `A::optionalFields`, and not the destination of a
/// `A::computedFields` entry (a computed field is never something the user
/// must fill -- see `morph::forms::recomputeAll`). Intended as the body of the
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
            const bool isComputed =
                detail::isComputedDestinationMember(action, static_cast<const void*>(std::addressof(member)));
            if (!detail::declaredOptional<A>(name) && !isComputed && !member.hasValue()) {
                allEngaged = false;
            }
        } else {
            static_cast<void>(name);
            static_cast<void>(member);
        }
    });
    return allEngaged;
}

/// @brief Whether every numeric member of @p action satisfies the
///        `minimum`/`maximum`/`multipleOf` its `A::fieldMetadata` entry
///        declares.
///
/// The evaluating half of the per-field bounds vocabulary: `schemaJson<A>()`
/// serves the same three declarations as JSON-Schema keys, so a client can
/// gate on them, and this is what the server checks — one declaration, two
/// consumers, no second source of truth (`examples/IMPLEMENTATION.md` rule 3).
/// Intended as (part of) the action's `validate()`, which puts it on every
/// dispatch path `ActionValidator<A>::ready` already guards.
///
/// **An unengaged `EmptyCapableField` is vacuously satisfied**, exactly as the
/// `formRules` comparison kinds are: a form still being filled in must not
/// fail a bound on a field that has no value yet, and whether the field has to
/// be filled at all is `required`/`allRequiredEngaged`'s question. A field
/// with no empty state is always checked.
///
/// Bounds are read from these member kinds and no others: `Quantity` (its
/// engaged `math::Rational`, in the canonical unit), a bare `math::Rational`,
/// and an integral member satisfying `detail::BoundCheckableInteger`. A
/// declaration on a member of any other type — a `std::string`, a `bool`, a
/// `std::uint64_t` — is inert here; the schema still advertises it.
/// @tparam A     Action type (a reflectable aggregate).
/// @param action Draft whose fields are checked.
/// @return `true` when no declared bound is violated, and trivially `true`
///         for an action that declares no `fieldMetadata`.
template <typename A>
[[nodiscard]] bool allFieldBoundsSatisfied(const A& action) noexcept {
    if constexpr (!detail::HasFieldMetadata<A>) {
        static_cast<void>(action);
        return true;
    } else {
        bool satisfied = true;
        detail::forEachNamedMember(action, [&]<std::size_t I>(std::string_view name, const auto& member) {
            using Member = std::remove_cvref_t<decltype(member)>;
            const FieldMeta* meta = detail::findFieldMeta<A>(name);
            if (meta == nullptr || !detail::declaresAnyBound(*meta)) {
                return;
            }
            if constexpr (units::isQuantity<Member>) {
                if (member.hasValue() && !detail::satisfiesDeclaredBounds(*meta, *member)) {
                    satisfied = false;
                }
            } else if constexpr (std::same_as<Member, ::morph::math::Rational>) {
                if (!detail::satisfiesDeclaredBounds(*meta, member)) {
                    satisfied = false;
                }
            } else if constexpr (detail::BoundCheckableInteger<Member>) {
                const ::morph::math::Rational value{static_cast<std::int64_t>(member),
                                                    ::morph::math::DecimalPlaces{0}};
                if (!detail::satisfiesDeclaredBounds(*meta, value)) {
                    satisfied = false;
                }
            }
        });
        return satisfied;
    }
}

/// @brief Generates the JSON Schema for action type @p A, ready for a
///        client-side form renderer.
///
/// glaze's `write_json_schema<A>()` output, post-processed with:
///   - a top-level `required` array (see file docs for the rule),
///   - `x-decimalPlaces` on every `Quantity` property (the field's declared
///     precision — see `reconcileDeclaredPrecision`),
///   - `x-order` (declaration index) on every property.
///
/// The result is fixed per type, so it is computed once and cached. On any
/// internal failure the unmerged glaze schema (or an empty string if even
/// that failed) is returned rather than throwing — schema generation is a
/// description facility, never worth crashing a server over.
///
/// The single exception is a **self-contradicting declaration**. When an
/// `A::formRules` entry caps engagement over two or more fields that `A` also
/// makes `required`, the pair describes a form no submission can satisfy, and
/// `UnsatisfiableFormError` is thrown. That is an author error in `A`, decidable
/// at generation time, and every client would otherwise inherit an
/// unsubmittable form (see `detail::rejectUnsatisfiableRules`). Because the
/// cache is a function-local `static`, the throw leaves it uninitialised and a
/// later call re-runs the check rather than serving a half-built schema.
///
/// Returned **by reference**, like `model::payloadFingerprint<A>()` and
/// `model::payloadShapeString<A>()` (`core/payload_schema.hpp`), which cache the
/// same way: the cached string is created once per type per process, is never
/// mutated afterwards, and lives until the process exits, so the reference stays
/// valid for as long as any caller could hold it. Returning it by value made
/// every call — including the per-request ones on a server's descriptor path —
/// an allocation plus a copy of the whole schema for a string the caller almost
/// always only reads. A caller that genuinely needs its own mutable copy asks
/// for one (`std::string mine = schemaJson<A>();`), which is what the by-value
/// signature used to do unconditionally.
/// @tparam A Action type (a reflectable aggregate).
/// @return Reference to the process-lifetime merged schema JSON for `A`.
/// @throws UnsatisfiableFormError if `A::formRules` declares a capping rule
///         (`exactlyOneOf` / `mutuallyExclusive`) over two or more fields that
///         are also in `A`'s derived `required` array.
template <typename A>
[[nodiscard]] const std::string& schemaJson() {
    static const std::string cached =
        detail::mergeSchemaExtras<A>(glz::write_json_schema<A>().value_or(std::string{}));
    return cached;
}

}  // namespace morph::forms
