// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <glaze/glaze.hpp>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "../util/datetime.hpp"
#include "../util/rational.hpp"

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

}  // namespace morph::ui
