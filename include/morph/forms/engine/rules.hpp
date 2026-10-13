// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/rules.hpp
/// @brief `x-rules` read into `RuleExpr` trees, translated into the UI document's expression language (`expr/1`)
///        and evaluated by its one evaluator, three-valued and exactly.
///
/// Specified in `docs/spec/forms/engine.md`, "Rules".

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../render/locale_format.hpp"
#include "../../ui/expression.hpp"
#include "../../ui/value.hpp"
#include "../../util/rational.hpp"
#include "detail/text.hpp"

namespace morph::forms {

/// @brief Why a schema does not read as a form: where, and why.
struct SchemaError {
    /// @brief Where in the schema: a dotted path such as `properties.amount` or `x-rules[2].when`.
    std::string path;
    /// @brief Why, in one sentence.
    std::string message;
    /// @brief Field-wise equality.
    /// @param other The error to compare with.
    /// @return Whether both members are equal.
    bool operator==(SchemaError const& other) const = default;
};

/// @brief A three-valued verdict: a half-typed form has fields with no value yet, so a rule can be undecided.
enum class Tri : std::uint8_t {
    False,   ///< Definitely does not hold.
    True,    ///< Definitely holds.
    Unknown  ///< Cannot be decided: an operand does not encode, or the kind is not one this engine knows.
};

/// @brief Kleene negation.
/// @param value The verdict.
/// @return `True` for `False`, `False` for `True`, `Unknown` for `Unknown`.
[[nodiscard]] constexpr Tri triNot(Tri value) noexcept {
    switch (value) {
        case Tri::False:
            return Tri::True;
        case Tri::True:
            return Tri::False;
        case Tri::Unknown:
        default:
            return Tri::Unknown;
    }
}

/// @brief What a rule reads of one field.
struct RuleOperand {
    /// @brief Whether the field has a value a rule can use.
    enum class State : std::uint8_t {
        Blank,    ///< Nothing entered.
        Invalid,  ///< Something entered that does not encode.
        Valid     ///< Entered and encodes; `value` holds the exact value.
    };
    /// @brief The state.
    State state = State::Blank;
    /// @brief The encoded value while `Valid`: an `int`, `decimal`, `bool`, `string` or `timestamp`; `null`
    ///        otherwise, and for a value the expression language cannot hold exactly.
    ui::Value value{};
};

/// @brief Reads one field by wire name, for a rule. The session's lookup is tracked, so a verdict computed inside a
///        reactive computation recomputes when a field it read changes.
using RuleLookup = std::function<RuleOperand(std::string_view field)>;

namespace detail::engine {

/// @brief The operands of one rule tree: each field it names, numbered in first-seen order.
class RuleOperands {
public:
    /// @brief The number of a field, adding it when it is new.
    /// @param field The wire name.
    /// @return Its index.
    std::size_t indexOf(std::string const& field) {
        auto const found = std::ranges::find(_fields, field);
        if (found != _fields.end()) {
            return static_cast<std::size_t>(found - _fields.begin());
        }
        _fields.push_back(field);
        return _fields.size() - 1;
    }

    /// @brief The fields, by index.
    /// @return The fields.
    [[nodiscard]] std::vector<std::string> take() && { return std::move(_fields); }

private:
    std::vector<std::string> _fields;
};

/// @brief A string node.
/// @param value The text.
/// @return The node.
[[nodiscard]] inline Json jsonText(std::string value) {
    Json node{};
    node.data = std::move(value);
    return node;
}

/// @brief `{"fn": name, "args": [...]}`.
/// @param name The function.
/// @param args Its arguments.
/// @return The call's JSON.
[[nodiscard]] inline Json call(std::string_view name, std::vector<Json> args) {
    Json node = emptyObject();
    node.get_object().insert_or_assign("fn", jsonText(std::string{name}));
    Json list{};
    list.data.emplace<Json::array_t>(std::move(args));
    node.get_object().insert_or_assign("args", std::move(list));
    return node;
}

/// @brief A one-key object `{key: value}`.
/// @param key   The key.
/// @param value The value.
/// @return The object.
[[nodiscard]] inline Json single(std::string_view key, Json value) {
    Json node = emptyObject();
    node.get_object().insert_or_assign(std::string{key}, std::move(value));
    return node;
}

/// @brief `{"if": [condition, then, otherwise]}`.
/// @param condition The condition.
/// @param then      The value when it is true.
/// @param otherwise The value when it is false.
/// @return The expression.
[[nodiscard]] inline Json choose(Json condition, Json then, Json otherwise) {
    Json list{};
    list.data.emplace<Json::array_t>(Json::array_t{std::move(condition), std::move(then), std::move(otherwise)});
    return single("if", std::move(list));
}

/// @brief A boolean literal.
/// @param flag The value.
/// @return The literal.
[[nodiscard]] inline Json literal(bool flag) {
    Json node{};
    node.data = flag;
    return node;
}

/// @brief The `null` literal: an undecided verdict.
/// @return The literal.
[[nodiscard]] inline Json nullLiteral() {
    Json node{};
    node.data = nullptr;
    return node;
}

/// @brief An integer literal.
/// @param number The value.
/// @return The literal.
[[nodiscard]] inline Json integer(std::int64_t number) {
    Json node{};
    node.data = number;
    return node;
}

}  // namespace detail::engine

/// @brief One `x-rules` entry, or a condition inside one, and its translation into `expr/1`.
///
/// The 16 kinds the emitters write are read; any other kind parses and is `Unknown`, so an older client never
/// refuses a newer server's form. A rule's operands are numbered `f0`, `f1`, … in its translation, and each
/// operand reads as two names: `fN.engaged` (`true`, `false`, or `null` when its draft does not encode) and
/// `fN.value` (the encoded value, `null` unless it is valid). The two are needed because a blank operand and an
/// invalid one are both "no value" to the expression language, and the rules tell them apart: a comparison with a
/// blank operand holds vacuously, one with an invalid operand is `Unknown`.
// NOLINTNEXTLINE(misc-no-recursion): a rule's conditions are rules, so copying one copies them.
class RuleExpr {
public:
    /// @brief The rule kinds.
    enum class Kind : std::uint8_t {
        Engaged,            ///< `engaged`: the field has a value.
        NotEngaged,         ///< `notEngaged`.
        Equals,             ///< `equals`: the field equals the literal `value` (`valueText` for wide integers).
        Greater,            ///< `greater`: the first field is greater than the second, when both are engaged.
        GreaterOrEqual,     ///< `greaterOrEqual`.
        Less,               ///< `less`.
        LessOrEqual,        ///< `lessOrEqual`.
        RequiredWhen,       ///< `requiredWhen`: the fields are required while `when` holds.
        ExactlyOneOf,       ///< `exactlyOneOf`: exactly one field is engaged.
        AtLeastOneOf,       ///< `atLeastOneOf`.
        MutuallyExclusive,  ///< `mutuallyExclusive`: at most one field is engaged.
        VisibleWhen,        ///< `visibleWhen`: presentation; the fields are shown while `when` holds.
        ReadonlyWhen,       ///< `readonlyWhen`: presentation; the fields are frozen while `when` holds.
        And,                ///< `and` over `conditions`.
        Or,                 ///< `or` over `conditions`.
        Not,                ///< `not` of `condition`.
        Unknown             ///< A kind this engine does not know.
    };

    /// @brief Reads one rule or condition node and translates it.
    /// @param node The node.
    /// @param path Where it is, for errors.
    /// @return The rule, or why it does not read: a node that is not an object, a missing `kind`, a malformed
    ///         `fields`, a known kind without the operands it needs, or a translation larger than `expr/1` admits.
    [[nodiscard]] static std::expected<RuleExpr, SchemaError> parse(detail::engine::Json const& node,
                                                                    std::string const& path) {
        auto parsed = parseNode(node, path);
        if (!parsed) {
            return parsed;
        }
        detail::engine::RuleOperands operands;
        detail::engine::Json const translation = parsed->verdictExpression(operands);
        auto compiled = ui::parseExpression(translation);
        if (!compiled) {
            return std::unexpected(
                SchemaError{.path = path,
                            .message = "the rule does not translate into an expression: " + compiled.error().reason});
        }
        parsed->_expression = translation;
        parsed->_compiled = *std::move(compiled);
        parsed->_operands = std::move(operands).take();
        return parsed;
    }

    /// @brief The kind.
    /// @return The kind; `Unknown` for a kind this engine does not know.
    [[nodiscard]] Kind kind() const noexcept { return _kind; }

    /// @brief The kind as the schema spells it.
    /// @return The `kind` string.
    [[nodiscard]] std::string_view kindName() const noexcept { return _kindName; }

    /// @brief The fields this node names directly (`fields`).
    /// @return The wire names.
    [[nodiscard]] std::span<std::string const> fields() const noexcept { return _fields; }

    /// @brief Whether this node names @p field directly. A presentation rule applies to every field it names.
    /// @param field The wire name.
    /// @return Whether it is among `fields()`.
    [[nodiscard]] bool names(std::string_view field) const noexcept {
        return std::ranges::find(_fields, field) != _fields.end();
    }

    /// @brief The `when` clause of `requiredWhen`, `visibleWhen` and `readonlyWhen`.
    /// @return The clause, or null for any other kind.
    [[nodiscard]] RuleExpr const* when() const noexcept {
        bool const hasWhen = _kind == Kind::RequiredWhen || _kind == Kind::VisibleWhen || _kind == Kind::ReadonlyWhen;
        return hasWhen && !_children.empty() ? &_children.front() : nullptr;
    }

    /// @brief The operands of `and` and `or`, or the one operand of `not`.
    /// @return The child conditions; empty for any other kind.
    [[nodiscard]] std::span<RuleExpr const> conditions() const noexcept {
        bool const composite = _kind == Kind::And || _kind == Kind::Or || _kind == Kind::Not;
        return composite ? std::span<RuleExpr const>{_children} : std::span<RuleExpr const>{};
    }

    /// @brief Whether the rule decides readiness. `visibleWhen` and `readonlyWhen` only present.
    /// @return `false` for the presentation kinds.
    [[nodiscard]] bool gates() const noexcept { return _kind != Kind::VisibleWhen && _kind != Kind::ReadonlyWhen; }

    /// @brief Whether the rule is a presentation rule of @p kind that names @p field.
    /// @param kind  `RequiredWhen`, `VisibleWhen` or `ReadonlyWhen`.
    /// @param field The wire name.
    /// @return Whether it applies to the field's presentation.
    [[nodiscard]] bool presents(Kind kind, std::string_view field) const noexcept {
        return _kind == kind && names(field);
    }

    /// @brief The translation, as `expr/1` JSON: for a gating rule its test, for a presentation rule its `when`
    ///        clause. Set on a node `parse` returned; a child node holds none.
    /// @return The expression's JSON.
    [[nodiscard]] detail::engine::Json const& expression() const noexcept { return _expression; }

    /// @brief The wire names the translation reads, by operand index (`f0` is the first).
    /// @return The operands.
    [[nodiscard]] std::span<std::string const> operands() const noexcept { return _operands; }

    /// @brief Evaluates the translation with the UI document's evaluator.
    ///
    /// For a gating rule this is its test; for `visibleWhen`, `readonlyWhen` and the presentation side of
    /// `requiredWhen` use `whenHolds`.
    /// @param lookup Reads each operand.
    /// @return The verdict: `null` is `Unknown`.
    [[nodiscard]] Tri evaluate(RuleLookup const& lookup) const {
        if (_compiled == nullptr) {
            return Tri::Unknown;
        }
        Environment const env{_operands, lookup};
        return verdictOf(ui::evaluate(*_compiled, env));
    }

    /// @brief Evaluates the `when` clause of a `requiredWhen`, `visibleWhen` or `readonlyWhen`.
    /// @param lookup Reads each operand.
    /// @return The clause's verdict; `Unknown` for any other kind.
    [[nodiscard]] Tri whenHolds(RuleLookup const& lookup) const {
        if (_whenCompiled == nullptr) {
            return Tri::Unknown;
        }
        Environment const env{_operands, lookup};
        return verdictOf(ui::evaluate(*_whenCompiled, env));
    }

private:
    /// @brief Answers `fN.engaged` and `fN.value` from a lookup; every other name reads `null`.
    class Environment final : public ui::Environment {
    public:
        Environment(std::span<std::string const> operands, RuleLookup const& lookup) noexcept
            : _operands{operands}, _lookup{&lookup} {}

        [[nodiscard]] ui::Value ref(std::string_view head, std::span<std::string const> path) const override {
            if (head.size() < 2 || head.front() != 'f' || path.size() != 1) {
                return {};
            }
            std::size_t index = 0;
            for (char const chr : head.substr(1)) {
                if (chr < '0' || chr > '9' || index > (std::numeric_limits<std::size_t>::max() / 10U) - 1U) {
                    return {};
                }
                index = (index * 10U) + static_cast<std::size_t>(chr - '0');
            }
            if (index >= _operands.size()) {
                return {};
            }
            RuleOperand const operand = (*_lookup)(*std::next(_operands.begin(), static_cast<std::ptrdiff_t>(index)));
            if (path.front() == "engaged") {
                switch (operand.state) {
                    case RuleOperand::State::Blank:
                        return false;
                    case RuleOperand::State::Valid:
                        return true;
                    case RuleOperand::State::Invalid:
                    default:
                        return {};
                }
            }
            if (path.front() == "value" && operand.state == RuleOperand::State::Valid) {
                return operand.value;
            }
            return {};
        }

        [[nodiscard]] ui::Value row(std::span<std::string const> path) const override {
            static_cast<void>(path);
            return {};
        }

        [[nodiscard]] ui::Value control(ui::ExprKind form, std::string_view name) const override {
            static_cast<void>(form);
            static_cast<void>(name);
            return {};
        }

        [[nodiscard]] ui::Value pref(std::string_view point, std::span<std::string const> path) const override {
            static_cast<void>(point);
            static_cast<void>(path);
            return {};
        }

        [[nodiscard]] ui::Value client(ui::ExprKind form, std::string_view name) const override {
            static_cast<void>(form);
            static_cast<void>(name);
            return {};
        }

        [[nodiscard]] ui::FormatContext const& formatting() const override {
            static ui::FormatContext const kContext{};
            return kContext;
        }

        // A comparison of two kinds that do not order (a string field against a number field) is `null`, which the
        // verdict already reads as `Unknown`; there is nothing further to report.
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the evaluator's interface.
        void report(std::string_view path, std::string_view reason) const override {
            static_cast<void>(path);
            static_cast<void>(reason);
        }

    private:
        std::span<std::string const> _operands;
        RuleLookup const* _lookup;
    };

    [[nodiscard]] static Tri verdictOf(ui::Value const& value) noexcept {
        bool const* const flag = value.asBool();
        if (flag == nullptr) {
            return Tri::Unknown;
        }
        return *flag ? Tri::True : Tri::False;
    }

    // A condition tree is read by the same function, node by node; one case per rule kind.
    // NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity)
    [[nodiscard]] static std::expected<RuleExpr, SchemaError> parseNode(detail::engine::Json const& node,
                                                                        std::string const& path) {
        using detail::engine::Json;
        if (!node.is_object()) {
            return std::unexpected(SchemaError{.path = path, .message = "a rule must be a JSON object"});
        }
        auto const* const kind = detail::engine::stringAt(node, "kind");
        if (kind == nullptr) {
            return std::unexpected(SchemaError{.path = path, .message = "a rule needs a string \"kind\""});
        }
        RuleExpr expr{};
        expr._kindName = *kind;
        expr._kind = kindOf(*kind);
        if (Json const* const fields = detail::engine::member(node, "fields")) {
            if (!fields->is_array()) {
                return std::unexpected(SchemaError{.path = path, .message = "\"fields\" must be an array of strings"});
            }
            for (Json const& name : fields->get_array()) {
                auto const* const text = name.get_if<std::string>();
                if (text == nullptr) {
                    return std::unexpected(
                        SchemaError{.path = path, .message = "\"fields\" must be an array of strings"});
                }
                expr._fields.push_back(*text);
            }
        }
        // NOLINTNEXTLINE(misc-no-recursion): a child is a condition tree read by parseNode.
        auto const child = [&](std::string_view key) -> std::expected<void, SchemaError> {
            Json const* const nested = detail::engine::member(node, key);
            if (nested == nullptr) {
                return std::unexpected(
                    SchemaError{.path = path, .message = expr._kindName + " needs \"" + std::string{key} + "\""});
            }
            auto parsed = parseNode(*nested, path + "." + std::string{key});
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            expr._children.push_back(*std::move(parsed));
            return {};
        };
        auto const needFields = [&](std::size_t count, bool exact) -> std::expected<void, SchemaError> {
            if (exact ? expr._fields.size() != count : expr._fields.size() < count) {
                return std::unexpected(SchemaError{.path = path,
                                                   .message = "\"" + expr._kindName + "\" needs " +
                                                              (exact ? "exactly " : "at least ") +
                                                              std::to_string(count) + " field(s)"});
            }
            return {};
        };
        std::expected<void, SchemaError> shape{};
        switch (expr._kind) {
            case Kind::RequiredWhen:
            case Kind::VisibleWhen:
            case Kind::ReadonlyWhen:
                shape = needFields(1, false);
                if (shape) {
                    shape = child("when");
                }
                break;
            case Kind::Not:
                shape = child("condition");
                break;
            case Kind::And:
            case Kind::Or: {
                Json const* const list = detail::engine::member(node, "conditions");
                if (list == nullptr || !list->is_array()) {
                    shape = std::unexpected(SchemaError{
                        .path = path, .message = "\"" + expr._kindName + R"(" needs a "conditions" array)"});
                    break;
                }
                std::size_t index = 0;
                for (Json const& nested : list->get_array()) {
                    auto parsed = parseNode(nested, path + ".conditions[" + std::to_string(index++) + "]");
                    if (!parsed) {
                        return std::unexpected(parsed.error());
                    }
                    expr._children.push_back(*std::move(parsed));
                }
                break;
            }
            case Kind::Greater:
            case Kind::GreaterOrEqual:
            case Kind::Less:
            case Kind::LessOrEqual:
                shape = needFields(2, true);
                break;
            case Kind::Equals:
                shape = needFields(1, false);
                expr._literal = literalOf(node);
                break;
            case Kind::Engaged:
            case Kind::NotEngaged:
            case Kind::ExactlyOneOf:
            case Kind::AtLeastOneOf:
            case Kind::MutuallyExclusive:
                shape = needFields(1, false);
                break;
            case Kind::Unknown:
            default:
                break;
        }
        if (!shape) {
            return std::unexpected(shape.error());
        }
        return expr;
    }

    [[nodiscard]] static Kind kindOf(std::string_view name) noexcept {
        constexpr std::array<std::pair<std::string_view, Kind>, 16> kNames{{
            {"engaged", Kind::Engaged},
            {"notEngaged", Kind::NotEngaged},
            {"equals", Kind::Equals},
            {"greater", Kind::Greater},
            {"greaterOrEqual", Kind::GreaterOrEqual},
            {"less", Kind::Less},
            {"lessOrEqual", Kind::LessOrEqual},
            {"requiredWhen", Kind::RequiredWhen},
            {"exactlyOneOf", Kind::ExactlyOneOf},
            {"atLeastOneOf", Kind::AtLeastOneOf},
            {"mutuallyExclusive", Kind::MutuallyExclusive},
            {"visibleWhen", Kind::VisibleWhen},
            {"readonlyWhen", Kind::ReadonlyWhen},
            {"and", Kind::And},
            {"or", Kind::Or},
            {"not", Kind::Not},
        }};
        const auto* const found = std::ranges::find(kNames, name, &std::pair<std::string_view, Kind>::first);
        return found == kNames.end() ? Kind::Unknown : found->second;
    }

    /// The literal `equals` compares against, as an `expr/1` literal: a bool, a string, an `int` (from `valueText`
    /// when the emitter wrote the digits, so nothing passes through a double), or a decimal. A literal the language
    /// cannot hold exactly (an integer beyond int64) is `null`, so the comparison is `Unknown`.
    [[nodiscard]] static detail::engine::Json literalOf(detail::engine::Json const& node) {
        using detail::engine::Json;
        auto const wide = [](std::string_view digits) -> Json {
            auto const parsed = render::parseDecimal(digits, render::NumericLocale{});
            if (!parsed || !parsed->isInteger()) {
                return detail::engine::nullLiteral();
            }
            return detail::engine::integer(parsed->numerator);
        };
        if (auto const* const digits = detail::engine::stringAt(node, "valueText");
            digits != nullptr && detail::engine::isCanonicalInteger(*digits)) {
            return wide(*digits);
        }
        Json const* const value = detail::engine::member(node, "value");
        if (value == nullptr) {
            return detail::engine::nullLiteral();
        }
        if (auto const* const flag = value->get_if<bool>()) {
            return detail::engine::literal(*flag);
        }
        if (auto const* const text = value->get_if<std::string>()) {
            return detail::engine::jsonText(*text);
        }
        if (value->get_if<std::int64_t>() != nullptr || value->get_if<std::uint64_t>() != nullptr) {
            return wide(detail::engine::numberText(*value).value_or(std::string{}));
        }
        std::optional<math::Rational> exact;
        if (auto const number = detail::engine::numberText(*value)) {
            exact = render::parseDecimal(*number, render::NumericLocale{});
        } else if (value->is_object()) {
            auto const decoded = ui::valueFromJson(*value);
            if (decoded && decoded->asDecimal() != nullptr) {
                exact = decoded->asDecimal()->value;
            }
        }
        if (!exact) {
            return detail::engine::nullLiteral();
        }
        Json decimal = detail::engine::emptyObject();
        decimal.get_object().insert_or_assign("num", detail::engine::integer(exact->numerator));
        decimal.get_object().insert_or_assign("den", detail::engine::integer(exact->denominator));
        decimal.get_object().insert_or_assign(
            "dp", detail::engine::integer(static_cast<std::int64_t>(exact->decimalPlaces.value)));
        return detail::engine::single("lit", std::move(decimal));
    }

    [[nodiscard]] static detail::engine::Json engagedRef(detail::engine::RuleOperands& operands,
                                                         std::string const& field) {
        return detail::engine::single(
            "ref", detail::engine::jsonText("f" + std::to_string(operands.indexOf(field)) + ".engaged"));
    }

    [[nodiscard]] static detail::engine::Json valueRef(detail::engine::RuleOperands& operands,
                                                       std::string const& field) {
        return detail::engine::single(
            "ref", detail::engine::jsonText("f" + std::to_string(operands.indexOf(field)) + ".value"));
    }

    /// The rule's whole verdict: a gating rule's test, a presentation rule's `when` clause.
    [[nodiscard]] detail::engine::Json verdictExpression(detail::engine::RuleOperands& operands) {
        using detail::engine::call;
        using detail::engine::choose;
        using detail::engine::Json;
        if (_kind == Kind::RequiredWhen) {
            Json const condition = _children.front().holds(operands);
            std::vector<Json> engaged;
            engaged.reserve(_fields.size());
            for (std::string const& field : _fields) {
                engaged.push_back(engagedRef(operands, field));
            }
            compileWhen(condition);
            // Not required unless the condition is definitely true: an undecided condition does not demand
            // the fields.
            return choose(call("coalesce", {condition, detail::engine::literal(false)}),
                          call("and", std::move(engaged)), detail::engine::literal(true));
        }
        if (_kind == Kind::VisibleWhen || _kind == Kind::ReadonlyWhen) {
            Json condition = _children.front().holds(operands);
            compileWhen(condition);
            return condition;
        }
        return holds(operands);
    }

    void compileWhen(detail::engine::Json const& condition) {
        if (auto compiled = ui::parseExpression(condition)) {
            _whenCompiled = *std::move(compiled);
        }
    }

    /// Whether the node holds, as a condition. Presentation kinds and unknown kinds are not conditions: `null`.
    // One case per kind; and, or and not translate their operands with the same function.
    // NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity)
    [[nodiscard]] detail::engine::Json holds(detail::engine::RuleOperands& operands) const {
        using detail::engine::call;
        using detail::engine::choose;
        using detail::engine::integer;
        using detail::engine::Json;
        using detail::engine::literal;
        using detail::engine::single;
        switch (_kind) {
            case Kind::Engaged:
                return engagedRef(operands, _fields.front());
            case Kind::NotEngaged:
                return call("not", {engagedRef(operands, _fields.front())});
            case Kind::Equals:
                // Blank is false, invalid is unknown (the `if` of a null condition), valid compares.
                return choose(engagedRef(operands, _fields.front()),
                              call("eq", {valueRef(operands, _fields.front()), _literal}), literal(false));
            case Kind::Greater:
            case Kind::GreaterOrEqual:
            case Kind::Less:
            case Kind::LessOrEqual: {
                constexpr std::array<std::string_view, 4> kOps = {"gt", "ge", "lt", "le"};
                auto const op = kOps.at(static_cast<std::size_t>(_kind) - static_cast<std::size_t>(Kind::Greater));
                std::string const& lhs = _fields.front();
                std::string const& rhs = _fields.back();
                // A blank operand makes the comparison hold vacuously; an invalid one leaves it undecided.
                return choose(call("and", {engagedRef(operands, lhs), engagedRef(operands, rhs)}),
                              call(op, {valueRef(operands, lhs), valueRef(operands, rhs)}), literal(true));
            }
            case Kind::ExactlyOneOf:
            case Kind::AtLeastOneOf:
            case Kind::MutuallyExclusive: {
                std::vector<Json> engaged;
                engaged.reserve(_fields.size());
                for (std::string const& field : _fields) {
                    engaged.push_back(engagedRef(operands, field));
                }
                Json list{};
                list.data.emplace<Json::array_t>(std::move(engaged));
                Json const all = single("list", list);
                auto const counted = [&](Json where) {
                    Json comprehension = detail::engine::emptyObject();
                    comprehension.get_object().insert_or_assign("filter", all);
                    comprehension.get_object().insert_or_assign("as", detail::engine::jsonText("x"));
                    comprehension.get_object().insert_or_assign("where", std::move(where));
                    return call("count", {std::move(comprehension)});
                };
                Json const x = single("ref", detail::engine::jsonText("x"));
                Json const definite = counted(x);
                Json const open = counted(call("not", {call("engaged", {x})}));
                Json const undecided = detail::engine::nullLiteral();
                if (_kind == Kind::ExactlyOneOf) {
                    return choose(
                        call("ge", {definite, integer(2)}), literal(false),
                        choose(call("eq", {open, integer(0)}), call("eq", {definite, integer(1)}), undecided));
                }
                if (_kind == Kind::AtLeastOneOf) {
                    return choose(call("ge", {definite, integer(1)}), literal(true),
                                  choose(call("eq", {open, integer(0)}), literal(false), undecided));
                }
                return choose(
                    call("ge", {definite, integer(2)}), literal(false),
                    choose(call("le", {call("add", {definite, open}), integer(1)}), literal(true), undecided));
            }
            case Kind::And:
            case Kind::Or: {
                if (_children.empty()) {
                    return literal(_kind == Kind::And);
                }
                std::vector<Json> parts;
                parts.reserve(_children.size());
                for (RuleExpr const& operand : _children) {
                    parts.push_back(operand.holds(operands));
                }
                return call(_kind == Kind::And ? "and" : "or", std::move(parts));
            }
            case Kind::Not:
                return call("not", {_children.front().holds(operands)});
            case Kind::RequiredWhen:
            case Kind::VisibleWhen:
            case Kind::ReadonlyWhen:
            case Kind::Unknown:
            default:
                return detail::engine::nullLiteral();
        }
    }

    Kind _kind = Kind::Unknown;
    std::string _kindName;
    std::vector<std::string> _fields;
    std::vector<RuleExpr> _children;
    detail::engine::Json _literal;
    detail::engine::Json _expression;
    std::vector<std::string> _operands;
    ui::ExprPtr _compiled;
    ui::ExprPtr _whenCompiled;
};

/// @brief Whether the gating rules let the form submit.
///
/// Only a `False` rule blocks: an `Unknown` one is waiting on a field that is required or does not encode, and that
/// field blocks readiness itself; and a rule kind this engine does not know must not lock an older client out of a
/// newer server's forms.
/// @param rules  The rules.
/// @param lookup Reads each operand.
/// @return `False` when some gating rule is false, `Unknown` when none is false and some is undecided, else `True`.
[[nodiscard]] inline Tri gatingVerdict(std::span<RuleExpr const> rules, RuleLookup const& lookup) {
    Tri verdict = Tri::True;
    for (RuleExpr const& entry : rules) {
        if (!entry.gates()) {
            continue;
        }
        Tri const one = entry.evaluate(lookup);
        if (one == Tri::False) {
            return Tri::False;
        }
        if (one == Tri::Unknown) {
            verdict = Tri::Unknown;
        }
    }
    return verdict;
}

namespace detail::engine {

/// @brief Combines every presentation rule of @p kind that names @p field.
/// @param rules    The rules.
/// @param kind     The presentation kind.
/// @param field    The wire name.
/// @param lookup   Reads each operand.
/// @param decisive The `when` verdict that decides the presentation as soon as one rule has it.
/// @param absent   The verdict when no rule names the field.
/// @return `decisive` when some rule's clause has it, `Unknown` when some is undecided, otherwise its negation.
[[nodiscard]] inline Tri presentation(std::span<RuleExpr const> rules, RuleExpr::Kind kind, std::string_view field,
                                      RuleLookup const& lookup, Tri decisive, Tri absent) {
    bool named = false;
    bool unknown = false;
    for (RuleExpr const& entry : rules) {
        if (!entry.presents(kind, field)) {
            continue;
        }
        named = true;
        Tri const when = entry.whenHolds(lookup);
        if (when == decisive) {
            return decisive;
        }
        unknown = unknown || when == Tri::Unknown;
    }
    if (!named) {
        return absent;
    }
    return unknown ? Tri::Unknown : triNot(decisive);
}

}  // namespace detail::engine

/// @brief Whether @p field is shown: hidden as soon as one `visibleWhen` naming it is false.
/// @param rules  The rules.
/// @param field  The wire name.
/// @param lookup Reads each operand.
/// @return `False` when hidden; `Unknown` presents as shown.
[[nodiscard]] inline Tri visibleVerdict(std::span<RuleExpr const> rules, std::string_view field,
                                        RuleLookup const& lookup) {
    return detail::engine::presentation(rules, RuleExpr::Kind::VisibleWhen, field, lookup, Tri::False, Tri::True);
}

/// @brief Whether @p field is frozen: as soon as one `readonlyWhen` naming it is true.
/// @param rules  The rules.
/// @param field  The wire name.
/// @param lookup Reads each operand.
/// @return `True` when frozen; `Unknown` presents as editable.
[[nodiscard]] inline Tri readonlyVerdict(std::span<RuleExpr const> rules, std::string_view field,
                                         RuleLookup const& lookup) {
    return detail::engine::presentation(rules, RuleExpr::Kind::ReadonlyWhen, field, lookup, Tri::True, Tri::False);
}

/// @brief Whether @p field is required by a rule: as soon as one `requiredWhen` naming it is true.
/// @param rules  The rules.
/// @param field  The wire name.
/// @param lookup Reads each operand.
/// @return `True` when required; `Unknown` presents as not required.
[[nodiscard]] inline Tri requiredVerdict(std::span<RuleExpr const> rules, std::string_view field,
                                         RuleLookup const& lookup) {
    return detail::engine::presentation(rules, RuleExpr::Kind::RequiredWhen, field, lookup, Tri::True, Tri::False);
}

}  // namespace morph::forms
