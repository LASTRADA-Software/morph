// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <glaze/glaze.hpp>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "../render/i18n.hpp"
#include "../render/locale_format.hpp"
#include "../util/datetime.hpp"
#include "../util/rational.hpp"
#include "message.hpp"

/// @file
/// @brief The values, types and expressions of the UI document (`expr/1`).
///
/// Specified in `docs/spec/ui/expressions.md`.

namespace morph::ui {

/// @brief The JSON tree a document is read from: glaze's generic value with full 64-bit integers.
using Json = glz::generic_u64;

/// @brief An exact quantity: an exact amount and the id of its unit.
struct QuantityValue {
    /// @brief The amount, with its declared decimal places.
    math::Rational amount;
    /// @brief The unit's stable id, as the bundle's unit table names it (for example `"mg_per_l"`).
    std::string unit;

    /// @brief The same amount, compared by value as a rational is (its places aside), and the same unit.
    /// @return Whether the two are equal.
    bool operator==(QuantityValue const&) const = default;
};

class Value;

/// @brief The elements of a list value.
using ValueList = std::vector<Value>;

/// @brief The fields of a record value, in declaration order.
using ValueRecord = std::vector<std::pair<std::string, Value>>;

/// @brief A value of the UI document: what an expression evaluates to and what a declaration holds.
///
/// Immutable once made, and cheap to copy: lists and records are shared.
class Value {
public:
    /// @brief The kinds a value can be.
    enum class Kind : std::uint8_t {
        Null,       ///< Absent.
        Bool,       ///< `true` or `false`.
        Int,        ///< An exact `int64`.
        String,     ///< UTF-8 text.
        Decimal,    ///< An exact rational with its declared decimal places.
        Quantity,   ///< A decimal and a unit.
        Timestamp,  ///< A UTC instant.
        List,       ///< An ordered list of values.
        Record,     ///< Named fields, in order.
    };

    /// @brief The null value.
    Value() = default;

    /// @brief A bool.
    /// @param flag The value.
    Value(bool flag) : _data{flag} {}  // NOLINT(google-explicit-constructor): a value is written as its literal

    /// @brief An int.
    /// @param number The value.
    Value(std::int64_t number) : _data{number} {}  // NOLINT(google-explicit-constructor)

    /// @brief A string.
    /// @param text The value, UTF-8.
    Value(std::string text) : _data{std::move(text)} {}  // NOLINT(google-explicit-constructor)

    /// @brief A string.
    /// @param text The value, UTF-8.
    Value(char const* text) : _data{std::string{text}} {}  // NOLINT(google-explicit-constructor)

    /// @brief A decimal.
    /// @param decimal The value.
    Value(math::Rational decimal) : _data{decimal} {}  // NOLINT(google-explicit-constructor)

    /// @brief A quantity.
    /// @param quantity The value.
    Value(QuantityValue quantity) : _data{std::move(quantity)} {}  // NOLINT(google-explicit-constructor)

    /// @brief A timestamp.
    /// @param instant The instant.
    Value(time::DateTime instant) : _data{instant} {}  // NOLINT(google-explicit-constructor)

    /// @brief A list.
    /// @param elements The elements.
    Value(ValueList elements)  // NOLINT(google-explicit-constructor)
        : _data{std::make_shared<ValueList const>(std::move(elements))} {}

    /// @brief A record.
    /// @param fields The fields, in order.
    Value(ValueRecord fields)  // NOLINT(google-explicit-constructor)
        : _data{std::make_shared<ValueRecord const>(std::move(fields))} {}

    /// @brief What kind of value this is.
    /// @return The kind.
    [[nodiscard]] Kind kind() const noexcept { return static_cast<Kind>(_data.index()); }

    /// @brief Whether this is the null value.
    /// @return True for null.
    [[nodiscard]] bool isNull() const noexcept { return kind() == Kind::Null; }

    /// @brief The bool, if this is one.
    /// @return A pointer to it, or null.
    [[nodiscard]] bool const* asBool() const noexcept { return std::get_if<bool>(&_data); }

    /// @brief The int, if this is one.
    /// @return A pointer to it, or null.
    [[nodiscard]] std::int64_t const* asInt() const noexcept { return std::get_if<std::int64_t>(&_data); }

    /// @brief The string, if this is one.
    /// @return A pointer to it, or null.
    [[nodiscard]] std::string const* asString() const noexcept { return std::get_if<std::string>(&_data); }

    /// @brief The decimal, if this is one.
    /// @return A pointer to it, or null.
    [[nodiscard]] math::Rational const* asDecimal() const noexcept { return std::get_if<math::Rational>(&_data); }

    /// @brief The quantity, if this is one.
    /// @return A pointer to it, or null.
    [[nodiscard]] QuantityValue const* asQuantity() const noexcept { return std::get_if<QuantityValue>(&_data); }

    /// @brief The timestamp, if this is one.
    /// @return A pointer to it, or null.
    [[nodiscard]] time::DateTime const* asTimestamp() const noexcept { return std::get_if<time::DateTime>(&_data); }

    /// @brief The list's elements, if this is a list.
    /// @return A pointer to them, or null.
    [[nodiscard]] ValueList const* asList() const noexcept {
        auto const* list = std::get_if<std::shared_ptr<ValueList const>>(&_data);
        return list == nullptr ? nullptr : list->get();
    }

    /// @brief The record's fields, if this is a record.
    /// @return A pointer to them, or null.
    [[nodiscard]] ValueRecord const* asRecord() const noexcept {
        auto const* record = std::get_if<std::shared_ptr<ValueRecord const>>(&_data);
        return record == nullptr ? nullptr : record->get();
    }

    /// @brief A record's field by name.
    /// @param name The field's name.
    /// @return A pointer to the field's value, or null when this is not a record or has no such field.
    [[nodiscard]] Value const* field(std::string_view name) const noexcept {
        ValueRecord const* const record = asRecord();
        if (record == nullptr) {
            return nullptr;
        }
        auto const found = std::ranges::find_if(*record, [name](auto const& entry) { return entry.first == name; });
        return found == record->end() ? nullptr : &found->second;
    }

    /// @brief Structural equality: same kind and same content, records field by field in order. An int and a decimal
    ///        of equal value are different values; the `eq` function compares them by value.
    /// @param other The other value.
    /// @return Whether the two are the same value.
    [[nodiscard]] bool operator==(Value const& other) const {
        if (kind() != other.kind()) {
            return false;
        }
        if (ValueList const* const list = asList(); list != nullptr) {
            return *list == *other.asList();
        }
        if (ValueRecord const* const record = asRecord(); record != nullptr) {
            return *record == *other.asRecord();
        }
        return _data == other._data;
    }

private:
    std::variant<std::monostate, bool, std::int64_t, std::string, math::Rational, QuantityValue, time::DateTime,
                 std::shared_ptr<ValueList const>, std::shared_ptr<ValueRecord const>>
        _data;
};

/// @brief A value kind's name, as an error message writes it.
/// @param kind The kind.
/// @return Its name, for example `"decimal"`.
[[nodiscard]] constexpr std::string_view kindName(Value::Kind kind) noexcept {
    switch (kind) {
        case Value::Kind::Null:
            return "null";
        case Value::Kind::Bool:
            return "bool";
        case Value::Kind::Int:
            return "int";
        case Value::Kind::String:
            return "string";
        case Value::Kind::Decimal:
            return "decimal";
        case Value::Kind::Quantity:
            return "quantity";
        case Value::Kind::Timestamp:
            return "timestamp";
        case Value::Kind::List:
            return "list";
        case Value::Kind::Record:
        default:
            return "record";
    }
}

/// @brief A type of the UI document (spec 5 §3): a scalar, a list, a record, or any of them made optional.
class Type {
public:
    /// @brief The kinds a type can be.
    enum class Kind : std::uint8_t {
        Any,        ///< Not known: a value of any kind checks against it.
        Bool,       ///< `bool`.
        Int,        ///< `int`.
        String,     ///< `string`.
        Decimal,    ///< `decimal`.
        Quantity,   ///< `quantity`.
        Timestamp,  ///< `timestamp`.
        Key,        ///< `key`: an int or a string.
        List,       ///< `list<T>`.
        Record,     ///< A record with named, typed fields.
    };

    /// @brief The unknown type.
    Type() = default;

    /// @brief A scalar type.
    /// @param kind A scalar kind (not `List` or `Record`).
    /// @param optional Whether `null` is a value of it.
    explicit Type(Kind kind, bool optional = false) : _kind{kind}, _optional{optional} {}

    /// @brief `list<T>`.
    /// @param element The element type.
    /// @param optional Whether `null` is a value of it.
    /// @return The list type.
    [[nodiscard]] static Type list(Type element, bool optional = false) {
        Type type{Kind::List, optional};
        type._element = std::make_shared<Type const>(std::move(element));
        return type;
    }

    /// @brief A record type.
    /// @param fields The fields, in order, each with its type.
    /// @param optional Whether `null` is a value of it.
    /// @return The record type.
    [[nodiscard]] static Type record(std::vector<std::pair<std::string, Type>> fields, bool optional = false) {
        Type type{Kind::Record, optional};
        type._fields = std::make_shared<std::vector<std::pair<std::string, Type>> const>(std::move(fields));
        return type;
    }

    /// @brief This type's kind.
    /// @return The kind.
    [[nodiscard]] Kind kind() const noexcept { return _kind; }

    /// @brief Whether `null` is a value of this type. `Any` always admits it.
    /// @return True when optional.
    [[nodiscard]] bool optional() const noexcept { return _optional || _kind == Kind::Any; }

    /// @brief This type, with `null` admitted or not.
    /// @param optional Whether `null` is a value of the result.
    /// @return The changed copy.
    [[nodiscard]] Type withOptional(bool optional) const {
        Type copy = *this;
        copy._optional = optional;
        return copy;
    }

    /// @brief A list type's element type.
    /// @return The element type; `Any` for a type that is not a list.
    [[nodiscard]] Type element() const { return _element ? *_element : Type{}; }

    /// @brief A record type's fields.
    /// @return The fields, in order; empty for a type that is not a record.
    [[nodiscard]] std::vector<std::pair<std::string, Type>> const& fields() const noexcept {
        static std::vector<std::pair<std::string, Type>> const kNone;
        return _fields ? *_fields : kNone;
    }

    /// @brief A record type's field type by name.
    /// @param name The field's name.
    /// @return The field's type, or `nullopt` when there is no such field; `Any` for a type that is not a record.
    [[nodiscard]] std::optional<Type> field(std::string_view name) const {
        if (_kind == Kind::Any) {
            return Type{};
        }
        auto const found = std::ranges::find_if(fields(), [name](auto const& entry) { return entry.first == name; });
        if (found == fields().end()) {
            return std::nullopt;
        }
        return found->second;
    }

    /// @brief Declared decimal places, for a decimal or quantity read from a schema that states them.
    /// @return The places, or `nullopt` when not stated.
    [[nodiscard]] std::optional<std::uint32_t> places() const noexcept { return _places; }

    /// @brief The same type with stated decimal places.
    /// @param places The places.
    /// @return The changed copy.
    [[nodiscard]] Type withPlaces(std::uint32_t places) const {
        Type copy = *this;
        copy._places = places;
        return copy;
    }

    /// @brief The type as spec 5 §3 writes it, such as `list<decimal>?` or `{id: int, name: string}`.
    /// @return The text.
    [[nodiscard]] std::string text() const {
        std::string out;
        switch (_kind) {
            case Kind::Any:
                return "any";
            case Kind::Bool:
                out = "bool";
                break;
            case Kind::Int:
                out = "int";
                break;
            case Kind::String:
                out = "string";
                break;
            case Kind::Decimal:
                out = "decimal";
                break;
            case Kind::Quantity:
                out = "quantity";
                break;
            case Kind::Timestamp:
                out = "timestamp";
                break;
            case Kind::Key:
                out = "key";
                break;
            case Kind::List:
                out = "list<" + element().text() + ">";
                break;
            case Kind::Record:
            default:
                out = "{";
                for (auto const& [name, type] : fields()) {
                    out += (out.size() == 1 ? "" : ", ") + name + ": " + type.text();
                }
                out += "}";
                break;
        }
        return _optional ? out + "?" : out;
    }

private:
    Kind _kind = Kind::Any;
    bool _optional = false;
    std::optional<std::uint32_t> _places;
    std::shared_ptr<Type const> _element;
    std::shared_ptr<std::vector<std::pair<std::string, Type>> const> _fields;
};

/// @brief Whether every value of @p from is a value of @p into: the same kind (an `int` or a `string` is a `key`, an
///        `int` is a `decimal`), and `null` only into an optional type. `Any` on either side fits.
/// @param from The type a value has.
/// @param into The type a place expects.
/// @return Whether the value fits.
[[nodiscard]] inline bool fits(Type const& from, Type const& into) {
    if (from.kind() == Type::Kind::Any || into.kind() == Type::Kind::Any) {
        return true;
    }
    if (from.optional() && !into.optional()) {
        return false;
    }
    if (from.kind() == into.kind()) {
        if (from.kind() == Type::Kind::List) {
            return fits(from.element(), into.element());
        }
        if (from.kind() == Type::Kind::Record) {
            return std::ranges::all_of(into.fields(), [&from](auto const& field) {
                std::optional<Type> const own = from.field(field.first);
                return own.has_value() ? fits(*own, field.second) : field.second.optional();
            });
        }
        return true;
    }
    if (into.kind() == Type::Kind::Key) {
        return from.kind() == Type::Kind::Int || from.kind() == Type::Kind::String;
    }
    return into.kind() == Type::Kind::Decimal && from.kind() == Type::Kind::Int;
}

namespace detail {

// One step of a recursive-descent reading of a type's text.
struct TypeReader {
    std::string_view text;
    std::size_t at = 0;

    void skipSpace() {
        while (at < text.size() && text.at(at) == ' ') {
            ++at;
        }
    }

    [[nodiscard]] bool take(std::string_view token) {
        skipSpace();
        if (text.substr(at).starts_with(token)) {
            at += token.size();
            return true;
        }
        return false;
    }

    [[nodiscard]] std::string_view name() {
        skipSpace();
        std::size_t const start = at;
        while (at < text.size() &&
               (std::isalnum(static_cast<unsigned char>(text.at(at))) != 0 || text.at(at) == '_')) {
            ++at;
        }
        return text.substr(start, at - start);
    }

    // NOLINTNEXTLINE(misc-no-recursion): a type nests as deep as its text does.
    [[nodiscard]] std::expected<Type, std::string> type() {
        std::expected<Type, std::string> base = std::unexpected(std::string{});
        if (take("{")) {
            std::vector<std::pair<std::string, Type>> fields;
            if (!take("}")) {
                do {
                    std::string_view const fieldName = name();
                    if (fieldName.empty() || !take(":")) {
                        return std::unexpected("a record field must be written `name: type`");
                    }
                    std::expected<Type, std::string> fieldType = type();
                    if (!fieldType) {
                        return fieldType;
                    }
                    fields.emplace_back(std::string{fieldName}, std::move(*fieldType));
                } while (take(","));
                if (!take("}")) {
                    return std::unexpected("a record type must end with `}`");
                }
            }
            base = Type::record(std::move(fields));
        } else {
            std::string_view const word = name();
            static constexpr std::array<std::pair<std::string_view, Type::Kind>, 8> kScalars{{
                {"any", Type::Kind::Any},
                {"bool", Type::Kind::Bool},
                {"int", Type::Kind::Int},
                {"string", Type::Kind::String},
                {"decimal", Type::Kind::Decimal},
                {"quantity", Type::Kind::Quantity},
                {"timestamp", Type::Kind::Timestamp},
                {"key", Type::Kind::Key},
            }};
            if (word == "list") {
                if (!take("<")) {
                    return std::unexpected("`list` must be written `list<T>`");
                }
                std::expected<Type, std::string> element = type();
                if (!element) {
                    return element;
                }
                if (!take(">")) {
                    return std::unexpected("`list<T>` must end with `>`");
                }
                base = Type::list(std::move(*element));
            } else {
                auto const found = std::ranges::find(kScalars, word, &std::pair<std::string_view, Type::Kind>::first);
                if (found == kScalars.end()) {
                    return std::unexpected("unknown type `" + std::string{word} + "`");
                }
                base = Type{found->second};
            }
        }
        if (take("?")) {
            return base->withOptional(true);
        }
        return base;
    }
};

}  // namespace detail

/// @brief Reads a type as spec 5 §3 writes it: `int`, `string?`, `list<decimal>`, `{id: int, name: string?}`.
/// @param text The type's text.
/// @return The type, or why the text is not one.
[[nodiscard]] inline std::expected<Type, std::string> parseType(std::string_view text) {
    detail::TypeReader reader{.text = text};
    std::expected<Type, std::string> type = reader.type();
    reader.skipSpace();
    if (type && reader.at != text.size()) {
        return std::unexpected("unexpected `" + std::string{text.substr(reader.at)} + "` after the type");
    }
    return type;
}

/// @brief The type a JSON Schema describes, as the server writes an action's reply schema (`x-reply`).
///
/// An object with `num`, `den` and `dp` properties is a decimal, and with a `unit` as well a quantity; a string with
/// `format: date-time` is a timestamp; an integer is an `int`; a property outside `required` is optional; `$ref`
/// into `$defs` is followed. Anything the reader does not recognise is `Any`, so it checks against everything.
/// @param schema The schema.
/// @param root The document the schema belongs to, for `$ref`; the schema itself when it is the root.
/// @return The type.
// NOLINTNEXTLINE(misc-no-recursion): a schema nests as deep as the type it describes.
[[nodiscard]] inline Type typeFromSchema(Json const& schema, Json const& root) {
    if (!schema.is_object()) {
        return Type{};
    }
    auto const& object = schema.get_object();
    auto const text = [&object](std::string_view key) -> std::string {
        auto const found = object.find(key);
        return found != object.end() && found->second.is_string() ? found->second.get_string() : std::string{};
    };
    if (std::string const reference = text("$ref"); reference.starts_with("#/$defs/")) {
        std::string const name = reference.substr(std::string_view{"#/$defs/"}.size());
        if (root.is_object() && root.contains("$defs") && root["$defs"].contains(name)) {
            return typeFromSchema(root["$defs"][name], root);
        }
        return Type{};
    }
    // glaze writes an optional member as `type: [T, "null"]`.
    std::string kind = text("type");
    bool nullable = false;
    if (auto const found = object.find("type"); found != object.end() && found->second.is_array()) {
        for (auto const& entry : found->second.get_array()) {
            if (entry.is_string()) {
                if (entry.get_string() == "null") {
                    nullable = true;
                } else {
                    kind = entry.get_string();
                }
            }
        }
    }
    Type result;
    if (kind == "boolean") {
        result = Type{Type::Kind::Bool};
    } else if (kind == "integer") {
        result = Type{Type::Kind::Int};
    } else if (kind == "string") {
        result = Type{text("format") == "date-time" ? Type::Kind::Timestamp : Type::Kind::String};
    } else if (kind == "array") {
        auto const items = object.find("items");
        result = Type::list(items == object.end() ? Type{} : typeFromSchema(items->second, root));
    } else if (kind == "object") {
        auto const properties = object.find("properties");
        if (properties == object.end() || !properties->second.is_object()) {
            return Type{}.withOptional(nullable);
        }
        auto const& members = properties->second.get_object();
        if (members.contains("num") && members.contains("den") && members.contains("dp")) {
            result = Type{members.contains("unit") ? Type::Kind::Quantity : Type::Kind::Decimal};
        } else {
            std::vector<std::string> required;
            if (auto const found = object.find("required"); found != object.end() && found->second.is_array()) {
                for (auto const& name : found->second.get_array()) {
                    if (name.is_string()) {
                        required.push_back(name.get_string());
                    }
                }
            }
            std::vector<std::pair<std::string, Type>> fields;
            for (auto const& [name, member] : members) {
                Type fieldType = typeFromSchema(member, root);
                if (std::ranges::find(required, name) == required.end()) {
                    fieldType = fieldType.withOptional(true);
                }
                fields.emplace_back(name, std::move(fieldType));
            }
            result = Type::record(std::move(fields));
        }
    } else {
        return Type{};
    }
    if (auto const found = object.find("x-decimalPlaces"); found != object.end()) {
        if (auto const* const places = std::get_if<std::uint64_t>(&found->second.data); places != nullptr) {
            result = result.withPlaces(static_cast<std::uint32_t>(*places));
        }
    }
    return result.withOptional(nullable);
}

namespace detail {

// The int64 a JSON number holds exactly, if it holds one.
[[nodiscard]] inline std::optional<std::int64_t> exactInt(Json const& json) {
    if (auto const* const unsignedValue = std::get_if<std::uint64_t>(&json.data); unsignedValue != nullptr) {
        if (*unsignedValue > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(*unsignedValue);
    }
    if (auto const* const signedValue = std::get_if<std::int64_t>(&json.data); signedValue != nullptr) {
        return *signedValue;
    }
    return std::nullopt;
}

// A decimal written as `{num, den, dp}`; the canonicalising constructor reduces it.
[[nodiscard]] inline std::expected<math::Rational, std::string> decimalFrom(Json const& json) {
    if (!json.is_object() || !json.contains("num") || !json.contains("den") || !json.contains("dp")) {
        return std::unexpected("a decimal is written {num, den, dp}");
    }
    std::optional<std::int64_t> const numerator = exactInt(json["num"]);
    std::optional<std::int64_t> const denominator = exactInt(json["den"]);
    std::optional<std::int64_t> const places = exactInt(json["dp"]);
    if (!numerator || !denominator || !places || *denominator <= 0 || *places < 0 ||
        *places > static_cast<std::int64_t>(math::kMaxDecimalPlaces)) {
        return std::unexpected("a decimal needs an int64 `num`, a positive `den` and `dp` from 0 to 18");
    }
    return math::Rational{math::Numerator{*numerator}, math::Denominator{*denominator},
                          math::DecimalPlaces{static_cast<std::uint32_t>(*places)}};
}

}  // namespace detail

/// @brief Reads a value from JSON without a type: what a document's literal says on its face.
///
/// A number is an `int` (a fraction is refused: it has been rounded to a double already, and a document writes an
/// exact fraction as `{num, den, dp}`); an object with exactly `num`, `den` and `dp` is a decimal, and with `unit`
/// as well a quantity; any other object is a record; a string is a string, since only a type tells a timestamp
/// from one.
/// @param json The JSON.
/// @return The value, or why the JSON is not one.
// NOLINTNEXTLINE(misc-no-recursion): a value nests as deep as its JSON does.
[[nodiscard]] inline std::expected<Value, std::string> decodeUntyped(Json const& json) {
    if (json.is_null()) {
        return Value{};
    }
    if (json.is_boolean()) {
        return Value{json.get_boolean()};
    }
    if (json.is_string()) {
        return Value{json.get_string()};
    }
    if (json.is_number()) {
        if (std::optional<std::int64_t> const number = detail::exactInt(json); number.has_value()) {
            return Value{*number};
        }
        return std::unexpected("a number must be an int64; write a fraction as {num, den, dp}");
    }
    if (json.is_array()) {
        ValueList elements;
        for (auto const& element : json.get_array()) {
            std::expected<Value, std::string> value = decodeUntyped(element);
            if (!value) {
                return value;
            }
            elements.push_back(std::move(*value));
        }
        return Value{std::move(elements)};
    }
    auto const& object = json.get_object();
    bool const decimalShape = object.contains("num") && object.contains("den") && object.contains("dp");
    if (decimalShape && object.size() == 3) {
        return detail::decimalFrom(json).transform([](math::Rational decimal) { return Value{decimal}; });
    }
    if (decimalShape && object.size() == 4 && object.contains("unit") && json["unit"].is_string()) {
        return detail::decimalFrom(json).transform([&json](math::Rational decimal) {
            return Value{QuantityValue{.amount = decimal, .unit = json["unit"].get_string()}};
        });
    }
    ValueRecord fields;
    for (auto const& [name, member] : object) {
        std::expected<Value, std::string> value = decodeUntyped(member);
        if (!value) {
            return value;
        }
        fields.emplace_back(name, std::move(*value));
    }
    return Value{std::move(fields)};
}

namespace detail {

// @p noun with its indefinite article: "an int", "a decimal".
[[nodiscard]] inline std::string withArticle(std::string const& noun) {
    bool const vowel = !noun.empty() && std::string_view{"aeiou"}.find(noun.front()) != std::string_view::npos;
    return (vowel ? "an " : "a ") + noun;
}

}  // namespace detail

/// @brief Reads a value from JSON as a type says it is: a reply decoded through its action's reply schema.
/// @param json The JSON.
/// @param type The type it holds; `Any` reads it as `decodeUntyped` does.
/// @return The value, or why the JSON is not a value of @p type.
// NOLINTNEXTLINE(misc-no-recursion): a value nests as deep as its type does.
[[nodiscard]] inline std::expected<Value, std::string> decode(Json const& json, Type const& type) {
    if (json.is_null()) {
        if (type.optional()) {
            return Value{};
        }
        return std::unexpected("null where " + detail::withArticle(type.text()) + " is required");
    }
    auto const mismatch = [&type] { return std::unexpected("not " + detail::withArticle(type.text())); };
    switch (type.kind()) {
        case Type::Kind::Any:
            return decodeUntyped(json);
        case Type::Kind::Bool:
            if (!json.is_boolean()) {
                return mismatch();
            }
            return Value{json.get_boolean()};
        case Type::Kind::Int:
            if (std::optional<std::int64_t> const number = detail::exactInt(json); number.has_value()) {
                return Value{*number};
            }
            return mismatch();
        case Type::Kind::String:
            if (!json.is_string()) {
                return mismatch();
            }
            return Value{json.get_string()};
        case Type::Kind::Key:
            if (json.is_string()) {
                return Value{json.get_string()};
            }
            if (std::optional<std::int64_t> const number = detail::exactInt(json); number.has_value()) {
                return Value{*number};
            }
            return mismatch();
        case Type::Kind::Timestamp: {
            if (!json.is_string()) {
                return mismatch();
            }
            std::optional<time::DateTime> const instant = time::DateTime::fromIso8601(json.get_string());
            if (!instant.has_value()) {
                return std::unexpected("not an ISO-8601 UTC timestamp");
            }
            return Value{*instant};
        }
        case Type::Kind::Decimal:
            if (std::optional<std::int64_t> const number = detail::exactInt(json); number.has_value()) {
                return Value{math::Rational{*number, math::DecimalPlaces{type.places().value_or(0)}}};
            }
            return detail::decimalFrom(json).transform([](math::Rational decimal) { return Value{decimal}; });
        case Type::Kind::Quantity: {
            if (!json.is_object() || !json.contains("unit") || !json["unit"].is_string()) {
                return std::unexpected("a quantity is written {num, den, dp, unit}");
            }
            return detail::decimalFrom(json).transform([&json](math::Rational decimal) {
                return Value{QuantityValue{.amount = decimal, .unit = json["unit"].get_string()}};
            });
        }
        case Type::Kind::List: {
            if (!json.is_array()) {
                return mismatch();
            }
            ValueList elements;
            Type const element = type.element();
            for (auto const& entry : json.get_array()) {
                std::expected<Value, std::string> value = decode(entry, element);
                if (!value) {
                    return value;
                }
                elements.push_back(std::move(*value));
            }
            return Value{std::move(elements)};
        }
        case Type::Kind::Record:
        default: {
            if (!json.is_object()) {
                return mismatch();
            }
            ValueRecord fields;
            for (auto const& [name, fieldType] : type.fields()) {
                if (!json.contains(name)) {
                    if (!fieldType.optional()) {
                        return std::unexpected("missing field `" + name + "`");
                    }
                    fields.emplace_back(name, Value{});
                    continue;
                }
                std::expected<Value, std::string> value = decode(json[name], fieldType);
                if (!value) {
                    return std::unexpected("field `" + name + "`: " + value.error());
                }
                fields.emplace_back(name, std::move(*value));
            }
            return Value{std::move(fields)};
        }
    }
}

/// @brief Writes a value as JSON: decimals as `{num, den, dp}`, quantities with `unit`, timestamps as ISO-8601.
/// @param value The value.
/// @return The JSON.
// NOLINTNEXTLINE(misc-no-recursion): a value nests as deep as its JSON does.
[[nodiscard]] inline Json encode(Value const& value) {
    Json json{};
    auto const writeDecimal = [&json](math::Rational const& decimal) {
        json["num"] = decimal.numerator;
        json["den"] = decimal.denominator;
        json["dp"] = static_cast<std::int64_t>(decimal.getDecimalPlaces().value);
    };
    switch (value.kind()) {
        case Value::Kind::Null:
            json.data = nullptr;
            break;
        case Value::Kind::Bool:
            json.data = *value.asBool();
            break;
        case Value::Kind::Int:
            json.data = *value.asInt();
            break;
        case Value::Kind::String:
            json.data = *value.asString();
            break;
        case Value::Kind::Decimal:
            writeDecimal(*value.asDecimal());
            break;
        case Value::Kind::Quantity:
            writeDecimal(value.asQuantity()->amount);
            json["unit"] = value.asQuantity()->unit;
            break;
        case Value::Kind::Timestamp:
            json.data = value.asTimestamp()->toIso8601();
            break;
        case Value::Kind::List: {
            Json::array_t elements;
            for (Value const& element : *value.asList()) {
                elements.push_back(encode(element));
            }
            json.data = std::move(elements);
            break;
        }
        case Value::Kind::Record:
        default:
            json.data = Json::object_t{};
            for (auto const& [name, field] : *value.asRecord()) {
                json[name] = encode(field);
            }
            break;
    }
    return json;
}

/// @brief A value's JSON with every object's keys sorted: two values with the same content, whatever their fields'
///        order, give the same text. The query cache keys on it.
/// @param value The value.
/// @return The canonical JSON text.
// NOLINTNEXTLINE(misc-no-recursion): a value nests as deep as its JSON does.
[[nodiscard]] inline std::string canonicalJson(Value const& value) {
    if (ValueList const* const list = value.asList(); list != nullptr) {
        std::string out = "[";
        for (Value const& element : *list) {
            out += (out.size() == 1 ? "" : ",") + canonicalJson(element);
        }
        return out + "]";
    }
    if (ValueRecord const* const record = value.asRecord(); record != nullptr) {
        std::vector<std::pair<std::string, Value>> sorted = *record;
        std::ranges::sort(sorted, {}, &std::pair<std::string, Value>::first);
        std::string out = "{";
        for (auto const& [name, field] : sorted) {
            std::string key;
            static_cast<void>(glz::write_json(name, key));
            out += (out.size() == 1 ? "" : ",") + key + ":" + canonicalJson(field);
        }
        return out + "}";
    }
    Json json = encode(value);
    if (json.is_object()) {
        // A decimal or quantity: its keys are written in a fixed order, sorted here like any record's.
        std::map<std::string, Json> sorted;
        for (auto const& [name, member] : json.get_object()) {
            sorted.emplace(name, member);
        }
        std::string out = "{";
        for (auto const& [name, member] : sorted) {
            std::string text;
            static_cast<void>(glz::write_json(member, text));
            out += (out.size() == 1 ? "\"" : ",\"") + name + "\":" + text;
        }
        return out + "}";
    }
    std::string out;
    static_cast<void>(glz::write_json(json, out));
    return out;
}

/// @brief Where in a document something is, and what is wrong there.
struct Problem {
    /// @brief The JSON pointer of the offending node, from the document's root (for example `/body/0/text`).
    std::string path;
    /// @brief Why it is refused, in a sentence a document's author can act on.
    std::string reason;

    /// @brief Same path, same reason.
    /// @return Whether the two are equal.
    bool operator==(Problem const&) const = default;
};

/// @brief The most nodes one expression may hold; a larger one is refused at load.
inline constexpr std::size_t kMaxExpressionNodes = 256;

/// @brief A function of `expr/1`, and how many arguments it takes.
struct FunctionSignature {
    /// @brief The name a `{"fn": ...}` form calls it by.
    std::string_view name;
    /// @brief The fewest arguments it takes.
    std::uint8_t minArgs;
    /// @brief The most arguments it takes; `kVariadic` for no bound.
    std::uint8_t maxArgs;
};

/// @brief The `maxArgs` of a function that takes any number of arguments.
inline constexpr std::uint8_t kVariadic = 255;

/// @brief The function library `expr/1`, by name.
inline constexpr std::array<FunctionSignature, 40> kFunctions{{
    {.name = "and", .minArgs = 1, .maxArgs = kVariadic},
    {.name = "or", .minArgs = 1, .maxArgs = kVariadic},
    {.name = "not", .minArgs = 1, .maxArgs = 1},
    {.name = "eq", .minArgs = 2, .maxArgs = 2},
    {.name = "ne", .minArgs = 2, .maxArgs = 2},
    {.name = "lt", .minArgs = 2, .maxArgs = 2},
    {.name = "le", .minArgs = 2, .maxArgs = 2},
    {.name = "gt", .minArgs = 2, .maxArgs = 2},
    {.name = "ge", .minArgs = 2, .maxArgs = 2},
    {.name = "add", .minArgs = 2, .maxArgs = 2},
    {.name = "sub", .minArgs = 2, .maxArgs = 2},
    {.name = "mul", .minArgs = 2, .maxArgs = 2},
    {.name = "div", .minArgs = 2, .maxArgs = 2},
    {.name = "neg", .minArgs = 1, .maxArgs = 1},
    {.name = "round", .minArgs = 2, .maxArgs = 2},
    {.name = "engaged", .minArgs = 1, .maxArgs = 1},
    {.name = "coalesce", .minArgs = 1, .maxArgs = kVariadic},
    {.name = "concat", .minArgs = 1, .maxArgs = kVariadic},
    {.name = "format", .minArgs = 1, .maxArgs = kVariadic},
    {.name = "trim", .minArgs = 1, .maxArgs = 1},
    {.name = "length", .minArgs = 1, .maxArgs = 1},
    {.name = "isEmpty", .minArgs = 1, .maxArgs = 1},
    {.name = "contains", .minArgs = 2, .maxArgs = 2},
    {.name = "count", .minArgs = 1, .maxArgs = 1},
    {.name = "sum", .minArgs = 1, .maxArgs = 1},
    {.name = "mean", .minArgs = 1, .maxArgs = 1},
    {.name = "min", .minArgs = 1, .maxArgs = 1},
    {.name = "max", .minArgs = 1, .maxArgs = 1},
    {.name = "any", .minArgs = 1, .maxArgs = 1},
    {.name = "all", .minArgs = 1, .maxArgs = 1},
    {.name = "first", .minArgs = 1, .maxArgs = 1},
    {.name = "includes", .minArgs = 2, .maxArgs = 2},
    {.name = "indexOf", .minArgs = 2, .maxArgs = 2},
    {.name = "number", .minArgs = 1, .maxArgs = 1},
    {.name = "decimal", .minArgs = 2, .maxArgs = 2},
    {.name = "quantity", .minArgs = 1, .maxArgs = 2},
    {.name = "money", .minArgs = 2, .maxArgs = 2},
    {.name = "date", .minArgs = 1, .maxArgs = 2},
    {.name = "dateTime", .minArgs = 1, .maxArgs = 2},
    {.name = "t", .minArgs = 1, .maxArgs = 3},
}};

/// @brief An `expr/1` expression: an immutable tree parsed once from a document's JSON.
///
/// Every node records its JSON path, which names it in a load-time refusal and in a runtime report.
struct Expr {
    /// @brief The forms of spec 5 §4.
    enum class Form : std::uint8_t {
        Literal,    ///< A JSON scalar or `{"lit": v}`: `literal`.
        Ref,        ///< `{"ref": "name.path"}`: `name`.
        Event,      ///< `{"ref": "event"}` or `{"ref": "event.path"}`: `name` is the path after `event`.
        Row,        ///< `{"row": "path"}`: `name`.
        Query,      ///< `{"query": "q"}`: `name`.
        Result,     ///< `{"result": "m"}`: `name`.
        Pending,    ///< `{"pending": "q"}`, a query's or a mutation's: `name`.
        Error,      ///< `{"error": "q"}`, a query's or a mutation's: `name`.
        ErrorKind,  ///< `{"errorKind": "m"}`: `name`.
        Can,        ///< `{"can": "m"}`: `name`.
        Pref,       ///< `{"pref": "point.path"}`: `name`.
        Env,        ///< `{"env": "widthClass"}`: `name`.
        Session,    ///< `{"session": "displayName"}`: `name`.
        Host,       ///< `{"host": "name"}`: `name`.
        Call,       ///< `{"fn": "name", "args": [...]}`: `name` and `args`.
        Translate,  ///< `{"t": "key", "fallback": "text", "args": {...}}`: `name`, `fallback`, `keys` and `args`.
        If,         ///< `{"if": [cond, then, else]}`: `args`.
        Object,     ///< `{"obj": {"k": e}}`: `keys` and `args`.
        List,       ///< `{"list": [e, ...]}`: `args`.
        Map,        ///< `{"map": e, "as": "x", "to": e}`: `args` (the list, then the body) and `name` (the `as`).
        Filter,     ///< `{"filter": e, "as": "x", "where": e}`: `args` (the list, then the test) and `name`.
    };

    /// @brief Which form this node is.
    Form form = Form::Literal;
    /// @brief The JSON pointer of this node in its document.
    std::string path;
    /// @brief The constant of a `Literal`.
    Value literal;
    /// @brief The name, path or key the form carries; empty when it carries none.
    std::string name;
    /// @brief The fallback text of a `Translate`.
    std::string fallback;
    /// @brief The operands, in order: a call's arguments, `if`'s three, a list's elements, a comprehension's list
    ///        and body, an `obj`'s fields or a `t`'s arguments.
    std::vector<Expr> args;
    /// @brief The names of an `obj`'s fields or a `t`'s arguments, one per operand in `args`, in the order written.
    std::vector<std::string> keys;

    /// @brief How many nodes the tree rooted here holds, this one included.
    /// @return The count.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by kMaxExpressionNodes once parsed.
    [[nodiscard]] std::size_t size() const {
        std::size_t total = 1;
        for (Expr const& operand : args) {
            total += operand.size();
        }
        return total;
    }
};

/// @brief The signature of the `expr/1` function @p name.
/// @param name The function's name.
/// @return Its signature, or null when `expr/1` has no such function.
[[nodiscard]] inline FunctionSignature const* findFunction(std::string_view name) noexcept {
    auto const found = std::ranges::find(kFunctions, name, &FunctionSignature::name);
    return found == kFunctions.end() ? nullptr : &*found;
}

namespace detail {

// The single-string forms, by their key.
inline constexpr std::array<std::pair<std::string_view, Expr::Form>, 12> kNameForms{{
    {"ref", Expr::Form::Ref},
    {"row", Expr::Form::Row},
    {"query", Expr::Form::Query},
    {"result", Expr::Form::Result},
    {"pending", Expr::Form::Pending},
    {"error", Expr::Form::Error},
    {"errorKind", Expr::Form::ErrorKind},
    {"can", Expr::Form::Can},
    {"pref", Expr::Form::Pref},
    {"env", Expr::Form::Env},
    {"session", Expr::Form::Session},
    {"host", Expr::Form::Host},
}};

// Escapes a key for a JSON pointer: `~` as `~0`, `/` as `~1`.
[[nodiscard]] inline std::string pointerToken(std::string_view key) {
    std::string out;
    for (char const letter : key) {
        if (letter == '~') {
            out += "~0";
        } else if (letter == '/') {
            out += "~1";
        } else {
            out += letter;
        }
    }
    return out;
}

// Parses one node; `nodes` counts what has been parsed so far, so an oversized tree stops at the first node
// past the bound instead of being built whole.
struct ExprParser {
    std::size_t nodes = 0;

    [[nodiscard]] static std::unexpected<Problem> refuse(std::string path, std::string reason) {
        return std::unexpected(Problem{.path = std::move(path), .reason = std::move(reason)});
    }

    // Refuses every key of @p object outside @p allowed.
    [[nodiscard]] static std::optional<Problem> onlyKeys(Json::object_t const& object, std::string const& path,
                                                         std::string_view form,
                                                         std::initializer_list<std::string_view> allowed) {
        for (auto const& [key, member] : object) {
            if (std::ranges::find(allowed, key) == allowed.end()) {
                return Problem{.path = path + "/" + pointerToken(key),
                               .reason = "`" + key + "` is not a key of the `" + std::string{form} + "` form"};
            }
        }
        return std::nullopt;
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by kMaxExpressionNodes.
    [[nodiscard]] std::expected<std::vector<Expr>, Problem> many(Json const& json, std::string const& path) {
        std::vector<Expr> out;
        std::size_t index = 0;
        for (Json const& element : json.get_array()) {
            std::expected<Expr, Problem> operand = parse(element, path + "/" + std::to_string(index++));
            if (!operand) {
                return std::unexpected(std::move(operand.error()));
            }
            out.push_back(std::move(*operand));
        }
        return out;
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by kMaxExpressionNodes.
    // Parses each member of the object @p json into @p node's `keys` and `args`.
    [[nodiscard]] std::optional<Problem> named(Json const& json, std::string const& path, Expr& node) {
        for (auto const& [key, member] : json.get_object()) {
            std::expected<Expr, Problem> operand = parse(member, path + "/" + pointerToken(key));
            if (!operand) {
                return std::move(operand.error());
            }
            node.keys.push_back(key);
            node.args.push_back(std::move(*operand));
        }
        return std::nullopt;
    }

    // NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity): one branch per form.
    [[nodiscard]] std::expected<Expr, Problem> parse(Json const& json, std::string const& path) {
        if (++nodes > kMaxExpressionNodes) {
            return refuse(path, "the expression holds more than " + std::to_string(kMaxExpressionNodes) + " nodes");
        }
        Expr node{.path = path};
        if (json.is_array()) {
            return refuse(path, R"(a list is written {"list": [...]}, or {"lit": [...]} for a constant)");
        }
        if (!json.is_object()) {
            std::expected<Value, std::string> constant = decodeUntyped(json);
            if (!constant) {
                return refuse(path, constant.error());
            }
            node.literal = std::move(*constant);
            return node;
        }
        auto const& object = json.get_object();
        auto const has = [&object](std::string_view key) { return object.contains(key); };
        auto const stringAt = [&json, &path](std::string_view key) -> std::expected<std::string, Problem> {
            Json const& member = json[key];
            if (!member.is_string() || member.get_string().empty()) {
                return refuse(path + "/" + std::string{key}, "`" + std::string{key} + "` takes a non-empty name");
            }
            return member.get_string();
        };
        if (has("lit")) {
            if (std::optional<Problem> extra = onlyKeys(object, path, "lit", {"lit"})) {
                return std::unexpected(std::move(*extra));
            }
            std::expected<Value, std::string> constant = decodeUntyped(json["lit"]);
            if (!constant) {
                return refuse(path + "/lit", constant.error());
            }
            node.literal = std::move(*constant);
            return node;
        }
        for (auto const& [key, form] : kNameForms) {
            if (!has(key)) {
                continue;
            }
            if (std::optional<Problem> extra = onlyKeys(object, path, key, {key})) {
                return std::unexpected(std::move(*extra));
            }
            std::expected<std::string, Problem> name = stringAt(key);
            if (!name) {
                return std::unexpected(std::move(name.error()));
            }
            node.form = form;
            node.name = std::move(*name);
            if (form == Expr::Form::Ref && (node.name == "event" || node.name.starts_with("event."))) {
                node.form = Expr::Form::Event;
                node.name = node.name == "event" ? std::string{} : node.name.substr(std::string_view{"event."}.size());
            }
            return node;
        }
        if (has("fn")) {
            if (std::optional<Problem> extra = onlyKeys(object, path, "fn", {"fn", "args"})) {
                return std::unexpected(std::move(*extra));
            }
            std::expected<std::string, Problem> name = stringAt("fn");
            if (!name) {
                return std::unexpected(std::move(name.error()));
            }
            FunctionSignature const* const signature = findFunction(*name);
            if (signature == nullptr) {
                return refuse(path + "/fn", "`" + *name + "` is not a function of expr/1");
            }
            if (has("args")) {
                if (!json["args"].is_array()) {
                    return refuse(path + "/args", "`args` is a list of expressions");
                }
                std::expected<std::vector<Expr>, Problem> operands = many(json["args"], path + "/args");
                if (!operands) {
                    return std::unexpected(std::move(operands.error()));
                }
                node.args = std::move(*operands);
            }
            if (node.args.size() < signature->minArgs || node.args.size() > signature->maxArgs) {
                std::string const wanted =
                    signature->maxArgs == kVariadic ? "at least " + std::to_string(signature->minArgs)
                    : signature->minArgs == signature->maxArgs
                        ? std::to_string(signature->minArgs)
                        : std::to_string(signature->minArgs) + " to " + std::to_string(signature->maxArgs);
                bool const one =
                    signature->maxArgs == 1 || (signature->maxArgs == kVariadic && signature->minArgs == 1);
                return refuse(path, "`" + *name + "` takes " + wanted +
                                        (one ? " argument, not " : " arguments, not ") +
                                        std::to_string(node.args.size()));
            }
            node.form = Expr::Form::Call;
            node.name = std::move(*name);
            return node;
        }
        if (has("t")) {
            if (std::optional<Problem> extra = onlyKeys(object, path, "t", {"t", "fallback", "args"})) {
                return std::unexpected(std::move(*extra));
            }
            std::expected<std::string, Problem> key = stringAt("t");
            if (!key) {
                return std::unexpected(std::move(key.error()));
            }
            node.form = Expr::Form::Translate;
            node.name = std::move(*key);
            if (has("fallback")) {
                if (!json["fallback"].is_string()) {
                    return refuse(path + "/fallback", "`fallback` is the message's text");
                }
                node.fallback = json["fallback"].get_string();
            }
            if (has("args")) {
                if (!json["args"].is_object()) {
                    return refuse(path + "/args", "`args` is an object of named expressions");
                }
                if (std::optional<Problem> problem = named(json["args"], path + "/args", node)) {
                    return std::unexpected(std::move(*problem));
                }
            }
            return node;
        }
        if (has("if")) {
            if (std::optional<Problem> extra = onlyKeys(object, path, "if", {"if"})) {
                return std::unexpected(std::move(*extra));
            }
            if (!json["if"].is_array() || json["if"].get_array().size() != 3) {
                return refuse(path + "/if", "`if` takes [condition, then, else]");
            }
            std::expected<std::vector<Expr>, Problem> operands = many(json["if"], path + "/if");
            if (!operands) {
                return std::unexpected(std::move(operands.error()));
            }
            node.form = Expr::Form::If;
            node.args = std::move(*operands);
            return node;
        }
        if (has("obj")) {
            if (std::optional<Problem> extra = onlyKeys(object, path, "obj", {"obj"})) {
                return std::unexpected(std::move(*extra));
            }
            if (!json["obj"].is_object()) {
                return refuse(path + "/obj", "`obj` takes an object of named expressions");
            }
            if (std::optional<Problem> problem = named(json["obj"], path + "/obj", node)) {
                return std::unexpected(std::move(*problem));
            }
            node.form = Expr::Form::Object;
            return node;
        }
        if (has("list")) {
            if (std::optional<Problem> extra = onlyKeys(object, path, "list", {"list"})) {
                return std::unexpected(std::move(*extra));
            }
            if (!json["list"].is_array()) {
                return refuse(path + "/list", "`list` takes a list of expressions");
            }
            std::expected<std::vector<Expr>, Problem> operands = many(json["list"], path + "/list");
            if (!operands) {
                return std::unexpected(std::move(operands.error()));
            }
            node.form = Expr::Form::List;
            node.args = std::move(*operands);
            return node;
        }
        for (auto const& [key, body, form] :
             {std::tuple{std::string_view{"map"}, std::string_view{"to"}, Expr::Form::Map},
              std::tuple{std::string_view{"filter"}, std::string_view{"where"}, Expr::Form::Filter}}) {
            if (!has(key)) {
                continue;
            }
            if (std::optional<Problem> extra = onlyKeys(object, path, key, {key, "as", body})) {
                return std::unexpected(std::move(*extra));
            }
            if (!has("as") || !has(body)) {
                return refuse(path, "`" + std::string{key} + "` takes `as` and `" + std::string{body} + "`");
            }
            std::expected<std::string, Problem> name = stringAt("as");
            if (!name) {
                return std::unexpected(std::move(name.error()));
            }
            std::expected<Expr, Problem> source = parse(json[key], path + "/" + std::string{key});
            if (!source) {
                return source;
            }
            std::expected<Expr, Problem> each = parse(json[body], path + "/" + std::string{body});
            if (!each) {
                return each;
            }
            node.form = form;
            node.name = std::move(*name);
            node.args.push_back(std::move(*source));
            node.args.push_back(std::move(*each));
            return node;
        }
        std::string const first = object.empty() ? std::string{} : object.begin()->first;
        return refuse(path, object.empty() ? "an empty object is not an expression; a constant is {\"lit\": {}}"
                                           : "`" + first + "` is not an expression form");
    }
};

}  // namespace detail

/// @brief Parses an expression from a document's JSON.
/// @param json The expression's JSON.
/// @param path The JSON pointer of @p json in its document; every node's path extends it.
/// @return The tree, or the first problem: an unknown form, an unknown function or a wrong argument count, a
///         malformed operand, a fractional number, or more than `kMaxExpressionNodes` nodes.
[[nodiscard]] inline std::expected<Expr, Problem> parseExpression(Json const& json, std::string path = {}) {
    detail::ExprParser parser;
    return parser.parse(json, path);
}

/// @brief The units a bundle declares: each unit's display text and default places, and the exact ratios between
///        the units of one dimension. A client compiles in no unit enum; it converts through this table.
class UnitTable {
public:
    /// @brief One unit.
    struct Unit {
        /// @brief The stable id a quantity names it by (for example `"mg_per_l"`).
        std::string id;
        /// @brief The text a display shows after the amount (for example `"mg/L"`).
        std::string display;
        /// @brief The places a value in this unit is shown with by default.
        std::uint32_t places = 3;
    };

    /// @brief One declared ratio: `1 from == fromTo × to`.
    struct Ratio {
        /// @brief The unit converted from.
        std::string from;
        /// @brief The unit converted to.
        std::string to;
        /// @brief The exact, positive factor.
        math::Rational fromTo;
    };

    /// @brief Declares a unit.
    /// @param unit The unit; a later declaration of the same id replaces it.
    void add(Unit unit) {
        if (auto found = std::ranges::find(_units, unit.id, &Unit::id); found != _units.end()) {
            *found = std::move(unit);
            return;
        }
        _units.push_back(std::move(unit));
    }

    /// @brief Declares a ratio; the reverse direction follows from it.
    /// @param ratio The ratio; its factor must be positive.
    void relate(Ratio ratio) { _ratios.push_back(std::move(ratio)); }

    /// @brief A unit, by id.
    /// @param id The unit's id.
    /// @return The unit, or null when the table does not declare it.
    [[nodiscard]] Unit const* find(std::string_view id) const noexcept {
        auto const found = std::ranges::find(_units, id, &Unit::id);
        return found == _units.end() ? nullptr : &*found;
    }

    /// @brief The exact factor that converts an amount in @p from into @p to, along the declared ratios.
    /// @param from The unit converted from.
    /// @param to The unit converted to.
    /// @return The factor, or nothing when no chain of ratios joins the two or the product overflows.
    [[nodiscard]] std::optional<math::Rational> factor(std::string_view from, std::string_view to) const {
        // A breadth-first walk over the ratios, each edge usable both ways: a dimension holds a handful of units,
        // so the walk is short, and it lets the table declare a chain instead of every pair.
        std::vector<std::pair<std::string_view, math::Rational>> reached{{from, math::Rational{1, {}}}};
        for (std::size_t next = 0; next < reached.size(); ++next) {
            auto const [unit, soFar] = reached.at(next);
            if (unit == to) {
                return soFar;
            }
            for (Ratio const& ratio : _ratios) {
                std::optional<std::pair<std::string_view, math::Rational>> step;
                if (ratio.from == unit) {
                    step.emplace(ratio.to, ratio.fromTo);
                } else if (ratio.to == unit) {
                    std::expected<math::Rational, math::RationalError> inverse = ratio.fromTo.reciprocal();
                    if (inverse) {
                        step.emplace(ratio.from, *inverse);
                    }
                }
                if (!step ||
                    std::ranges::find(reached, step->first, &decltype(reached)::value_type::first) != reached.end()) {
                    continue;
                }
                std::expected<math::Rational, math::RationalError> product = math::checkedMul(soFar, step->second);
                if (product) {
                    reached.emplace_back(step->first, *product);
                }
            }
        }
        return std::nullopt;
    }

    /// @brief Every declared unit, in declaration order.
    /// @return The units.
    [[nodiscard]] std::vector<Unit> const& units() const noexcept { return _units; }

    /// @brief Every declared ratio, in declaration order.
    /// @return The ratios.
    [[nodiscard]] std::vector<Ratio> const& ratios() const noexcept { return _ratios; }

private:
    std::vector<Unit> _units;
    std::vector<Ratio> _ratios;
};

/// @brief Writes dates, times and money for display. morph carries no CLDR date or currency patterns, so a
///        renderer with its toolkit's locale data supplies its own; this default writes ISO-8601 in the display
///        zone, and an amount followed by its currency code.
class DisplayFormatter {
public:
    DisplayFormatter() = default;
    DisplayFormatter(DisplayFormatter const&) = default;
    DisplayFormatter(DisplayFormatter&&) = default;
    DisplayFormatter& operator=(DisplayFormatter const&) = default;
    DisplayFormatter& operator=(DisplayFormatter&&) = default;
    virtual ~DisplayFormatter() = default;

    /// @brief A date: `YYYY-MM-DD` in the display zone.
    /// @param instant The instant.
    /// @param style `short`, `medium` or `long`; the default writes every style alike.
    /// @param offset The display zone's offset from UTC at @p instant.
    /// @param bcp47Locale The locale.
    /// @return The text.
    [[nodiscard]] virtual std::string date(time::DateTime instant, std::string_view style, std::chrono::minutes offset,
                                           std::string_view bcp47Locale) const {
        static_cast<void>(style);
        static_cast<void>(bcp47Locale);
        return local(instant, offset).substr(0, std::string_view{"YYYY-MM-DD"}.size());
    }

    /// @brief A date and time in the display zone: `short` writes the date, `medium` the date and minutes, `long`
    ///        the date and seconds.
    /// @param instant The instant.
    /// @param style `short`, `medium` or `long`.
    /// @param offset The display zone's offset from UTC at @p instant.
    /// @param bcp47Locale The locale.
    /// @return The text.
    [[nodiscard]] virtual std::string dateTime(time::DateTime instant, std::string_view style,
                                               std::chrono::minutes offset, std::string_view bcp47Locale) const {
        static_cast<void>(bcp47Locale);
        std::string const text = local(instant, offset);
        std::size_t const length = style == "short"  ? std::string_view{"YYYY-MM-DD"}.size()
                                   : style == "long" ? std::string_view{"YYYY-MM-DD HH:MM:SS"}.size()
                                                     : std::string_view{"YYYY-MM-DD HH:MM"}.size();
        return text.substr(0, length);
    }

    /// @brief An amount of money.
    /// @param number The amount, already written with the locale's digits and separators.
    /// @param currency The ISO 4217 code, for example `"EUR"`.
    /// @param bcp47Locale The locale.
    /// @return The text: the amount, a no-break space and the code.
    [[nodiscard]] virtual std::string money(std::string const& number, std::string_view currency,
                                            std::string_view bcp47Locale) const {
        static_cast<void>(bcp47Locale);
        return number + " " + std::string{currency};
    }

private:
    // `YYYY-MM-DD HH:MM:SS` at @p offset from UTC.
    [[nodiscard]] static std::string local(time::DateTime instant, std::chrono::minutes offset) {
        std::string const iso = (instant + offset).toIso8601();
        return iso.substr(0, std::string_view{"YYYY-MM-DD"}.size()) + " " +
               iso.substr(std::string_view{"YYYY-MM-DDT"}.size(), std::string_view{"HH:MM:SS"}.size());
    }
};

/// @brief Where an expression's runtime errors go: each expression path is reported once, however often the
///        expression is evaluated, so a binding that re-evaluates on every change does not flood the channel.
class Diagnostics {
public:
    /// @brief What receives a report.
    using Sink = std::function<void(Problem const&)>;

    /// @brief Diagnostics that keep their reports and also pass each to @p sink.
    /// @param sink Receives each report once; empty to only keep them.
    explicit Diagnostics(Sink sink = {}) : _sink{std::move(sink)} {}

    /// @brief Reports a runtime error at @p path, unless that path has been reported already.
    /// @param path The JSON pointer of the expression node that failed.
    /// @param reason Why it failed.
    void report(std::string const& path, std::string reason) {
        if (std::ranges::find(_reported, path, &Problem::path) != _reported.end()) {
            return;
        }
        _reported.push_back(Problem{.path = path, .reason = std::move(reason)});
        if (_sink) {
            _sink(_reported.back());
        }
    }

    /// @brief Every report so far, in the order made.
    /// @return The reports.
    [[nodiscard]] std::vector<Problem> const& reported() const noexcept { return _reported; }

private:
    Sink _sink;
    std::vector<Problem> _reported;
};

/// @brief What an expression reads from the world: the names in scope, the innermost row, the queries and mutations,
///        and the client's permissions, preferences, environment, session and host values.
///
/// Every reader answers `null` for what it does not know; a context overrides the ones its caller can answer.
class ExpressionContext {
public:
    ExpressionContext() = default;
    ExpressionContext(ExpressionContext const&) = default;
    ExpressionContext(ExpressionContext&&) = default;
    ExpressionContext& operator=(ExpressionContext const&) = default;
    ExpressionContext& operator=(ExpressionContext&&) = default;
    virtual ~ExpressionContext() = default;

    /// @brief A param, state, `let`, element value or app name, by its first segment.
    /// @param name The name, without its path.
    /// @return Its value.
    [[nodiscard]] virtual Value name(std::string_view name) const {
        static_cast<void>(name);
        return {};
    }

    /// @brief The innermost row: the record a repeated node is rendering.
    /// @return The row, or null outside one.
    [[nodiscard]] virtual Value row() const { return {}; }

    /// @brief The payload of the event whose command list is running.
    /// @return The payload, or null outside a command list.
    [[nodiscard]] virtual Value event() const { return {}; }

    /// @brief A query's value.
    /// @param query The query's name.
    /// @return Its last value, or null before it has one.
    [[nodiscard]] virtual Value query(std::string_view query) const {
        static_cast<void>(query);
        return {};
    }

    /// @brief A mutation's last result.
    /// @param mutation The mutation's name.
    /// @return Its last result, or null before it has one.
    [[nodiscard]] virtual Value result(std::string_view mutation) const {
        static_cast<void>(mutation);
        return {};
    }

    /// @brief Whether a query or a mutation is in flight.
    /// @param name The query's or mutation's name.
    /// @return `true` or `false`.
    [[nodiscard]] virtual Value pending(std::string_view name) const {
        static_cast<void>(name);
        return Value{false};
    }

    /// @brief A query's or a mutation's error text.
    /// @param name The query's or mutation's name.
    /// @return The text, or null when it has none.
    [[nodiscard]] virtual Value error(std::string_view name) const {
        static_cast<void>(name);
        return {};
    }

    /// @brief The kind of a query's or a mutation's error: `notFound`, `validation`, `connectionLost`, `denied`,
    ///        `conflict`, `backendChanged` or `other`.
    /// @param name The query's or mutation's name.
    /// @return The kind, or null when it has no error.
    [[nodiscard]] virtual Value errorKind(std::string_view name) const {
        static_cast<void>(name);
        return {};
    }

    /// @brief Whether the signed-in principal may run a query or mutation.
    /// @param name The query's or mutation's name, or `host:name` for a host command.
    /// @return `true` or `false`; null while unknown.
    [[nodiscard]] virtual Value can(std::string_view name) const {
        static_cast<void>(name);
        return {};
    }

    /// @brief The user's choice at a customization point.
    /// @param point The point's name, without its path.
    /// @return The choice, or the point's default.
    [[nodiscard]] virtual Value pref(std::string_view point) const {
        static_cast<void>(point);
        return {};
    }

    /// @brief A client environment attribute: `widthClass`, `locale`, `platform`, `online` or `backend`.
    /// @param attribute The attribute.
    /// @return Its value.
    [[nodiscard]] virtual Value env(std::string_view attribute) const {
        static_cast<void>(attribute);
        return {};
    }

    /// @brief A session attribute: `principal`, `userName`, `displayName`, `userKey`, `tenant` or `authenticated`.
    /// @param attribute The attribute.
    /// @return Its value.
    [[nodiscard]] virtual Value session(std::string_view attribute) const {
        static_cast<void>(attribute);
        return {};
    }

    /// @brief A value the application registered with the client.
    /// @param name The registered name.
    /// @return Its value, or null when the client lacks it.
    [[nodiscard]] virtual Value host(std::string_view name) const {
        static_cast<void>(name);
        return {};
    }

    /// @brief The units quantities convert through.
    /// @return The bundle's unit table; empty by default, so only like units compare and add.
    [[nodiscard]] virtual UnitTable const& units() const {
        static UnitTable const kNone;
        return kNone;
    }

    /// @brief The active locale, for plural rules, `t` and display.
    /// @return A BCP 47 tag; `"en"` by default.
    [[nodiscard]] virtual std::string locale() const { return "en"; }

    /// @brief The active locale's digits and separators, for `number`, `decimal`, `quantity`, `money` and
    ///        `:number`.
    /// @return The locale's facts; the `"C"` spelling by default.
    [[nodiscard]] virtual render::NumericLocale numericLocale() const { return {}; }

    /// @brief The display zone's offset from UTC at an instant, for `date` and `dateTime`.
    /// @param instant The instant shown.
    /// @return The offset; zero (UTC) by default.
    [[nodiscard]] virtual std::chrono::minutes displayOffset(time::DateTime instant) const {
        static_cast<void>(instant);
        return std::chrono::minutes{0};
    }

    /// @brief The message catalogue `t` reads.
    /// @return The provider; empty by default, so every `t` shows its fallback.
    [[nodiscard]] virtual render::TranslationProvider const& translations() const {
        static render::TranslationProvider const kNone;
        return kNone;
    }

    /// @brief What writes dates, times and money.
    /// @return The formatter; the ISO-8601 default unless the renderer supplies its own.
    [[nodiscard]] virtual DisplayFormatter const& displayFormatter() const {
        static DisplayFormatter const kDefault;
        return kDefault;
    }
};

namespace detail {

// The value at a dotted @p path inside @p value: each segment a record's field; null past a missing one.
[[nodiscard]] inline Value valueAt(Value value, std::string_view path) {
    while (!path.empty() && !value.isNull()) {
        std::size_t const dot = path.find('.');
        Value const* const next = value.field(path.substr(0, dot));
        value = next == nullptr ? Value{} : *next;
        path = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
    }
    return value;
}

// The first segment of a dotted name and the path after it.
[[nodiscard]] inline std::pair<std::string_view, std::string_view> splitHead(std::string_view name) {
    std::size_t const dot = name.find('.');
    if (dot == std::string_view::npos) {
        return {name, {}};
    }
    return {name.substr(0, dot), name.substr(dot + 1)};
}

struct Evaluator {
    ExpressionContext const& context;
    Diagnostics& diagnostics;
    // The comprehension variables in scope, innermost last.
    std::vector<std::pair<std::string_view, Value>> locals;

    Value fail(Expr const& node, std::string reason) {
        diagnostics.report(node.path, std::move(reason));
        return {};
    }

    // Kleene's and/or: a deciding operand (false for and, true for or) decides; otherwise a null makes null.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by kMaxExpressionNodes.
    Value kleene(Expr const& node, bool decider) {
        bool unknown = false;
        for (Expr const& operand : node.args) {
            Value const value = eval(operand);
            if (value.isNull()) {
                unknown = true;
                continue;
            }
            bool const* const flag = value.asBool();
            if (flag == nullptr) {
                return fail(operand, "`" + node.name + "` takes bools");
            }
            if (*flag == decider) {
                return Value{decider};
            }
        }
        return unknown ? Value{} : Value{!decider};
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by kMaxExpressionNodes.
    Value call(Expr const& node) {
        if (node.name == "and") {
            return kleene(node, false);
        }
        if (node.name == "or") {
            return kleene(node, true);
        }
        if (node.name == "not") {
            Value const value = eval(node.args.front());
            if (value.isNull()) {
                return value;
            }
            bool const* const flag = value.asBool();
            return flag == nullptr ? fail(node.args.front(), "`not` takes a bool") : Value{!*flag};
        }
        std::vector<Value> operands;
        operands.reserve(node.args.size());
        for (Expr const& operand : node.args) {
            operands.push_back(eval(operand));
        }
        return callFunction(node, operands);
    }

    Value callFunction(Expr const& node, std::vector<Value> const& operands);
    Value format(Expr const& node, std::vector<Value> const& operands);
    Value display(Expr const& node, std::vector<Value> const& operands);
    Value message(Expr const& node, std::string const& key, std::string const& fallback, ValueRecord const& arguments);

    // A comprehension: binds each element of the list to `as` and evaluates the body under it.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by kMaxExpressionNodes.
    Value comprehend(Expr const& node) {
        Value const source = eval(node.args.front());
        if (source.isNull()) {
            return source;
        }
        ValueList const* const elements = source.asList();
        if (elements == nullptr) {
            return fail(node.args.front(),
                        "`" + std::string{node.form == Expr::Form::Map ? "map" : "filter"} + "` takes a list");
        }
        ValueList out;
        for (Value const& element : *elements) {
            locals.emplace_back(node.name, element);
            Value const each = eval(node.args.back());
            locals.pop_back();
            if (node.form == Expr::Form::Map) {
                out.push_back(each);
                continue;
            }
            if (!each.isNull() && each.asBool() == nullptr) {
                return fail(node.args.back(), "`where` takes a bool");
            }
            if (each == Value{true}) {
                out.push_back(element);
            }
        }
        return Value{std::move(out)};
    }

    // NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity): one branch per form.
    Value eval(Expr const& node) {
        switch (node.form) {
            case Expr::Form::Literal:
                return node.literal;
            case Expr::Form::Ref: {
                auto const [head, rest] = splitHead(node.name);
                for (auto local = locals.rbegin(); local != locals.rend(); ++local) {
                    if (local->first == head) {
                        return valueAt(local->second, rest);
                    }
                }
                return valueAt(context.name(head), rest);
            }
            case Expr::Form::Event:
                return valueAt(context.event(), node.name);
            case Expr::Form::Row:
                return valueAt(context.row(), node.name);
            case Expr::Form::Query:
                return context.query(node.name);
            case Expr::Form::Result:
                return context.result(node.name);
            case Expr::Form::Pending:
                return context.pending(node.name);
            case Expr::Form::Error:
                return context.error(node.name);
            case Expr::Form::ErrorKind:
                return context.errorKind(node.name);
            case Expr::Form::Can:
                return context.can(node.name);
            case Expr::Form::Pref: {
                auto const [head, rest] = splitHead(node.name);
                return valueAt(context.pref(head), rest);
            }
            case Expr::Form::Env:
                return context.env(node.name);
            case Expr::Form::Session:
                return context.session(node.name);
            case Expr::Form::Host:
                return context.host(node.name);
            case Expr::Form::Call:
                return call(node);
            case Expr::Form::Translate: {
                ValueRecord arguments;
                for (std::size_t index = 0; index < node.args.size(); ++index) {
                    arguments.emplace_back(node.keys.at(index), eval(node.args.at(index)));
                }
                return message(node, node.name, node.fallback, arguments);
            }
            case Expr::Form::If: {
                Value const condition = eval(node.args.at(0));
                if (condition.isNull()) {
                    return condition;
                }
                bool const* const flag = condition.asBool();
                if (flag == nullptr) {
                    return fail(node.args.at(0), "`if` takes a bool condition");
                }
                return eval(node.args.at(*flag ? 1 : 2));
            }
            case Expr::Form::Object: {
                ValueRecord fields;
                for (std::size_t index = 0; index < node.args.size(); ++index) {
                    fields.emplace_back(node.keys.at(index), eval(node.args.at(index)));
                }
                return Value{std::move(fields)};
            }
            case Expr::Form::List: {
                ValueList elements;
                for (Expr const& operand : node.args) {
                    elements.push_back(eval(operand));
                }
                return Value{std::move(elements)};
            }
            case Expr::Form::Map:
            case Expr::Form::Filter:
            default:
                return comprehend(node);
        }
    }
};

}  // namespace detail

namespace detail {

// An int, decimal or quantity operand as an exact amount; `unit` is empty for a plain number.
struct Amount {
    math::Rational value;
    std::string unit;
    bool integral = false;
};

[[nodiscard]] inline std::optional<Amount> amountOf(Value const& value) {
    if (std::int64_t const* const number = value.asInt(); number != nullptr) {
        return Amount{.value = math::Rational{*number, math::DecimalPlaces{0}}, .unit = {}, .integral = true};
    }
    if (math::Rational const* const decimal = value.asDecimal(); decimal != nullptr) {
        return Amount{.value = *decimal, .unit = {}, .integral = false};
    }
    if (QuantityValue const* const quantity = value.asQuantity(); quantity != nullptr) {
        return Amount{.value = quantity->amount, .unit = quantity->unit, .integral = false};
    }
    return std::nullopt;
}

[[nodiscard]] inline Value valueOf(Amount const& amount) {
    if (!amount.unit.empty()) {
        return Value{QuantityValue{.amount = amount.value, .unit = amount.unit}};
    }
    if (amount.integral) {
        return Value{amount.value.numerator};
    }
    return Value{amount.value};
}

// The outcome of one step of arithmetic: the value, or why there is none.
using Exact = std::expected<Amount, std::string>;

// @p amount in @p unit, through the table, keeping its declared places.
[[nodiscard]] inline Exact convert(Amount amount, std::string const& unit, UnitTable const& units) {
    if (amount.unit == unit) {
        return amount;
    }
    std::optional<math::Rational> const factor = units.factor(amount.unit, unit);
    if (!factor) {
        return std::unexpected("no conversion from `" + amount.unit + "` to `" + unit + "`");
    }
    std::expected<math::Rational, math::RationalError> converted = math::checkedMul(amount.value, *factor);
    if (!converted) {
        return std::unexpected("overflow");
    }
    math::Rational value = *converted;
    // The factor's own places would otherwise widen the result's: a value keeps the places it was declared with.
    value = math::Rational{math::Numerator{value.numerator}, math::Denominator{value.denominator},
                           amount.value.getDecimalPlaces()};
    return Amount{.value = value, .unit = unit, .integral = false};
}

// Brings two operands to one unit: a quantity's into the left one's, a plain number with a quantity is refused.
[[nodiscard]] inline std::expected<std::pair<Amount, Amount>, std::string> aligned(Amount lhs, Amount rhs,
                                                                                   UnitTable const& units) {
    if (lhs.unit.empty() != rhs.unit.empty()) {
        return std::unexpected("a quantity meets a plain number");
    }
    if (lhs.unit.empty()) {
        return std::pair{std::move(lhs), std::move(rhs)};
    }
    Exact converted = convert(std::move(rhs), lhs.unit, units);
    if (!converted) {
        return std::unexpected(std::move(converted.error()));
    }
    return std::pair{std::move(lhs), std::move(*converted)};
}

[[nodiscard]] inline Exact arithmetic(std::string_view op, Amount const& lhs, Amount const& rhs,
                                      UnitTable const& units) {
    if (op == "mul" || op == "div") {
        if (!lhs.unit.empty() && !rhs.unit.empty()) {
            return std::unexpected("`" + std::string{op} + "` takes at most one quantity");
        }
        if (op == "div" && !rhs.unit.empty()) {
            return std::unexpected("`div` divides a quantity by a number, not a number by a quantity");
        }
        if (op == "div" && rhs.value.isZero()) {
            return std::unexpected("division by zero");
        }
        if (op == "mul" && lhs.integral && rhs.integral) {
            if (math::detail::mulOverflows(lhs.value.numerator, rhs.value.numerator)) {
                return std::unexpected("overflow");
            }
            return Amount{.value = math::Rational{lhs.value.numerator * rhs.value.numerator, math::DecimalPlaces{0}},
                          .unit = {},
                          .integral = true};
        }
        std::expected<math::Rational, math::RationalError> const product =
            op == "mul" ? math::checkedMul(lhs.value, rhs.value) : math::checkedDiv(lhs.value, rhs.value);
        if (!product) {
            return std::unexpected("overflow");
        }
        return Amount{.value = *product, .unit = lhs.unit.empty() ? rhs.unit : lhs.unit, .integral = false};
    }
    std::expected<std::pair<Amount, Amount>, std::string> both = aligned(lhs, rhs, units);
    if (!both) {
        return std::unexpected(std::move(both.error()));
    }
    auto const& [left, right] = *both;
    bool const add = op == "add";
    if (left.integral && right.integral) {
        std::int64_t const lhsInt = left.value.numerator;
        std::int64_t const rhsInt = right.value.numerator;
        if (add ? math::detail::addOverflows(lhsInt, rhsInt) : math::detail::subOverflows(lhsInt, rhsInt)) {
            return std::unexpected("overflow");
        }
        return Amount{.value = math::Rational{add ? lhsInt + rhsInt : lhsInt - rhsInt, math::DecimalPlaces{0}},
                      .unit = {},
                      .integral = true};
    }
    std::expected<math::Rational, math::RationalError> const sum =
        add ? math::checkedAdd(left.value, right.value) : math::checkedSub(left.value, right.value);
    if (!sum) {
        return std::unexpected("overflow");
    }
    return Amount{.value = *sum, .unit = left.unit, .integral = false};
}

// The order of two values of one comparable kind, or why they have none.
[[nodiscard]] inline std::expected<std::strong_ordering, std::string> compare(Value const& lhs, Value const& rhs,
                                                                              UnitTable const& units) {
    std::optional<Amount> const left = amountOf(lhs);
    std::optional<Amount> const right = amountOf(rhs);
    if (left && right) {
        std::expected<std::pair<Amount, Amount>, std::string> both = aligned(*left, *right, units);
        if (!both) {
            return std::unexpected(std::move(both.error()));
        }
        return both->first.value <=> both->second.value;
    }
    if (lhs.asString() != nullptr && rhs.asString() != nullptr) {
        int const order = lhs.asString()->compare(*rhs.asString());
        return order < 0   ? std::strong_ordering::less
               : order > 0 ? std::strong_ordering::greater
                           : std::strong_ordering::equal;
    }
    if (lhs.asTimestamp() != nullptr && rhs.asTimestamp() != nullptr) {
        return *lhs.asTimestamp() <=> *rhs.asTimestamp();
    }
    return std::unexpected("cannot compare " + withArticle(std::string{kindName(lhs.kind())}) + " with " +
                           withArticle(std::string{kindName(rhs.kind())}));
}

// Whether two values are equal: numbers and quantities by exact value across kinds and units, anything else as
// `Value` compares.
[[nodiscard]] inline std::expected<bool, std::string> equal(Value const& lhs, Value const& rhs,
                                                            UnitTable const& units) {
    if (amountOf(lhs) && amountOf(rhs)) {
        return compare(lhs, rhs, units).transform([](std::strong_ordering order) { return std::is_eq(order); });
    }
    return lhs == rhs;
}

// The number of code points in UTF-8 @p text.
[[nodiscard]] inline std::int64_t codePoints(std::string_view text) {
    return static_cast<std::int64_t>(
        std::ranges::count_if(text, [](char byte) { return (static_cast<unsigned char>(byte) & 0xC0U) != 0x80U; }));
}

// @p text without leading and trailing white space (ASCII, and U+00A0 no-break space).
[[nodiscard]] inline std::string trimmed(std::string_view text) {
    auto const leading = [](std::string_view rest) -> std::size_t {
        if (!rest.empty() && (rest.front() == ' ' || (rest.front() >= '\t' && rest.front() <= '\r'))) {
            return 1;
        }
        return rest.starts_with("\xC2\xA0") ? 2 : 0;
    };
    auto const trailing = [](std::string_view rest) -> std::size_t {
        if (!rest.empty() && (rest.back() == ' ' || (rest.back() >= '\t' && rest.back() <= '\r'))) {
            return 1;
        }
        return rest.ends_with("\xC2\xA0") ? 2 : 0;
    };
    for (std::size_t step = leading(text); step != 0; step = leading(text)) {
        text.remove_prefix(step);
    }
    for (std::size_t step = trailing(text); step != 0; step = trailing(text)) {
        text.remove_suffix(step);
    }
    return std::string{text};
}

// A value as `format` writes it: a decimal at its places with `.`, a quantity followed by its unit id.
[[nodiscard]] inline std::string plainText(Value const& value) {
    switch (value.kind()) {
        case Value::Kind::String:
            return *value.asString();
        case Value::Kind::Bool:
            return *value.asBool() ? "true" : "false";
        case Value::Kind::Int:
            return std::to_string(*value.asInt());
        case Value::Kind::Decimal:
            return decimalText(*value.asDecimal(), value.asDecimal()->getDecimalPlaces().value);
        case Value::Kind::Quantity:
            return decimalText(value.asQuantity()->amount, value.asQuantity()->amount.getDecimalPlaces().value) + " " +
                   value.asQuantity()->unit;
        case Value::Kind::Timestamp:
            return value.asTimestamp()->toIso8601();
        case Value::Kind::Null:
            return {};
        case Value::Kind::List:
        case Value::Kind::Record:
        default:
            return canonicalJson(value);
    }
}

}  // namespace detail

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per function of the library.
inline Value detail::Evaluator::callFunction(Expr const& node, std::vector<Value> const& operands) {
    std::string_view const name = node.name;
    UnitTable const& units = context.units();
    if (name == "engaged") {
        return Value{!operands.front().isNull()};
    }
    if (name == "coalesce") {
        auto const found = std::ranges::find_if(operands, [](Value const& value) { return !value.isNull(); });
        return found == operands.end() ? Value{} : *found;
    }
    // Every other function needs all of its arguments: absence propagates.
    if (std::ranges::any_of(operands, &Value::isNull)) {
        return {};
    }
    auto const mismatch = [&](std::string const& wants) { return fail(node, "`" + node.name + "` takes " + wants); };
    if (name == "eq" || name == "ne") {
        std::expected<bool, std::string> const same = equal(operands.at(0), operands.at(1), units);
        if (!same) {
            return fail(node, same.error());
        }
        return Value{*same == (name == "eq")};
    }
    if (name == "lt" || name == "le" || name == "gt" || name == "ge") {
        std::expected<std::strong_ordering, std::string> const order = compare(operands.at(0), operands.at(1), units);
        if (!order) {
            return fail(node, order.error());
        }
        bool const holds = name == "lt"   ? std::is_lt(*order)
                           : name == "le" ? std::is_lteq(*order)
                           : name == "gt" ? std::is_gt(*order)
                                          : std::is_gteq(*order);
        return Value{holds};
    }
    if (name == "add" || name == "sub" || name == "mul" || name == "div") {
        std::optional<Amount> const lhs = amountOf(operands.at(0));
        std::optional<Amount> const rhs = amountOf(operands.at(1));
        if (!lhs || !rhs) {
            return mismatch("numbers or quantities");
        }
        Exact const outcome = arithmetic(name, *lhs, *rhs, units);
        return outcome ? valueOf(*outcome) : fail(node, outcome.error());
    }
    if (name == "neg") {
        std::optional<Amount> amount = amountOf(operands.front());
        if (!amount) {
            return mismatch("a number or a quantity");
        }
        if (amount->value.numerator == std::numeric_limits<std::int64_t>::min()) {
            return fail(node, "overflow");
        }
        amount->value = -amount->value;
        return valueOf(*amount);
    }
    if (name == "round") {
        std::optional<Amount> amount = amountOf(operands.at(0));
        std::int64_t const* const places = operands.at(1).asInt();
        if (!amount || places == nullptr || *places < 0 ||
            *places > static_cast<std::int64_t>(math::kMaxDecimalPlaces)) {
            return mismatch("a number or a quantity, and places from 0 to 18");
        }
        // roundToDecimalPlaces saturates instead of failing; scaling first finds the overflow it would hide.
        std::int64_t scale = 1;
        for (std::int64_t digit = 0; digit < *places; ++digit) {
            scale *= 10;
        }
        if (!math::checkedMul(amount->value, math::Rational{scale, math::DecimalPlaces{0}})) {
            return fail(node, "overflow");
        }
        amount->value = math::roundToDecimalPlaces(
            amount->value, math::DecimalPlaces{static_cast<std::uint32_t>(*places)}, math::RoundingMode::HalfEven);
        amount->integral = false;
        return valueOf(*amount);
    }
    if (name == "concat") {
        std::string out;
        for (Value const& operand : operands) {
            if (operand.asString() == nullptr) {
                return mismatch("strings");
            }
            out += *operand.asString();
        }
        return Value{std::move(out)};
    }
    if (name == "format") {
        return format(node, operands);
    }
    if (name == "number" || name == "decimal" || name == "quantity" || name == "money" || name == "date" ||
        name == "dateTime") {
        return display(node, operands);
    }
    if (name == "t") {
        std::string const* const key = operands.at(0).asString();
        std::string const* const fallback = operands.size() > 1 ? operands.at(1).asString() : nullptr;
        ValueRecord const* const arguments = operands.size() > 2 ? operands.at(2).asRecord() : nullptr;
        if (key == nullptr || (operands.size() > 1 && fallback == nullptr) ||
            (operands.size() > 2 && arguments == nullptr)) {
            return mismatch("a key, a fallback and an object of arguments");
        }
        return message(node, *key, fallback == nullptr ? std::string{} : *fallback,
                       arguments == nullptr ? ValueRecord{} : *arguments);
    }
    if (name == "trim" || name == "length") {
        std::string const* const text = operands.front().asString();
        if (text == nullptr) {
            return mismatch("a string");
        }
        return name == "trim" ? Value{trimmed(*text)} : Value{codePoints(*text)};
    }
    if (name == "contains") {
        std::string const* const text = operands.at(0).asString();
        std::string const* const part = operands.at(1).asString();
        if (text == nullptr || part == nullptr) {
            return mismatch("two strings");
        }
        return Value{text->find(*part) != std::string::npos};
    }
    if (name == "isEmpty") {
        if (std::string const* const text = operands.front().asString(); text != nullptr) {
            return Value{text->empty()};
        }
        if (ValueList const* const list = operands.front().asList(); list != nullptr) {
            return Value{list->empty()};
        }
        return mismatch("a string or a list");
    }
    ValueList const* const list = operands.front().asList();
    if (list == nullptr) {
        return mismatch("a list");
    }
    if (name == "count") {
        return Value{static_cast<std::int64_t>(list->size())};
    }
    if (name == "first") {
        return list->empty() ? Value{} : list->front();
    }
    if (name == "includes" || name == "indexOf") {
        for (std::size_t index = 0; index < list->size(); ++index) {
            std::expected<bool, std::string> const same = equal(list->at(index), operands.at(1), units);
            if (same && *same) {
                return name == "includes" ? Value{true} : Value{static_cast<std::int64_t>(index)};
            }
        }
        return name == "includes" ? Value{false} : Value{};
    }
    if (name == "any" || name == "all") {
        // Kleene over the elements, as `or` and `and` are over their operands.
        bool const decider = name == "any";
        bool unknown = false;
        for (Value const& element : *list) {
            if (element.isNull()) {
                unknown = true;
                continue;
            }
            if (element.asBool() == nullptr) {
                return mismatch("a list of bools");
            }
            if (*element.asBool() == decider) {
                return Value{decider};
            }
        }
        return unknown ? Value{} : Value{!decider};
    }
    if (name == "min" || name == "max") {
        if (list->empty() || std::ranges::any_of(*list, &Value::isNull)) {
            return {};
        }
        Value best = list->front();
        for (Value const& element : *list) {
            std::expected<std::strong_ordering, std::string> const order = compare(element, best, units);
            if (!order) {
                return fail(node, order.error());
            }
            if (name == "min" ? std::is_lt(*order) : std::is_gt(*order)) {
                best = element;
            }
        }
        return best;
    }
    // sum and mean.
    if (std::ranges::any_of(*list, &Value::isNull)) {
        return {};
    }
    if (list->empty()) {
        return name == "sum" ? Value{std::int64_t{0}} : Value{};
    }
    std::optional<Amount> total = amountOf(list->front());
    if (!total) {
        return mismatch("a list of numbers or quantities");
    }
    std::uint32_t places = total->value.getDecimalPlaces().value;
    for (std::size_t index = 1; index < list->size(); ++index) {
        std::optional<Amount> const element = amountOf(list->at(index));
        if (!element) {
            return mismatch("a list of numbers or quantities");
        }
        places = std::max(places, element->value.getDecimalPlaces().value);
        Exact const sum = arithmetic("add", *total, *element, units);
        if (!sum) {
            return fail(node, sum.error());
        }
        total = *sum;
    }
    if (name == "sum") {
        return valueOf(*total);
    }
    // The exact quotient, rounded once at the operands' places: no double, so no binary rounding on the way.
    std::expected<math::Rational, math::RationalError> const quotient = math::checkedDiv(
        total->value, math::Rational{static_cast<std::int64_t>(list->size()), math::DecimalPlaces{0}});
    if (!quotient) {
        return fail(node, "overflow");
    }
    total->value = math::roundToDecimalPlaces(*quotient, math::DecimalPlaces{places}, math::RoundingMode::HalfEven);
    total->integral = false;
    return valueOf(*total);
}

// `format(template, args...)`: `{0}` is the first argument after the template, `{{` and `}}` are braces.
inline Value detail::Evaluator::format(Expr const& node, std::vector<Value> const& operands) {
    std::string const* const pattern = operands.front().asString();
    if (pattern == nullptr) {
        return fail(node, "`format` takes a string template");
    }
    std::string out;
    for (std::size_t at = 0; at < pattern->size(); ++at) {
        char const letter = pattern->at(at);
        if ((letter == '{' || letter == '}') && at + 1 < pattern->size() && pattern->at(at + 1) == letter) {
            out += letter;
            ++at;
            continue;
        }
        if (letter == '}') {
            return fail(node, "`format`'s template has an unmatched `}`");
        }
        if (letter != '{') {
            out += letter;
            continue;
        }
        std::size_t const close = pattern->find('}', at);
        std::size_t index = 0;
        std::string_view const digits = std::string_view{*pattern}.substr(at + 1, close - at - 1);
        auto const [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
        if (close == std::string::npos || digits.empty() || error != std::errc{} ||
            end != digits.data() + digits.size()) {
            return fail(node, "`format`'s placeholders are written {0}, {1}, ...");
        }
        if (index + 1 >= operands.size()) {
            return fail(node, "`format` has no argument {" + std::to_string(index) + "}");
        }
        out += plainText(operands.at(index + 1));
        at = close;
    }
    return Value{std::move(out)};
}

// `number`, `decimal`, `quantity`, `money`, `date` and `dateTime`: a value as display text.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per display function.
inline Value detail::Evaluator::display(Expr const& node, std::vector<Value> const& operands) {
    std::string_view const name = node.name;
    std::string const locale = context.locale();
    if (name == "date" || name == "dateTime") {
        time::DateTime const* const instant = operands.front().asTimestamp();
        std::string const* const style = operands.size() > 1 ? operands.at(1).asString() : nullptr;
        if (instant == nullptr || (operands.size() > 1 && style == nullptr)) {
            return fail(node, "`" + node.name + "` takes a timestamp and a style");
        }
        std::string_view const chosen = style == nullptr ? std::string_view{"medium"} : std::string_view{*style};
        if (chosen != "short" && chosen != "medium" && chosen != "long") {
            return fail(node, "a style is `short`, `medium` or `long`");
        }
        DisplayFormatter const& formatter = context.displayFormatter();
        std::chrono::minutes const offset = context.displayOffset(*instant);
        return Value{name == "date" ? formatter.date(*instant, chosen, offset, locale)
                                    : formatter.dateTime(*instant, chosen, offset, locale)};
    }
    std::optional<Amount> amount = amountOf(operands.front());
    if (!amount) {
        return fail(node, "`" + node.name + "` takes a number or a quantity");
    }
    render::NumericLocale const numeric = context.numericLocale();
    auto const digits = [&numeric](math::Rational const& value, std::uint32_t places) {
        return render::formatCanonicalNumber(decimalText(value, places), numeric);
    };
    std::uint32_t const places = amount->value.getDecimalPlaces().value;
    if (name == "number") {
        return Value{digits(amount->value, places)};
    }
    if (name == "decimal") {
        std::int64_t const* const wanted = operands.at(1).asInt();
        if (wanted == nullptr || *wanted < 0 || *wanted > static_cast<std::int64_t>(math::kMaxDecimalPlaces)) {
            return fail(node, "`decimal` takes places from 0 to 18");
        }
        return Value{digits(amount->value, static_cast<std::uint32_t>(*wanted))};
    }
    if (name == "money") {
        std::string const* const currency = operands.at(1).asString();
        if (currency == nullptr || !amount->unit.empty()) {
            return fail(node, "`money` takes a number and a currency code");
        }
        return Value{context.displayFormatter().money(digits(amount->value, places), *currency, locale)};
    }
    // quantity(x, unit?): in the unit asked for, or its own, with the unit's display text.
    if (amount->unit.empty()) {
        return fail(node, "`quantity` takes a quantity");
    }
    UnitTable const& units = context.units();
    if (operands.size() > 1) {
        std::string const* const unit = operands.at(1).asString();
        if (unit == nullptr) {
            return fail(node, "`quantity`'s unit is a unit id");
        }
        Exact converted = convert(*amount, *unit, units);
        if (!converted) {
            return fail(node, converted.error());
        }
        amount = std::move(*converted);
    }
    UnitTable::Unit const* const unit = units.find(amount->unit);
    return Value{digits(amount->value, places) + " " + (unit == nullptr ? amount->unit : unit->display)};
}

// `t(key, fallback, args)`: the catalogue's message for the key in the active locale, else the fallback, formatted
// with the arguments. A catalogue message outside the supported subset is reported and the fallback shown.
inline Value detail::Evaluator::message(Expr const& node, std::string const& key, std::string const& fallback,
                                        ValueRecord const& arguments) {
    std::string const locale = context.locale();
    std::optional<std::string> catalogued;
    if (render::TranslationProvider const& translations = context.translations(); translations) {
        catalogued = translations(key, locale);
    }
    std::expected<Message, std::string> parsed = parseMessage(catalogued.value_or(fallback));
    if (!parsed && catalogued) {
        diagnostics.report(node.path, "the catalogue's `" + key + "` is refused: " + parsed.error());
        parsed = parseMessage(fallback);
    }
    if (!parsed) {
        return fail(node, "the fallback is refused: " + parsed.error());
    }
    std::vector<std::pair<std::string, MessageArgument>> named;
    for (auto const& [name, value] : arguments) {
        if (value.isNull()) {
            continue;
        }
        MessageArgument argument{.number = std::nullopt, .places = 0, .text = {}};
        if (std::optional<Amount> const amount = amountOf(value); amount && amount->unit.empty()) {
            argument.number = amount->value;
            argument.places = amount->value.getDecimalPlaces().value;
        } else {
            argument.text = plainText(value);
        }
        named.emplace_back(name, std::move(argument));
    }
    std::expected<std::string, std::string> text = formatMessage(*parsed, named, locale, context.numericLocale());
    if (!text) {
        return fail(node, text.error());
    }
    return Value{std::move(*text)};
}

/// @brief Evaluates an expression. Never throws: a runtime error (a type mismatch, an overflow, a division by zero)
///        yields `null` and is reported once per expression path to @p diagnostics.
/// @param expr The expression.
/// @param context What the expression reads.
/// @param diagnostics Where runtime errors go.
/// @return The value.
[[nodiscard]] inline Value evaluate(Expr const& expr, ExpressionContext const& context, Diagnostics& diagnostics) {
    detail::Evaluator evaluator{.context = context, .diagnostics = diagnostics, .locals = {}};
    return evaluator.eval(expr);
}

}  // namespace morph::ui
