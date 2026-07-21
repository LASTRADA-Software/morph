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
///   vocabulary (`requiredWhen`, comparisons, membership, presentation)
///   evaluated identically by the schema, the client, and the server. See
///   `morph::forms::allRulesSatisfied` below and docs/spec/forms/forms.md.
/// - **`x-computed` / `x-readonly`** — for a member listed as the destination
///   of an action's `computedFields` declaration: the field is derived from
///   sibling inputs (named in `x-computed.inputs`) and must not be rendered as
///   an editable control (`x-readonly: true`). See `morph::forms::computed`,
///   `morph::forms::computeList`, and `morph::forms::recomputeAll`.
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
#include <cctype>
#include <compare>
#include <concepts>
#include <cstddef>
#include <glaze/glaze.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../util/quantity.hpp"
#include "choice.hpp"
#include "layout.hpp"

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
        default:
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
/// `greaterOrEqual`, `less`, `lessOrEqual`, added in a later task): an
/// `EmptyCapableField` whose engaged value (`operator*()`) is three-way
/// comparable to itself — satisfied by `Quantity` (dereferences to
/// `math::Rational`) and `morph::time::Timestamp` (dereferences to
/// `DateTime`), matching forms.md's "numeric / Timestamp" scope for
/// comparisons.
template <typename V>
concept ComparableField = ::morph::forms::EmptyCapableField<V> && requires(const V& value) {
    { *value <=> *value };
};

}  // namespace detail

/// @brief Broader than `EmptyCapableField`: also covers a plain
/// `std::optional<T>` member (e.g. `std::optional<std::string> email`),
/// which does **not** satisfy `EmptyCapableField` — it exposes
/// `has_value()`, not `hasValue()` (see forms.md's `allRequiredEngaged`
/// "two exclusions" note). The cross-field rule vocabulary's engagement
/// checks (`engaged`, `notEngaged`, `requiredWhen`, and the membership rules
/// added in a later task) accept either kind of field, since the planned
/// spec's own worked example ranges an `exactlyOneOf` over two plain
/// `std::optional<std::string>` fields.
template <typename T>
concept EngageableField = EmptyCapableField<T> || detail::isStdOptional<T>;

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

    /// @brief Evaluates the condition against @p action.
    /// @param action The action snapshot to inspect.
    /// @return `true` when the field is engaged.
    [[nodiscard]] constexpr bool test(const A& action) const noexcept { return detail::isEngaged(action.*field); }

    /// @brief Emits this condition's `x-rules` JSON node.
    /// @return `{"kind":"engaged","fields":["<wire name>"]}`.
    [[nodiscard]] glz::generic_u64 emit() const {
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

    /// @brief Evaluates the condition against @p action.
    /// @param action The action snapshot to inspect.
    /// @return `true` when the field is **not** engaged.
    [[nodiscard]] constexpr bool test(const A& action) const noexcept { return !detail::isEngaged(action.*field); }

    /// @brief Emits this condition's `x-rules` JSON node.
    /// @return `{"kind":"notEngaged","fields":["<wire name>"]}`.
    [[nodiscard]] glz::generic_u64 emit() const {
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
    [[nodiscard]] glz::generic_u64 emit() const {
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
    [[nodiscard]] glz::generic_u64 emit() const {
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
    [[nodiscard]] glz::generic_u64 emit() const {
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
    [[nodiscard]] glz::generic_u64 emit() const {
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
                      std::same_as<T, ::morph::math::Rational>;

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
    [[nodiscard]] glz::generic_u64 emit() const {
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
        } else {
            node["value"] = literal;
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
[[nodiscard]] constexpr auto equals(V A::* field, L literal) {
    return Equals<V, A, L>{field, std::move(literal)};
}

/// @brief `equals` overload for a string-literal argument (`equals(&A::code,
/// "X")`), so callers do not have to spell `std::string{"X"}` explicitly.
/// @tparam V Field member type (deduced).
/// @tparam A Action type (deduced).
/// @tparam N String literal length (deduced), including the trailing `'\0'`.
/// @param field   Pointer to the member to test.
/// @param literal The string literal to compare against.
/// @return The condition node, with the literal stored as `std::string`.
template <typename V, typename A, std::size_t N>
[[nodiscard]] constexpr auto equals(V A::* field, const char (&literal)[N]) {
    return Equals<V, A, std::string>{field, std::string{literal}};
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
    [[nodiscard]] glz::generic_u64 emit() const {
        glz::generic_u64 node{};
        node["kind"] = std::string{detail::ruleKindName(kind)};
        glz::generic_u64::array_t fields{};
        fields.emplace_back(detail::resolveFieldName(field));
        node["fields"] = fields;
        node["when"] = when.emit();
        return node;
    }
};

/// @brief Builds a `RequiredWhen<V, A, Cond>` rule: @p field becomes
/// required exactly when @p when holds.
/// @tparam V    Field member type (deduced; must satisfy `EngageableField`).
/// @tparam A    Action type (deduced).
/// @tparam Cond Condition node type (deduced).
/// @param field Pointer to the member that becomes conditionally required.
/// @param when  The condition node (`engaged(...)`, `notEngaged(...)`, or —
///              starting a later task — a comparison or `equals(...)`).
/// @return The rule node.
template <typename V, typename A, typename Cond>
    requires EngageableField<V>
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
    [[nodiscard]] glz::generic_u64 emit() const {
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
    [[nodiscard]] glz::generic_u64 emit() const {
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
    [[nodiscard]] glz::generic_u64 emit() const {
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
    [[nodiscard]] glz::generic_u64 emit() const {
        glz::generic_u64 node{};
        node["kind"] = std::string{detail::ruleKindName(kind)};
        glz::generic_u64::array_t fields{};
        fields.emplace_back(detail::resolveFieldName(field));
        node["fields"] = fields;
        node["when"] = when.emit();
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
template <typename V, typename A, typename Cond>
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
    [[nodiscard]] glz::generic_u64 emit() const {
        glz::generic_u64 node{};
        node["kind"] = std::string{detail::ruleKindName(kind)};
        glz::generic_u64::array_t fields{};
        fields.emplace_back(detail::resolveFieldName(field));
        node["fields"] = fields;
        node["when"] = when.emit();
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
template <typename V, typename A, typename Cond>
[[nodiscard]] constexpr auto readonlyWhen(V A::* field, Cond when) {
    return ReadonlyWhen<V, A, Cond>{field, when};
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

namespace detail {

/// @brief Evaluates @p rule against @p action, skipping presentation rules
/// (`VisibleWhen` / `ReadonlyWhen`, added in a later task) by construction —
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
/// destination's own `DeclaredDecimals`) and then retagged to that type's
/// declared precision (`Quantity::atDeclaredPrecision()`), so the stored value
/// matches the field's advertised `x-decimalPlaces` regardless of what
/// declared precision `Fn`'s return type happened to carry. If any declared
/// input is unengaged, the destination is instead reset to its
/// default-constructed (empty, for `Quantity`/`Choice`/`Timestamp`) value
/// rather than computed from a missing operand.
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

/// @brief The DOM post-merge behind `schemaJson`: adds the derived `required`
///        array, `x-order`, `x-decimalPlaces`, and (for actions declaring
///        `computedFields`) `x-computed`/`x-readonly` to a glaze-produced schema.
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
    // Wire keys of every reflected member, in declaration order — reused
    // below to silently ignore a formLayout/fieldSpans entry that names a
    // field the action does not actually have (schema generation never
    // throws over an author's declaration mistake).
    std::vector<std::string_view> memberNames{};
    A probe{};

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
            if (!fieldMeta->i18nKey.empty()) {
                property["x-i18nKey"] = std::string{fieldMeta->i18nKey};
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

            // Sibling fields whose current values parameterise the options
            // action (cascading picklists). Omitted entirely for an
            // independent Choice, so the emitted schema is byte-for-byte
            // unchanged from before this key existed.
            if constexpr (!Member::optionsDependsOn().empty()) {
                glz::generic_u64::array_t dependsOn{};
                for (auto const& parentName : Member::optionsDependsOn()) {
                    dependsOn.emplace_back(std::string{parentName});
                }
                property["x-optionsDependsOn"] = dependsOn;
            }
        }

        // Widget hint: the field type's own widget() (e.g. Multiline,
        // Ranged), overridden — if present — by a fieldMetadata-shaped entry
        // naming this field. The override always wins over the type-derived
        // default.
        std::string_view widgetHint{};
        if constexpr (DeclaresWidget<Member>) {
            widgetHint = Member::widget();
        }
        if constexpr (HasFieldMetadataWidgets<A>) {
            if (auto const overrideWidget = widgetOverride<A>(name); !overrideWidget.empty()) {
                widgetHint = overrideWidget;
            }
        }
        if (!widgetHint.empty()) {
            property["x-widget"] = std::string{widgetHint};
        }
        if constexpr (DeclaresRangedBounds<Member>) {
            // The slider's control track: a UI hint, not a validation bound
            // (glaze's own minimum/maximum, when present, stay authoritative
            // for validation regardless of what x-widget ends up here).
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
        // schema generation never throws over an author's declaration
        // mistake).
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
    // and future exposes the same emit() -> glz::generic_u64 shape, so this
    // loop needs no changes as new rule kinds are added.
    if constexpr (HasFormRules<A>) {
        glz::generic_u64::array_t xRules{};
        std::apply([&](const auto&... rule) { (xRules.emplace_back(rule.emit()), ...); }, A::formRules.rules);
        dom["x-rules"] = xRules;
    }

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
/// reflection for it is not `constexpr` either — see this feature's plan for
/// the two compile errors this produces). Declare the member in the class
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
