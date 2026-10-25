// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../render/i18n.hpp"
#include "../render/locale_format.hpp"
#include "../table/data_source.hpp"
#include "../util/datetime.hpp"
#include "../util/rational.hpp"
#include "message.hpp"
#include "value.hpp"

/// @file
/// @brief The `expr/1` expression language of the UI document: parsing a JSON expression into an `Expr` tree,
///        checking it against the names a document declares, and evaluating it.
///
/// Specified in `docs/spec/ui/expressions.md`.

namespace morph::ui {

/// @brief The most nodes one expression may have; a document with a larger expression is refused at load.
inline constexpr std::size_t kMaxExpressionNodes = 256;

/// @brief Whether a string is a document name: `[A-Za-z_][A-Za-z0-9_]*`.
/// @param text The candidate.
/// @return Whether @p text is a well-formed name.
[[nodiscard]] constexpr bool isName(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    bool first = true;
    for (char const chr : text) {
        bool const letter = (chr >= 'a' && chr <= 'z') || (chr >= 'A' && chr <= 'Z') || chr == '_';
        bool const digit = chr >= '0' && chr <= '9';
        if (!letter && (first || !digit)) {
            return false;
        }
        first = false;
    }
    return true;
}

/// @brief The forms of an `expr/1` expression (spec 5 §4).
enum class ExprKind : std::uint8_t {
    Literal,    ///< A JSON scalar, or `{"lit": v}`.
    Ref,        ///< `{"ref": "name.path"}`.
    Row,        ///< `{"row": "path"}`.
    Query,      ///< `{"query": "q"}`: a query's value.
    Pending,    ///< `{"pending": "q"}`: whether a query or mutation is in flight.
    Error,      ///< `{"error": "q"}`: a query's or mutation's error text.
    Result,     ///< `{"result": "m"}`: a mutation's last result.
    ErrorKind,  ///< `{"errorKind": "m"}`: the kind of a query's or mutation's error.
    Can,        ///< `{"can": "m"}`: whether the principal may run a mutation or query.
    Pref,       ///< `{"pref": "point.path"}`: the user's choice at a customization point.
    Call,       ///< `{"fn": "name", "args": [...]}`, and `{"t": ...}`.
    If,         ///< `{"if": [cond, then, else]}`.
    Object,     ///< `{"obj": {...}}`.
    List,       ///< `{"list": [...]}`.
    Map,        ///< `{"map": e, "as": "x", "to": e}`.
    Filter,     ///< `{"filter": e, "as": "x", "where": e}`.
    Sort,       ///< `{"sort": e, "as": "x", "by": e, "order": e}`.
    Runs,       ///< `{"runs": e, "as": "x", "by": e}`.
    Env,        ///< `{"env": "name"}`: the client environment.
    Session,    ///< `{"session": "name"}`: a session attribute.
    Host,       ///< `{"host": "name"}`: a value the application registered with the client.
    Get,        ///< `{"get": e, "path": "a.b"}`: a path read out of a computed value.
};

/// @brief The functions of the `expr/1` library.
enum class FunctionId : std::uint8_t {
    And,
    Or,
    Not,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Add,
    Sub,
    Mul,
    Div,
    Neg,
    Round,
    Engaged,
    Coalesce,
    Concat,
    Format,
    Trim,
    Length,
    IsEmpty,
    Contains,
    Count,
    Sum,
    Mean,
    Min,
    Max,
    Any,
    All,
    First,
    Includes,
    IndexOf,
    Number,
    DecimalText,
    QuantityText,
    Money,
    Date,
    DateTime,
    Translate,
    ParseDecimal,
    ConcatLists,
    Slice,
};

struct Expr;

/// @brief A shared, immutable expression node.
using ExprPtr = std::shared_ptr<Expr const>;

/// @brief One node of a parsed expression. Immutable once parsed; shared between the document and its readers.
struct Expr {
    /// @brief The form.
    ExprKind kind = ExprKind::Literal;
    /// @brief The JSON Pointer of the node inside its document, for refusals and diagnostics.
    std::string path;
    /// @brief The constant of a `Literal`.
    Value literal;
    /// @brief The name a form reads: a ref's head, a query or mutation, a pref's point, a comprehension's variable,
    ///        an `env`, `session` or `host` name.
    std::string name;
    /// @brief The path after the head of a `Ref` or `Pref`, or the path of a `Row` or `Get`.
    std::vector<std::string> fieldPath;
    /// @brief The function of a `Call`.
    FunctionId function = FunctionId::And;
    /// @brief Operands: a call's arguments; `if`'s condition, then and else; a list's items; a comprehension's
    ///        source and body (and a `sort`'s `order`, when given); the value a `get` reads from.
    std::vector<ExprPtr> args;
    /// @brief The fields of an `Object`, in the order written.
    std::vector<std::pair<std::string, ExprPtr>> fields;
};

/// @brief Whether a form names each element of its list with `as`, for its second operand alone.
/// @param kind The form.
/// @return True for `map`, `filter`, `sort` and `runs`.
[[nodiscard]] constexpr bool bindsElement(ExprKind kind) noexcept {
    return kind == ExprKind::Map || kind == ExprKind::Filter || kind == ExprKind::Sort || kind == ExprKind::Runs;
}

/// @brief One run of adjacent elements of a list that share a value, as `runs` and a table's group heading read
///        it: `{"key": <the shared value>, "count": n, "first": <element>, "last": <element>, "items": [...]}`.
/// @param key   The shared value.
/// @param items The run's elements, in order; not empty.
/// @return The record.
[[nodiscard]] inline Value runRecord(Value key, ValueList items) {
    ValueRecord record;
    record.insert_or_assign("key", std::move(key));
    record.insert_or_assign("count", static_cast<std::int64_t>(items.size()));
    record.insert_or_assign("first", items.front());
    record.insert_or_assign("last", items.back());
    record.insert_or_assign("items", Value{std::move(items)});
    return Value{std::move(record)};
}

namespace detail {

/// @brief A set of type kinds, as bits.
using KindSet = std::uint16_t;

/// @brief The bit of one kind.
/// @param kind The kind.
/// @return Its bit.
[[nodiscard]] constexpr KindSet kindBit(TypeKind kind) noexcept {
    return static_cast<KindSet>(1U << static_cast<unsigned>(kind));
}

/// @brief Every kind.
inline constexpr KindSet kAllKinds = 0xFFFF;
/// @brief The numbers.
inline constexpr KindSet kNumeric = kindBit(TypeKind::Int) | kindBit(TypeKind::Decimal) | kindBit(TypeKind::Quantity);
/// @brief The ordered kinds.
inline constexpr KindSet kOrdered = kNumeric | kindBit(TypeKind::Timestamp) | kindBit(TypeKind::String);

/// @brief The signature of a library function.
struct FunctionSpec {
    /// @brief The name a document calls it by.
    std::string_view name;
    /// @brief The fewest arguments.
    std::uint8_t minArgs;
    /// @brief The most arguments; 255 for any number.
    std::uint8_t maxArgs;
    /// @brief The kinds the first, second and every further argument accept.
    std::array<KindSet, 3> accepts;
    /// @brief The result's kind; `Any` when it depends on the arguments.
    TypeKind result;
};

/// @brief The library, indexed by `FunctionId`.
// NOLINTBEGIN(modernize-use-designated-initializers): one row per function reads as a table; names on every field would not.
inline constexpr std::array<FunctionSpec, 43> kFunctions = {{
    {"and", 1, 255, {kindBit(TypeKind::Bool), kindBit(TypeKind::Bool), kindBit(TypeKind::Bool)}, TypeKind::Bool},
    {"or", 1, 255, {kindBit(TypeKind::Bool), kindBit(TypeKind::Bool), kindBit(TypeKind::Bool)}, TypeKind::Bool},
    {"not", 1, 1, {kindBit(TypeKind::Bool), 0, 0}, TypeKind::Bool},
    {"eq", 2, 2, {kAllKinds, kAllKinds, 0}, TypeKind::Bool},
    {"ne", 2, 2, {kAllKinds, kAllKinds, 0}, TypeKind::Bool},
    {"lt", 2, 2, {kOrdered, kOrdered, 0}, TypeKind::Bool},
    {"le", 2, 2, {kOrdered, kOrdered, 0}, TypeKind::Bool},
    {"gt", 2, 2, {kOrdered, kOrdered, 0}, TypeKind::Bool},
    {"ge", 2, 2, {kOrdered, kOrdered, 0}, TypeKind::Bool},
    {"add", 2, 2, {kNumeric, kNumeric, 0}, TypeKind::Any},
    {"sub", 2, 2, {kNumeric, kNumeric, 0}, TypeKind::Any},
    {"mul", 2, 2, {kNumeric, kNumeric, 0}, TypeKind::Any},
    {"div", 2, 2, {kNumeric, kNumeric, 0}, TypeKind::Any},
    {"neg", 1, 1, {kNumeric, 0, 0}, TypeKind::Any},
    {"round", 2, 3, {kNumeric, kindBit(TypeKind::Int), kindBit(TypeKind::String)}, TypeKind::Any},
    {"engaged", 1, 1, {kAllKinds, 0, 0}, TypeKind::Bool},
    {"coalesce", 1, 255, {kAllKinds, kAllKinds, kAllKinds}, TypeKind::Any},
    {"concat",
     1,
     255,
     {kindBit(TypeKind::String), kindBit(TypeKind::String), kindBit(TypeKind::String)},
     TypeKind::String},
    {"format", 1, 255, {kindBit(TypeKind::String), kAllKinds, kAllKinds}, TypeKind::String},
    {"trim", 1, 1, {kindBit(TypeKind::String), 0, 0}, TypeKind::String},
    {"length", 1, 1, {kindBit(TypeKind::String), 0, 0}, TypeKind::Int},
    {"isEmpty", 1, 1, {kindBit(TypeKind::String) | kindBit(TypeKind::List), 0, 0}, TypeKind::Bool},
    {"contains", 2, 2, {kindBit(TypeKind::String), kindBit(TypeKind::String), 0}, TypeKind::Bool},
    {"count", 1, 1, {kindBit(TypeKind::List), 0, 0}, TypeKind::Int},
    {"sum", 1, 1, {kindBit(TypeKind::List), 0, 0}, TypeKind::Any},
    {"mean", 1, 1, {kindBit(TypeKind::List), 0, 0}, TypeKind::Any},
    {"min", 1, 1, {kindBit(TypeKind::List), 0, 0}, TypeKind::Any},
    {"max", 1, 1, {kindBit(TypeKind::List), 0, 0}, TypeKind::Any},
    {"any", 1, 1, {kindBit(TypeKind::List), 0, 0}, TypeKind::Bool},
    {"all", 1, 1, {kindBit(TypeKind::List), 0, 0}, TypeKind::Bool},
    {"first", 1, 1, {kindBit(TypeKind::List), 0, 0}, TypeKind::Any},
    {"includes", 2, 2, {kindBit(TypeKind::List), kAllKinds, 0}, TypeKind::Bool},
    {"indexOf", 2, 2, {kindBit(TypeKind::List), kAllKinds, 0}, TypeKind::Int},
    {"number", 1, 1, {kNumeric, 0, 0}, TypeKind::String},
    {"decimal", 2, 3, {kNumeric, kindBit(TypeKind::Int), kindBit(TypeKind::String)}, TypeKind::String},
    {"quantity", 1, 2, {kindBit(TypeKind::Quantity), kindBit(TypeKind::String), 0}, TypeKind::String},
    {"money",
     2,
     2,
     {kindBit(TypeKind::Int) | kindBit(TypeKind::Decimal), kindBit(TypeKind::String), 0},
     TypeKind::String},
    {"date",
     1,
     2,
     {kindBit(TypeKind::Timestamp) | kindBit(TypeKind::String), kindBit(TypeKind::String), 0},
     TypeKind::String},
    {"dateTime",
     1,
     2,
     {kindBit(TypeKind::Timestamp) | kindBit(TypeKind::String), kindBit(TypeKind::String), 0},
     TypeKind::String},
    {"t", 2, 3, {kindBit(TypeKind::String), kindBit(TypeKind::String), kindBit(TypeKind::Record)}, TypeKind::String},
    {"parseDecimal", 1, 2, {kindBit(TypeKind::String), kindBit(TypeKind::Int), 0}, TypeKind::Decimal},
    {"concatLists",
     1,
     255,
     {kindBit(TypeKind::List), kindBit(TypeKind::List), kindBit(TypeKind::List)},
     TypeKind::Any},
    {"slice", 2, 3, {kindBit(TypeKind::List), kindBit(TypeKind::Int), kindBit(TypeKind::Int)}, TypeKind::Any},
}};
// NOLINTEND(modernize-use-designated-initializers)

/// @brief The signature of a function.
/// @param function The function.
/// @return Its entry in `kFunctions`.
[[nodiscard]] constexpr FunctionSpec const& functionSpec(FunctionId function) {
    return kFunctions.at(static_cast<std::size_t>(function));
}

/// @brief The position of a function's optional rounding mode, `"halfEven"` or `"halfUp"`.
/// @param function The function.
/// @return The argument index of its mode, or empty for a function that does not round.
[[nodiscard]] constexpr std::optional<std::size_t> modeArgument(FunctionId function) noexcept {
    if (function == FunctionId::Round || function == FunctionId::DecimalText) {
        return 2;
    }
    return std::nullopt;
}

/// @brief Reads a rounding mode: `"halfEven"`, ties to the even neighbour, or `"halfUp"`, ties away from zero.
/// @param mode The mode's value.
/// @return The mode, or empty for any other value.
[[nodiscard]] inline std::optional<math::RoundingMode> roundingMode(Value const& mode) {
    std::string const* const name = mode.asString();
    if (name != nullptr && *name == "halfEven") {
        return math::RoundingMode::HalfEven;
    }
    if (name != nullptr && *name == "halfUp") {
        return math::RoundingMode::HalfAwayFromZero;
    }
    return std::nullopt;
}

/// @brief The names `{"env": ...}` reads.
inline constexpr std::array<std::string_view, 5> kEnvNames = {"widthClass", "locale", "platform", "online", "backend"};

/// @brief The names `{"session": ...}` reads.
inline constexpr std::array<std::string_view, 6> kSessionNames = {"principal", "userName", "displayName",
                                                                  "userKey",   "tenant",   "authenticated"};

/// @brief Whether a name is one of a closed set.
/// @param names The set.
/// @param name  The candidate.
/// @return Whether @p name is in @p names.
template <std::size_t N>
[[nodiscard]] constexpr bool oneOf(std::array<std::string_view, N> const& names, std::string_view name) noexcept {
    return std::ranges::find(names, name) != names.end();
}

/// @brief Splits `head.a.b` into its segments.
/// @param text The dotted path.
/// @return The segments, or the reason the path is malformed.
[[nodiscard]] inline std::expected<std::vector<std::string>, std::string> splitPath(std::string_view text) {
    std::vector<std::string> segments;
    std::size_t start = 0;
    for (;;) {
        std::size_t const dot = text.find('.', start);
        std::string_view const segment =
            text.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (segment.empty()) {
            return std::unexpected("`" + std::string{text} + "` has an empty segment");
        }
        segments.emplace_back(segment);
        if (dot == std::string_view::npos) {
            return segments;
        }
        start = dot + 1;
    }
}

/// @brief Reads one JSON expression tree into `Expr` nodes, counting them against the size bound.
// NOLINTBEGIN(misc-no-recursion): an expression is a tree and parsing descends it; the node bound limits the depth.
class ExprParser {
public:
    /// @brief The keys a form may carry beside its own.
    struct FormKeys {
        /// @brief The form's own key.
        std::string_view key;
        /// @brief The form.
        ExprKind kind;
        /// @brief The keys allowed beside it.
        std::array<std::string_view, 3> companions;
    };

    /// @brief Parses @p json, whose JSON Pointer is @p path.
    /// @param json The expression.
    /// @param path Its JSON Pointer.
    /// @return The expression, or the refusal.
    [[nodiscard]] std::expected<ExprPtr, Refusal> parse(Json const& json, std::string const& path) {
        return node(json, path);
    }

private:
    // NOLINTBEGIN(modernize-use-designated-initializers): one row per form reads as a table.
    static constexpr std::array<FormKeys, 23> kForms = {{
        {"lit", ExprKind::Literal, {}},
        {"ref", ExprKind::Ref, {}},
        {"row", ExprKind::Row, {}},
        {"query", ExprKind::Query, {}},
        {"pending", ExprKind::Pending, {}},
        {"error", ExprKind::Error, {}},
        {"result", ExprKind::Result, {}},
        {"errorKind", ExprKind::ErrorKind, {}},
        {"can", ExprKind::Can, {}},
        {"pref", ExprKind::Pref, {}},
        {"fn", ExprKind::Call, {"args", ""}},
        {"t", ExprKind::Call, {"fallback", "args"}},
        {"if", ExprKind::If, {}},
        {"obj", ExprKind::Object, {}},
        {"list", ExprKind::List, {}},
        {"map", ExprKind::Map, {"as", "to"}},
        {"filter", ExprKind::Filter, {"as", "where"}},
        {"sort", ExprKind::Sort, {"as", "by", "order"}},
        {"runs", ExprKind::Runs, {"as", "by"}},
        {"env", ExprKind::Env, {}},
        {"session", ExprKind::Session, {}},
        {"host", ExprKind::Host, {}},
        {"get", ExprKind::Get, {"path", ""}},
    }};
    // NOLINTEND(modernize-use-designated-initializers)

    [[nodiscard]] static std::unexpected<Refusal> refuse(std::string const& path, std::string reason) {
        return std::unexpected(Refusal{.document = {}, .path = path, .reason = std::move(reason)});
    }

    [[nodiscard]] static std::string const* text(Json const& json) { return std::get_if<std::string>(&json.data); }

    [[nodiscard]] static Json const* member(Json::object_t const& object, std::string_view key) {
        auto const* const found = object.find(key);
        return found == object.end() ? nullptr : &found->second;
    }

    [[nodiscard]] static ExprPtr literal(Value value, std::string path) {
        auto made = std::make_shared<Expr>();
        made->kind = ExprKind::Literal;
        made->literal = std::move(value);
        made->path = std::move(path);
        return made;
    }

    [[nodiscard]] std::expected<ExprPtr, Refusal> node(Json const& json, std::string const& path) {
        if (++_count > kMaxExpressionNodes) {
            return refuse(path, "the expression has more than " + std::to_string(kMaxExpressionNodes) +
                                    " nodes, the bound of expr/1");
        }
        if (std::holds_alternative<Json::array_t>(json.data)) {
            return refuse(path, R"(a list is written {"list": [...]}, a constant list {"lit": [...]})");
        }
        auto const* const object = std::get_if<Json::object_t>(&json.data);
        if (object == nullptr) {
            auto value = valueFromJson(json);
            if (!value) {
                return refuse(path, value.error());
            }
            return literal(std::move(*value), path);
        }
        auto made = std::make_shared<Expr>();
        made->path = path;
        if (auto refused = form(*made, *object, path)) {
            return std::unexpected(std::move(*refused));
        }
        return made;
    }

    [[nodiscard]] std::optional<Refusal> form(Expr& made, Json::object_t const& object, std::string const& path) {
        FormKeys const* chosen = nullptr;
        for (auto const& [key, value] : object) {
            static_cast<void>(value);
            for (FormKeys const& candidate : kForms) {
                if (candidate.key != key) {
                    continue;
                }
                if (chosen != nullptr) {
                    return refuse(path, "an expression has one form; found `" + std::string{chosen->key} + "` and `" +
                                            key + "`")
                        .error();
                }
                chosen = &candidate;
            }
        }
        if (chosen == nullptr) {
            if (object.empty()) {
                return refuse(path, "an empty object is not an expression; a constant record is {\"lit\": {}}")
                    .error();
            }
            return refuse(path, "`" + object.begin()->first + "` is not an expression form of expr/1").error();
        }
        for (auto const& [key, value] : object) {
            static_cast<void>(value);
            if (key != chosen->key && !std::ranges::contains(chosen->companions, key)) {
                return refuse(pointerAppend(path, key),
                              "`" + key + "` does not belong to the `" + std::string{chosen->key} + "` form")
                    .error();
            }
        }
        Json const& primary = *member(object, chosen->key);
        std::string const primaryPath = pointerAppend(path, chosen->key);
        made.kind = chosen->kind;
        switch (chosen->kind) {
            case ExprKind::Literal: {
                auto value = valueFromJson(primary);
                if (!value) {
                    return refuse(primaryPath, value.error()).error();
                }
                made.literal = std::move(*value);
                return std::nullopt;
            }
            case ExprKind::Ref:
            case ExprKind::Pref:
                return dotted(made, primary, primaryPath);
            case ExprKind::Row:
                return rowPath(made, primary, primaryPath);
            case ExprKind::Query:
            case ExprKind::Pending:
            case ExprKind::Error:
            case ExprKind::Result:
            case ExprKind::ErrorKind:
            case ExprKind::Can:
                return controlName(made, primary, primaryPath, chosen->key);
            case ExprKind::Env:
            case ExprKind::Session:
            case ExprKind::Host:
                return clientName(made, primary, primaryPath, chosen->key);
            case ExprKind::Call:
                if (chosen->key == "t") {
                    return translate(made, object, path);
                }
                return call(made, object, path);
            case ExprKind::If:
                return operands(made.args, primary, primaryPath, 3, "`if` takes [condition, then, else]");
            case ExprKind::List:
                return operands(made.args, primary, primaryPath, 0, "`list` takes an array of expressions");
            case ExprKind::Object:
                return fields(made.fields, primary, primaryPath);
            case ExprKind::Get:
                return get(made, object, path);
            case ExprKind::Map:
            case ExprKind::Filter:
            case ExprKind::Sort:
            case ExprKind::Runs:
            default:
                return comprehension(made, object, path);
        }
    }

    [[nodiscard]] static std::optional<Refusal> dotted(Expr& made, Json const& primary, std::string const& path) {
        std::string const* const dottedPath = text(primary);
        if (dottedPath == nullptr) {
            return refuse(path, "a name and path is written as a string `name.path`").error();
        }
        auto segments = splitPath(*dottedPath);
        if (!segments) {
            return refuse(path, segments.error()).error();
        }
        if (!isName(segments->front())) {
            return refuse(path, "`" + segments->front() + "` is not a name").error();
        }
        made.name = std::move(segments->front());
        made.fieldPath.assign(std::make_move_iterator(segments->begin() + 1),
                              std::make_move_iterator(segments->end()));
        return std::nullopt;
    }

    [[nodiscard]] static std::optional<Refusal> rowPath(Expr& made, Json const& primary, std::string const& path) {
        std::string const* const dottedPath = text(primary);
        if (dottedPath == nullptr) {
            return refuse(path, "`row` names a field path as a string, \"\" for the whole row").error();
        }
        if (dottedPath->empty()) {
            return std::nullopt;
        }
        auto segments = splitPath(*dottedPath);
        if (!segments) {
            return refuse(path, segments.error()).error();
        }
        made.fieldPath = std::move(*segments);
        return std::nullopt;
    }

    [[nodiscard]] static std::optional<Refusal> controlName(Expr& made, Json const& primary, std::string const& path,
                                                            std::string_view key) {
        std::string const* const name = text(primary);
        bool const can = made.kind == ExprKind::Can;
        bool const hostCommand = can && name != nullptr && name->starts_with("host:") && name->size() > 5;
        if (name == nullptr || (!isName(*name) && !hostCommand)) {
            return refuse(path, "`" + std::string{key} + "` takes the name of a " +
                                    (can ? "mutation, query or `host:` command" : "query or mutation"))
                .error();
        }
        made.name = *name;
        return std::nullopt;
    }

    [[nodiscard]] static std::optional<Refusal> clientName(Expr& made, Json const& primary, std::string const& path,
                                                           std::string_view key) {
        std::string const* const name = text(primary);
        bool known = name != nullptr && !name->empty();
        if (known && made.kind == ExprKind::Env) {
            known = oneOf(kEnvNames, *name);
        } else if (known && made.kind == ExprKind::Session) {
            known = oneOf(kSessionNames, *name);
        }
        if (!known) {
            return refuse(path, "`" + std::string{key} + "` does not read this name").error();
        }
        made.name = *name;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Refusal> operands(std::vector<ExprPtr>& out, Json const& primary,
                                                  std::string const& path, std::size_t exactly, char const* shape) {
        auto const* const array = std::get_if<Json::array_t>(&primary.data);
        if (array == nullptr || (exactly != 0 && array->size() != exactly)) {
            return refuse(path, shape).error();
        }
        for (std::size_t index = 0; index < array->size(); ++index) {
            auto operand = node(array->at(index), pointerAppend(path, index));
            if (!operand) {
                return std::move(operand.error());
            }
            out.push_back(std::move(*operand));
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Refusal> fields(std::vector<std::pair<std::string, ExprPtr>>& out, Json const& primary,
                                                std::string const& path) {
        auto const* const object = std::get_if<Json::object_t>(&primary.data);
        if (object == nullptr) {
            return refuse(path, "`obj` takes an object of expressions").error();
        }
        for (auto const& [key, value] : *object) {
            auto field = node(value, pointerAppend(path, key));
            if (!field) {
                return std::move(field.error());
            }
            out.emplace_back(key, std::move(*field));
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Refusal> call(Expr& made, Json::object_t const& object, std::string const& path) {
        std::string const* const name = text(*member(object, "fn"));
        std::string const namePath = pointerAppend(path, "fn");
        if (name == nullptr) {
            return refuse(namePath, "`fn` names a function as a string").error();
        }
        auto const index =
            std::ranges::distance(kFunctions.begin(), std::ranges::find(kFunctions, *name, &FunctionSpec::name));
        if (std::cmp_equal(index, kFunctions.size())) {
            return refuse(namePath, "the function `" + *name + "` is not in expr/1").error();
        }
        FunctionSpec const& spec = *std::next(kFunctions.begin(), index);
        made.function = static_cast<FunctionId>(index);
        if (Json const* const args = member(object, "args")) {
            if (auto refused = operands(made.args, *args, pointerAppend(path, "args"), 0,
                                        "`args` takes an array of expressions")) {
                return refused;
            }
        }
        if (made.args.size() < spec.minArgs || made.args.size() > spec.maxArgs) {
            std::string arity = std::to_string(static_cast<unsigned>(spec.minArgs));
            if (spec.maxArgs == 255) {
                arity += " or more";
            } else if (spec.maxArgs != spec.minArgs) {
                arity += " to " + std::to_string(static_cast<unsigned>(spec.maxArgs));
            }
            return refuse(path,
                          "`" + *name + "` takes " + arity + " arguments, not " + std::to_string(made.args.size()))
                .error();
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Refusal> translate(Expr& made, Json::object_t const& object, std::string const& path) {
        made.function = FunctionId::Translate;
        std::string const* const key = text(*member(object, "t"));
        Json const* const fallback = member(object, "fallback");
        std::string const* const fallbackText = fallback == nullptr ? nullptr : text(*fallback);
        if (key == nullptr || key->empty()) {
            return refuse(pointerAppend(path, "t"), "`t` names a message key as a string").error();
        }
        if (fallbackText == nullptr) {
            return refuse(pointerAppend(path, "fallback"), "`t` needs its `fallback` text as a string").error();
        }
        made.args.push_back(literal(*key, pointerAppend(path, "t")));
        made.args.push_back(literal(*fallbackText, pointerAppend(path, "fallback")));
        if (Json const* const args = member(object, "args")) {
            auto record = std::make_shared<Expr>();
            record->kind = ExprKind::Object;
            record->path = pointerAppend(path, "args");
            if (auto refused = fields(record->fields, *args, record->path)) {
                return refused;
            }
            made.args.push_back(std::move(record));
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Refusal> get(Expr& made, Json::object_t const& object, std::string const& path) {
        Json const* const fieldPath = member(object, "path");
        if (fieldPath == nullptr) {
            return refuse(path, "`get` needs `path`").error();
        }
        std::string const* const dottedPath = text(*fieldPath);
        std::string const pathPointer = pointerAppend(path, "path");
        if (dottedPath == nullptr || dottedPath->empty()) {
            return refuse(pathPointer, "`get` reads a `path` written as a string `a.b.0`").error();
        }
        auto segments = splitPath(*dottedPath);
        if (!segments) {
            return refuse(pathPointer, segments.error()).error();
        }
        auto source = node(*member(object, "get"), pointerAppend(path, "get"));
        if (!source) {
            return std::move(source.error());
        }
        made.fieldPath = std::move(*segments);
        made.args.push_back(std::move(*source));
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Refusal> comprehension(Expr& made, Json::object_t const& object,
                                                       std::string const& path) {
        std::string_view sourceKey = "filter";
        std::string_view bodyKey = "where";
        if (made.kind == ExprKind::Map) {
            sourceKey = "map";
            bodyKey = "to";
        } else if (made.kind == ExprKind::Sort) {
            sourceKey = "sort";
            bodyKey = "by";
        } else if (made.kind == ExprKind::Runs) {
            sourceKey = "runs";
            bodyKey = "by";
        }
        Json const* const variable = member(object, "as");
        Json const* const body = member(object, bodyKey);
        std::string const* const name = variable == nullptr ? nullptr : text(*variable);
        if (name == nullptr || !isName(*name)) {
            return refuse(pointerAppend(path, "as"), "`" + std::string{sourceKey} + "` names its element with `as`")
                .error();
        }
        if (body == nullptr) {
            return refuse(path, "`" + std::string{sourceKey} + "` needs `" + std::string{bodyKey} + "`").error();
        }
        made.name = *name;
        auto source = node(*member(object, sourceKey), pointerAppend(path, sourceKey));
        if (!source) {
            return std::move(source.error());
        }
        auto each = node(*body, pointerAppend(path, bodyKey));
        if (!each) {
            return std::move(each.error());
        }
        made.args.push_back(std::move(*source));
        made.args.push_back(std::move(*each));
        if (Json const* const order = member(object, "order"); order != nullptr && made.kind == ExprKind::Sort) {
            auto direction = node(*order, pointerAppend(path, "order"));
            if (!direction) {
                return std::move(direction.error());
            }
            made.args.push_back(std::move(*direction));
        }
        return std::nullopt;
    }

    std::size_t _count = 0;
    // NOLINTEND(misc-no-recursion)
};

}  // namespace detail

/// @brief Parses one expression.
///
/// Every form of spec 5 §4 is read; an unknown form, a key that does not belong to its form, a malformed name, an
/// unknown function or wrong arity, a number with a fraction, or more than `kMaxExpressionNodes` nodes is refused.
/// @param json The expression's JSON.
/// @param path Its JSON Pointer inside the document, carried by every node and every refusal.
/// @return The expression, or the refusal.
[[nodiscard]] inline std::expected<ExprPtr, Refusal> parseExpression(Json const& json, std::string const& path = "") {
    return detail::ExprParser{}.parse(json, path);
}

/// @brief The names of the `expr/1` library, in `FunctionId` order.
/// @return The names.
[[nodiscard]] inline std::vector<std::string_view> functionNames() {
    std::vector<std::string_view> names;
    names.reserve(detail::kFunctions.size());
    for (auto const& spec : detail::kFunctions) {
        names.push_back(spec.name);
    }
    return names;
}

/// @brief What an expression's names resolve to, for `checkExpression`: the document's scopes implement it.
class NameResolver {
public:
    NameResolver() = default;
    NameResolver(NameResolver const&) = delete;
    NameResolver& operator=(NameResolver const&) = delete;
    NameResolver(NameResolver&&) = delete;
    NameResolver& operator=(NameResolver&&) = delete;
    virtual ~NameResolver() = default;

    /// @brief Resolves `{"ref": "head.path"}`.
    /// @param head The first segment.
    /// @param path The segments after it.
    /// @return The type read, or why the reference does not resolve.
    [[nodiscard]] virtual std::expected<Type, std::string> ref(std::string_view head,
                                                               std::span<std::string const> path) const = 0;

    /// @brief Resolves `{"row": "path"}`.
    /// @param path The field path inside the row.
    /// @return The type read, or why there is no row here.
    [[nodiscard]] virtual std::expected<Type, std::string> row(std::span<std::string const> path) const = 0;

    /// @brief Resolves the name of `query`, `pending`, `error`, `result`, `errorKind` or `can`.
    /// @param form The form.
    /// @param name The query or mutation name.
    /// @return The type read, or why the name does not resolve.
    [[nodiscard]] virtual std::expected<Type, std::string> control(ExprKind form, std::string_view name) const = 0;

    /// @brief Resolves `{"pref": "point.path"}`.
    /// @param point The customization point.
    /// @param path  The segments after it.
    /// @return The type read, or why the point is not declared.
    [[nodiscard]] virtual std::expected<Type, std::string> pref(std::string_view point,
                                                                std::span<std::string const> path) const = 0;
};

/// @brief A resolver that accepts every name as `any`: for an expression checked outside a document.
class PermissiveResolver final : public NameResolver {
public:
    /// @brief Accepts any reference.
    /// @param head Unused.
    /// @param path Unused.
    /// @return `any`.
    [[nodiscard]] std::expected<Type, std::string> ref(std::string_view head,
                                                       std::span<std::string const> path) const override {
        static_cast<void>(head);
        static_cast<void>(path);
        return Type::any();
    }

    /// @brief Accepts any row path.
    /// @param path Unused.
    /// @return `any`.
    [[nodiscard]] std::expected<Type, std::string> row(std::span<std::string const> path) const override {
        static_cast<void>(path);
        return Type::any();
    }

    /// @brief Accepts any query or mutation.
    /// @param form Unused.
    /// @param name Unused.
    /// @return `any`.
    [[nodiscard]] std::expected<Type, std::string> control(ExprKind form, std::string_view name) const override {
        static_cast<void>(form);
        static_cast<void>(name);
        return Type::any();
    }

    /// @brief Accepts any point.
    /// @param point Unused.
    /// @param path  Unused.
    /// @return `any`.
    [[nodiscard]] std::expected<Type, std::string> pref(std::string_view point,
                                                        std::span<std::string const> path) const override {
        static_cast<void>(point);
        static_cast<void>(path);
        return Type::any();
    }
};

namespace detail {

/// @brief The static type of a literal.
/// @param value The literal.
/// @return Its type; `any` for `null`.
[[nodiscard]] inline Type typeOfValue(Value const& value) {
    // The type kind of each value kind, indexed by `ValueKind`; `null` has no type of its own.
    constexpr std::array<TypeKind, 9> kTypes = {TypeKind::Any,       TypeKind::Bool,    TypeKind::Int,
                                                TypeKind::String,    TypeKind::Decimal, TypeKind::Quantity,
                                                TypeKind::Timestamp, TypeKind::List,    TypeKind::Record};
    return Type::of(kTypes.at(static_cast<std::size_t>(value.kind())));
}

/// @brief The type a field path reads from a value of a known type.
/// @param type The type of the head.
/// @param head The head's name, for the reason.
/// @param path The path.
/// @return `any` when the path can read something, or why it cannot.
[[nodiscard]] inline std::expected<Type, std::string> typeAlong(Type const& type, std::string_view head,
                                                                std::span<std::string const> path) {
    if (path.empty()) {
        return type;
    }
    if (type.kind == TypeKind::Any || type.kind == TypeKind::Record || type.kind == TypeKind::List) {
        return Type::any();
    }
    return std::unexpected("`" + std::string{head} + "` is " + type.name() + " and has no field `" + path.front() +
                           "`");
}

/// @brief Walks an expression once, resolving names and checking the statically known types.
// NOLINTBEGIN(misc-no-recursion): an expression is a tree and checking descends it; the node bound limits the depth.
class ExprChecker {
public:
    /// @brief Checks against @p resolver.
    /// @param resolver What the names resolve to.
    explicit ExprChecker(NameResolver const& resolver) : _resolver{&resolver} {}

    /// @brief Checks one node and its operands.
    /// @param expr The node.
    /// @return Its static type, or the refusal.
    [[nodiscard]] std::expected<Type, Refusal> check(Expr const& expr) {
        switch (expr.kind) {
            case ExprKind::Literal:
                return typeOfValue(expr.literal);
            case ExprKind::Ref:
                return reference(expr);
            case ExprKind::Row:
                return wrap(expr, _resolver->row(expr.fieldPath));
            case ExprKind::Query:
            case ExprKind::Pending:
            case ExprKind::Error:
            case ExprKind::Result:
            case ExprKind::ErrorKind:
            case ExprKind::Can:
                return wrap(expr, _resolver->control(expr.kind, expr.name));
            case ExprKind::Pref:
                return wrap(expr, _resolver->pref(expr.name, expr.fieldPath));
            case ExprKind::Env:
                return Type::of(expr.name == "online" ? TypeKind::Bool : TypeKind::String, expr.name != "online");
            case ExprKind::Session:
                return Type::of(expr.name == "authenticated" ? TypeKind::Bool : TypeKind::String,
                                expr.name != "authenticated");
            case ExprKind::Host:
                return Type::any();
            case ExprKind::If:
                return choice(expr);
            case ExprKind::Object: {
                for (auto const& [name, field] : expr.fields) {
                    static_cast<void>(name);
                    auto checked = check(*field);
                    if (!checked) {
                        return checked;
                    }
                }
                return Type::of(TypeKind::Record);
            }
            case ExprKind::List: {
                for (ExprPtr const& item : expr.args) {
                    auto checked = check(*item);
                    if (!checked) {
                        return checked;
                    }
                }
                return Type::of(TypeKind::List);
            }
            case ExprKind::Map:
            case ExprKind::Filter:
            case ExprKind::Sort:
            case ExprKind::Runs:
                return comprehension(expr);
            case ExprKind::Get: {
                auto source = check(*expr.args.at(0));
                if (!source) {
                    return source;
                }
                if (!admitsKind(*source, kindBit(TypeKind::Record) | kindBit(TypeKind::List))) {
                    return refuse(expr, "`get` reads a path out of a record or a list, not " + source->name());
                }
                return Type::any();
            }
            case ExprKind::Call:
            default:
                return call(expr);
        }
    }

private:
    [[nodiscard]] static std::expected<Type, Refusal> wrap(Expr const& expr, std::expected<Type, std::string> type) {
        if (!type) {
            return std::unexpected(Refusal{.document = {}, .path = expr.path, .reason = std::move(type.error())});
        }
        return std::move(*type);
    }

    [[nodiscard]] static std::unexpected<Refusal> refuse(Expr const& expr, std::string reason) {
        return std::unexpected(Refusal{.document = {}, .path = expr.path, .reason = std::move(reason)});
    }

    [[nodiscard]] std::expected<Type, Refusal> reference(Expr const& expr) {
        for (auto const& [name, type] : std::views::reverse(_locals)) {
            if (name == expr.name) {
                return wrap(expr, typeAlong(type, expr.name, expr.fieldPath));
            }
        }
        return wrap(expr, _resolver->ref(expr.name, expr.fieldPath));
    }

    [[nodiscard]] static bool admitsKind(Type const& type, KindSet accepted) {
        return type.kind == TypeKind::Any || (accepted & kindBit(type.kind)) != 0;
    }

    [[nodiscard]] std::expected<Type, Refusal> condition(Expr const& expr, std::string_view what) {
        auto type = check(expr);
        if (type && !admitsKind(*type, kindBit(TypeKind::Bool))) {
            return refuse(expr, std::string{what} + " is " + type->name() + "; expected bool");
        }
        return type;
    }

    [[nodiscard]] std::expected<Type, Refusal> choice(Expr const& expr) {
        auto cond = condition(*expr.args.at(0), "the condition of `if`");
        if (!cond) {
            return cond;
        }
        auto then = check(*expr.args.at(1));
        if (!then) {
            return then;
        }
        auto otherwise = check(*expr.args.at(2));
        if (!otherwise) {
            return otherwise;
        }
        if (then->kind == otherwise->kind && then->kind != TypeKind::List) {
            return Type::of(then->kind, then->optional || otherwise->optional);
        }
        return Type::any();
    }

    [[nodiscard]] std::expected<Type, Refusal> comprehension(Expr const& expr) {
        auto source = check(*expr.args.at(0));
        if (!source) {
            return source;
        }
        std::string_view form = "filter";
        if (expr.kind == ExprKind::Map) {
            form = "map";
        } else if (expr.kind == ExprKind::Sort) {
            form = "sort";
        } else if (expr.kind == ExprKind::Runs) {
            form = "runs";
        }
        if (!admitsKind(*source, kindBit(TypeKind::List))) {
            return refuse(*expr.args.at(0), "`" + std::string{form} + "` runs over a list, not " + source->name());
        }
        Type element = source->element ? *source->element : Type::any();
        _locals.emplace_back(expr.name, element);
        auto body = expr.kind == ExprKind::Filter ? condition(*expr.args.at(1), "the `where` of `filter`")
                                                  : check(*expr.args.at(1));
        _locals.pop_back();
        if (!body) {
            return body;
        }
        if (expr.kind == ExprKind::Map) {
            return Type::listOf(*body);
        }
        if (expr.kind == ExprKind::Runs) {
            return Type::listOf(Type::of(TypeKind::Record));
        }
        if (expr.kind == ExprKind::Sort && expr.args.size() > 2) {
            auto order = check(*expr.args.at(2));
            if (!order) {
                return order;
            }
            if (!admitsKind(*order, kindBit(TypeKind::String))) {
                return refuse(*expr.args.at(2), "the `order` of `sort` is " + order->name() + "; expected string");
            }
        }
        return Type::listOf(std::move(element));
    }

    [[nodiscard]] std::expected<Type, Refusal> call(Expr const& expr) {
        FunctionSpec const& spec = functionSpec(expr.function);
        std::vector<Type> types;
        for (std::size_t index = 0; index < expr.args.size(); ++index) {
            auto type = check(*expr.args.at(index));
            if (!type) {
                return type;
            }
            KindSet const accepted = spec.accepts.at(std::min<std::size_t>(index, 2));
            if (!admitsKind(*type, accepted)) {
                return refuse(*expr.args.at(index), "argument " + std::to_string(index + 1) + " of `" +
                                                        std::string{spec.name} + "` is " + type->name() +
                                                        ", which it does not take");
            }
            types.push_back(std::move(*type));
        }
        if (auto refused = literalRefusal(expr)) {
            return std::unexpected(std::move(*refused));
        }
        if (spec.result != TypeKind::Any) {
            return Type::of(spec.result, true);
        }
        if (expr.function == FunctionId::ConcatLists) {
            return concatenated(types);
        }
        if (expr.function == FunctionId::Slice) {
            Type const& source = types.front();
            return Type::listOf(source.element ? *source.element : Type::any(), true);
        }
        return numericResult(expr.function, types);
    }

    /// The checks of an argument written as a literal: a rounding mode is checked at load, because a wrong mode is a
    /// wrong value, not a wrong look; a `t` fallback must be in the message subset.
    [[nodiscard]] static std::optional<Refusal> literalRefusal(Expr const& expr) {
        auto const literalText = [&expr](std::size_t index) -> std::string const* {
            if (index >= expr.args.size() || expr.args.at(index)->kind != ExprKind::Literal) {
                return nullptr;
            }
            return expr.args.at(index)->literal.asString();
        };
        if (auto const position = modeArgument(expr.function)) {
            std::string const* const mode = literalText(*position);
            if (mode != nullptr && !roundingMode(Value{*mode})) {
                return refuse(*expr.args.at(*position), "the rounding mode is `halfEven` or `halfUp`").error();
            }
        }
        if (std::string const* const pattern = literalText(1);
            expr.function == FunctionId::Translate && pattern != nullptr) {
            auto parsed = parseMessage(*pattern);
            if (!parsed) {
                return refuse(*expr.args.at(1), "the fallback is outside the ui/1 message subset: " + parsed.error())
                    .error();
            }
        }
        return std::nullopt;
    }

    /// `concatLists`'s type: `list<T>` when every element type that is known is `T`, else `list<any>`.
    [[nodiscard]] static Type concatenated(std::vector<Type> const& types) {
        std::optional<Type> common;
        for (Type const& type : types) {
            if (type.isAny() || !type.element || type.element->isAny()) {
                continue;
            }
            if (!common) {
                common = *type.element;
            } else if (!(*common == *type.element)) {
                return Type::listOf(Type::any(), true);
            }
        }
        return Type::listOf(common.value_or(Type::any()), true);
    }

    [[nodiscard]] static Type numericResult(FunctionId function, std::vector<Type> const& types) {
        if (function == FunctionId::Coalesce || function == FunctionId::Sum || function == FunctionId::Mean ||
            function == FunctionId::Min || function == FunctionId::Max || function == FunctionId::First) {
            return Type::any();
        }
        if (function == FunctionId::Neg || function == FunctionId::Round) {
            return Type::of(types.front().kind, true);
        }
        Type const& lhs = types.at(0);
        Type const& rhs = types.at(1);
        if (lhs.isAny() || rhs.isAny()) {
            return Type::any();
        }
        bool const lhsQuantity = lhs.kind == TypeKind::Quantity;
        bool const rhsQuantity = rhs.kind == TypeKind::Quantity;
        if (lhsQuantity || rhsQuantity) {
            if (function == FunctionId::Div && lhsQuantity && rhsQuantity) {
                return Type::of(TypeKind::Decimal, true);
            }
            return Type::of(TypeKind::Quantity, true);
        }
        if (lhs.kind == TypeKind::Int && rhs.kind == TypeKind::Int && function != FunctionId::Div) {
            return Type::of(TypeKind::Int, true);
        }
        return function == FunctionId::Div ? Type::any() : Type::of(TypeKind::Decimal, true);
    }

    NameResolver const* _resolver;
    std::vector<std::pair<std::string, Type>> _locals;
    // NOLINTEND(misc-no-recursion)
};

}  // namespace detail

/// @brief Checks an expression against the names a document declares.
///
/// Every reference is resolved through @p resolver, comprehension variables first; the arguments of a function, the
/// condition of `if` and the `where` of `filter` are checked against their accepted kinds where the argument's type
/// is statically known; a literal `t` fallback must be in the `ui/1` message subset.
/// @param expr     The expression.
/// @param resolver What the document's names resolve to.
/// @return The expression's static type (`any` where it cannot be known), or the refusal.
[[nodiscard]] inline std::expected<Type, Refusal> checkExpression(Expr const& expr, NameResolver const& resolver) {
    return detail::ExprChecker{resolver}.check(expr);
}

/// @brief One unit of the server's unit systems, as `x-unitAlternatives` declares it.
struct UnitInfo {
    /// @brief The canonical unit of the unit's dimension; units with the same canonical unit convert.
    std::string canonical;
    /// @brief The exact factor from this unit to the canonical one: one of this unit is `toCanonical` canonical
    ///        units.
    math::Rational toCanonical{1, math::DecimalPlaces{0}};
    /// @brief The unit's display text; empty to show its id.
    std::string display;
};

/// @brief The units a client knows, by id. A unit missing from the table is its own canonical unit.
using UnitTable = std::map<std::string, UnitInfo, std::less<>>;

/// @brief Formats a date for display: the instant already shifted to the display zone, the style (`short`,
///        `medium` or `long`), and whether to show the time.
using DateFormatter = std::function<std::string(time::DateTime local, std::string_view style, bool withTime)>;

/// @brief What the display functions and `t` format with: the client's locale and its formatting facts.
struct FormatContext {
    /// @brief The active BCP 47 locale.
    std::string locale = "en";
    /// @brief The locale's number formatting. Holds views: the strings it names must outlive the context.
    render::NumericLocale numeric{};
    /// @brief The message catalogue; empty shows every fallback.
    render::TranslationProvider translations;
    /// @brief The units quantities convert through.
    UnitTable units;
    /// @brief The display zone's offset from UTC, in minutes.
    std::int32_t utcOffsetMinutes = 0;
    /// @brief The date formatter; empty uses ISO 8601 (`2026-10-11`, `2026-10-11 08:30`).
    DateFormatter dates;
};

/// @brief What an evaluated expression reads: the interpreter implements it over its reactive values.
///
/// Every read returns a value; a name the environment does not hold reads as `null`. `report` is the client's
/// diagnostic channel: the evaluator reports each runtime error there, with the expression's path.
class Environment {
public:
    Environment() = default;
    Environment(Environment const&) = delete;
    Environment& operator=(Environment const&) = delete;
    Environment(Environment&&) = delete;
    Environment& operator=(Environment&&) = delete;
    virtual ~Environment() = default;

    /// @brief Reads `{"ref": "head.path"}`.
    /// @param head The first segment.
    /// @param path The segments after it.
    /// @return The value.
    [[nodiscard]] virtual Value ref(std::string_view head, std::span<std::string const> path) const = 0;

    /// @brief Reads `{"row": "path"}`.
    /// @param path The field path inside the innermost row.
    /// @return The value.
    [[nodiscard]] virtual Value row(std::span<std::string const> path) const = 0;

    /// @brief Reads `query`, `pending`, `error`, `result`, `errorKind` or `can`.
    /// @param form The form.
    /// @param name The query or mutation.
    /// @return The value: a `bool` for `pending` and `can`.
    [[nodiscard]] virtual Value control(ExprKind form, std::string_view name) const = 0;

    /// @brief Reads `{"pref": "point.path"}`.
    /// @param point The customization point.
    /// @param path  The segments after it.
    /// @return The value.
    [[nodiscard]] virtual Value pref(std::string_view point, std::span<std::string const> path) const = 0;

    /// @brief Reads `env`, `session` or `host`.
    /// @param form The form.
    /// @param name The name read.
    /// @return The value.
    [[nodiscard]] virtual Value client(ExprKind form, std::string_view name) const = 0;

    /// @brief The formatting facts.
    /// @return The context the display functions and `t` use.
    [[nodiscard]] virtual FormatContext const& formatting() const = 0;

    /// @brief Reports a runtime error of an expression.
    /// @param path   The JSON Pointer of the expression node that failed.
    /// @param reason What went wrong.
    virtual void report(std::string_view path, std::string_view reason) const = 0;
};

/// @brief Reads a path out of a value: a record's field by name, a list's element by index.
/// @param value The value.
/// @param path  The segments.
/// @return The value at the path, or `null` when a segment does not exist.
[[nodiscard]] inline Value walk(Value const& value, std::span<std::string const> path) {
    Value const* current = &value;
    for (std::string const& segment : path) {
        if (ValueList const* const items = current->asList()) {
            // A segment past the largest index is out of range, not wrapped onto an element that exists.
            std::size_t index = 0;
            char const* const end = std::to_address(segment.end());
            auto const [stop, error] = std::from_chars(std::to_address(segment.begin()), end, index);
            if (error != std::errc{} || stop != end || index >= items->size()) {
                return Value{};
            }
            current = &items->at(index);
            continue;
        }
        current = current->field(segment);
        if (current == nullptr) {
            return Value{};
        }
    }
    return *current;
}

namespace detail {

/// @brief A number taken apart for arithmetic: its exact value, whether it is an `int`, and its unit.
struct Number {
    /// @brief The exact value.
    math::Rational value;
    /// @brief Whether it is an `int`.
    bool integer = false;
    /// @brief The unit of a quantity; `nullptr` for a plain number.
    std::string const* unit = nullptr;
};

/// @brief Takes a numeric value apart.
/// @param value The value.
/// @return The number, or `std::nullopt` when it is not one.
[[nodiscard]] inline std::optional<Number> numeric(Value const& value) {
    if (std::int64_t const* const whole = value.asInt()) {
        return Number{.value = math::Rational{*whole, math::DecimalPlaces{0}}, .integer = true, .unit = nullptr};
    }
    if (Decimal const* const decimal = value.asDecimal()) {
        return Number{.value = decimal->value, .integer = false, .unit = nullptr};
    }
    if (Quantity const* const quantity = value.asQuantity()) {
        return Number{.value = quantity->value, .integer = false, .unit = &quantity->unit};
    }
    return std::nullopt;
}

/// @brief Builds a numeric value.
/// @param value   The exact value.
/// @param integer Whether every operand was an `int`.
/// @param unit    The unit, or `nullptr`.
/// @return A quantity with a unit; an `int` when integral and every operand was one; a decimal otherwise.
[[nodiscard]] inline Value makeNumber(math::Rational const& value, bool integer, std::string const* unit) {
    if (unit != nullptr) {
        return Quantity{.value = value, .unit = *unit};
    }
    if (integer && value.isInteger()) {
        return Value{value.numerator};
    }
    return Decimal{value};
}

/// @brief Converts an exact value between two units of one dimension.
/// @param value  The value in @p from.
/// @param from   Its unit.
/// @param target The wanted unit.
/// @param units  The unit table.
/// @return The value in @p target, its places widened by the decimal places of the conversion ratio; `std::nullopt`
///         when the units do not convert or the result overflows.
[[nodiscard]] inline std::optional<math::Rational> convertUnit(math::Rational const& value, std::string_view from,
                                                               std::string_view target, UnitTable const& units) {
    if (from == target) {
        return value;
    }
    auto factor = [&units](std::string_view unit) {
        auto const found = units.find(unit);
        if (found == units.end()) {
            return std::pair{std::string{unit}, math::Rational{1, math::DecimalPlaces{0}}};
        }
        return std::pair{found->second.canonical, found->second.toCanonical};
    };
    auto const [fromBase, fromFactor] = factor(from);
    auto const [toBase, toFactor] = factor(target);
    if (fromBase != toBase) {
        return std::nullopt;
    }
    auto const ratio =
        math::checkedDiv(math::Rational{math::Numerator{fromFactor.numerator},
                                        math::Denominator{fromFactor.denominator}, math::DecimalPlaces{0}},
                         math::Rational{math::Numerator{toFactor.numerator}, math::Denominator{toFactor.denominator},
                                        math::DecimalPlaces{0}});
    if (!ratio) {
        return std::nullopt;
    }
    auto converted = math::checkedMul(value, *ratio);
    if (!converted) {
        return std::nullopt;
    }
    // The places widen by the ratio's own decimal places, so 1500 mg shows as 1.500 g rather than rounding to 2 g;
    // a ratio that is not a terminating decimal adds none.
    auto denominator = static_cast<std::uint64_t>(ratio->denominator);
    std::uint32_t twos = 0;
    std::uint32_t fives = 0;
    for (; denominator % 2U == 0; denominator /= 2U) {
        ++twos;
    }
    for (; denominator % 5U == 0; denominator /= 5U) {
        ++fives;
    }
    std::uint32_t const extra = denominator == 1 ? std::max(twos, fives) : 0U;
    converted->decimalPlaces =
        math::DecimalPlaces{std::min(value.decimalPlaces.value + extra, math::kMaxDecimalPlaces)};
    return *converted;
}

/// @brief The display text of a unit.
/// @param unit  The unit id.
/// @param units  The unit table.
/// @return Its display text, or the id.
[[nodiscard]] inline std::string unitDisplay(std::string const& unit, UnitTable const& units) {
    auto const found = units.find(unit);
    return found == units.end() || found->second.display.empty() ? unit : found->second.display;
}

/// @brief The fraction digits a currency shows (ISO 4217's minor unit).
/// @param currency The currency code.
/// @return 0, 2 or 3.
[[nodiscard]] inline std::uint32_t currencyDigits(std::string_view currency) {
    static constexpr std::array<std::string_view, 9> kNone = {"JPY", "KRW", "VND", "CLP", "ISK",
                                                              "UGX", "XAF", "XOF", "PYG"};
    static constexpr std::array<std::string_view, 7> kThree = {"BHD", "KWD", "OMR", "JOD", "TND", "IQD", "LYD"};
    if (oneOf(kNone, currency)) {
        return 0;
    }
    return oneOf(kThree, currency) ? 3U : 2U;
}

/// @brief Evaluates expressions against an environment, with the comprehension variables in scope.
// NOLINTBEGIN(misc-no-recursion): an expression is a tree and evaluating it descends it; the node bound limits the depth.
class Evaluator {
public:
    /// @brief Evaluates against @p env.
    /// @param env What the expression reads.
    explicit Evaluator(Environment const& env) : _env{&env} {}

    /// @brief Evaluates one node.
    /// @param expr The node.
    /// @return Its value; `null` on a runtime error, which is reported.
    [[nodiscard]] Value eval(Expr const& expr) {
        switch (expr.kind) {
            case ExprKind::Literal:
                return expr.literal;
            case ExprKind::Ref:
                for (auto const& [name, value] : std::views::reverse(_locals)) {
                    if (name == expr.name) {
                        return walk(value, expr.fieldPath);
                    }
                }
                return _env->ref(expr.name, expr.fieldPath);
            case ExprKind::Row:
                return _env->row(expr.fieldPath);
            case ExprKind::Query:
            case ExprKind::Pending:
            case ExprKind::Error:
            case ExprKind::Result:
            case ExprKind::ErrorKind:
            case ExprKind::Can:
                return _env->control(expr.kind, expr.name);
            case ExprKind::Pref:
                return _env->pref(expr.name, expr.fieldPath);
            case ExprKind::Env:
            case ExprKind::Session:
            case ExprKind::Host:
                return _env->client(expr.kind, expr.name);
            case ExprKind::If:
                return choice(expr);
            case ExprKind::Object: {
                ValueRecord record;
                for (auto const& [name, field] : expr.fields) {
                    record.insert_or_assign(name, eval(*field));
                }
                return record;
            }
            case ExprKind::List: {
                ValueList items;
                items.reserve(expr.args.size());
                for (ExprPtr const& item : expr.args) {
                    items.push_back(eval(*item));
                }
                return items;
            }
            case ExprKind::Map:
            case ExprKind::Filter:
                return comprehension(expr);
            case ExprKind::Sort:
                return sorted(expr);
            case ExprKind::Runs:
                return grouped(expr);
            case ExprKind::Get:
                return walk(eval(*expr.args.at(0)), expr.fieldPath);
            case ExprKind::Call:
            default:
                return call(expr);
        }
    }

    /// @brief Orders two values as sorting does: as `lt` orders them, except that strings compare case-folded as
    ///        the table engine's default collator folds them, and by their own bytes when they fold the same, so
    ///        the order never depends on the source order.
    /// @param lhs A value, not `null`.
    /// @param rhs A value, not `null`.
    /// @return The ordering; `nullopt` when the two are not of one comparable kind.
    [[nodiscard]] std::optional<std::strong_ordering> sortOrder(Value const& lhs, Value const& rhs) {
        std::string const* const lhsText = lhs.asString();
        std::string const* const rhsText = rhs.asString();
        if (lhsText != nullptr && rhsText != nullptr) {
            table::CodePointCollator const collator;
            std::strong_ordering const folded = collator.fold(*lhsText) <=> collator.fold(*rhsText);
            return folded != std::strong_ordering::equal ? folded : *lhsText <=> *rhsText;
        }
        return order(lhs, rhs);
    }

    /// @brief A stable order of rows by their sort keys: by each key in turn, `null` last in either direction, and
    ///        a descending key reversing the comparison, not the rows, so rows with equal keys keep their order.
    /// @param keys       Each row's keys, most significant first; every row has one per entry of @p descending.
    /// @param descending Whether each key sorts descending.
    /// @return The rows' indices in sorted order; `nullopt` when a key's values are not of one comparable kind.
    [[nodiscard]] std::optional<std::vector<std::size_t>> sortRows(std::vector<std::vector<Value>> const& keys,
                                                                   std::vector<bool> const& descending) {
        // Every value of a key is checked against the key's first value, so the comparison below never meets a
        // pair it cannot order and the sort's ordering is strict and weak.
        for (std::size_t level = 0; level < descending.size(); ++level) {
            if (!orderable(keys, level)) {
                return std::nullopt;
            }
        }
        std::vector<std::size_t> indices(keys.size());
        std::ranges::iota(indices, std::size_t{0});
        std::ranges::stable_sort(indices, [&](std::size_t left, std::size_t right) {
            for (std::size_t level = 0; level < descending.size(); ++level) {
                std::strong_ordering const ordering =
                    keyOrder(keys.at(left).at(level), keys.at(right).at(level), descending.at(level));
                if (ordering != std::strong_ordering::equal) {
                    return ordering == std::strong_ordering::less;
                }
            }
            return false;
        });
        return indices;
    }

    /// @brief Whether two `by` values put their elements in one run: `eq`'s comparison, except that `null` equals
    ///        `null`, so the elements without the value make one run.
    /// @param lhs A value.
    /// @param rhs A value.
    /// @return Whether they are the same run's.
    [[nodiscard]] bool sameRun(Value const& lhs, Value const& rhs) { return sameElement(lhs, rhs); }

private:
    Value mismatch(Expr const& expr, std::string_view reason) {
        ++_mismatches;
        _env->report(expr.path, reason);
        return Value{};
    }

    /// The rounding mode of a call whose mode is its third argument: half to even when it is absent.
    static std::optional<math::RoundingMode> modeOf(std::vector<Value> const& args) {
        if (args.size() < 3) {
            return math::RoundingMode::HalfEven;
        }
        return roundingMode(args.at(2));
    }

    Value choice(Expr const& expr) {
        Value const cond = eval(*expr.args.at(0));
        if (cond.isNull()) {
            return Value{};
        }
        bool const* const flag = cond.asBool();
        if (flag == nullptr) {
            return mismatch(expr, "the condition of `if` is not a bool");
        }
        return eval(*expr.args.at(*flag ? 1 : 2));
    }

    Value comprehension(Expr const& expr) {
        Value const source = eval(*expr.args.at(0));
        if (source.isNull()) {
            return Value{};
        }
        ValueList const* const items = source.asList();
        if (items == nullptr) {
            return mismatch(expr, "a comprehension runs over a list");
        }
        ValueList out;
        for (Value const& item : *items) {
            _locals.emplace_back(expr.name, item);
            Value body = eval(*expr.args.at(1));
            _locals.pop_back();
            if (expr.kind == ExprKind::Map) {
                out.push_back(std::move(body));
                continue;
            }
            bool const* const keep = body.asBool();
            if (keep == nullptr && !body.isNull()) {
                return mismatch(expr, "the `where` of `filter` is not a bool");
            }
            if (keep != nullptr && *keep) {
                out.push_back(item);
            }
        }
        return out;
    }

    /// Whether every value of one sort key orders against the key's first value; `null`s aside.
    bool orderable(std::vector<std::vector<Value>> const& keys, std::size_t level) {
        Value const* first = nullptr;
        for (std::vector<Value> const& row : keys) {
            Value const& value = row.at(level);
            if (value.isNull()) {
                continue;
            }
            if (first == nullptr) {
                first = &value;
            } else if (!sortOrder(*first, value)) {
                return false;
            }
        }
        return true;
    }

    /// Where @p lhs sorts against @p rhs for one key: `null` last either way, and a descending key reverses the
    /// comparison, so equal values compare equal and keep their order.
    std::strong_ordering keyOrder(Value const& lhs, Value const& rhs, bool descending) {
        if (lhs.isNull() || rhs.isNull()) {
            if (lhs.isNull() == rhs.isNull()) {
                return std::strong_ordering::equal;
            }
            return lhs.isNull() ? std::strong_ordering::greater : std::strong_ordering::less;
        }
        std::strong_ordering const ordering = sortOrder(lhs, rhs).value_or(std::strong_ordering::equal);
        if (!descending || ordering == std::strong_ordering::equal) {
            return ordering;
        }
        return ordering == std::strong_ordering::less ? std::strong_ordering::greater : std::strong_ordering::less;
    }

    /// `runs`: the list split into runs of adjacent elements whose `by` values are equal.
    Value grouped(Expr const& expr) {
        Value const source = eval(*expr.args.at(0));
        if (source.isNull()) {
            return Value{};
        }
        ValueList const* const items = source.asList();
        if (items == nullptr) {
            return mismatch(expr, "`runs` runs over a list");
        }
        ValueList out;
        Value key;
        ValueList run;
        for (Value const& item : *items) {
            std::size_t const before = _mismatches;
            _locals.emplace_back(expr.name, item);
            Value value = eval(*expr.args.at(1));
            _locals.pop_back();
            // A `by` that fails for an element fails the whole form, as a mismatch inside any operand does.
            if (_mismatches != before) {
                return Value{};
            }
            if (run.empty()) {
                key = std::move(value);
            } else if (!sameRun(key, value)) {
                out.push_back(runRecord(std::exchange(key, std::move(value)), std::exchange(run, {})));
            }
            run.push_back(item);
        }
        if (!run.empty()) {
            out.push_back(runRecord(std::move(key), std::move(run)));
        }
        return out;
    }

    /// `sort`: the list ordered by `by`, ascending unless `order` is `"desc"`.
    Value sorted(Expr const& expr) {
        Value const source = eval(*expr.args.at(0));
        if (source.isNull()) {
            return Value{};
        }
        ValueList const* const items = source.asList();
        if (items == nullptr) {
            return mismatch(expr, "`sort` runs over a list");
        }
        bool descending = false;
        if (expr.args.size() > 2) {
            Value const given = eval(*expr.args.at(2));
            std::string const* const direction = given.asString();
            if (!given.isNull() && (direction == nullptr || (*direction != "asc" && *direction != "desc"))) {
                return mismatch(expr, R"(the `order` of `sort` is "asc" or "desc")");
            }
            descending = direction != nullptr && *direction == "desc";
        }
        std::vector<std::vector<Value>> keys;
        keys.reserve(items->size());
        for (Value const& item : *items) {
            _locals.emplace_back(expr.name, item);
            keys.emplace_back(1, eval(*expr.args.at(1)));
            _locals.pop_back();
        }
        auto const ranked = sortRows(keys, {descending});
        if (!ranked) {
            return mismatch(expr, "the `by` values of `sort` are not of one comparable kind");
        }
        ValueList out;
        out.reserve(items->size());
        for (std::size_t const index : *ranked) {
            out.push_back(items->at(index));
        }
        return out;
    }

    /// `slice`: `count` elements of the list from `start`, or every element from it.
    Value sliced(Expr const& expr, std::vector<Value> const& args) {
        ValueList const* const items = args.at(0).asList();
        std::int64_t const* const start = args.at(1).asInt();
        std::int64_t const* const count = args.size() > 2 ? args.at(2).asInt() : nullptr;
        bool const badCount = args.size() > 2 && (count == nullptr || *count < 0);
        if (items == nullptr || start == nullptr || *start < 0 || badCount) {
            return mismatch(expr, "`slice` takes a list, a start and a count of 0 or more");
        }
        auto const from = static_cast<std::size_t>(std::min<std::int64_t>(*start, std::ssize(*items)));
        std::size_t const left = items->size() - from;
        std::size_t const taken =
            count == nullptr ? left : std::min(left, static_cast<std::size_t>(*count));
        auto const first = items->begin() + static_cast<std::ptrdiff_t>(from);
        return ValueList{first, first + static_cast<std::ptrdiff_t>(taken)};
    }

    /// Three-valued `and` / `or`: the dominant value wins over `null`.
    Value kleene(Expr const& expr, bool dominant) {
        bool sawNull = false;
        for (ExprPtr const& arg : expr.args) {
            Value const value = eval(*arg);
            if (bool const* const flag = value.asBool()) {
                if (*flag == dominant) {
                    return dominant;
                }
                continue;
            }
            if (!value.isNull()) {
                _env->report(arg->path, "a logical operand is not a bool");
            }
            sawNull = true;
        }
        if (sawNull) {
            return Value{};
        }
        return !dominant;
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): the four operators over plain numbers and quantities, one rule each.
    Value arithmetic(Expr const& expr, Value const& lhsValue, Value const& rhsValue) {
        auto const lhs = numeric(lhsValue);
        auto const rhs = numeric(rhsValue);
        if (!lhs || !rhs) {
            return mismatch(expr, "an arithmetic operand is not a number");
        }
        UnitTable const& units = _env->formatting().units;
        FunctionId const function = expr.function;
        math::Rational right = rhs->value;
        std::string const* unit = nullptr;
        if (lhs->unit != nullptr && rhs->unit != nullptr) {
            if (function == FunctionId::Mul) {
                return mismatch(expr, "a product of two quantities has no declared unit");
            }
            auto converted = convertUnit(rhs->value, *rhs->unit, *lhs->unit, units);
            if (!converted) {
                return mismatch(expr, "the units `" + *lhs->unit + "` and `" + *rhs->unit + "` do not convert");
            }
            right = *converted;
            unit = function == FunctionId::Div ? nullptr : lhs->unit;
        } else if (lhs->unit != nullptr || rhs->unit != nullptr) {
            bool const scales = function == FunctionId::Mul || (function == FunctionId::Div && lhs->unit != nullptr);
            if (!scales) {
                return mismatch(expr, "a quantity and a plain number do not combine this way");
            }
            unit = lhs->unit != nullptr ? lhs->unit : rhs->unit;
        }
        if (function == FunctionId::Div && right.isZero()) {
            return mismatch(expr, "division by zero");
        }
        std::expected<math::Rational, math::RationalError> result = math::Rational{};
        if (function == FunctionId::Add) {
            result = math::checkedAdd(lhs->value, right);
        } else if (function == FunctionId::Sub) {
            result = math::checkedSub(lhs->value, right);
        } else if (function == FunctionId::Mul) {
            result = math::checkedMul(lhs->value, right);
        } else {
            result = math::checkedDiv(lhs->value, right);
        }
        if (!result) {
            return mismatch(expr, "the result overflows");
        }
        // A quantity is never an `int`, so a ratio of two quantities comes back a decimal.
        return makeNumber(*result, lhs->integer && rhs->integer, unit);
    }

    /// Orders two values of one comparable kind: numbers exactly (quantities converted), timestamps, strings.
    std::optional<std::strong_ordering> order(Value const& lhs, Value const& rhs) {
        auto const lhsNumber = numeric(lhs);
        auto const rhsNumber = numeric(rhs);
        if (lhsNumber && rhsNumber) {
            if ((lhsNumber->unit == nullptr) != (rhsNumber->unit == nullptr)) {
                return std::nullopt;
            }
            math::Rational right = rhsNumber->value;
            if (lhsNumber->unit != nullptr) {
                auto converted = convertUnit(right, *rhsNumber->unit, *lhsNumber->unit, _env->formatting().units);
                if (!converted) {
                    return std::nullopt;
                }
                right = *converted;
            }
            return lhsNumber->value <=> right;
        }
        if (time::DateTime const* const lhsTime = lhs.asTimestamp()) {
            if (time::DateTime const* const rhsTime = rhs.asTimestamp()) {
                return *lhsTime <=> *rhsTime;
            }
            return std::nullopt;
        }
        if (std::string const* const lhsText = lhs.asString()) {
            if (std::string const* const rhsText = rhs.asString()) {
                return *lhsText <=> *rhsText;
            }
        }
        return std::nullopt;
    }

    /// Equality over non-null values: numbers by value, lists and records element-wise, other kinds by identity.
    bool equal(Value const& lhs, Value const& rhs) {
        if (numeric(lhs) && numeric(rhs)) {
            auto const ordering = order(lhs, rhs);
            return ordering.has_value() && *ordering == std::strong_ordering::equal;
        }
        if (lhs.kind() != rhs.kind()) {
            return false;
        }
        if (ValueList const* const lhsItems = lhs.asList()) {
            return std::ranges::equal(*lhsItems, *rhs.asList(), [this](Value const& left, Value const& right) {
                return sameElement(left, right);
            });
        }
        if (ValueRecord const* const lhsFields = lhs.asRecord()) {
            // Records are ordered by field name, so equal records have equal sequences of fields.
            return std::ranges::equal(*lhsFields, *rhs.asRecord(), [this](auto const& left, auto const& right) {
                return left.first == right.first && sameElement(left.second, right.second);
            });
        }
        return lhs == rhs;
    }

    /// Equality of two elements of a list or record, where `null` equals only `null`.
    bool sameElement(Value const& left, Value const& right) {
        if (left.isNull() || right.isNull()) {
            return left.isNull() && right.isNull();
        }
        return equal(left, right);
    }

    /// The display text of a value, as `format` shows an argument.
    std::optional<std::string> display(Value const& value) {
        FormatContext const& context = _env->formatting();
        if (std::string const* const text = value.asString()) {
            return *text;
        }
        if (bool const* const flag = value.asBool()) {
            return std::string{*flag ? "true" : "false"};
        }
        if (Quantity const* const quantity = value.asQuantity()) {
            auto number = formatNumber(value, context.numeric);
            if (!number) {
                return std::nullopt;
            }
            return *number + " " + unitDisplay(quantity->unit, context.units);
        }
        if (auto number = formatNumber(value, context.numeric)) {
            return number;
        }
        if (time::DateTime const* const instant = value.asTimestamp()) {
            return dateText(*instant, "medium", true);
        }
        return std::nullopt;
    }

    std::optional<std::string> dateText(time::DateTime instant, std::string_view style, bool withTime) {
        if (style != "short" && style != "medium" && style != "long") {
            return std::nullopt;
        }
        FormatContext const& context = _env->formatting();
        time::DateTime const local = instant + std::chrono::minutes{context.utcOffsetMinutes};
        if (context.dates) {
            return context.dates(local, style, withTime);
        }
        std::string const iso = local.toIso8601();
        if (!withTime) {
            return iso.substr(0, 10);
        }
        return iso.substr(0, 10) + " " + iso.substr(11, style == "long" ? 8 : 5);
    }

    /// Why `format` cannot show an argument: a number that overflows at its places, or a value it has no text for.
    Value cannotShow(Expr const& expr, Value const& arg, std::size_t index) {
        if (numeric(arg)) {
            return mismatch(expr, "the result overflows");
        }
        return mismatch(expr, "`format` cannot show argument " + std::to_string(index + 1));
    }

    Value formatTemplate(Expr const& expr, std::vector<Value> const& args) {
        std::string const& pattern = *args.front().asString();
        std::vector<std::string> shown;
        for (std::size_t index = 1; index < args.size(); ++index) {
            auto text = display(args.at(index));
            if (!text) {
                return cannotShow(expr, args.at(index), index);
            }
            shown.push_back(std::move(*text));
        }
        std::string out;
        for (std::size_t pos = 0; pos < pattern.size(); ++pos) {
            char const chr = pattern.at(pos);
            if ((chr == '{' || chr == '}') && pos + 1 < pattern.size() && pattern.at(pos + 1) == chr) {
                out += chr;
                ++pos;
                continue;
            }
            if (chr == '}') {
                return mismatch(expr, "`format`'s template has an unmatched `}`");
            }
            if (chr != '{') {
                out += chr;
                continue;
            }
            std::size_t const close = pattern.find('}', pos);
            std::string_view const digits = close == std::string::npos
                                                ? std::string_view{}
                                                : std::string_view{pattern}.substr(pos + 1, close - pos - 1);
            std::size_t index = 0;
            bool valid = !digits.empty() && digits.size() < 4;
            for (char const digit : digits) {
                valid = valid && digit >= '0' && digit <= '9';
                index = valid ? (index * 10) + static_cast<std::size_t>(digit - '0') : index;
            }
            if (!valid || index >= shown.size()) {
                return mismatch(expr, "`format`'s template has a placeholder with no argument");
            }
            out += shown.at(index);
            pos = close;
        }
        return out;
    }

    Value translate(Expr const& expr, std::vector<Value> const& args) {
        FormatContext const& context = _env->formatting();
        std::string const& key = *args.at(0).asString();
        std::string const& fallback = *args.at(1).asString();
        ValueRecord const empty;
        ValueRecord const* named = &empty;
        if (args.size() > 2) {
            named = args.at(2).asRecord();
            if (named == nullptr) {
                return mismatch(expr, "the arguments of `t` are a record");
            }
        }
        std::optional<MessagePattern> message;
        if (context.translations) {
            if (auto const pattern = context.translations(key, context.locale)) {
                auto parsed = parseMessage(*pattern);
                if (parsed) {
                    message = std::move(*parsed);
                } else {
                    _env->report(expr.path, "the catalogue's message `" + key +
                                                "` is outside the ui/1 subset: " + parsed.error());
                }
            }
        }
        if (!message) {
            auto parsed = parseMessage(fallback);
            if (!parsed) {
                _env->report(expr.path, "the fallback is outside the ui/1 subset: " + parsed.error());
                return fallback;
            }
            message = std::move(*parsed);
        }
        FormattedMessage formatted =
            formatMessage(*message, *named, MessageLocale{.locale = context.locale, .numeric = context.numeric});
        for (std::string const& error : formatted.errors) {
            _env->report(expr.path, error);
        }
        return std::move(formatted.text);
    }

    Value listFunction(Expr const& expr, ValueList const& items) {
        FunctionId const function = expr.function;
        if (function == FunctionId::Count) {
            return static_cast<std::int64_t>(items.size());
        }
        if (function == FunctionId::First) {
            if (items.empty()) {
                return Value{};
            }
            return items.front();
        }
        if (function == FunctionId::Any || function == FunctionId::All) {
            bool const dominant = function == FunctionId::Any;
            bool sawNull = false;
            for (Value const& item : items) {
                bool const* const flag = item.asBool();
                if (flag != nullptr && *flag == dominant) {
                    return dominant;
                }
                if (flag == nullptr && !item.isNull()) {
                    return mismatch(expr, "an element is not a bool");
                }
                sawNull = sawNull || item.isNull();
            }
            if (sawNull) {
                return Value{};
            }
            return !dominant;
        }
        if (function == FunctionId::Min || function == FunctionId::Max) {
            return extreme(expr, items);
        }
        return aggregate(expr, items);
    }

    Value extreme(Expr const& expr, ValueList const& items) {
        if (items.empty()) {
            return Value{};
        }
        Value const* best = &items.front();
        for (Value const& item : items) {
            if (item.isNull()) {
                return Value{};
            }
            auto const ordering = order(item, *best);
            if (!ordering) {
                return mismatch(expr, "the elements are not of one comparable kind");
            }
            bool const better = expr.function == FunctionId::Min ? std::is_lt(*ordering) : std::is_gt(*ordering);
            if (better) {
                best = &item;
            }
        }
        return *best;
    }

    Value aggregate(Expr const& expr, ValueList const& items) {
        bool const mean = expr.function == FunctionId::Mean;
        if (items.empty()) {
            return mean ? Value{} : Value{std::int64_t{0}};
        }
        math::Rational total{0, math::DecimalPlaces{0}};
        bool integer = true;
        std::string const* unit = nullptr;
        std::uint32_t places = 0;
        for (std::size_t index = 0; index < items.size(); ++index) {
            Value const& item = items.at(index);
            if (item.isNull()) {
                return Value{};
            }
            auto const number = numeric(item);
            if (!number) {
                return mismatch(expr, "an element is not a number");
            }
            if (index == 0) {
                unit = number->unit;
            } else if ((unit == nullptr) != (number->unit == nullptr)) {
                return mismatch(expr, "quantities and plain numbers do not sum together");
            }
            math::Rational value = number->value;
            if (unit != nullptr) {
                auto converted = convertUnit(value, *number->unit, *unit, _env->formatting().units);
                if (!converted) {
                    return mismatch(expr, "the elements' units do not convert");
                }
                value = *converted;
            }
            places = std::max(places, value.decimalPlaces.value);
            integer = integer && number->integer;
            auto next = math::checkedAdd(total, value);
            if (!next) {
                return mismatch(expr, "the sum overflows");
            }
            total = *next;
        }
        if (!mean) {
            return makeNumber(total, integer, unit);
        }
        auto const quotient =
            math::checkedDiv(total, math::Rational{static_cast<std::int64_t>(items.size()), math::DecimalPlaces{0}});
        auto const rounded = quotient ? math::checkedRoundToDecimalPlaces(*quotient, math::DecimalPlaces{places},
                                                                          math::RoundingMode::HalfEven)
                                      : std::nullopt;
        if (!rounded) {
            return mismatch(expr, "the result overflows");
        }
        return makeNumber(*rounded, integer, unit);
    }

    Value call(Expr const& expr) {
        FunctionId const function = expr.function;
        if (function == FunctionId::And || function == FunctionId::Or) {
            return kleene(expr, function == FunctionId::Or);
        }
        if (function == FunctionId::Coalesce) {
            for (ExprPtr const& arg : expr.args) {
                Value value = eval(*arg);
                if (!value.isNull()) {
                    return value;
                }
            }
            return Value{};
        }
        if (function == FunctionId::Engaged) {
            return !eval(*expr.args.front()).isNull();
        }
        std::vector<Value> args;
        args.reserve(expr.args.size());
        for (ExprPtr const& arg : expr.args) {
            args.push_back(eval(*arg));
            if (args.back().isNull()) {
                return Value{};
            }
        }
        return strict(expr, args);
    }

    /// The functions whose every argument is required: any `null` argument has already made the result `null`.
    // GCC's -Wstrict-overflow fires on this whole function at -O3, attributing it to the function's opening line
    // rather than a specific statement -- a known false positive from its range analysis across the switch's many
    // checked-arithmetic calls (Rational's own checkedAdd/checkedSub/checkedMul are all free of signed-overflow UB).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-overflow"
#endif
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per function of the library.
    Value strict(Expr const& expr, std::vector<Value> const& args) {
        FormatContext const& context = _env->formatting();
        switch (expr.function) {
            case FunctionId::Not: {
                bool const* const flag = args.front().asBool();
                if (flag == nullptr) {
                    return mismatch(expr, "`not` takes a bool");
                }
                return !*flag;
            }
            case FunctionId::Eq:
                return equal(args.at(0), args.at(1));
            case FunctionId::Ne:
                return !equal(args.at(0), args.at(1));
            case FunctionId::Lt:
            case FunctionId::Le:
            case FunctionId::Gt:
            case FunctionId::Ge: {
                auto const ordering = order(args.at(0), args.at(1));
                if (!ordering) {
                    return mismatch(expr, "the operands are not of one comparable kind");
                }
                if (expr.function == FunctionId::Lt) {
                    return std::is_lt(*ordering);
                }
                if (expr.function == FunctionId::Le) {
                    return std::is_lteq(*ordering);
                }
                if (expr.function == FunctionId::Gt) {
                    return std::is_gt(*ordering);
                }
                return std::is_gteq(*ordering);
            }
            case FunctionId::Add:
            case FunctionId::Sub:
            case FunctionId::Mul:
            case FunctionId::Div:
                return arithmetic(expr, args.at(0), args.at(1));
            case FunctionId::Neg: {
                auto const number = numeric(args.front());
                if (!number) {
                    return mismatch(expr, "`neg` takes a number");
                }
                auto negated = math::checkedSub(math::Rational{0, number->value.decimalPlaces}, number->value);
                if (!negated) {
                    return mismatch(expr, "the result overflows");
                }
                return makeNumber(*negated, number->integer, number->unit);
            }
            case FunctionId::Round: {
                auto const number = numeric(args.at(0));
                std::int64_t const* const places = args.at(1).asInt();
                if (!number || places == nullptr || *places < 0 ||
                    std::cmp_greater(*places, math::kMaxDecimalPlaces)) {
                    return mismatch(expr, "`round` takes a number and places from 0 to 18");
                }
                auto const mode = modeOf(args);
                if (!mode) {
                    return mismatch(expr, "the rounding mode is `halfEven` or `halfUp`");
                }
                if (number->integer) {
                    return args.at(0);
                }
                auto const rounded = math::checkedRoundToDecimalPlaces(
                    number->value, math::DecimalPlaces{static_cast<std::uint32_t>(*places)}, *mode);
                if (!rounded) {
                    return mismatch(expr, "the result overflows");
                }
                return makeNumber(*rounded, false, number->unit);
            }
            case FunctionId::Concat: {
                std::string out;
                for (Value const& arg : args) {
                    std::string const* const text = arg.asString();
                    if (text == nullptr) {
                        return mismatch(expr, "`concat` takes strings");
                    }
                    out += *text;
                }
                return out;
            }
            case FunctionId::Format:
                if (args.front().asString() == nullptr) {
                    return mismatch(expr, "`format`'s template is a string");
                }
                return formatTemplate(expr, args);
            case FunctionId::Trim: {
                std::string const* const text = args.front().asString();
                if (text == nullptr) {
                    return mismatch(expr, "`trim` takes a string");
                }
                constexpr std::string_view kSpace = " \t\n\r\f\v";
                std::size_t const first = text->find_first_not_of(kSpace);
                if (first == std::string::npos) {
                    return std::string{};
                }
                return text->substr(first, text->find_last_not_of(kSpace) - first + 1);
            }
            case FunctionId::Length: {
                std::string const* const text = args.front().asString();
                if (text == nullptr) {
                    return mismatch(expr, "`length` takes a string");
                }
                std::int64_t points = 0;
                for (char const chr : *text) {
                    points += (static_cast<unsigned char>(chr) & 0xC0U) == 0x80U ? 0 : 1;
                }
                return points;
            }
            case FunctionId::IsEmpty: {
                if (std::string const* const text = args.front().asString()) {
                    return text->empty();
                }
                if (ValueList const* const items = args.front().asList()) {
                    return items->empty();
                }
                return mismatch(expr, "`isEmpty` takes a string or a list");
            }
            case FunctionId::Contains: {
                std::string const* const text = args.at(0).asString();
                std::string const* const part = args.at(1).asString();
                if (text == nullptr || part == nullptr) {
                    return mismatch(expr, "`contains` takes two strings");
                }
                return text->contains(*part);
            }
            case FunctionId::Count:
            case FunctionId::Sum:
            case FunctionId::Mean:
            case FunctionId::Min:
            case FunctionId::Max:
            case FunctionId::Any:
            case FunctionId::All:
            case FunctionId::First: {
                ValueList const* const items = args.front().asList();
                if (items == nullptr) {
                    return mismatch(expr, "the argument is not a list");
                }
                return listFunction(expr, *items);
            }
            case FunctionId::Includes:
            case FunctionId::IndexOf: {
                ValueList const* const items = args.at(0).asList();
                if (items == nullptr) {
                    return mismatch(expr, "the first argument is not a list");
                }
                for (std::size_t index = 0; index < items->size(); ++index) {
                    if (!items->at(index).isNull() && equal(items->at(index), args.at(1))) {
                        return expr.function == FunctionId::Includes ? Value{true}
                                                                     : Value{static_cast<std::int64_t>(index)};
                    }
                }
                return expr.function == FunctionId::Includes ? Value{false} : Value{};
            }
            case FunctionId::Number: {
                auto text = formatNumber(args.front(), context.numeric);
                if (!text) {
                    return mismatch(expr, numeric(args.front()) ? "the result overflows" : "`number` takes a number");
                }
                return std::move(*text);
            }
            case FunctionId::DecimalText: {
                auto const number = numeric(args.at(0));
                std::int64_t const* const places = args.at(1).asInt();
                if (!number || places == nullptr || *places < 0 ||
                    std::cmp_greater(*places, math::kMaxDecimalPlaces)) {
                    return mismatch(expr, "`decimal` takes a number and places from 0 to 18");
                }
                auto const mode = modeOf(args);
                if (!mode) {
                    return mismatch(expr, "the rounding mode is `halfEven` or `halfUp`");
                }
                auto const text = decimalText(number->value, static_cast<std::uint32_t>(*places), *mode);
                if (!text) {
                    return mismatch(expr, "the result overflows");
                }
                return render::formatCanonicalNumber(*text, context.numeric);
            }
            case FunctionId::QuantityText: {
                Quantity const* const quantity = args.at(0).asQuantity();
                if (quantity == nullptr) {
                    return mismatch(expr, "`quantity` takes a quantity");
                }
                Quantity shown = *quantity;
                if (args.size() > 1) {
                    std::string const* const unit = args.at(1).asString();
                    auto converted = unit == nullptr
                                         ? std::nullopt
                                         : convertUnit(quantity->value, quantity->unit, *unit, context.units);
                    if (!converted) {
                        return mismatch(expr, "the quantity does not convert to that unit");
                    }
                    shown = Quantity{.value = *converted, .unit = *unit};
                }
                auto const text = formatNumber(shown, context.numeric);
                if (!text) {
                    return mismatch(expr, "the result overflows");
                }
                return *text + " " + unitDisplay(shown.unit, context.units);
            }
            case FunctionId::Money: {
                auto const number = numeric(args.at(0));
                std::string const* const currency = args.at(1).asString();
                if (!number || number->unit != nullptr || currency == nullptr) {
                    return mismatch(expr, "`money` takes an amount and a currency code");
                }
                auto const text = decimalText(number->value, currencyDigits(*currency));
                if (!text) {
                    return mismatch(expr, "the result overflows");
                }
                return render::formatCanonicalNumber(*text, context.numeric) + " " + *currency;
            }
            case FunctionId::Date:
            case FunctionId::DateTime: {
                std::optional<time::DateTime> instant;
                if (time::DateTime const* const held = args.at(0).asTimestamp()) {
                    instant = *held;
                } else if (std::string const* const text = args.at(0).asString()) {
                    instant = time::DateTime::fromIso8601(*text);
                }
                std::string const* const style = args.size() > 1 ? args.at(1).asString() : nullptr;
                std::optional<std::string> text;
                if (instant && (args.size() == 1 || style != nullptr)) {
                    text = dateText(*instant, style == nullptr ? std::string_view{"medium"} : *style,
                                    expr.function == FunctionId::DateTime);
                }
                if (!text) {
                    return mismatch(expr, "the date functions take a timestamp and a style");
                }
                return std::move(*text);
            }
            case FunctionId::ParseDecimal: {
                std::string const* const text = args.at(0).asString();
                std::optional<std::uint32_t> declared;
                if (args.size() > 1) {
                    std::int64_t const* const places = args.at(1).asInt();
                    if (places == nullptr || !std::in_range<std::uint32_t>(*places) ||
                        std::cmp_greater(*places, math::kMaxDecimalPlaces)) {
                        return mismatch(expr, "`parseDecimal` takes text and places from 0 to 18");
                    }
                    declared = static_cast<std::uint32_t>(*places);
                }
                if (text == nullptr) {
                    return mismatch(expr, "`parseDecimal` takes text and places from 0 to 18");
                }
                // Text that does not spell a number is what an entry holds while it is typed: `null`, not an error.
                if (auto parsed = render::parseDecimal(*text, context.numeric, declared)) {
                    return Decimal{*parsed};
                }
                return Value{};
            }
            case FunctionId::ConcatLists: {
                ValueList out;
                for (Value const& arg : args) {
                    ValueList const* const items = arg.asList();
                    if (items == nullptr) {
                        return mismatch(expr, "`concatLists` takes lists");
                    }
                    out.insert(out.end(), items->begin(), items->end());
                }
                return out;
            }
            case FunctionId::Slice:
                return sliced(expr, args);
            case FunctionId::Translate:
                if (args.at(0).asString() == nullptr || args.at(1).asString() == nullptr) {
                    return mismatch(expr, "`t` takes a key and a fallback string");
                }
                return translate(expr, args);
            case FunctionId::And:
            case FunctionId::Or:
            case FunctionId::Coalesce:
            case FunctionId::Engaged:
            default:
                return Value{};
        }
    }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

    Environment const* _env;
    std::vector<std::pair<std::string, Value>> _locals;
    // The mismatches reported so far, so a form can tell that an operand failed rather than gave `null`.
    std::size_t _mismatches = 0;
    // NOLINTEND(misc-no-recursion)
};

}  // namespace detail

/// @brief Evaluates an expression.
///
/// Total: it always returns. A function whose required argument is `null` yields `null`; `and`, `or` and `not` are
/// three-valued; a type mismatch, an overflow, a division by zero or a unit that does not convert yields `null`
/// and is reported through `Environment::report` with the failing node's path.
/// @param expr The expression.
/// @param env  What it reads.
/// @return Its value.
[[nodiscard]] inline Value evaluate(Expr const& expr, Environment const& env) {
    return detail::Evaluator{env}.eval(expr);
}

/// @brief An environment that holds its values: for tests, and for evaluating outside a mounted screen.
///
/// Each runtime error is recorded once per expression path in `diagnostics`.
class StaticEnvironment final : public Environment {
public:
    /// @brief The values `ref` reads, by head name.
    ValueRecord names;
    /// @brief The innermost row.
    Value rowValue;
    /// @brief Query values, by name.
    ValueRecord queries;
    /// @brief Mutation results, by name.
    ValueRecord results;
    /// @brief Error texts, by query or mutation name.
    ValueRecord errors;
    /// @brief Error kinds, by query or mutation name.
    ValueRecord errorKinds;
    /// @brief The queries and mutations in flight.
    std::set<std::string, std::less<>> pendingNames;
    /// @brief What the principal may run.
    std::set<std::string, std::less<>> permitted;
    /// @brief Customization choices, by point.
    ValueRecord prefs;
    /// @brief `env` values.
    ValueRecord environment;
    /// @brief `session` values.
    ValueRecord session;
    /// @brief `host` values.
    ValueRecord host;
    /// @brief The formatting facts.
    FormatContext format;
    /// @brief The runtime errors reported, each path once.
    mutable std::vector<Refusal> diagnostics;

    /// @brief Reads a name and walks its path.
    /// @param head The name.
    /// @param path The path.
    /// @return The value, or `null`.
    [[nodiscard]] Value ref(std::string_view head, std::span<std::string const> path) const override {
        return lookup(names, head, path);
    }

    /// @brief Walks the row.
    /// @param path The path.
    /// @return The value, or `null`.
    [[nodiscard]] Value row(std::span<std::string const> path) const override { return walk(rowValue, path); }

    /// @brief Reads a control form from the maps.
    /// @param form The form.
    /// @param name The query or mutation.
    /// @return The value.
    [[nodiscard]] Value control(ExprKind form, std::string_view name) const override {
        if (form == ExprKind::Pending) {
            return pendingNames.contains(name);
        }
        if (form == ExprKind::Can) {
            return permitted.contains(name);
        }
        if (form == ExprKind::Error) {
            return lookup(errors, name, {});
        }
        if (form == ExprKind::ErrorKind) {
            return lookup(errorKinds, name, {});
        }
        return lookup(form == ExprKind::Result ? results : queries, name, {});
    }

    /// @brief Reads a customization choice.
    /// @param point The point.
    /// @param path  The path.
    /// @return The value, or `null`.
    [[nodiscard]] Value pref(std::string_view point, std::span<std::string const> path) const override {
        return lookup(prefs, point, path);
    }

    /// @brief Reads `env`, `session` or `host`.
    /// @param form The form.
    /// @param name The name.
    /// @return The value, or `null`.
    [[nodiscard]] Value client(ExprKind form, std::string_view name) const override {
        if (form == ExprKind::Env) {
            return lookup(environment, name, {});
        }
        return lookup(form == ExprKind::Session ? session : host, name, {});
    }

    /// @brief The formatting facts.
    /// @return `format`.
    [[nodiscard]] FormatContext const& formatting() const override { return format; }

    /// @brief Records a runtime error, once per path.
    /// @param path   The expression node's path.
    /// @param reason What went wrong.
    void report(std::string_view path, std::string_view reason) const override {
        for (Refusal const& seen : diagnostics) {
            if (seen.path == path) {
                return;
            }
        }
        diagnostics.push_back(Refusal{.document = {}, .path = std::string{path}, .reason = std::string{reason}});
    }

private:
    [[nodiscard]] static Value lookup(ValueRecord const& values, std::string_view name,
                                      std::span<std::string const> path) {
        auto const found = values.find(name);
        return found == values.end() ? Value{} : walk(found->second, path);
    }
};

}  // namespace morph::ui
