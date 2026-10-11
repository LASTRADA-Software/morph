// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <glaze/glaze.hpp>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "../util/datetime.hpp"
#include "../util/rational.hpp"

/// @file
/// @brief The UI document's typed values (`ui::Value`), its types (`ui::Type`), their JSON form, and `ui::Refusal`,
///        the error every load-time check of the document layer returns.
///
/// Specified in `docs/spec/ui/document.md`, "Values and types".

namespace morph::ui {

/// @brief The JSON tree the document layer reads and writes: glaze's generic value in its integer-preserving mode,
///        so an integer is never read through a double.
using Json = glz::generic_u64;

/// @brief Why a document, an expression or a value was refused: where, and the reason.
struct Refusal {
    /// @brief The id of the document refused; empty when the refusal is not about a whole document.
    std::string document;
    /// @brief A JSON Pointer (RFC 6901) to the refused part, `""` for the whole input.
    std::string path;
    /// @brief The reason, in one sentence.
    std::string reason;

    /// @brief The refusal as one line of text.
    /// @return `document <id> at <path>: <reason>`, leaving out the parts that are empty.
    [[nodiscard]] std::string message() const {
        std::string out;
        if (!document.empty()) {
            out += "document " + document + " ";
        }
        out += "at " + (path.empty() ? std::string{"/"} : path) + ": " + reason;
        return out;
    }

    /// @brief Field-wise equality.
    /// @param other The refusal to compare with.
    /// @return Whether the document, path and reason are equal.
    bool operator==(Refusal const& other) const = default;
};

/// @brief Appends one reference token to a JSON Pointer, escaping `~` and `/` as RFC 6901 requires.
/// @param base  The pointer to extend, `""` for the root.
/// @param token The object key to append.
/// @return `base + "/" + escaped(token)`.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the pointer, then the token appended to it, as they read.
[[nodiscard]] inline std::string pointerAppend(std::string_view base, std::string_view token) {
    std::string out{base};
    out += '/';
    for (char const chr : token) {
        if (chr == '~') {
            out += "~0";
        } else if (chr == '/') {
            out += "~1";
        } else {
            out += chr;
        }
    }
    return out;
}

/// @brief Appends an array index to a JSON Pointer.
/// @param base  The pointer to extend, `""` for the root.
/// @param index The array index to append.
/// @return `base + "/" + index`.
[[nodiscard]] inline std::string pointerAppend(std::string_view base, std::size_t index) {
    return std::string{base} + "/" + std::to_string(index);
}

/// @brief An exact decimal: a `math::Rational` whose `DecimalPlaces` are the value's declared places.
///
/// The places are part of the value: two decimals equal in magnitude and different in places compare unequal,
/// because they display differently. `expr/1`'s `eq` compares magnitudes.
struct Decimal {
    /// @brief The exact value, with its declared places.
    math::Rational value;

    /// @brief Equality of magnitude and places.
    /// @param other The decimal to compare with.
    /// @return Whether both the canonical fraction and the places are equal.
    [[nodiscard]] bool operator==(Decimal const& other) const noexcept {
        return value.numerator == other.value.numerator && value.denominator == other.value.denominator &&
               value.decimalPlaces == other.value.decimalPlaces;
    }
};

/// @brief An exact decimal with a unit: the id of a unit of the server's unit systems (`x-unitAlternatives`).
struct Quantity {
    /// @brief The exact magnitude, with its declared places.
    math::Rational value;
    /// @brief The unit id, such as `mg/L`.
    std::string unit;

    /// @brief Equality of magnitude, places and unit.
    /// @param other The quantity to compare with.
    /// @return Whether magnitude, places and unit are all equal.
    [[nodiscard]] bool operator==(Quantity const& other) const noexcept {
        return Decimal{value} == Decimal{other.value} && unit == other.unit;
    }
};

class Value;

/// @brief A list value.
using ValueList = std::vector<Value>;

/// @brief A record value: field name to value, ordered by name.
using ValueRecord = std::map<std::string, Value, std::less<>>;

/// @brief The kinds a `Value` holds.
enum class ValueKind : std::uint8_t {
    Null,       ///< No value: absent, not entered, or the result of an error.
    Bool,       ///< `bool`.
    Int,        ///< `int`: a 64-bit signed integer, exact.
    String,     ///< `string`: UTF-8 text.
    Decimal,    ///< `decimal`: an exact rational with declared places.
    Quantity,   ///< `quantity`: a decimal with a unit.
    Timestamp,  ///< `timestamp`: a UTC instant with millisecond precision.
    List,       ///< `list<T>`.
    Record,     ///< A record of named fields.
};

/// @brief The name of a value kind, as a document spells its type.
/// @param kind The kind.
/// @return `null`, `bool`, `int`, `string`, `decimal`, `quantity`, `timestamp`, `list` or `record`.
[[nodiscard]] constexpr std::string_view kindName(ValueKind kind) {
    constexpr std::array<std::string_view, 9> kNames = {"null",     "bool",      "int",  "string", "decimal",
                                                        "quantity", "timestamp", "list", "record"};
    return kNames.at(static_cast<std::size_t>(kind));
}

/// @brief The document's typed value (spec 5 §3): what every expression evaluates to.
///
/// A value is immutable once built. Lists and records share their storage between copies, so copying a value —
/// which the reactive core does on every read — costs a reference count, not a deep copy. Equality is deep.
class Value {
public:
    /// @brief Constructs `null`.
    Value() noexcept = default;

    /// @brief Constructs `null`.
    /// @param null Unused.
    Value(std::nullptr_t null) noexcept { static_cast<void>(null); }  // NOLINT(google-explicit-constructor)

    /// @brief Constructs a `bool`.
    /// @param flag The value.
    Value(bool flag) noexcept : _data{flag} {}  // NOLINT(google-explicit-constructor)

    /// @brief Constructs an `int` from any signed integer.
    /// @tparam I A signed integer type.
    /// @param number The value.
    template <std::signed_integral I>
    Value(I number) noexcept : _data{static_cast<std::int64_t>(number)} {}  // NOLINT(google-explicit-constructor)

    /// @brief Constructs a `string`.
    /// @param text UTF-8 text.
    Value(std::string text) noexcept : _data{std::move(text)} {}  // NOLINT(google-explicit-constructor)

    /// @brief Constructs a `string` from a C string, which would otherwise convert to `bool`.
    /// @param text NUL-terminated UTF-8 text.
    Value(char const* text) : _data{std::string{text}} {}  // NOLINT(google-explicit-constructor)

    /// @brief Constructs a `decimal`.
    /// @param number The exact value with its places.
    Value(Decimal number) noexcept : _data{number} {}  // NOLINT(google-explicit-constructor)

    /// @brief Constructs a `quantity`.
    /// @param quantity The magnitude and unit.
    Value(Quantity quantity) noexcept : _data{std::move(quantity)} {}  // NOLINT(google-explicit-constructor)

    /// @brief Constructs a `timestamp`.
    /// @param instant The UTC instant.
    Value(time::DateTime instant) noexcept : _data{instant} {}  // NOLINT(google-explicit-constructor)

    /// @brief Constructs a list.
    /// @param items The elements.
    Value(ValueList items)  // NOLINT(google-explicit-constructor)
        : _data{std::make_shared<ValueList const>(std::move(items))} {}

    /// @brief Constructs a record.
    /// @param fields The fields.
    Value(ValueRecord fields)  // NOLINT(google-explicit-constructor)
        : _data{std::make_shared<ValueRecord const>(std::move(fields))} {}

    /// @brief The kind of value held.
    /// @return The kind.
    [[nodiscard]] ValueKind kind() const noexcept { return static_cast<ValueKind>(_data.index()); }

    /// @brief Whether the value is `null`.
    /// @return `kind() == ValueKind::Null`.
    [[nodiscard]] bool isNull() const noexcept { return _data.index() == 0; }

    /// @brief The `bool`, if held.
    /// @return A pointer to it, or `nullptr`.
    [[nodiscard]] bool const* asBool() const noexcept { return std::get_if<bool>(&_data); }

    /// @brief The `int`, if held.
    /// @return A pointer to it, or `nullptr`.
    [[nodiscard]] std::int64_t const* asInt() const noexcept { return std::get_if<std::int64_t>(&_data); }

    /// @brief The `string`, if held.
    /// @return A pointer to it, or `nullptr`.
    [[nodiscard]] std::string const* asString() const noexcept { return std::get_if<std::string>(&_data); }

    /// @brief The `decimal`, if held.
    /// @return A pointer to it, or `nullptr`.
    [[nodiscard]] Decimal const* asDecimal() const noexcept { return std::get_if<Decimal>(&_data); }

    /// @brief The `quantity`, if held.
    /// @return A pointer to it, or `nullptr`.
    [[nodiscard]] Quantity const* asQuantity() const noexcept { return std::get_if<Quantity>(&_data); }

    /// @brief The `timestamp`, if held.
    /// @return A pointer to it, or `nullptr`.
    [[nodiscard]] time::DateTime const* asTimestamp() const noexcept { return std::get_if<time::DateTime>(&_data); }

    /// @brief The list, if held.
    /// @return A pointer to it, or `nullptr`.
    [[nodiscard]] ValueList const* asList() const noexcept {
        auto const* held = std::get_if<std::shared_ptr<ValueList const>>(&_data);
        return held == nullptr ? nullptr : held->get();
    }

    /// @brief The record, if held.
    /// @return A pointer to it, or `nullptr`.
    [[nodiscard]] ValueRecord const* asRecord() const noexcept {
        auto const* held = std::get_if<std::shared_ptr<ValueRecord const>>(&_data);
        return held == nullptr ? nullptr : held->get();
    }

    /// @brief A field of a record.
    /// @param name The field name.
    /// @return A pointer to the field, or `nullptr` when this is not a record or has no such field.
    [[nodiscard]] Value const* field(std::string_view name) const {
        ValueRecord const* const record = asRecord();
        if (record == nullptr) {
            return nullptr;
        }
        auto const found = record->find(name);
        return found == record->end() ? nullptr : &found->second;
    }

    /// @brief Deep equality: same kind and equal content; a decimal's places take part.
    /// @param other The value to compare with.
    /// @return Whether the two values are equal.
    // NOLINTNEXTLINE(misc-no-recursion): a list or a record compares its elements, which are values.
    [[nodiscard]] bool operator==(Value const& other) const {
        if (_data.index() != other._data.index()) {
            return false;
        }
        if (ValueList const* const mine = asList()) {
            return *mine == *other.asList();
        }
        if (ValueRecord const* const mine = asRecord()) {
            return *mine == *other.asRecord();
        }
        return _data == other._data;
    }

private:
    std::variant<std::monostate, bool, std::int64_t, std::string, Decimal, Quantity, time::DateTime,
                 std::shared_ptr<ValueList const>, std::shared_ptr<ValueRecord const>>
        _data;
};

/// @brief The kinds of a `Type`.
enum class TypeKind : std::uint8_t {
    Any,        ///< Not known to the document: a query result, a reply, an element's value.
    Bool,       ///< `bool`.
    Int,        ///< `int`.
    String,     ///< `string`.
    Decimal,    ///< `decimal`.
    Quantity,   ///< `quantity`.
    Timestamp,  ///< `timestamp`.
    Key,        ///< `key`: an `int` or a `string`.
    Record,     ///< `record`: a record whose fields the document does not declare.
    List,       ///< `list<T>`.
};

/// @brief A document type (spec 5 §3): a kind, whether `null` is allowed, and a list's element type.
struct Type {
    /// @brief The kind.
    TypeKind kind = TypeKind::Any;
    /// @brief Whether the type is `T?`, admitting `null`.
    bool optional = false;
    /// @brief The element type of a `list<T>`; `nullptr` for any other kind.
    std::shared_ptr<Type const> element;

    /// @brief The type the document cannot know.
    /// @return `any`.
    [[nodiscard]] static Type any() { return Type{}; }

    /// @brief A scalar or record type.
    /// @param kind     The kind; not `List`.
    /// @param optional Whether it admits `null`.
    /// @return The type.
    [[nodiscard]] static Type of(TypeKind kind, bool optional = false) {
        return Type{.kind = kind, .optional = optional, .element = nullptr};
    }

    /// @brief A list type.
    /// @param element  The element type.
    /// @param optional Whether the list itself admits `null`.
    /// @return `list<element>`, or `list<element>?`.
    [[nodiscard]] static Type listOf(Type element, bool optional = false) {
        return Type{
            .kind = TypeKind::List, .optional = optional, .element = std::make_shared<Type const>(std::move(element))};
    }

    /// @brief Whether the type is `any`.
    /// @return `kind == TypeKind::Any`.
    [[nodiscard]] bool isAny() const noexcept { return kind == TypeKind::Any; }

    /// @brief The type as a document spells it.
    /// @return For example `int`, `decimal?`, `list<key>`.
    // NOLINTNEXTLINE(misc-no-recursion): a list type names its element type.
    [[nodiscard]] std::string name() const {
        constexpr std::array<std::string_view, 10> kNames = {"any",      "bool",      "int", "string", "decimal",
                                                             "quantity", "timestamp", "key", "record", "list"};
        std::string out{kNames.at(static_cast<std::size_t>(kind))};
        if (kind == TypeKind::List) {
            out += "<" + (element ? element->name() : std::string{"any"}) + ">";
        }
        if (optional) {
            out += '?';
        }
        return out;
    }

    /// @brief Whether a value conforms to the type.
    ///
    /// `null` conforms to an optional type and to `any`; an `int` conforms to `decimal`, which holds it exactly; a
    /// list conforms when every element conforms to the element type.
    /// @param value The value to test.
    /// @return Whether @p value is a value of this type.
    // NOLINTNEXTLINE(misc-no-recursion): a list type admits a list whose elements its element type admits.
    [[nodiscard]] bool admits(Value const& value) const {
        if (kind == TypeKind::Any) {
            return true;
        }
        if (value.isNull()) {
            return optional;
        }
        ValueKind const held = value.kind();
        if (kind == TypeKind::Key) {
            return held == ValueKind::Int || held == ValueKind::String;
        }
        if (kind == TypeKind::Decimal && held == ValueKind::Int) {
            return true;
        }
        if (kind != TypeKind::List) {
            // The value kind each scalar or record type holds, indexed by `TypeKind`; `any` and `key` are handled
            // above, so their entries are never read.
            constexpr std::array<ValueKind, 10> kHolds = {
                ValueKind::Null,     ValueKind::Bool,      ValueKind::Int,  ValueKind::String, ValueKind::Decimal,
                ValueKind::Quantity, ValueKind::Timestamp, ValueKind::Null, ValueKind::Record, ValueKind::List};
            return kHolds.at(static_cast<std::size_t>(kind)) == held;
        }
        ValueList const* const items = value.asList();
        if (items == nullptr) {
            return false;
        }
        // NOLINTNEXTLINE(misc-no-recursion): each element is admitted by the element type.
        return std::ranges::all_of(*items, [this](Value const& item) { return !element || element->admits(item); });
    }

    /// @brief Deep equality.
    /// @param other The type to compare with.
    /// @return Whether kind, optionality and element type are equal.
    // NOLINTNEXTLINE(misc-no-recursion): a list type compares its element types.
    [[nodiscard]] bool operator==(Type const& other) const {
        if (kind != other.kind || optional != other.optional) {
            return false;
        }
        if (!element || !other.element) {
            return !element && !other.element;
        }
        return *element == *other.element;
    }
};

namespace detail {

/// @brief Parses one type at the front of @p text, advancing it.
/// @param text  The remaining text; on success, what follows the type.
/// @param depth The `list<>` nesting depth so far, bounded so a hostile type cannot recurse without limit.
/// @return The type, or the reason it does not parse.
// NOLINTNEXTLINE(misc-no-recursion): `list<T>` parses `T`; the depth bound stops a hostile nesting.
[[nodiscard]] inline std::expected<Type, std::string> parseTypePrefix(std::string_view& text, int depth) {
    constexpr int kMaxDepth = 8;
    if (depth > kMaxDepth) {
        return std::unexpected(std::string{"type nests list<> too deeply"});
    }
    static constexpr std::array<std::pair<std::string_view, TypeKind>, 9> kScalars = {{
        {"bool", TypeKind::Bool},
        {"int", TypeKind::Int},
        {"string", TypeKind::String},
        {"decimal", TypeKind::Decimal},
        {"quantity", TypeKind::Quantity},
        {"timestamp", TypeKind::Timestamp},
        {"key", TypeKind::Key},
        {"record", TypeKind::Record},
        {"any", TypeKind::Any},
    }};
    Type result;
    if (text.starts_with("list<")) {
        text.remove_prefix(5);
        auto inner = parseTypePrefix(text, depth + 1);
        if (!inner) {
            return inner;
        }
        if (!text.starts_with('>')) {
            return std::unexpected(std::string{"list<...> is not closed with >"});
        }
        text.remove_prefix(1);
        result = Type::listOf(std::move(*inner));
    } else {
        std::size_t length = 0;
        while (length < text.size() && text.at(length) >= 'a' && text.at(length) <= 'z') {
            ++length;
        }
        std::string_view const word = text.substr(0, length);
        bool known = false;
        for (auto const& [spelling, kind] : kScalars) {
            if (spelling == word) {
                result = Type::of(kind);
                known = true;
                break;
            }
        }
        if (!known) {
            return std::unexpected("unknown type `" + std::string{word} + "`");
        }
        text.remove_prefix(length);
    }
    if (text.starts_with('?')) {
        text.remove_prefix(1);
        result.optional = result.kind != TypeKind::Any;
    }
    return result;
}

}  // namespace detail

/// @brief Parses a document type: `bool`, `int`, `string`, `decimal`, `quantity`, `timestamp`, `key`, `record`,
///        `any`, `list<T>`, each optionally followed by `?`.
/// @param text The type as written.
/// @return The type, or the reason it does not parse.
[[nodiscard]] inline std::expected<Type, std::string> parseType(std::string_view text) {
    std::string_view rest = text;
    auto parsed = detail::parseTypePrefix(rest, 0);
    if (!parsed) {
        return parsed;
    }
    if (!rest.empty()) {
        return std::unexpected("unexpected `" + std::string{rest} + "` after the type");
    }
    return parsed;
}

namespace detail {

/// @brief Reads a JSON integer as an int64.
/// @param json The JSON value.
/// @return The integer; `std::nullopt` when it is not an integer or does not fit int64.
[[nodiscard]] inline std::optional<std::int64_t> jsonInt(Json const& json) noexcept {
    if (auto const* const signedValue = std::get_if<std::int64_t>(&json.data)) {
        return *signedValue;
    }
    if (auto const* const unsignedValue = std::get_if<std::uint64_t>(&json.data)) {
        if (*unsignedValue <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(*unsignedValue);
        }
    }
    return std::nullopt;
}

/// @brief Reads the wire form of a decimal or quantity: an object of exactly `num`, `den`, `dp` (and `unit`).
/// @param object The JSON object.
/// @return The value when @p object has that shape; `std::nullopt` when it is some other record; the reason when it
///         has the shape and a bad member.
[[nodiscard]] inline std::optional<std::expected<Value, std::string>> exactFromJson(Json::object_t const& object) {
    if (object.size() != 3 && object.size() != 4) {
        return std::nullopt;
    }
    Json const* num = nullptr;
    Json const* den = nullptr;
    Json const* places = nullptr;
    Json const* unit = nullptr;
    for (auto const& [name, member] : object) {
        if (name == "num") {
            num = &member;
        } else if (name == "den") {
            den = &member;
        } else if (name == "dp") {
            places = &member;
        } else if (name == "unit") {
            unit = &member;
        } else {
            return std::nullopt;
        }
    }
    if (num == nullptr || den == nullptr || places == nullptr) {
        return std::nullopt;
    }
    auto const numerator = jsonInt(*num);
    auto const denominator = jsonInt(*den);
    auto const decimals = jsonInt(*places);
    if (!numerator || !denominator || !decimals) {
        return std::unexpected(std::string{"num, den and dp of an exact value are integers"});
    }
    if (*decimals < 0 || std::cmp_greater(*decimals, math::kMaxDecimalPlaces)) {
        return std::unexpected(std::string{"dp of an exact value is between 0 and 18"});
    }
    auto rational = math::Rational::from(math::Numerator{*numerator}, math::Denominator{*denominator},
                                         math::DecimalPlaces{static_cast<std::uint32_t>(*decimals)});
    if (!rational) {
        return std::unexpected(std::string{"den of an exact value is not zero"});
    }
    if (unit == nullptr) {
        return Value{Decimal{*rational}};
    }
    auto const* const unitText = std::get_if<std::string>(&unit->data);
    if (unitText == nullptr) {
        return std::unexpected(std::string{"unit of a quantity is a string"});
    }
    return Value{Quantity{.value = *rational, .unit = *unitText}};
}

}  // namespace detail

/// @brief Reads a value from its JSON form, structurally.
///
/// `null`, booleans, integers, strings, arrays and objects map to their kinds. An object of exactly `num`, `den` and
/// `dp` is a decimal, and with `unit` a quantity: the wire form of `math::Rational`. A number with a fraction or an
/// exponent is refused, because an exact value is never written as a double; an integer beyond int64 is refused.
/// A timestamp has no structural form — it is a string — so it is read through the typed overload.
/// @param json The JSON value.
/// @return The value, or the reason it is refused.
[[nodiscard]] inline std::expected<Value, std::string> valueFromJson(Json const& json) {
    struct Visitor {
        std::expected<Value, std::string> operator()(std::nullptr_t) const { return Value{}; }
        std::expected<Value, std::string> operator()(bool flag) const { return Value{flag}; }
        std::expected<Value, std::string> operator()(std::uint64_t number) const {
            if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return std::unexpected(std::string{"integer does not fit int64"});
            }
            return Value{static_cast<std::int64_t>(number)};
        }
        std::expected<Value, std::string> operator()(std::int64_t number) const { return Value{number}; }
        std::expected<Value, std::string> operator()(double number) const {
            static_cast<void>(number);
            return std::unexpected(
                std::string{R"(a number with a fraction is not exact; write a decimal as {"num", "den", "dp"})"});
        }
        std::expected<Value, std::string> operator()(std::string const& text) const { return Value{text}; }
        std::expected<Value, std::string> operator()(Json::array_t const& array) const {
            ValueList items;
            items.reserve(array.size());
            for (Json const& element : array) {
                auto item = valueFromJson(element);
                if (!item) {
                    return item;
                }
                items.push_back(std::move(*item));
            }
            return Value{std::move(items)};
        }
        std::expected<Value, std::string> operator()(Json::object_t const& object) const {
            if (auto exact = detail::exactFromJson(object)) {
                return std::move(*exact);
            }
            ValueRecord fields;
            for (auto const& [name, member] : object) {
                auto field = valueFromJson(member);
                if (!field) {
                    return field;
                }
                fields.insert_or_assign(name, std::move(*field));
            }
            return Value{std::move(fields)};
        }
    };
    return std::visit(Visitor{}, json.data);
}

/// @brief Reads a value from its JSON form as a value of @p type.
///
/// A `timestamp` is read from its ISO-8601 text; an `int` given for a `decimal` becomes a decimal of zero places; a
/// list's elements are read as its element type. The result must then conform to @p type.
/// @param json The JSON value.
/// @param type The type the value must have.
/// @return The value, or the reason it is refused.
// NOLINTNEXTLINE(misc-no-recursion): a list is read element by element, as its element type.
[[nodiscard]] inline std::expected<Value, std::string> valueFromJson(Json const& json, Type const& type) {
    if (type.kind == TypeKind::Timestamp) {
        if (auto const* const text = std::get_if<std::string>(&json.data)) {
            auto const instant = time::DateTime::fromIso8601(*text);
            if (!instant) {
                return std::unexpected("`" + *text + "` is not an ISO-8601 UTC timestamp");
            }
            return Value{*instant};
        }
    }
    if (type.kind == TypeKind::List && type.element) {
        if (auto const* const array = std::get_if<Json::array_t>(&json.data)) {
            ValueList items;
            items.reserve(array->size());
            for (Json const& element : *array) {
                auto item = valueFromJson(element, *type.element);
                if (!item) {
                    return item;
                }
                items.push_back(std::move(*item));
            }
            return Value{std::move(items)};
        }
    }
    auto value = valueFromJson(json);
    if (!value) {
        return value;
    }
    if (type.kind == TypeKind::Decimal) {
        if (std::int64_t const* const whole = value->asInt()) {
            return Value{Decimal{math::Rational{*whole, math::DecimalPlaces{0}}}};
        }
    }
    if (!type.admits(*value)) {
        return std::unexpected("expected " + type.name() + ", got " + std::string{kindName(value->kind())});
    }
    return value;
}

/// @brief Writes a value in its JSON form: the inverse of `valueFromJson`, with a timestamp as ISO-8601 text.
/// @param value The value.
/// @return The JSON value.
// NOLINTNEXTLINE(misc-no-recursion): a list or a record is written element by element.
[[nodiscard]] inline Json valueToJson(Value const& value) {
    Json out;
    auto exact = [](math::Rational const& rational) {
        Json::object_t object;
        object.insert({"num", Json(rational.numerator)});
        object.insert({"den", Json(rational.denominator)});
        object.insert({"dp", Json(static_cast<std::int64_t>(rational.decimalPlaces.value))});
        return object;
    };
    if (bool const* const flag = value.asBool()) {
        out.data = *flag;
    } else if (std::int64_t const* const number = value.asInt()) {
        out.data = *number;
    } else if (std::string const* const text = value.asString()) {
        out.data = *text;
    } else if (Decimal const* const decimal = value.asDecimal()) {
        out.data = exact(decimal->value);
    } else if (Quantity const* const quantity = value.asQuantity()) {
        auto object = exact(quantity->value);
        object.insert({"unit", Json(quantity->unit)});
        out.data = std::move(object);
    } else if (time::DateTime const* const instant = value.asTimestamp()) {
        out.data = instant->toIso8601();
    } else if (ValueList const* const items = value.asList()) {
        Json::array_t array;
        for (Value const& item : *items) {
            array.push_back(valueToJson(item));
        }
        out.data = std::move(array);
    } else if (ValueRecord const* const fields = value.asRecord()) {
        Json::object_t object;
        for (auto const& [name, field] : *fields) {
            object.insert({name, valueToJson(field)});
        }
        out.data = std::move(object);
    } else {
        out.data = nullptr;
    }
    return out;
}

/// @brief The deepest a document's JSON may nest, counting each object and array as one level.
///
/// Documents written by hand or by the builders nest a dozen levels or so. The bound is checked before the JSON
/// parser runs, because the parser and every later pass over the tree descend recursively, and a hostile document
/// nested a few hundred levels deep overflows a 1 MB thread stack before anything else can refuse it.
inline constexpr std::size_t kMaxJsonDepth = 64;

namespace detail {

/// @brief Whether JSON text nests objects and arrays deeper than `kMaxJsonDepth`.
/// @param text The JSON text; brackets inside string literals do not count.
/// @return True when some point of the text is inside more than `kMaxJsonDepth` objects and arrays.
[[nodiscard]] inline bool nestsTooDeep(std::string_view text) {
    std::size_t depth = 0;
    bool inString = false;
    bool escaped = false;
    for (char const c : text) {
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
        } else if (c == '"') {
            inString = true;
        } else if (c == '{' || c == '[') {
            if (++depth > kMaxJsonDepth) {
                return true;
            }
        } else if ((c == '}' || c == ']') && depth > 0) {
            --depth;
        }
    }
    return false;
}

}  // namespace detail

/// @brief Parses JSON text into the document layer's JSON tree.
/// @param text The JSON text.
/// @return The tree, or the reason the text is not JSON or nests deeper than `kMaxJsonDepth`.
[[nodiscard]] inline std::expected<Json, std::string> parseJson(std::string_view text) {
    if (detail::nestsTooDeep(text)) {
        return std::unexpected("the JSON nests deeper than " + std::to_string(kMaxJsonDepth) + " levels");
    }
    Json json;
    if (auto const error = glz::read_json(json, text)) {
        return std::unexpected("not JSON: " + glz::format_error(error, text));
    }
    return json;
}

}  // namespace morph::ui
