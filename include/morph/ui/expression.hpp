// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/export.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../render/i18n.hpp"
#include "../render/locale_format.hpp"
#include "../util/datetime.hpp"
#include "../util/rational.hpp"
#include "locale_table.hpp"
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
    Refusal,    ///< `{"refusal": "m"}`: a query's or mutation's action refusal, `null` or `{code, args}`.
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
    Abs,
    Sqrt,
    Stdev,
    Rsd,
    DateAdd,
    DateRange,
    RefusalText,
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
[[nodiscard]] MORPH_API Value runRecord(Value key, ValueList items);

namespace detail {

/// @brief Splits `head.a.b` into its segments.
/// @param text The dotted path.
/// @return The segments, or the reason the path is malformed.
[[nodiscard]] MORPH_API std::expected<std::vector<std::string>, std::string> splitPath(std::string_view text);

}  // namespace detail

/// @brief Parses one expression.
///
/// Every form of spec 5 §4 is read; an unknown form, a key that does not belong to its form, a malformed name, an
/// unknown function or wrong arity, a number with a fraction, or more than `kMaxExpressionNodes` nodes is refused.
/// @param json The expression's JSON.
/// @param path Its JSON Pointer inside the document, carried by every node and every refusal.
/// @return The expression, or the refusal.
[[nodiscard]] MORPH_API std::expected<ExprPtr, Refusal> parseExpression(Json const& json,
                                                                        std::string const& path = "");

/// @brief The names of the `expr/1` library, in `FunctionId` order.
/// @return The names.
[[nodiscard]] MORPH_API std::vector<std::string_view> functionNames();

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

/// @brief The type a field path reads from a value of a known type.
/// @param type The type of the head.
/// @param head The head's name, for the reason.
/// @param path The path.
/// @return `any` when the path can read something, or why it cannot.
[[nodiscard]] MORPH_API std::expected<Type, std::string> typeAlong(Type const& type, std::string_view head,
                                                                   std::span<std::string const> path);

}  // namespace detail

/// @brief Checks an expression against the names a document declares.
///
/// Every reference is resolved through @p resolver, comprehension variables first; the arguments of a function, the
/// condition of `if` and the `where` of `filter` are checked against their accepted kinds where the argument's type
/// is statically known; a literal `t` fallback must be in the `ui/1` message subset.
/// @param expr     The expression.
/// @param resolver What the document's names resolve to.
/// @return The expression's static type (`any` where it cannot be known), or the refusal.
[[nodiscard]] MORPH_API std::expected<Type, Refusal> checkExpression(Expr const& expr, NameResolver const& resolver);

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
    /// @brief How `money` writes a currency. Empty `symbols` (the default) writes every currency by its code.
    CurrencyFacts currency{};
    /// @brief The units quantities convert through.
    UnitTable units;
    /// @brief The display zone's offset from UTC, in minutes.
    std::int32_t utcOffsetMinutes = 0;
    /// @brief The date formatter; empty uses the patterns below, else ISO 8601 (`2026-10-11`, `2026-10-11 08:30`).
    DateFormatter dates;
    /// @brief The pattern every date is written in, whatever its style (`checkDatePattern`), such as `dd-MMM-yyyy`;
    ///        empty keeps ISO.
    std::string datePattern;
    /// @brief The pattern every date and time is written in, such as `dd-MMM-yyyy HH:mm`; empty keeps ISO.
    std::string dateTimePattern;
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

    /// @brief The facts typed text is read in: the person's own, whatever place the expression is evaluated in. A
    ///        locale scope changes how values are shown, not how a person types, so `parseDecimal` reads these.
    /// @return The context `parseDecimal` uses; by default the display facts.
    [[nodiscard]] virtual FormatContext const& entryFormatting() const { return formatting(); }

    /// @brief Reports a runtime error of an expression.
    /// @param path   The JSON Pointer of the expression node that failed.
    /// @param reason What went wrong.
    virtual void report(std::string_view path, std::string_view reason) const = 0;
};

/// @brief Reads a path out of a value: a record's field by name, a list's element by index.
/// @param value The value.
/// @param path  The segments.
/// @return The value at the path, or `null` when a segment does not exist.
[[nodiscard]] MORPH_API Value walk(Value const& value, std::span<std::string const> path);

/// @brief The form of a value the date functions read.
enum class DateForm : std::uint8_t {
    Absent,       ///< `null` or `""`: a date not given yet, such as a cleared date input.
    Date,         ///< `yyyy-MM-dd` text: a calendar day, with no time of day and no zone.
    Timestamp,    ///< A timestamp: an instant.
    InstantText,  ///< ISO-8601 date-time text that `DateTime::fromIso8601` reads: an instant.
    Unreadable,   ///< Any other value.
};

/// @brief A value as the date functions read it.
struct DateValue {
    /// @brief Its form.
    DateForm form = DateForm::Absent;
    /// @brief The calendar day of a `Date`.
    std::chrono::year_month_day date{};
    /// @brief The instant of a `Timestamp` or `InstantText`.
    time::DateTime instant{};
};

/// @brief Reads a value as a date, an instant or nothing: the one reading the date functions and a date input share.
///
/// A `yyyy-MM-dd` string is a date when its ten characters are digits and dashes and name a day that exists, so
/// `2026-02-30` is unreadable. A timestamp is an instant, and so is text `DateTime::fromIso8601` reads. `null` and
/// `""` are absent.
/// @param value The value.
/// @return The reading.
[[nodiscard]] MORPH_API DateValue readDateValue(Value const& value);

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
[[nodiscard]] MORPH_API std::optional<Number> numeric(Value const& value);

/// @brief Converts an exact value between two units of one dimension.
/// @param value  The value in @p from.
/// @param from   Its unit.
/// @param target The wanted unit.
/// @param units  The unit table.
/// @return The value in @p target, its places widened by the decimal places of the conversion ratio; `std::nullopt`
///         when the units do not convert or the result overflows.
[[nodiscard]] MORPH_API std::optional<math::Rational> convertUnit(math::Rational const& value, std::string_view from,
                                                                  std::string_view target, UnitTable const& units);

/// @brief Orders two values of one comparable kind as the expression language does: numbers by exact value, a
///        quantity converted to the other's unit, timestamps by instant, strings by their bytes, and
///        booleans with `false` before `true`.
/// @param lhs   A value.
/// @param rhs   The other.
/// @param units The unit table quantities convert through.
/// @return The ordering; `std::nullopt` when the two are not of one comparable kind.
[[nodiscard]] MORPH_API std::optional<std::strong_ordering> valueOrder(Value const& lhs, Value const& rhs,
                                                                       UnitTable const& units);

/// @brief Equality of two non-null values as the expression language's `eq` reads it: numbers by value (an `int`
///        equals the equal `decimal`, places do not count, a quantity equals one of another unit that converts to
///        it), lists and records element by element, other kinds by `Value ==`.
/// @param lhs   A value.
/// @param rhs   The other.
/// @param units The unit table quantities convert through.
/// @return Whether they are equal.
[[nodiscard]] MORPH_API bool valuesEqual(Value const& lhs, Value const& rhs, UnitTable const& units);

/// @brief Equality of two elements, as `valuesEqual` where `null` equals only `null`: whether two adjacent rows
///        belong to one run of a `runs` or of a table's `group`.
/// @param left  A value.
/// @param right The other.
/// @param units The unit table quantities convert through.
/// @return Whether they are the same element.
[[nodiscard]] MORPH_API bool sameElement(Value const& left, Value const& right, UnitTable const& units);

/// @brief The display text of a unit.
/// @param unit  The unit id.
/// @param units  The unit table.
/// @return Its display text, or the id.
[[nodiscard]] MORPH_API std::string unitDisplay(std::string const& unit, UnitTable const& units);

/// @brief Evaluates expressions against an environment: the part of the evaluator that other code drives
///        directly, such as grouping and ordering a table's rows. `evaluate` is the entry point for one
///        expression.
class MORPH_API Evaluator {
public:
    /// @brief Evaluates against @p env.
    /// @param env What the expression reads; it must outlive the evaluator.
    explicit Evaluator(Environment const& env);
    Evaluator(Evaluator const&) = delete;
    Evaluator& operator=(Evaluator const&) = delete;
    Evaluator(Evaluator&&) = delete;
    Evaluator& operator=(Evaluator&&) = delete;
    ~Evaluator();

    /// @brief Evaluates one node.
    /// @param expr The node.
    /// @return Its value; `null` on a runtime error, which is reported.
    [[nodiscard]] Value eval(Expr const& expr);

    /// @brief Orders two values as sorting does: as `lt` orders them, except that strings compare case-folded as
    ///        the table engine's default collator folds them, and by their own bytes when they fold the same, so
    ///        the order never depends on the source order.
    /// @param lhs A value, not `null`.
    /// @param rhs A value, not `null`.
    /// @return The ordering; `nullopt` when the two are not of one comparable kind.
    [[nodiscard]] std::optional<std::strong_ordering> sortOrder(Value const& lhs, Value const& rhs);

    /// @brief A stable order of rows by their sort keys: by each key in turn, `null` last in either direction, and
    ///        a descending key reversing the comparison, not the rows, so rows with equal keys keep their order.
    /// @param keys       Each row's keys, most significant first; every row has one per entry of @p descending.
    /// @param descending Whether each key sorts descending.
    /// @return The rows' indices in sorted order; `nullopt` when a key's values are not of one comparable kind.
    [[nodiscard]] std::optional<std::vector<std::size_t>> sortRows(std::vector<std::vector<Value>> const& keys,
                                                                   std::vector<bool> const& descending);

    /// @brief Whether two values belong to one run when adjacent rows are grouped.
    /// @param lhs A value.
    /// @param rhs The other.
    /// @return Whether they are the same element.
    [[nodiscard]] bool sameRun(Value const& lhs, Value const& rhs);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
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
[[nodiscard]] MORPH_API Value evaluate(Expr const& expr, Environment const& env);

/// @brief An environment that holds its values: for tests, and for evaluating outside a mounted screen.
///
/// Each runtime error is recorded once per expression path in `diagnostics`.
class MORPH_API StaticEnvironment final : public Environment {
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
    /// @brief Action refusals, by query or mutation name.
    ValueRecord refusals;
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
    [[nodiscard]] Value control(ExprKind form, std::string_view name) const override;

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
    void report(std::string_view path, std::string_view reason) const override;

private:
    [[nodiscard]] static Value lookup(ValueRecord const& values, std::string_view name,
                                      std::span<std::string const> path) {
        auto const found = values.find(name);
        return found == values.end() ? Value{} : walk(found->second, path);
    }
};

}  // namespace morph::ui
