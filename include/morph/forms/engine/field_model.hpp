// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/field_model.hpp
/// @brief `FormModel`: the immutable runtime model of one action's form, read from the schema
///        JSON `schemaJson<A>()` emits (or any document in that vocabulary).
///
/// Specified in `docs/spec/forms/engine.md`, "The field model".

#include <algorithm>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../util/rational.hpp"
#include "../blank_as.hpp"
#include "../layout.hpp"
#include "detail/text.hpp"
#include "rules.hpp"

namespace morph::forms {

/// @brief The control a field gets, which the JSON type alone does not say.
enum class FieldKind : std::uint8_t {
    Text,           ///< A string.
    Multiline,      ///< A string edited over several lines (`x-widget: textarea`).
    Integer,        ///< An integer of any width; every digit is kept.
    Number,         ///< A plain JSON number (a `double`/`float` member).
    Decimal,        ///< An exact `{num, den, dp}` with a declared precision and no unit.
    Quantity,       ///< An exact `{num, den, dp}` in a unit, with optional convertible alternatives.
    Boolean,        ///< `true` / `false`.
    Enum,           ///< A closed set the schema states.
    Choice,         ///< A set fetched by executing an options action.
    DateTime,       ///< An instant, entered in the display zone, sent as ISO-8601 UTC.
    Date,           ///< A calendar date, `YYYY-MM-DD`.
    EpochDateTime,  ///< An instant as int64 seconds since the epoch (UTC): `x-widget: epochSeconds` on an integer.
    EpochDate,      ///< A calendar date as int64 days since the epoch: `x-widget: epochDays` on an integer.
    Slider,         ///< A number on a track (`x-widget: slider` with `x-min`/`x-max`).
    Array,          ///< A list of scalars, entered comma-separated.
    Object,         ///< A nested object.
    ObjectArray     ///< A list of nested objects.
};

/// @brief The stable name of a field kind (what `Overrides::byKind` documents and tests print).
/// @param kind The kind.
/// @return `"text"`, `"multiline"`, `"integer"`, `"number"`, `"decimal"`, `"quantity"`, `"boolean"`,
///         `"enum"`, `"choice"`, `"datetime"`, `"date"`, `"epochDateTime"`, `"epochDate"`, `"slider"`, `"array"`,
///         `"object"` or `"objectArray"`.
[[nodiscard]] constexpr std::string_view fieldKindName(FieldKind kind) noexcept {
    switch (kind) {
        case FieldKind::Text:
            return "text";
        case FieldKind::Multiline:
            return "multiline";
        case FieldKind::Integer:
            return "integer";
        case FieldKind::Number:
            return "number";
        case FieldKind::Decimal:
            return "decimal";
        case FieldKind::Quantity:
            return "quantity";
        case FieldKind::Boolean:
            return "boolean";
        case FieldKind::Enum:
            return "enum";
        case FieldKind::Choice:
            return "choice";
        case FieldKind::DateTime:
            return "datetime";
        case FieldKind::Date:
            return "date";
        case FieldKind::EpochDateTime:
            return "epochDateTime";
        case FieldKind::EpochDate:
            return "epochDate";
        case FieldKind::Slider:
            return "slider";
        case FieldKind::Array:
            return "array";
        case FieldKind::Object:
            return "object";
        case FieldKind::ObjectArray:
            return "objectArray";
        default:
            return "text";
    }
}

/// @brief When a form submits.
enum class SubmitMode : std::uint8_t {
    Automatic,  ///< Whenever it becomes ready with a changed body (no `x-submitMode`).
    Explicit    ///< Only on `FormSession::submit()` (`x-submitMode: explicit`).
};

/// @brief How many object levels below the action a schema is described; a deeper member is
///        `FieldSpec::truncated`.
inline constexpr std::size_t kMaxNestingDepth = 4;

/// @brief One unit a Quantity can be entered in; the first of a field's units is canonical.
struct UnitOption {
    /// @brief The unit id (`ExtUnits.unitAscii`, or the alternative's `id`).
    std::string id;
    /// @brief What a selector shows.
    std::string display;
    /// @brief Fraction digits accepted in this unit.
    std::uint32_t decimals = 0;
    /// @brief Numerator of the exact unit-to-canonical ratio.
    std::int64_t num = 1;
    /// @brief Denominator of the exact unit-to-canonical ratio.
    std::int64_t den = 1;
    /// @brief Member-wise equality.
    /// @param other The other unit.
    /// @return `true` when every member matches.
    bool operator==(UnitOption const& other) const = default;
};

/// @brief One selectable value of an Enum or a Choice.
struct ChoiceOption {
    /// @brief The value as the JSON literal the body carries (`"\"Viewer\""`, `"9007199254740993"`).
    std::string valueJson;
    /// @brief What the user sees.
    std::string label;
    /// @brief Member-wise equality.
    /// @param other The other option.
    /// @return `true` when value and label match.
    bool operator==(ChoiceOption const& other) const = default;
};

/// @brief Where a Choice's options come from (`x-optionsAction` and its companions).
struct ChoiceSpec {
    /// @brief The action whose result rows are the options.
    std::string optionsAction;
    /// @brief The row member submitted as the value (`x-optionValue`).
    std::string valueField = "id";
    /// @brief The row member shown (`x-optionLabel`).
    std::string labelField = "name";
    /// @brief Sibling wire names whose values parameterise the options request (`x-optionsDependsOn`).
    std::vector<std::string> dependsOn;
    /// @brief Member-wise equality.
    /// @param other The other descriptor.
    /// @return `true` when every member matches.
    bool operator==(ChoiceSpec const& other) const = default;
};

/// @brief Everything the engine knows about one field, from its schema property.
// NOLINTNEXTLINE(misc-no-recursion): a field's members are fields, so copying one copies them.
struct FieldSpec {
    /// @brief The wire name.
    std::string name;
    /// @brief The path from the action: `name`, `parent.name`, or `rows[].name` for a collection's member.
    std::string path;
    /// @brief The control and encoder.
    FieldKind kind = FieldKind::Text;
    /// @brief The first declared JSON type (`"integer"`, …), empty when the property declares none.
    std::string jsonType;
    /// @brief The schema's title, or the wire name.
    std::string title;
    /// @brief The schema's `description`.
    std::string help;
    /// @brief `x-placeholder`.
    std::string placeholder;
    /// @brief `x-i18nKey`: the explicit message-key stem, empty when none is declared.
    std::string i18nKey;
    /// @brief Named in the containing object's `required`.
    bool required = false;
    /// @brief `x-blankAs` (Text and Multiline only): whether a cleared field submits `""`.
    BlankAs blankAs = BlankAs::Omit;
    /// @brief `x-readonly`.
    bool readOnly = false;
    /// @brief `x-hidden`.
    bool hidden = false;
    /// @brief `x-computed.inputs`: the members a computed field derives from.
    std::vector<std::string> computedInputs;
    /// @brief `minimum`, as canonical decimal text.
    std::optional<std::string> minimum;
    /// @brief `maximum`, as canonical decimal text.
    std::optional<std::string> maximum;
    /// @brief A positive `multipleOf`, as canonical decimal text.
    std::optional<std::string> multipleOf;
    /// @brief `x-exactMinimum`: an integer bound's exact digits.
    std::optional<std::string> exactMinimum;
    /// @brief `x-exactMaximum`: an integer bound's exact digits.
    std::optional<std::string> exactMaximum;
    /// @brief `x-minimum`: a per-instance lower bound (`InstanceConstraints::decorate`).
    std::optional<math::Rational> instanceMinimum;
    /// @brief `x-maximum`: a per-instance upper bound.
    std::optional<math::Rational> instanceMaximum;
    /// @brief `x-decimalPlaces`: the declared precision of a Decimal or Quantity.
    std::optional<std::uint32_t> decimalPlaces;
    /// @brief `x-displayDecimals`: a plain Number's entry and display precision.
    std::optional<std::uint32_t> displayDecimals;
    /// @brief The display unit (`ExtUnits.unitUnicode`, else `unitAscii`), empty when none.
    std::string unit;
    /// @brief `ExtUnits.unitAscii`.
    std::string unitAscii;
    /// @brief A Decimal's or Quantity's entry units: the canonical one first, then `x-unitAlternatives`.
    std::vector<UnitOption> units;
    /// @brief The index into `units` a draft starts in (and returns to on prefill and reset): 0, the canonical
    ///        unit, unless a field overlay's `unit` names another.
    std::size_t entryUnit = 0;
    /// @brief An Enum's options, in schema order.
    std::vector<ChoiceOption> enumOptions;
    /// @brief A Choice's options source.
    std::optional<ChoiceSpec> choice;
    /// @brief `x-section`: the index of the `x-layout` group the field sits in.
    std::optional<std::size_t> group;
    /// @brief `x-colspan`: grid columns the field spans (at least 1).
    int colspan = 1;
    /// @brief `x-widget`, empty when none.
    std::string widget;
    /// @brief A Slider's track minimum (`x-min`).
    std::int64_t sliderMin = 0;
    /// @brief A Slider's track maximum (`x-max`).
    std::int64_t sliderMax = 100;
    /// @brief A Slider's increment (`x-step`, at least 1).
    std::int64_t sliderStep = 1;
    /// @brief An Array's item JSON type (`"string"` when undeclared).
    std::string itemType;
    /// @brief An Object's members, or an ObjectArray's element members, in `x-order`.
    std::vector<FieldSpec> children;
    /// @brief An Object or ObjectArray whose members are not described: its `$ref` is already on
    ///        the path, it is deeper than `kMaxNestingDepth`, or its schema states no members.
    bool truncated = false;
};

/// @brief What a document (or an application) changes about one top-level field before the session is built: a
///        `form` node's `fields` entry (spec 6 §10). An unset member keeps the schema's.
struct FieldOverlay {
    /// @brief Removes the member from the form and from its body.
    bool omit = false;
    /// @brief Keeps the member in the body and out of the view (`x-hidden`).
    bool hidden = false;
    /// @brief Replaces the label (`title`).
    std::optional<std::string> label;
    /// @brief The unit a Quantity is entered in: the id of one of its units (its canonical unit or an alternative).
    ///        On a field without units, the unit shown beside it.
    std::optional<std::string> unit;
    /// @brief The places a Decimal or Quantity is entered with in its entry unit, or a Number's display decimals.
    ///        The wire value keeps the schema's places.
    std::optional<std::uint32_t> decimals;
    /// @brief Replaces `x-widget`, so the kind is read again (`textarea`, `radio`, `slider`, `epochDays`, …).
    std::optional<std::string> widget;
    /// @brief Replaces `x-blankAs`.
    std::optional<BlankAs> blankAs;
};

/// @brief The overlays of a form's fields, by wire name.
using FieldOverlays = std::map<std::string, FieldOverlay, std::less<>>;

/// @brief One `x-layout` group, or the implicit group of the fields no group names.
struct FieldGroupSpec {
    /// @brief The group's title (empty for the implicit group).
    std::string title;
    /// @brief Section, tab or accordion; the implicit group is a `Section` with `implicit` set.
    GroupKind kind = GroupKind::Section;
    /// @brief The wire names of the fields in the group, in `x-order`.
    std::vector<std::string> fields;
    /// @brief The trailing group of fields no declared group names (or every field, without `x-layout`).
    bool implicit = false;
};

namespace detail::engine {

/// @brief Whether @p range holds @p value.
/// @param range A range.
/// @param value The value to look for.
/// @return `true` when an element equals @p value.
[[nodiscard]] inline bool contains(auto const& range, auto const& value) {
    return std::ranges::find(range, value) != std::ranges::end(range);
}

/// @brief Bounds-checked element access for a span, which has no `at()` before C++26.
/// @tparam T The element type.
/// @param items The span.
/// @param index The position.
/// @return The element at @p index.
/// @throws std::out_of_range when @p index is not below `items.size()`.
template <class T>
[[nodiscard]] T& elementAt(std::span<T> items, std::size_t index) {
    if (index >= items.size()) {
        throw std::out_of_range{"morph::forms: index " + std::to_string(index) + " past a span of " +
                                std::to_string(items.size())};
    }
    return *std::next(items.begin(), static_cast<std::ptrdiff_t>(index));
}

/// @brief Reads property schemas: resolves `$ref` into `$defs`, collapses nullable
///        `anyOf`/`oneOf`, and describes an object's properties as `FieldSpec`s.
class SchemaReader {
public:
    /// @param root The whole schema document. Borrowed: it must outlive the reader.
    explicit SchemaReader(Json const& root) : _defs{member(root, "$defs")} {}

    /// @brief Follows a `$ref` into `$defs`; the property's own keys win over the definition's.
    /// @param prop A property schema.
    /// @return The merged object (`{}` for a non-object).
    [[nodiscard]] Json resolveRef(Json const& prop) const {
        if (!prop.is_object()) {
            return emptyObject();
        }
        auto const* const ref = stringAt(prop, "$ref");
        if (ref == nullptr) {
            return prop;
        }
        std::string_view name{*ref};
        name = name.substr(name.rfind('/') + 1);
        Json merged = emptyObject();
        if (Json const* const def = _defs == nullptr ? nullptr : member(*_defs, name);
            def != nullptr && def->is_object()) {
            merged = *def;
        }
        for (auto const& [key, value] : prop.get_object()) {
            merged.get_object().insert_or_assign(key, value);
        }
        return merged;
    }

    /// @brief Resolves `$ref`, then collapses a nullable `anyOf`/`oneOf` onto its first non-null
    ///        branch; the outer keys win, and the branches' `anyOf`/`oneOf`/`const` are dropped.
    /// @param prop A property schema.
    /// @return The resolved property.
    [[nodiscard]] Json resolveProp(Json const& prop) const {
        Json resolved = resolveRef(prop);
        Json const* branches = member(resolved, "anyOf");
        if (branches == nullptr || !branches->is_array()) {
            branches = member(resolved, "oneOf");
        }
        if (branches == nullptr || !branches->is_array()) {
            return resolved;
        }
        auto const dropped = [](std::string_view key) { return key == "anyOf" || key == "oneOf" || key == "const"; };
        for (Json const& raw : branches->get_array()) {
            Json const branch = resolveRef(raw);
            if (auto const* const type = stringAt(branch, "type"); type != nullptr && *type == "null") {
                continue;
            }
            Json merged = emptyObject();
            for (auto const& [key, value] : branch.get_object()) {
                if (member(resolved, key) == nullptr && !dropped(key)) {
                    merged.get_object().insert_or_assign(key, value);
                }
            }
            for (auto const& [key, value] : resolved.get_object()) {
                if (!dropped(key)) {
                    merged.get_object().insert_or_assign(key, value);
                }
            }
            return merged;
        }
        return resolved;
    }

    /// @brief Describes an object schema's properties, in `x-order` (stable for ties).
    /// @param objectSchema The object schema.
    /// @param depth        0 for the action, one more per nested level.
    /// @param pathPrefix   Prefix of every member's path (`""`, `"parent."`, `"rows[]."`).
    /// @param refChain     The `$defs` references on the path here.
    /// @return The fields, or the error naming the malformed property.
    // NOLINTNEXTLINE(misc-no-recursion) -- an object's members are described by the same walk, to kMaxNestingDepth
    [[nodiscard]] std::expected<std::vector<FieldSpec>, SchemaError> describe(
        Json const& objectSchema, std::size_t depth, std::string const& pathPrefix,
        std::vector<std::string> const& refChain) const {
        std::vector<FieldSpec> fields;
        Json const* const props = member(objectSchema, "properties");
        if (props == nullptr) {
            return fields;
        }
        if (!props->is_object()) {
            return std::unexpected(
                SchemaError{.path = pathPrefix + "properties", .message = "\"properties\" must be an object"});
        }
        std::vector<std::string> required;
        if (Json const* const list = member(objectSchema, "required"); list != nullptr && list->is_array()) {
            for (Json const& entry : list->get_array()) {
                if (auto const* const text = entry.get_if<std::string>()) {
                    required.push_back(*text);
                }
            }
        }
        std::vector<std::pair<std::string, std::uint64_t>> order;
        for (auto const& [name, prop] : props->get_object()) {
            order.emplace_back(name, unsignedOf(member(prop, "x-order")).value_or(0));
        }
        std::ranges::stable_sort(order, {}, &std::pair<std::string, std::uint64_t>::second);
        for (auto const& [name, ignoredOrder] : order) {
            static_cast<void>(ignoredOrder);
            Json const& raw = *member(*props, name);
            if (!raw.is_object()) {
                std::string path{"properties."};
                path += pathPrefix;
                path += name;
                return std::unexpected(
                    SchemaError{.path = std::move(path), .message = "a property schema must be a JSON object"});
            }
            auto field = describeOne(raw, name, depth, pathPrefix, refChain, contains(required, name));
            if (!field) {
                return std::unexpected(field.error());
            }
            fields.push_back(*std::move(field));
        }
        return fields;
    }

    /// @brief A non-negative integer member, also from an integral double.
    /// @param value The member, or null.
    /// @return The value, or `std::nullopt`.
    [[nodiscard]] static std::optional<std::uint64_t> unsignedOf(Json const* value) {
        if (value == nullptr) {
            return std::nullopt;
        }
        if (auto const* const asUnsigned = value->get_if<std::uint64_t>()) {
            return *asUnsigned;
        }
        if (auto const* const asSigned = value->get_if<std::int64_t>(); asSigned != nullptr && *asSigned >= 0) {
            return static_cast<std::uint64_t>(*asSigned);
        }
        if (auto const* const asDouble = value->get_if<double>();
            asDouble != nullptr && *asDouble >= 0 && *asDouble == std::floor(*asDouble) && *asDouble < 1.8e19) {
            return static_cast<std::uint64_t>(*asDouble);
        }
        return std::nullopt;
    }

    /// @brief A signed integer member; a double is truncated toward zero.
    /// @param value The member, or null.
    /// @return The value, or `std::nullopt`.
    [[nodiscard]] static std::optional<std::int64_t> signedOf(Json const* value) {
        if (value == nullptr) {
            return std::nullopt;
        }
        if (auto const* const asSigned = value->get_if<std::int64_t>()) {
            return *asSigned;
        }
        if (auto const* const asUnsigned = value->get_if<std::uint64_t>();
            asUnsigned != nullptr && std::in_range<std::int64_t>(*asUnsigned)) {
            return static_cast<std::int64_t>(*asUnsigned);
        }
        if (auto const* const asDouble = value->get_if<double>();
            asDouble != nullptr && std::isfinite(*asDouble) && std::abs(*asDouble) < 9.2e18) {
            return static_cast<std::int64_t>(*asDouble);
        }
        return std::nullopt;
    }

private:
    [[nodiscard]] static std::vector<std::string> jsonTypes(Json const& prop) {
        std::vector<std::string> types;
        Json const* const type = member(prop, "type");
        if (type == nullptr) {
            return types;
        }
        if (auto const* const one = type->get_if<std::string>()) {
            types.push_back(*one);
        } else if (type->is_array()) {
            for (Json const& entry : type->get_array()) {
                if (auto const* const text = entry.get_if<std::string>()) {
                    types.push_back(*text);
                }
            }
        }
        return types;
    }

    /// The key from the raw property first, then from the resolved one (the QML reader's order).
    [[nodiscard]] static Json const* either(Json const& raw, Json const& resolved, std::string_view key) {
        Json const* const own = member(raw, key);
        return own != nullptr ? own : member(resolved, key);
    }

    [[nodiscard]] static std::string textOf(Json const& raw, Json const& resolved, std::string_view key) {
        Json const* const value = either(raw, resolved, key);
        auto const* const text = value == nullptr ? nullptr : value->get_if<std::string>();
        return text == nullptr ? std::string{} : *text;
    }

    [[nodiscard]] static bool flagOf(Json const& raw, Json const& resolved, std::string_view key) {
        Json const* const value = either(raw, resolved, key);
        auto const* const flag = value == nullptr ? nullptr : value->get_if<bool>();
        return flag != nullptr && *flag;
    }

    [[nodiscard]] static std::string refNameOf(Json const& raw) {
        if (auto const* const ref = stringAt(raw, "$ref")) {
            return *ref;
        }
        for (std::string_view const key : {"anyOf", "oneOf"}) {
            if (Json const* const branches = member(raw, key); branches != nullptr && branches->is_array()) {
                for (Json const& branch : branches->get_array()) {
                    if (auto const* const ref = stringAt(branch, "$ref")) {
                        return *ref;
                    }
                }
            }
        }
        return {};
    }

    // NOLINTNEXTLINE(misc-no-recursion) -- one nested level of branches, bounded by `nested`
    [[nodiscard]] std::optional<std::vector<ChoiceOption>> constBranches(Json const& prop, bool nested) const {
        Json const* branches = member(prop, "anyOf");
        if (branches == nullptr || !branches->is_array()) {
            branches = member(prop, "oneOf");
        }
        if (branches == nullptr || !branches->is_array()) {
            return std::nullopt;
        }
        std::vector<ChoiceOption> rows;
        for (Json const& raw : branches->get_array()) {
            Json const branch = resolveRef(raw);
            if (auto const* const type = stringAt(branch, "type"); type != nullptr && *type == "null") {
                continue;
            }
            if (Json const* const literal = member(branch, "const")) {
                auto const* const title = stringAt(branch, "title");
                rows.push_back(ChoiceOption{.valueJson = toJson(*literal),
                                            .label = title != nullptr ? *title : displayText(*literal)});
                continue;
            }
            auto inner = nested ? constBranches(branch, false) : std::nullopt;
            if (!inner || inner->empty()) {
                return std::nullopt;
            }
            rows.insert(rows.end(), inner->begin(), inner->end());
        }
        return rows;
    }

    [[nodiscard]] std::vector<ChoiceOption> enumChoices(Json const& raw) const {
        Json const prop = resolveRef(raw);
        if (Json const* const literals = member(prop, "enum"); literals != nullptr && literals->is_array()) {
            std::vector<ChoiceOption> rows;
            for (Json const& literal : literals->get_array()) {
                if (!literal.is_null()) {
                    rows.push_back(ChoiceOption{.valueJson = toJson(literal), .label = displayText(literal)});
                }
            }
            return rows;
        }
        return constBranches(prop, true).value_or(std::vector<ChoiceOption>{});
    }

    [[nodiscard]] static std::optional<math::Rational> rationalOf(Json const* node) {
        if (node == nullptr) {
            return std::nullopt;
        }
        auto const num = signedOf(member(*node, "num"));
        auto const den = signedOf(member(*node, "den"));
        if (!num || !den || *den == 0) {
            return std::nullopt;
        }
        auto const places = unsignedOf(member(*node, "dp")).value_or(0);
        return math::Rational{
            math::Numerator{*num}, math::Denominator{*den},
            math::DecimalPlaces{static_cast<std::uint32_t>(std::min<std::uint64_t>(places, math::kMaxDecimalPlaces))}};
    }

    // The other half of describe's walk, with one branch per schema keyword a field reads.
    // NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity)
    [[nodiscard]] std::expected<FieldSpec, SchemaError> describeOne(Json const& raw, std::string const& name,
                                                                    std::size_t depth, std::string const& pathPrefix,
                                                                    std::vector<std::string> const& refChain,
                                                                    bool required) const {
        Json const prop = resolveProp(raw);
        std::vector<std::string> const types = jsonTypes(prop);
        auto const hasType = [&](std::string_view type) { return contains(types, type); };
        FieldSpec spec{};
        spec.name = name;
        spec.path = pathPrefix + name;
        spec.jsonType = types.empty() ? std::string{} : types.front();
        spec.title = textOf(raw, prop, "title");
        if (spec.title.empty()) {
            spec.title = name;
        }
        spec.help = textOf(prop, prop, "description");
        spec.placeholder = textOf(raw, prop, "x-placeholder");
        spec.i18nKey = textOf(raw, prop, "x-i18nKey");
        spec.required = required;
        spec.readOnly = flagOf(raw, prop, "x-readonly");
        spec.hidden = flagOf(raw, prop, "x-hidden");
        spec.widget = textOf(raw, prop, "x-widget");
        if (Json const* const computed = either(raw, prop, "x-computed")) {
            if (Json const* const inputs = member(*computed, "inputs"); inputs != nullptr && inputs->is_array()) {
                for (Json const& input : inputs->get_array()) {
                    if (auto const* const text = input.get_if<std::string>()) {
                        spec.computedInputs.push_back(*text);
                    }
                }
            }
        }
        if (Json const* const minimum = member(prop, "minimum")) {
            spec.minimum = numberText(*minimum);
        }
        if (Json const* const maximum = member(prop, "maximum")) {
            spec.maximum = numberText(*maximum);
        }
        if (Json const* const step = member(prop, "multipleOf")) {
            auto text = numberText(*step);
            if (text && compareDecimal(*text, "0") == std::strong_ordering::greater) {
                spec.multipleOf = std::move(text);
            }
        }
        if (auto const* const exact = stringAt(prop, "x-exactMinimum");
            exact != nullptr && isCanonicalInteger(*exact)) {
            spec.exactMinimum = *exact;
        }
        if (auto const* const exact = stringAt(prop, "x-exactMaximum");
            exact != nullptr && isCanonicalInteger(*exact)) {
            spec.exactMaximum = *exact;
        }
        spec.instanceMinimum = rationalOf(either(raw, prop, "x-minimum"));
        spec.instanceMaximum = rationalOf(either(raw, prop, "x-maximum"));
        if (auto const places = unsignedOf(either(raw, prop, "x-decimalPlaces"))) {
            spec.decimalPlaces = static_cast<std::uint32_t>(std::min<std::uint64_t>(*places, math::kMaxDecimalPlaces));
        }
        if (Json const* const units = member(prop, "ExtUnits")) {
            auto const* const ascii = stringAt(*units, "unitAscii");
            auto const* const unicode = stringAt(*units, "unitUnicode");
            spec.unitAscii = ascii != nullptr ? *ascii : std::string{};
            spec.unit = unicode != nullptr && !unicode->empty() ? *unicode : spec.unitAscii;
        }
        if (auto const section = unsignedOf(either(raw, prop, "x-section"))) {
            spec.group = static_cast<std::size_t>(*section);
        }
        spec.colspan =
            static_cast<int>(std::clamp<std::int64_t>(signedOf(either(raw, prop, "x-colspan")).value_or(1), 1, 1024));

        auto const* const optionsAction = [&] -> std::string const* {
            Json const* const value = either(raw, prop, "x-optionsAction");
            return value == nullptr ? nullptr : value->get_if<std::string>();
        }();
        std::vector<ChoiceOption> enumRows = optionsAction != nullptr ? std::vector<ChoiceOption>{} : enumChoices(raw);
        auto const format = textOf(prop, prop, "format");
        auto const sliderMin = signedOf(either(raw, prop, "x-min"));
        auto const sliderMax = signedOf(either(raw, prop, "x-max"));
        std::string const refName = refNameOf(raw);
        bool const describesNested = depth < kMaxNestingDepth && (refName.empty() || !contains(refChain, refName));
        std::vector<std::string> nestedChain = refChain;
        if (!refName.empty()) {
            nestedChain.push_back(refName);
        }

        if (hasType("array")) {
            Json const* const rawItems = member(prop, "items");
            Json const items = rawItems == nullptr ? emptyObject() : resolveProp(*rawItems);
            std::vector<std::string> const itemTypes = jsonTypes(items);
            if (contains(itemTypes, "object") || contains(itemTypes, "array")) {
                spec.kind = FieldKind::ObjectArray;
                std::string const itemRef = rawItems == nullptr ? std::string{} : refNameOf(*rawItems);
                bool const itemsDescribed = depth < kMaxNestingDepth && member(items, "properties") != nullptr &&
                                            (itemRef.empty() || !contains(refChain, itemRef));
                if (itemsDescribed) {
                    std::vector<std::string> itemChain = refChain;
                    if (!itemRef.empty()) {
                        itemChain.push_back(itemRef);
                    }
                    auto children = describe(items, depth + 1, spec.path + "[].", itemChain);
                    if (!children) {
                        return std::unexpected(children.error());
                    }
                    spec.children = *std::move(children);
                } else {
                    spec.truncated = true;
                }
            } else {
                spec.kind = FieldKind::Array;
                spec.itemType = itemTypes.empty() ? std::string{"string"} : itemTypes.front();
            }
        } else if (!enumRows.empty()) {
            spec.kind = FieldKind::Enum;
            spec.enumOptions = std::move(enumRows);
        } else if (optionsAction != nullptr) {
            spec.kind = FieldKind::Choice;
            ChoiceSpec choice{};
            choice.optionsAction = *optionsAction;
            if (auto value = textOf(raw, prop, "x-optionValue"); !value.empty()) {
                choice.valueField = std::move(value);
            }
            if (auto label = textOf(raw, prop, "x-optionLabel"); !label.empty()) {
                choice.labelField = std::move(label);
            }
            if (Json const* const parents = either(raw, prop, "x-optionsDependsOn");
                parents != nullptr && parents->is_array()) {
                for (Json const& parent : parents->get_array()) {
                    if (auto const* const text = parent.get_if<std::string>()) {
                        choice.dependsOn.push_back(*text);
                    }
                }
            }
            spec.choice = std::move(choice);
        } else if (format == "date-time") {
            spec.kind = FieldKind::DateTime;
        } else if (format == "date") {
            spec.kind = FieldKind::Date;
        } else if (spec.decimalPlaces.has_value()) {
            spec.kind = member(prop, "ExtUnits") != nullptr ? FieldKind::Quantity : FieldKind::Decimal;
            spec.units.push_back(UnitOption{
                .id = spec.unitAscii, .display = spec.unit, .decimals = *spec.decimalPlaces, .num = 1, .den = 1});
            if (Json const* const alternatives = either(raw, prop, "x-unitAlternatives");
                spec.kind == FieldKind::Quantity && alternatives != nullptr && alternatives->is_array()) {
                for (Json const& alternative : alternatives->get_array()) {
                    UnitOption option{};
                    option.id = textOf(alternative, alternative, "id");
                    option.display = textOf(alternative, alternative, "display");
                    if (option.display.empty()) {
                        option.display = option.id;
                    }
                    option.decimals = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                        unsignedOf(member(alternative, "decimals")).value_or(0), math::kMaxDecimalPlaces));
                    option.num = signedOf(member(alternative, "num")).value_or(1);
                    option.den = signedOf(member(alternative, "den")).value_or(1);
                    if (option.num > 0 && option.den > 0) {
                        spec.units.push_back(std::move(option));
                    }
                }
            }
        } else if ((hasType("integer") || hasType("number")) && spec.widget == "slider" && sliderMin && sliderMax) {
            spec.kind = FieldKind::Slider;
            spec.sliderMin = *sliderMin;
            spec.sliderMax = *sliderMax;
            spec.sliderStep = std::max<std::int64_t>(1, signedOf(either(raw, prop, "x-step")).value_or(1));
        } else if (hasType("integer") && spec.widget == "epochSeconds") {
            spec.kind = FieldKind::EpochDateTime;
        } else if (hasType("integer") && spec.widget == "epochDays") {
            spec.kind = FieldKind::EpochDate;
        } else if (hasType("integer")) {
            spec.kind = FieldKind::Integer;
        } else if (hasType("boolean")) {
            spec.kind = FieldKind::Boolean;
        } else if (hasType("number")) {
            spec.kind = FieldKind::Number;
            if (auto const decimals = unsignedOf(either(raw, prop, "x-displayDecimals"))) {
                spec.displayDecimals =
                    static_cast<std::uint32_t>(std::min<std::uint64_t>(*decimals, math::kMaxDecimalPlaces));
            }
        } else if (hasType("object")) {
            spec.kind = FieldKind::Object;
            if (describesNested && member(prop, "properties") != nullptr) {
                auto children = describe(prop, depth + 1, spec.path + ".", nestedChain);
                if (!children) {
                    return std::unexpected(children.error());
                }
                spec.children = *std::move(children);
            } else {
                spec.truncated = true;
            }
        } else if (spec.widget == "textarea") {
            spec.kind = FieldKind::Multiline;
        } else {
            spec.kind = FieldKind::Text;
        }
        if ((spec.kind == FieldKind::Text || spec.kind == FieldKind::Multiline) &&
            textOf(raw, prop, "x-blankAs") == "empty") {
            spec.blankAs = BlankAs::Empty;
        }
        return spec;
    }

    Json const* _defs;
};

}  // namespace detail::engine

/// @brief The immutable runtime model of one action's form.
///
/// Built from schema JSON by `fromSchema`; `forAction<A>()` (which the typed facade `Form<A>`
/// uses) reads `schemaJson<A>()` through the same reader, so a typed and a runtime form of one
/// action are the same model.
class FormModel {
public:
    /// @brief Reads a schema document, with the overlays a document or an application states for its fields.
    ///
    /// `omit`, `hidden`, `label`, `widget` and `blankAs` are applied to the schema before it is read, so a widget
    /// overlay chooses the kind exactly as the schema's own `x-widget` would; `unit` and `decimals` are applied to
    /// the read field. An overlay naming a field the schema does not have is a `SchemaError`, as is a `unit` that is
    /// not one of a Quantity's units.
    /// @param actionType The action type the form submits (the registered type id).
    /// @param schemaJson The schema, in the vocabulary `schemaJson<A>()` emits.
    /// @param overlays   The overlays of top-level fields, by wire name.
    /// @return The model, or the error naming where the document is malformed.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a type id and a JSON document; swapped, neither reads.
    [[nodiscard]] static std::expected<FormModel, SchemaError> fromSchema(std::string_view actionType,
                                                                          std::string_view schemaJson,
                                                                          FieldOverlays const& overlays = {}) {
        using detail::engine::Json;
        auto parsedDom = detail::engine::parseJson(schemaJson);
        if (!parsedDom) {
            return std::unexpected(SchemaError{.path = "", .message = "the schema is not valid JSON"});
        }
        if (!parsedDom->is_object()) {
            return std::unexpected(SchemaError{.path = "", .message = "the schema is not a JSON object"});
        }
        if (auto applied = applyOverlays(*parsedDom, overlays); !applied) {
            return std::unexpected(applied.error());
        }
        Json const* const dom = &*parsedDom;
        FormModel model{};
        model._actionType = actionType;
        auto const* const title = detail::engine::stringAt(*dom, "title");
        model._title = title != nullptr ? *title : std::string{actionType};
        detail::engine::SchemaReader const reader{*dom};
        auto fields = reader.describe(*dom, 0, "", {});
        if (!fields) {
            return std::unexpected(fields.error());
        }
        model._fields = *std::move(fields);
        if (auto groups = readGroups(*dom, model._fields); groups) {
            model._groups = *std::move(groups);
        } else {
            return std::unexpected(groups.error());
        }
        if (Json const* const rules = detail::engine::member(*dom, "x-rules")) {
            if (!rules->is_array()) {
                return std::unexpected(SchemaError{.path = "x-rules", .message = "\"x-rules\" must be an array"});
            }
            std::size_t index = 0;
            for (Json const& node : rules->get_array()) {
                auto parsed = RuleExpr::parse(node, "x-rules[" + std::to_string(index++) + "]");
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                model._rules.push_back(*std::move(parsed));
            }
        }
        auto const* const mode = detail::engine::stringAt(*dom, "x-submitMode");
        model._submitMode = (mode != nullptr && *mode == "explicit") ? SubmitMode::Explicit : SubmitMode::Automatic;
        for (auto const& [name, overlay] : overlays) {
            if (overlay.omit) {
                continue;
            }
            auto const found = std::ranges::find(model._fields, name, &FieldSpec::name);
            if (auto applied = applyUnitOverlay(*found, overlay); !applied) {
                return std::unexpected(applied.error());
            }
        }
        return model;
    }

    /// @brief The same model with another submit mode: a document's `submit` choosing over `x-submitMode`.
    /// @param mode The mode.
    /// @return The model.
    [[nodiscard]] FormModel withSubmitMode(SubmitMode mode) const {
        FormModel copy = *this;
        copy._submitMode = mode;
        return copy;
    }

    /// @brief The model of a registered action's own form: `fromSchema` over
    ///        `ActionTraits<A>::typeId()` and `schemaJson<A>()`.
    ///
    /// Declared here and defined in `forms/engine/typed_form.hpp`, the one engine header that includes the
    /// schema emitters; include that header to call it. The schema is compiled from `A`, so a reader error is a
    /// defect in the emitter or the engine, never bad input: it throws rather than returning an `expected` every
    /// caller would have to unwrap.
    /// @tparam A The action type (registered with `BRIDGE_REGISTER_ACTION` or declared with
    ///           `BRIDGE_DECLARE_ACTION`).
    /// @return The model.
    /// @throws std::logic_error naming the action id and the error's path and message when the
    ///         engine cannot read the action's schema.
    template <class A>
    [[nodiscard]] static FormModel forAction();

    /// @brief The action type the form submits.
    /// @return The type id passed to `fromSchema`.
    [[nodiscard]] std::string_view actionType() const noexcept { return _actionType; }

    /// @brief The schema's top-level `title`, or the action type.
    /// @return The title.
    [[nodiscard]] std::string_view title() const noexcept { return _title; }

    /// @brief The top-level fields, in `x-order`.
    /// @return The fields.
    [[nodiscard]] std::span<FieldSpec const> fields() const noexcept { return _fields; }

    /// @brief The layout groups: the declared `x-layout` groups, then an implicit group of the
    ///        fields none names; without `x-layout`, one implicit group of every field.
    /// @return The groups.
    [[nodiscard]] std::span<FieldGroupSpec const> groups() const noexcept { return _groups; }

    /// @brief The parsed `x-rules`, in declaration order.
    /// @return The rules.
    [[nodiscard]] std::span<RuleExpr const> rules() const noexcept { return _rules; }

    /// @brief When the form submits.
    /// @return `Explicit` for `x-submitMode: explicit`, else `Automatic`.
    [[nodiscard]] SubmitMode submitMode() const noexcept { return _submitMode; }

    /// @brief A top-level field by wire name.
    /// @param name The wire name.
    /// @return The field, or null.
    [[nodiscard]] FieldSpec const* find(std::string_view name) const noexcept {
        auto const found = std::ranges::find(_fields, name, &FieldSpec::name);
        return found == _fields.end() ? nullptr : &*found;
    }

private:
    FormModel() = default;

    [[nodiscard]] static GroupKind groupKindOf(std::string const* kind) {
        if (kind != nullptr && *kind == "tab") {
            return GroupKind::Tab;
        }
        if (kind != nullptr && *kind == "accordion") {
            return GroupKind::Accordion;
        }
        return GroupKind::Section;
    }

    /// Writes the schema-level overlays into the schema's properties: `omit` removes the property
    /// (a `required` entry naming it marks nothing); the rest set the keywords the reader reads.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per overlay key.
    [[nodiscard]] static std::expected<void, SchemaError> applyOverlays(detail::engine::Json& dom,
                                                                        FieldOverlays const& overlays) {
        using detail::engine::Json;
        if (overlays.empty()) {
            return {};
        }
        auto* const properties = dom.get_object().contains("properties")
                                     ? dom.get_object().find("properties")->second.get_if<Json::object_t>()
                                     : nullptr;
        for (auto const& [name, overlay] : overlays) {
            if (properties == nullptr || !properties->contains(name)) {
                return std::unexpected(SchemaError{.path = "fields." + name,
                                                   .message = "the overlay names a field the schema does not have"});
            }
            Json& property = properties->find(name)->second;
            if (overlay.omit) {
                // Its `required` entry may stay: the reader marks required only the properties it reads.
                properties->erase(name);
                continue;
            }
            auto* const object = property.get_if<Json::object_t>();
            if (object == nullptr) {
                continue;
            }
            auto const assign = [&](std::string const& key, Json value) { object->insert_or_assign(key, value); };
            if (overlay.hidden) {
                Json flag{};
                flag.data = true;
                assign("x-hidden", flag);
            }
            if (overlay.label.has_value()) {
                Json text{};
                text.data = *overlay.label;
                assign("title", text);
            }
            if (overlay.widget.has_value()) {
                Json text{};
                text.data = *overlay.widget;
                assign("x-widget", text);
            }
            if (overlay.blankAs.has_value()) {
                Json text{};
                text.data = std::string{*overlay.blankAs == BlankAs::Empty ? "empty" : "omit"};
                assign("x-blankAs", text);
            }
        }
        return {};
    }

    /// Applies an overlay's `unit` and `decimals` to the field read from the schema.
    [[nodiscard]] static std::expected<void, SchemaError> applyUnitOverlay(FieldSpec& spec,
                                                                           FieldOverlay const& overlay) {
        if (overlay.unit.has_value()) {
            if (spec.units.empty()) {
                spec.unit = *overlay.unit;
            } else {
                auto const found = std::ranges::find(spec.units, *overlay.unit, &UnitOption::id);
                if (found == spec.units.end()) {
                    return std::unexpected(
                        SchemaError{.path = "fields." + spec.name + ".unit",
                                    .message = "\"" + *overlay.unit + "\" is not one of the field's units"});
                }
                spec.entryUnit = static_cast<std::size_t>(found - spec.units.begin());
            }
        }
        if (overlay.decimals.has_value()) {
            std::uint32_t const places = std::min(*overlay.decimals, math::kMaxDecimalPlaces);
            if (spec.units.empty()) {
                spec.displayDecimals = places;
            } else {
                spec.units.at(spec.entryUnit).decimals = places;
            }
        }
        return {};
    }

    [[nodiscard]] static std::expected<std::vector<FieldGroupSpec>, SchemaError> readGroups(
        detail::engine::Json const& dom, std::vector<FieldSpec> const& fields) {
        using detail::engine::Json;
        std::vector<FieldGroupSpec> groups;
        Json const* const layout = detail::engine::member(dom, "x-layout");
        Json const* const declared = layout == nullptr ? nullptr : detail::engine::member(*layout, "groups");
        if (declared != nullptr && !declared->is_array()) {
            return std::unexpected(SchemaError{.path = "x-layout.groups", .message = "\"groups\" must be an array"});
        }
        if (declared != nullptr) {
            for (Json const& node : declared->get_array()) {
                FieldGroupSpec group{};
                if (auto const* const title = detail::engine::stringAt(node, "title")) {
                    group.title = *title;
                }
                group.kind = groupKindOf(detail::engine::stringAt(node, "kind"));
                groups.push_back(std::move(group));
            }
        }
        FieldGroupSpec trailing{};
        trailing.implicit = true;
        for (FieldSpec const& field : fields) {
            if (field.group.has_value() && *field.group < groups.size()) {
                groups.at(*field.group).fields.push_back(field.name);
            } else {
                trailing.fields.push_back(field.name);
            }
        }
        if (!trailing.fields.empty()) {
            groups.push_back(std::move(trailing));
        }
        return groups;
    }

    std::string _actionType;
    std::string _title;
    std::vector<FieldSpec> _fields;
    std::vector<FieldGroupSpec> _groups;
    std::vector<RuleExpr> _rules;
    SubmitMode _submitMode = SubmitMode::Automatic;
};

}  // namespace morph::forms
