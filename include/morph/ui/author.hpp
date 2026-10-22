// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <glaze/glaze.hpp>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "../core/changes.hpp"
#include "../core/effects.hpp"
#include "../core/logger.hpp"
#include "../core/model_key.hpp"
#include "../core/registry.hpp"
#include "../detail/fixed_string.hpp"
#include "../detail/reflected_member.hpp"
#include "../forms/engine/field_layout.hpp"
#include "../ui/document.hpp"
#include "../ui/value.hpp"
#include "../util/datetime.hpp"
#include "../util/rational.hpp"
#include "alignment.hpp"
#include "button_emphasis.hpp"
#include "button_style.hpp"

/// @file
/// @brief Server-side authoring of UI documents (spec 5 §11): typed builders that write a screen's JSON, and the
///        registration that validates it once — `loadDocument` with the builders' catalog facts, then the
///        accessibility rules.
///
/// The builders live in `morph::ui::author`, beside the view-tree builders of `morph::ui` that share their names; a
/// screen's source writes `namespace ui = morph::ui::author;`. Specified in `docs/spec/ui/author.md`.

namespace morph::ui::author {

// NOLINTBEGIN(bugprone-easily-swappable-parameters): a builder takes its operands in the order the document writes
// them, and most of them are expressions, which every C++ value converts to; distinct parameter types would make the
// builders unusable rather than safer.

/// @brief The value type of a handle whose value the screen cannot know when it is written: a query result read by
///        name, an element value, an event payload.
struct Any {};

/// @brief What a builder throws when it is given something a document cannot say, such as a field read from a
///        value that has no name. Registration turns it into a `Refusal` at the document's root.
class AuthoringError : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

/// @brief Catalog facts of a model type that its C++ type does not show. Specialise it for a model whose shared
///        singleton is equivalent to a private instance.
/// @tparam Model The model type.
template <typename Model>
struct ModelFacts {
    /// @brief Whether a shared singleton is equivalent to a private instance.
    static constexpr bool stateless = false;
};

/// @brief Catalog facts of an action that its C++ type does not show. Specialise it for an action whose reply rows
///        carry server-calculated fields, whose reply carries a secret, or which sends a credential.
/// @tparam Action The action type.
template <typename Action>
struct ActionFacts {
    /// @brief Whether the action sends a credential.
    static constexpr bool credential = false;

    /// @brief The reply-row fields the server calculates; an `optimistic` write may not name them.
    /// @return The field names.
    [[nodiscard]] static std::vector<std::string> calculatedFields() { return {}; }

    /// @brief The reply fields the server marks `secret`.
    /// @return The field names.
    [[nodiscard]] static std::vector<std::string> secretFields() { return {}; }
};

/// @brief Satisfied when `Model` runs `Action`: the model has an `execute` that takes it.
template <typename Model, typename Action>
concept Runs = requires(Model& model, Action& action) { model.execute(action); };

namespace detail {

/// @brief The members of an action result that hold a list, when the result is a record.
/// @tparam Result The action's result type.
/// @return The names glaze writes them under, or `std::nullopt` when glaze does not write @p Result as a record.
template <typename Result>
[[nodiscard]] std::optional<std::vector<std::string>> listFields() {
    if constexpr (!std::is_class_v<Result> || (!glz::reflectable<Result> && !glz::glaze_object_t<Result>)) {
        return std::nullopt;
    } else {
        std::vector<std::string> names;
        [&names]<std::size_t... I>(std::index_sequence<I...>) {
            // An action whose result has no members expands the fold below to nothing.
            [[maybe_unused]] auto add = [&names]<std::size_t Index> {
                using Member =
                    std::remove_cvref_t<decltype(::morph::detail::reflectedMember<Index>(std::declval<Result&>()))>;
                if constexpr (glz::writable_array_t<Member>) {
                    names.emplace_back(glz::reflect<Result>::keys[Index]);
                }
            };
            (add.template operator()<I>(), ...);
        }(std::make_index_sequence<glz::reflect<Result>::size>{});
        return names;
    }
}

/// @brief The catalog's form of an action's declared effects.
/// @param actionId The action's id.
/// @return Its effects, or `std::nullopt` when it declared none: the action reads.
/// @throws morph::model::EffectsRegistrationError when the registered records and effects are invalid.
[[nodiscard]] inline std::optional<std::vector<ActionEffect>> effectsInfo(std::string_view actionId) {
    auto const declared = ::morph::effects::effectsOf(actionId);
    if (!declared) {
        return std::nullopt;
    }
    std::vector<ActionEffect> out;
    out.reserve(declared->size());
    for (auto const& effect : *declared) {
        std::string_view kind = "none";
        if (effect.kind == ::morph::effects::Kind::Updates) {
            kind = "updates";
        } else if (effect.kind == ::morph::effects::Kind::Changes) {
            kind = "changes";
        } else if (effect.kind == ::morph::effects::Kind::ChangesInstance) {
            kind = "changesInstance";
        }
        out.push_back(
            ActionEffect{.kind = std::string{kind},
                         .record = effect.record.empty() ? std::nullopt : std::optional<std::string>{effect.record}});
    }
    // A record derived from a changed one changes as a whole too. The server expands derivations before an event
    // leaves; stating them here as well lets the caller's own reply mark entries holding the derived record, which
    // matters in process, where no event comes back.
    ::morph::effects::Changes derived;
    for (auto const& effect : *declared) {
        if (!effect.record.empty() &&
            (effect.kind == ::morph::effects::Kind::Updates || effect.kind == ::morph::effects::Kind::Changes)) {
            ::morph::effects::detail::addDerivedChanges(::morph::effects::detail::resolved(), effect.record, derived);
        }
    }
    for (auto const& change : derived.records) {
        ActionEffect added{.kind = "changes", .record = change.record};
        if (std::ranges::find(out, added) == out.end()) {
            out.push_back(std::move(added));
        }
    }
    return out;
}

/// @brief The catalog's form of the records an action's result holds.
/// @param actionId The action's id.
/// @return One entry per record; empty when the result holds none.
/// @throws morph::model::EffectsRegistrationError when the registered records and effects are invalid.
[[nodiscard]] inline std::vector<ActionRecordInfo> recordsInfo(std::string_view actionId) {
    std::vector<ActionRecordInfo> out;
    for (auto const& held : ::morph::effects::recordsOf(actionId)) {
        out.push_back(ActionRecordInfo{.record = held.record, .keyField = held.keyField, .paths = held.paths});
    }
    return out;
}

}  // namespace detail

/// @brief The catalog facts the builders gathered from the model and action types a screen names.
struct CatalogFacts {
    /// @brief Model facts by registered type id.
    std::map<std::string, ModelInfo, std::less<>> models;
    /// @brief Action facts by action id.
    std::map<std::string, ActionInfo, std::less<>> actions;
    /// @brief The `v-*` view documents `collection` nodes build from, by the action of their `v-query`.
    std::map<std::string, std::string, std::less<>> views;
    /// @brief The `w-*` wizard documents `wizard` nodes build from, by the action of their first step.
    std::map<std::string, std::string, std::less<>> wizards;

    /// @brief Adds another set of facts; a fact already present is kept.
    /// @param other The facts to add.
    void merge(CatalogFacts const& other) {
        for (auto const& [type, info] : other.models) {
            models.emplace(type, info);
        }
        for (auto const& [action, info] : other.actions) {
            actions.emplace(action, info);
        }
        views.insert(other.views.begin(), other.views.end());
        wizards.insert(other.wizards.begin(), other.wizards.end());
    }

    /// @brief Records a model type.
    /// @tparam Model The model type; `::morph::model::ModelTraits<Model>` names its type id.
    template <typename Model>
    void model() {
        models.emplace(
            std::string{::morph::model::ModelTraits<Model>::typeId()},
            ModelInfo{.stateless = ModelFacts<Model>::stateless, .keyed = ::morph::model::KeyedModel<Model>});
    }

    /// @brief Records an action as one of a model type's.
    /// @tparam Model  The model type that runs it.
    /// @tparam Action The action type; `::morph::model::ActionTraits<Action>` names its id.
    /// @throws morph::model::EffectsRegistrationError when the registered records and effects are invalid: the
    ///         action's effects and records are read from the process registry here, so a misconfigured registry
    ///         fails at the start, not on a client's first watch.
    template <typename Model, typename Action>
        requires Runs<Model, Action>
    void action() {
        model<Model>();
        auto const actionId = std::string{::morph::model::ActionTraits<Action>::typeId()};
        actions.emplace(actionId, ActionInfo{
                                      .modelType = std::string{::morph::model::ModelTraits<Model>::typeId()},
                                      .calculatedFields = ActionFacts<Action>::calculatedFields(),
                                      .secretFields = ActionFacts<Action>::secretFields(),
                                      .credential = ActionFacts<Action>::credential,
                                      .listFields = detail::listFields<::morph::model::HandlerResultT<
                                          decltype(std::declval<Model&>().execute(std::declval<Action&>()))>>(),
                                      .effects = detail::effectsInfo(actionId),
                                      .records = detail::recordsInfo(actionId),
                                  });
    }

    /// @brief The facts as a `ui::Catalog`.
    /// @return Lookups over a copy of the facts, so the catalog outlives this object.
    [[nodiscard]] Catalog catalog() const {
        auto shared = std::make_shared<CatalogFacts const>(*this);
        Catalog out;
        out.model = [shared](std::string_view typeId) -> std::optional<ModelInfo> {
            auto const found = shared->models.find(typeId);
            if (found == shared->models.end()) {
                return std::nullopt;
            }
            return found->second;
        };
        out.action = [shared](std::string_view actionId) -> std::optional<ActionInfo> {
            auto const found = shared->actions.find(actionId);
            if (found == shared->actions.end()) {
                return std::nullopt;
            }
            return found->second;
        };
        // Left empty without documents, so a bundle that has none serves none.
        if (!views.empty()) {
            out.view = documentLookup(views);
        }
        if (!wizards.empty()) {
            out.wizard = documentLookup(wizards);
        }
        return out;
    }
};

template <typename T>
class Handle;

namespace detail {

/// @brief Whether a type is a `std::optional`.
/// @tparam T The type.
template <typename T>
struct IsOptional : std::false_type {};

/// @brief A `std::optional` is one.
/// @tparam T The engaged type.
template <typename T>
struct IsOptional<std::optional<T>> : std::true_type {};

/// @brief A type with one `std::optional` removed.
/// @tparam T The type.
template <typename T>
struct Unwrapped {
    /// @brief The type itself.
    using type = T;
};

/// @brief The engaged type of a `std::optional`.
/// @tparam T The engaged type.
template <typename T>
struct Unwrapped<std::optional<T>> {
    /// @brief The engaged type.
    using type = T;
};

/// @brief The type with one `std::optional` removed.
/// @tparam T The type.
template <typename T>
using UnwrappedT = Unwrapped<T>::type;

/// @brief Whether a type is a `std::vector`.
/// @tparam T The type.
template <typename T>
struct IsVector : std::false_type {};

/// @brief A `std::vector` is one.
/// @tparam T     The element type.
/// @tparam Alloc The allocator.
template <typename T, typename Alloc>
struct IsVector<std::vector<T, Alloc>> : std::true_type {};

/// @brief Whether a type is a `Handle`.
/// @tparam T The type.
template <typename T>
struct IsHandle : std::false_type {};

/// @brief A `Handle` is one.
/// @tparam T The handle's value type.
template <typename T>
struct IsHandle<Handle<T>> : std::true_type {};

/// @brief Whether a scalar value of one type can stand where a value of another is expected.
/// @tparam From The value's type, without `std::optional`.
/// @tparam To   The expected type, without `std::optional`.
/// @return True for `Any` on either side, the same type, or a conversion between them.
template <typename From, typename To>
[[nodiscard]] consteval bool scalarAssignable() {
    if constexpr (std::same_as<From, Any> || std::same_as<To, Any>) {
        return true;
    } else {
        return std::same_as<From, To> || std::is_convertible_v<From, To> || std::is_constructible_v<To, From>;
    }
}

/// @brief Whether an expression of one value type can stand where a value of another is expected. `null`
///        propagates through every expression, so an optional value may stand for a required one.
/// @tparam From The expression's value type.
/// @tparam To   The expected value type.
/// @return Whether it can.
template <typename From, typename To>
[[nodiscard]] consteval bool assignable() {
    using Source = UnwrappedT<From>;
    using Target = UnwrappedT<To>;
    if constexpr (IsVector<Source>::value && IsVector<Target>::value) {
        return scalarAssignable<UnwrappedT<typename Source::value_type>, UnwrappedT<typename Target::value_type>>();
    } else {
        return scalarAssignable<Source, Target>();
    }
}

/// @brief Satisfied by a C++ scalar a builder writes as a document constant.
template <typename L>
concept ScalarLiteral =
    std::same_as<L, bool> || (std::integral<L> && !std::same_as<L, char>) ||
    std::convertible_to<L const&, std::string_view> || std::same_as<L, math::Rational> || std::same_as<L, Decimal> ||
    std::same_as<L, Quantity> || std::same_as<L, time::DateTime> || std::same_as<L, std::nullptr_t>;

/// @brief Satisfied by a C++ value a builder writes as a document constant: a scalar, a list of scalars, or a
///        `ui::Value`.
template <typename L>
concept Literal =
    ScalarLiteral<L> || std::same_as<L, Value> || (IsVector<L>::value && ScalarLiteral<typename L::value_type>);

/// @brief The value type a constant stands for.
/// @tparam L The C++ type of the constant.
template <typename L>
struct LiteralOf {
    /// @brief The constant's own type.
    using type = L;
};

/// @brief Text is a `std::string`.
/// @tparam N The literal's size.
template <std::size_t N>
// NOLINTNEXTLINE(modernize-avoid-c-arrays, cppcoreguidelines-avoid-c-arrays): a string literal's own type.
struct LiteralOf<char[N]> {
    /// @brief `std::string`.
    using type = std::string;
};

/// @brief A C string is a `std::string`.
template <>
struct LiteralOf<char const*> {
    /// @brief `std::string`.
    using type = std::string;
};

/// @brief A string view is a `std::string`.
template <>
struct LiteralOf<std::string_view> {
    /// @brief `std::string`.
    using type = std::string;
};

/// @brief The value type a constant stands for.
/// @tparam L The C++ type of the constant.
template <typename L>
using LiteralT = LiteralOf<std::remove_cvref_t<L>>::type;

/// @brief The value type of an operand: a handle's value type, or the type its constant stands for.
/// @tparam X The operand's C++ type.
template <typename X>
struct ValueOf {
    /// @brief The constant's value type.
    using type = LiteralT<X>;
};

/// @brief A handle's value type.
/// @tparam T The value type.
template <typename T>
struct ValueOf<Handle<T>> {
    /// @brief The handle's value type.
    using type = T;
};

/// @brief Satisfied by an operand of an expression operator: a handle or a constant.
template <typename X>
concept Operand = IsHandle<X>::value || Literal<X>;

/// @brief Satisfied when two operands are operands and at least one is a handle, so the builders' operators never
///        apply to two plain C++ values.
template <typename L, typename R>
concept HandleOperands = Operand<L> && Operand<R> && (IsHandle<L>::value || IsHandle<R>::value);

/// @brief The value type of an arithmetic expression: the left handle's, else the right's.
/// @tparam L The left operand's C++ type.
/// @tparam R The right operand's C++ type.
template <typename L, typename R>
using ArithmeticT = std::conditional_t<IsHandle<L>::value, typename ValueOf<L>::type, typename ValueOf<R>::type>;

/// @brief A C++ scalar as a document value.
/// @tparam L The scalar's type.
/// @param literal The scalar.
/// @return The value.
template <typename L>
[[nodiscard]] Value scalarValue(L const& literal) {
    if constexpr (std::same_as<L, bool> || std::same_as<L, Decimal> || std::same_as<L, Quantity> ||
                  std::same_as<L, time::DateTime> || std::same_as<L, std::nullptr_t>) {
        return Value{literal};
    } else if constexpr (std::same_as<L, math::Rational>) {
        return Value{Decimal{literal}};
    } else if constexpr (std::integral<L>) {
        return Value{static_cast<std::int64_t>(literal)};
    } else {
        return Value{std::string{std::string_view{literal}}};
    }
}

/// @brief A C++ constant as a document value.
/// @tparam L The constant's type.
/// @param literal The constant.
/// @return The value.
template <typename L>
[[nodiscard]] Value literalValue(L const& literal) {
    if constexpr (std::same_as<L, Value>) {
        return literal;
    } else if constexpr (IsVector<L>::value) {
        ValueList items;
        items.reserve(literal.size());
        for (auto const& item : literal) {
            items.push_back(scalarValue(item));
        }
        return Value{std::move(items)};
    } else {
        return scalarValue(literal);
    }
}

/// @brief Sets a member of a JSON object, replacing one of the same name and keeping the order of the others.
/// @param object The object.
/// @param key    The member's name.
/// @param value  The member's value.
inline void put(Json& object, std::string_view key, Json value) {
    std::get<Json::object_t>(object.data).insert_or_assign(std::string{key}, std::move(value));
}

/// @brief Sets a member of a JSON object to another object, unless that one is empty.
/// @param object  The object.
/// @param key     The member's name.
/// @param section The value; an empty object is left out.
inline void putSection(Json& object, std::string_view key, Json const& section) {
    if (!std::get<Json::object_t>(section.data).empty()) {
        put(object, key, section);
    }
}

/// @brief How many members a JSON object has.
/// @param object The object.
/// @return The count.
[[nodiscard]] inline std::size_t members(Json const& object) { return std::get<Json::object_t>(object.data).size(); }

/// @brief A document value as a constant expression: a scalar as itself, a list, record, decimal or quantity
///        wrapped in `{"lit": v}`.
/// @param value The value.
/// @return The expression's JSON.
[[nodiscard]] inline Json constant(Value const& value) {
    Json json = valueToJson(value);
    if (std::holds_alternative<Json::object_t>(json.data) || std::holds_alternative<Json::array_t>(json.data)) {
        Json wrapped = Json::object_t{};
        detail::put(wrapped, "lit", std::move(json));
        return wrapped;
    }
    return json;
}

/// @brief A one-key JSON object.
/// @param key   The key.
/// @param value The value.
/// @return `{key: value}`.
[[nodiscard]] inline Json single(std::string_view key, Json value) {
    Json json = Json::object_t{};
    detail::put(json, key, std::move(value));
    return json;
}

/// @brief An alignment's document name.
/// @param align The alignment.
/// @return `start`, `center`, `end` or `stretch`.
[[nodiscard]] inline std::string alignName(Align align) {
    static constexpr std::array<char const*, 4> kNames = {"stretch", "start", "center", "end"};
    return kNames.at(static_cast<std::size_t>(align));
}

/// @brief A justification's document name.
/// @param justify The justification.
/// @return `start`, `center`, `end` or `between`.
[[nodiscard]] inline std::string justifyName(Justify justify) {
    static constexpr std::array<char const*, 4> kNames = {"start", "center", "end", "between"};
    return kNames.at(static_cast<std::size_t>(justify));
}

/// @brief The value of a constant expression, as `constant` writes one.
/// @param expression The expression's JSON.
/// @return A scalar as itself, the inside of `{"lit": v}`; `nullopt` for an expression that is not a constant.
[[nodiscard]] inline std::optional<Json> literalOf(Json const& expression) {
    if (auto const* const object = std::get_if<Json::object_t>(&expression.data)) {
        auto const* const found = object->find("lit");
        if (object->size() == 1 && found != object->end()) {
            return found->second;
        }
        return std::nullopt;
    }
    if (std::holds_alternative<Json::array_t>(expression.data)) {
        return std::nullopt;
    }
    return expression;
}

/// @brief A call of an `expr/1` library function.
/// @param function The function's name.
/// @param args     The argument expressions.
/// @return `{"fn": function, "args": [...]}`.
[[nodiscard]] inline Json call(std::string_view function, std::vector<Json> args) {
    Json json = Json::object_t{};
    detail::put(json, "fn", std::string{function});
    detail::put(json, "args",
                Json::array_t(std::make_move_iterator(args.begin()), std::make_move_iterator(args.end())));
    return json;
}

/// @brief The dotted path a named read reads, when the expression is one.
/// @param json The expression.
/// @return The form (`ref` or `row`) and its path, or nothing for any other expression.
[[nodiscard]] inline std::optional<std::pair<std::string, std::string>> namedRead(Json const& json) {
    auto const* const object = std::get_if<Json::object_t>(&json.data);
    if (object == nullptr || object->size() != 1) {
        return std::nullopt;
    }
    auto const& [form, path] = *object->begin();
    auto const* const text = std::get_if<std::string>(&path.data);
    if (text == nullptr || (form != "ref" && form != "row")) {
        return std::nullopt;
    }
    return std::pair{form, *text};
}

/// @brief A field read below a named read.
/// @param json  The named read.
/// @param field The field.
/// @return The read of `path.field`.
/// @throws AuthoringError when @p json is not a named read: `expr/1` reads fields only below a name.
[[nodiscard]] inline Json fieldOf(Json const& json, std::string_view field) {
    auto read = namedRead(json);
    if (!read) {
        throw AuthoringError{
            "a field is read below a name -- a param, state, let, query, row, element or bound "
            "name -- and not below a computed value; field `" +
            std::string{field} + "`"};
    }
    std::string path = read->second;
    if (!path.empty()) {
        path += '.';
    }
    path += field;
    return single(read->first, std::move(path));
}

/// @brief The name a named read reads.
/// @param json The expression.
/// @param what What the name is for, for the error.
/// @return The `ref` path.
/// @throws AuthoringError when @p json is not a `ref`.
[[nodiscard]] inline std::string refPath(Json const& json, std::string_view what) {
    auto read = namedRead(json);
    if (!read || read->first != "ref") {
        throw AuthoringError{std::string{what} + " names a state, an element or a declared value"};
    }
    return read->second;
}

/// @brief The reflected name of a data member, found by its address in a value-initialised object of its class.
/// @tparam Class  The class; glaze reflects it.
/// @tparam Member The member's type.
/// @param member The member.
/// @return The name the member has in the class's JSON.
/// @throws AuthoringError when the member is not one glaze reflects.
template <typename Class, typename Member>
[[nodiscard]] std::string memberName(Member Class::* member) {
    Class const probe{};
    void const* const target = std::addressof(probe.*member);
    std::string found;
    [&]<std::size_t... Index>(std::index_sequence<Index...>) {
        auto visit = [&]<std::size_t I>() {
            decltype(auto) value = morph::detail::reflectedMember<I>(probe);
            if constexpr (std::is_reference_v<decltype(value)>) {
                if (static_cast<void const*>(std::addressof(value)) == target) {
                    found = std::string{glz::reflect<Class>::keys.at(I)};
                }
            }
        };
        (visit.template operator()<Index>(), ...);
    }(std::make_index_sequence<glz::reflect<Class>::size>{});
    if (found.empty()) {
        throw AuthoringError{"a member pointer names a member its class's JSON does not have"};
    }
    return found;
}

}  // namespace detail

/// @brief An expression of a document, typed by the C++ value it evaluates to (spec 5 §11).
///
/// A handle is a JSON expression tree. Operators build calls of the `expr/1` library: `a + b` is `add`, `a < b`
/// is `lt`, `a && b` is `and`, `!a` is `not`; `at` reads a field by member pointer, and `orElse` is `coalesce`. A
/// default-constructed handle is absent: a builder leaves out the key it would fill.
/// @tparam T The C++ type of the value, `Any` when the screen cannot know it.
template <typename T = Any>
class Handle {
public:
    /// @brief The C++ type of the value.
    using ValueType = T;

    /// @brief An absent handle.
    Handle() = default;

    /// @brief A constant.
    /// @tparam L The constant's C++ type, which must be able to stand for `T`.
    /// @param literal The constant.
    template <typename L>
        requires detail::Literal<std::remove_cvref_t<L>> && (detail::assignable<detail::LiteralT<L>, T>())
    Handle(L const& literal)  // NOLINT(google-explicit-constructor, hicpp-explicit-conversions)
        : _json{std::make_shared<Json const>(detail::constant(detail::literalValue(normalise(literal))))} {}

    /// @brief A handle of another value type that can stand for this one.
    /// @tparam U The other value type.
    /// @param other The handle.
    template <typename U>
        requires(!std::same_as<U, T>) && (detail::assignable<U, T>())
    Handle(Handle<U> const& other)  // NOLINT(google-explicit-constructor, hicpp-explicit-conversions)
        : _json{other._json} {}

    /// @brief A handle over an expression's JSON, for a form no builder spells.
    /// @param json The expression.
    /// @return The handle.
    [[nodiscard]] static Handle raw(Json json) {
        Handle out;
        out._json = std::make_shared<Json const>(std::move(json));
        return out;
    }

    /// @brief Whether the handle holds an expression.
    /// @return False for a default-constructed handle.
    [[nodiscard]] bool present() const noexcept { return _json != nullptr; }

    /// @brief The expression's JSON.
    /// @return The JSON.
    /// @throws AuthoringError when the handle is absent.
    [[nodiscard]] Json const& json() const {
        if (_json == nullptr) {
            throw AuthoringError{"an absent expression is used as an operand"};
        }
        return *_json;
    }

    /// @brief A field of the value, by member pointer: a renamed field is a compile error.
    /// @tparam Class  The class of the member, the value's type.
    /// @tparam Member The member's type.
    /// @param member The member.
    /// @return The field read.
    /// @throws AuthoringError when the handle is not a named read.
    template <typename Class, typename Member>
        requires std::same_as<T, Any> || std::same_as<detail::UnwrappedT<T>, Class>
    [[nodiscard]] Handle<Member> at(Member Class::* member) const {
        return Handle<Member>::raw(detail::fieldOf(json(), detail::memberName(member)));
    }

    /// @brief A field of the value, by name.
    /// @param field The field, or a dotted path.
    /// @return The field read.
    /// @throws AuthoringError when the handle is not a named read.
    [[nodiscard]] Handle<Any> at(std::string_view field) const {
        return Handle<Any>::raw(detail::fieldOf(json(), field));
    }

    /// @brief The value, or a fallback when it is `null`.
    /// @param fallback The fallback.
    /// @return `coalesce(this, fallback)`.
    [[nodiscard]] Handle orElse(Handle const& fallback) const {
        return raw(detail::call("coalesce", {json(), fallback.json()}));
    }

private:
    template <typename U>
    friend class Handle;

    template <typename L>
    [[nodiscard]] static auto normalise(L const& literal) {
        if constexpr (std::convertible_to<L const&, std::string_view> && !std::same_as<L, std::nullptr_t>) {
            return std::string{std::string_view{literal}};
        } else {
            return literal;
        }
    }

    std::shared_ptr<Json const> _json;
};

/// @brief An expression whose value type is not tracked: the type of every node property.
using AnyHandle = Handle<Any>;

namespace detail {

/// @brief An operand's JSON.
/// @tparam X The operand's C++ type.
/// @param operand The operand.
/// @return The JSON.
template <typename X>
[[nodiscard]] Json operandJson(X const& operand) {
    if constexpr (IsHandle<X>::value) {
        return operand.json();
    } else {
        return Handle<LiteralT<X>>{operand}.json();
    }
}

/// @brief A two-operand call.
/// @tparam Result The result's value type.
/// @tparam L      The left operand's C++ type.
/// @tparam R      The right operand's C++ type.
/// @param function The library function.
/// @param lhs      The left operand.
/// @param rhs      The right operand.
/// @return The call.
template <typename Result, typename L, typename R>
[[nodiscard]] Handle<Result> binary(std::string_view function, L const& lhs, R const& rhs) {
    return Handle<Result>::raw(call(function, {operandJson(lhs), operandJson(rhs)}));
}

}  // namespace detail

/// @brief `add`.
/// @tparam L The left operand's C++ type.
/// @tparam R The right operand's C++ type.
/// @param lhs The left operand.
/// @param rhs The right operand.
/// @return The sum.
template <typename L, typename R>
    requires detail::HandleOperands<L, R>
[[nodiscard]] Handle<detail::ArithmeticT<L, R>> operator+(L const& lhs, R const& rhs) {
    return detail::binary<detail::ArithmeticT<L, R>>("add", lhs, rhs);
}

/// @brief `sub`.
/// @tparam L The left operand's C++ type.
/// @tparam R The right operand's C++ type.
/// @param lhs The left operand.
/// @param rhs The right operand.
/// @return The difference.
template <typename L, typename R>
    requires detail::HandleOperands<L, R>
[[nodiscard]] Handle<detail::ArithmeticT<L, R>> operator-(L const& lhs, R const& rhs) {
    return detail::binary<detail::ArithmeticT<L, R>>("sub", lhs, rhs);
}

/// @brief `mul`.
/// @tparam L The left operand's C++ type.
/// @tparam R The right operand's C++ type.
/// @param lhs The left operand.
/// @param rhs The right operand.
/// @return The product.
template <typename L, typename R>
    requires detail::HandleOperands<L, R>
[[nodiscard]] Handle<detail::ArithmeticT<L, R>> operator*(L const& lhs, R const& rhs) {
    return detail::binary<detail::ArithmeticT<L, R>>("mul", lhs, rhs);
}

/// @brief `div`.
/// @tparam L The left operand's C++ type.
/// @tparam R The right operand's C++ type.
/// @param lhs The left operand.
/// @param rhs The right operand.
/// @return The exact quotient.
template <typename L, typename R>
    requires detail::HandleOperands<L, R>
[[nodiscard]] Handle<detail::ArithmeticT<L, R>> operator/(L const& lhs, R const& rhs) {
    return detail::binary<detail::ArithmeticT<L, R>>("div", lhs, rhs);
}

/// @brief `lt`.
/// @tparam L The left operand's C++ type.
/// @tparam R The right operand's C++ type.
/// @param lhs The left operand.
/// @param rhs The right operand.
/// @return Whether the left is less.
template <typename L, typename R>
    requires detail::HandleOperands<L, R>
[[nodiscard]] Handle<bool> operator<(L const& lhs, R const& rhs) {
    return detail::binary<bool>("lt", lhs, rhs);
}

/// @brief `le`.
/// @tparam L The left operand's C++ type.
/// @tparam R The right operand's C++ type.
/// @param lhs The left operand.
/// @param rhs The right operand.
/// @return Whether the left is less or equal.
template <typename L, typename R>
    requires detail::HandleOperands<L, R>
[[nodiscard]] Handle<bool> operator<=(L const& lhs, R const& rhs) {
    return detail::binary<bool>("le", lhs, rhs);
}

/// @brief `gt`.
/// @tparam L The left operand's C++ type.
/// @tparam R The right operand's C++ type.
/// @param lhs The left operand.
/// @param rhs The right operand.
/// @return Whether the left is greater.
template <typename L, typename R>
    requires detail::HandleOperands<L, R>
[[nodiscard]] Handle<bool> operator>(L const& lhs, R const& rhs) {
    return detail::binary<bool>("gt", lhs, rhs);
}

/// @brief `ge`.
/// @tparam L The left operand's C++ type.
/// @tparam R The right operand's C++ type.
/// @param lhs The left operand.
/// @param rhs The right operand.
/// @return Whether the left is greater or equal.
template <typename L, typename R>
    requires detail::HandleOperands<L, R>
[[nodiscard]] Handle<bool> operator>=(L const& lhs, R const& rhs) {
    return detail::binary<bool>("ge", lhs, rhs);
}

/// @brief `and`, with Kleene's three-valued logic. Both operands are written: a document has no short circuit.
/// @tparam L The left operand's C++ type.
/// @tparam R The right operand's C++ type.
/// @param lhs The left operand.
/// @param rhs The right operand.
/// @return The conjunction.
template <typename L, typename R>
    requires detail::HandleOperands<L, R>
[[nodiscard]] Handle<bool> operator&&(L const& lhs, R const& rhs) {
    return detail::binary<bool>("and", lhs, rhs);
}

/// @brief `or`, with Kleene's three-valued logic.
/// @tparam L The left operand's C++ type.
/// @tparam R The right operand's C++ type.
/// @param lhs The left operand.
/// @param rhs The right operand.
/// @return The disjunction.
template <typename L, typename R>
    requires detail::HandleOperands<L, R>
[[nodiscard]] Handle<bool> operator||(L const& lhs, R const& rhs) {
    return detail::binary<bool>("or", lhs, rhs);
}

/// @brief `not`.
/// @tparam T The operand's value type.
/// @param operand The operand.
/// @return The negation.
template <typename T>
[[nodiscard]] Handle<bool> operator!(Handle<T> const& operand) {
    return Handle<bool>::raw(detail::call("not", {operand.json()}));
}

/// @brief `neg`.
/// @tparam T The operand's value type.
/// @param operand The operand.
/// @return The negated number.
template <typename T>
[[nodiscard]] Handle<T> operator-(Handle<T> const& operand) {
    return Handle<T>::raw(detail::call("neg", {operand.json()}));
}

/// @brief `eq`: equality of magnitude. A function, not `==`: an `==` that returns an expression rather than a
///        `bool` breaks the comparisons C++ rewrites from it.
/// @tparam L The left operand's C++ type.
/// @tparam R The right operand's C++ type.
/// @param lhs The left operand.
/// @param rhs The right operand.
/// @return Whether they are equal.
template <typename L, typename R>
    requires detail::HandleOperands<L, R>
[[nodiscard]] Handle<bool> eq(L const& lhs, R const& rhs) {
    return detail::binary<bool>("eq", lhs, rhs);
}

/// @brief `ne`.
/// @tparam L The left operand's C++ type.
/// @tparam R The right operand's C++ type.
/// @param lhs The left operand.
/// @param rhs The right operand.
/// @return Whether they differ.
template <typename L, typename R>
    requires detail::HandleOperands<L, R>
[[nodiscard]] Handle<bool> ne(L const& lhs, R const& rhs) {
    return detail::binary<bool>("ne", lhs, rhs);
}

/// @brief A call of any `expr/1` library function, for the functions without a builder of their own.
/// @tparam R The result's value type.
/// @param function The function's name.
/// @param args     The arguments.
/// @return The call.
template <typename R = Any>
[[nodiscard]] Handle<R> call(std::string_view function, std::vector<AnyHandle> const& args) {
    std::vector<Json> json;
    json.reserve(args.size());
    for (AnyHandle const& arg : args) {
        json.push_back(arg.json());
    }
    return Handle<R>::raw(detail::call(function, std::move(json)));
}

/// @brief `engaged`: whether a value is not `null`.
/// @param value The value.
/// @return The test.
[[nodiscard]] inline Handle<bool> engaged(AnyHandle const& value) { return call<bool>("engaged", {value}); }

/// @brief `coalesce`: the first value that is not `null`.
/// @param values The candidates, in order.
/// @return The call.
[[nodiscard]] inline AnyHandle coalesce(std::vector<AnyHandle> const& values) { return call("coalesce", values); }

/// @brief `concat`.
/// @param parts The strings.
/// @return The joined string.
[[nodiscard]] inline Handle<std::string> concat(std::vector<AnyHandle> const& parts) {
    return call<std::string>("concat", parts);
}

/// @brief `format`: a template with `{0}`, `{1}`, … placeholders.
/// @param pattern The template.
/// @param args    The values for the placeholders.
/// @return The formatted string.
[[nodiscard]] inline Handle<std::string> format(AnyHandle const& pattern, std::vector<AnyHandle> args) {
    std::vector<AnyHandle> operands;
    operands.reserve(args.size() + 1);
    operands.push_back(pattern);
    std::ranges::move(args, std::back_inserter(operands));
    return call<std::string>("format", operands);
}

/// @brief `concatLists`: the lists, one after another; `null` while any of them is `null`.
/// @param lists The lists, in order.
/// @return The joined list.
[[nodiscard]] inline AnyHandle concatLists(std::vector<AnyHandle> const& lists) { return call("concatLists", lists); }

/// @brief `concatLists` of lists of one element type.
/// @tparam T The element type.
/// @param lists The lists, in order.
/// @return The joined list.
template <typename T>
[[nodiscard]] Handle<std::vector<T>> concatLists(std::vector<Handle<std::vector<T>>> const& lists) {
    std::vector<Json> json;
    json.reserve(lists.size());
    for (Handle<std::vector<T>> const& list : lists) {
        json.push_back(list.json());
    }
    return Handle<std::vector<T>>::raw(detail::call("concatLists", std::move(json)));
}

/// @brief `count`: a list's length.
/// @param list The list.
/// @return The count.
[[nodiscard]] inline Handle<std::int64_t> count(AnyHandle const& list) { return call<std::int64_t>("count", {list}); }

/// @brief `sum` of a list of numbers.
/// @param list The list.
/// @return The exact sum.
[[nodiscard]] inline AnyHandle sum(AnyHandle const& list) { return call("sum", {list}); }

/// @brief `includes`: whether a list holds a value.
/// @param list  The list.
/// @param value The value.
/// @return The test.
[[nodiscard]] inline Handle<bool> includes(AnyHandle const& list, AnyHandle const& value) {
    return call<bool>("includes", {list, value});
}

/// @brief `isEmpty`: an empty string or list.
/// @param value The string or list.
/// @return The test.
[[nodiscard]] inline Handle<bool> isEmpty(AnyHandle const& value) { return call<bool>("isEmpty", {value}); }

/// @brief `round(x, dp)`, half to even.
/// @param value  The number.
/// @param places The places.
/// @return The rounded number.
[[nodiscard]] inline AnyHandle round(AnyHandle const& value, std::int64_t places) {
    return call("round", {value, places});
}

/// @brief `parseDecimal(text)`: typed text, in the client's locale, as an exact decimal; `null` while it does not
/// spell one.
/// @param text The text, typically a text input's `event`.
/// @return The decimal, at the places typed.
[[nodiscard]] inline AnyHandle parseDecimal(AnyHandle const& text) { return call("parseDecimal", {text}); }

/// @brief `parseDecimal(text, dp)`: typed text as an exact decimal at @p places; `null` while it does not spell one,
/// or when it has more fraction digits than @p places.
/// @param text   The text, typically a text input's `event`.
/// @param places The places, 0 to 18.
/// @return The decimal.
[[nodiscard]] inline AnyHandle parseDecimal(AnyHandle const& text, std::int64_t places) {
    return call("parseDecimal", {text, places});
}

/// @brief `quantity(x)`: a quantity for display in its own unit.
/// @param value The quantity.
/// @return The text.
[[nodiscard]] inline Handle<std::string> quantity(AnyHandle const& value) {
    return call<std::string>("quantity", {value});
}

/// @brief `quantity(x, unit)`: a quantity for display in another unit.
/// @param value The quantity.
/// @param unit  The display unit.
/// @return The text.
[[nodiscard]] inline Handle<std::string> quantity(AnyHandle const& value, AnyHandle const& unit) {
    return call<std::string>("quantity", {value, unit});
}

/// @brief `t(key, fallback, args)`: a translated MessageFormat 2 message.
/// @param key      The catalogue key.
/// @param fallback The pattern shown when the catalogue has none.
/// @param args     The named arguments.
/// @return The `{"t": ...}` form.
[[nodiscard]] inline Handle<std::string> t(std::string const& key, std::string const& fallback,
                                           std::vector<std::pair<std::string, AnyHandle>> const& args = {}) {
    Json json = Json::object_t{};
    detail::put(json, "t", key);
    detail::put(json, "fallback", fallback);
    if (!args.empty()) {
        Json record = Json::object_t{};
        for (auto const& [name, value] : args) {
            detail::put(record, name, value.json());
        }
        detail::put(json, "args", std::move(record));
    }
    return Handle<std::string>::raw(std::move(json));
}

/// @brief `{"if": [c, a, b]}`: a choice between two values.
/// @tparam T The value type of the branches.
/// @param condition The condition.
/// @param then      The value when it is true.
/// @param otherwise The value otherwise.
/// @return The choice.
template <typename T>
[[nodiscard]] Handle<T> cond(Handle<bool> const& condition, Handle<T> const& then, Handle<T> const& otherwise) {
    Json json = Json::object_t{};
    detail::put(json, "if", Json::array_t{condition.json(), then.json(), otherwise.json()});
    return Handle<T>::raw(std::move(json));
}

/// @brief `{"obj": {...}}`: a record built from expressions.
/// @param fields The fields, in order.
/// @return The record.
[[nodiscard]] inline AnyHandle obj(std::vector<std::pair<std::string, AnyHandle>> const& fields) {
    Json record = Json::object_t{};
    for (auto const& [name, value] : fields) {
        detail::put(record, name, value.json());
    }
    return AnyHandle::raw(detail::single("obj", std::move(record)));
}

/// @brief The content sizing, `"content"`: as much as the content needs.
/// @return The sizing, a constant the loader checks.
[[nodiscard]] inline AnyHandle content() { return AnyHandle{std::string{"content"}}; }

/// @brief A fixed sizing, `{"lit": {"fixed": units}}`.
/// @param units The size in backend units, 0 or more.
/// @return The sizing, a constant the loader checks.
[[nodiscard]] inline AnyHandle fixed(int units) {
    return AnyHandle::raw(detail::constant(Value{ValueRecord{{"fixed", Value{std::int64_t{units}}}}}));
}

/// @brief A stretch sizing, `{"lit": {"stretch": weight}}`: a share of the leftover space.
/// @param weight The share's weight, 1 or more.
/// @return The sizing, a constant the loader checks.
[[nodiscard]] inline AnyHandle stretch(int weight = 1) {
    return AnyHandle::raw(detail::constant(Value{ValueRecord{{"stretch", Value{std::int64_t{weight}}}}}));
}

/// @brief `{"list": [...]}`: a list built from expressions.
/// @param items The items.
/// @return The list.
[[nodiscard]] inline AnyHandle list(std::vector<AnyHandle> const& items) {
    Json::array_t array;
    array.reserve(items.size());
    for (AnyHandle const& item : items) {
        array.push_back(item.json());
    }
    return AnyHandle::raw(detail::single("list", std::move(array)));
}

/// @brief A named read, `{"ref": "name.path"}`: a param, state, `let`, query value, element value or bound name.
/// @tparam T The value type.
/// @param path The name and path.
/// @return The read.
template <typename T = Any>
[[nodiscard]] Handle<T> ref(std::string const& path) {
    return Handle<T>::raw(detail::single("ref", path));
}

/// @brief The innermost row, `{"row": ""}`.
/// @tparam T The row's type.
/// @return The read.
template <typename T = Any>
[[nodiscard]] Handle<T> row() {
    return Handle<T>::raw(detail::single("row", std::string{}));
}

/// @brief A field of the innermost row, by member pointer.
/// @tparam Class  The row type.
/// @tparam Member The member's type.
/// @param member The member.
/// @return The read.
template <typename Class, typename Member>
[[nodiscard]] Handle<Member> row(Member Class::* member) {
    return Handle<Member>::raw(detail::single("row", detail::memberName(member)));
}

/// @brief A field of the innermost row, by name.
/// @param path The field or dotted path.
/// @return The read.
[[nodiscard]] inline AnyHandle row(std::string const& path) { return AnyHandle::raw(detail::single("row", path)); }

/// @brief A comprehension over a list, `{"map": e, "as": "x", "to": e}`.
/// @tparam E The element type.
/// @param source  The list.
/// @param name    The element's name inside @p body.
/// @param body    Builds the mapped value from the element's handle.
/// @return The mapped list.
template <typename E = Any>
[[nodiscard]] AnyHandle map(AnyHandle const& source, std::string const& name,
                            std::type_identity_t<std::function<AnyHandle(Handle<E> const&)>> const& body) {
    Json json = Json::object_t{};
    detail::put(json, "map", source.json());
    detail::put(json, "as", name);
    detail::put(json, "to", body(author::ref<E>(name)).json());
    return AnyHandle::raw(std::move(json));
}

/// @brief A filter over a list, `{"filter": e, "as": "x", "where": e}`.
/// @tparam E The element type.
/// @param source The list.
/// @param name   The element's name inside @p where.
/// @param where  Builds the condition from the element's handle.
/// @return The filtered list.
template <typename E = Any>
[[nodiscard]] AnyHandle filter(AnyHandle const& source, std::string const& name,
                               std::type_identity_t<std::function<Handle<bool>(Handle<E> const&)>> const& where) {
    Json json = Json::object_t{};
    detail::put(json, "filter", source.json());
    detail::put(json, "as", name);
    detail::put(json, "where", where(author::ref<E>(name)).json());
    return AnyHandle::raw(std::move(json));
}

/// @brief The elements of a list whose boolean member is true.
/// @tparam Class The element type.
/// @param source The list.
/// @param member The member.
/// @return The filtered list.
template <typename Class>
[[nodiscard]] AnyHandle filter(AnyHandle const& source, bool Class::* member) {
    return filter<Class>(source, "item", [member](Handle<Class> const& item) { return item.at(member); });
}

/// @brief A client environment value, `{"env": name}`: `widthClass`, `locale`, `platform`, `online`, `backend`.
/// @param name The name.
/// @return The read.
[[nodiscard]] inline AnyHandle env(std::string const& name) { return AnyHandle::raw(detail::single("env", name)); }

/// @brief A session attribute, `{"session": name}`.
/// @param name The attribute.
/// @return The read.
[[nodiscard]] inline AnyHandle session(std::string const& name) {
    return AnyHandle::raw(detail::single("session", name));
}

/// @brief A value the application registered with the client, `{"host": name}`.
/// @param name The value's name.
/// @return The read; `null` on a client without it.
[[nodiscard]] inline AnyHandle hostValue(std::string const& name) {
    return AnyHandle::raw(detail::single("host", name));
}

/// @brief A prop of a component, inside the component's fallback.
/// @param name The prop.
/// @return `{"ref": "props.name"}`.
[[nodiscard]] inline AnyHandle prop(std::string const& name) { return author::ref("props." + name); }

/// @brief The payload of the event whose command list runs.
/// @return `{"ref": "event"}`.
[[nodiscard]] inline AnyHandle event() { return author::ref("event"); }

/// @brief An element of the view, named by its `id`, whose live values relations read (spec 5 §6).
class Element {
public:
    /// @brief An element.
    /// @param name The element's id.
    explicit Element(std::string name) : _name{std::move(name)} {}

    /// @brief The element's id.
    /// @return The id.
    [[nodiscard]] std::string const& name() const noexcept { return _name; }

    /// @brief One of the element's values.
    /// @param value The value's name, or a dotted path such as a form field's `mass.value`.
    /// @return The read.
    [[nodiscard]] AnyHandle get(std::string const& value) const { return author::ref(_name + "." + value); }

    /// @brief An input's committed value.
    /// @return The read.
    [[nodiscard]] AnyHandle value() const { return get("value"); }

    /// @brief An input's text while it is edited.
    /// @return The read.
    [[nodiscard]] AnyHandle draft() const { return get("draft"); }

    /// @brief A checkbox's state.
    /// @return The read.
    [[nodiscard]] Handle<bool> checked() const { return author::ref<bool>(_name + ".checked"); }

    /// @brief A select's chosen key.
    /// @return The read.
    [[nodiscard]] AnyHandle selected() const { return get("selected"); }

    /// @brief A table's selection.
    /// @return The read.
    [[nodiscard]] AnyHandle selection() const { return get("selection"); }

    /// @brief Whether a dialog or drawer is open.
    /// @return The read.
    [[nodiscard]] Handle<bool> open() const { return author::ref<bool>(_name + ".open"); }

    /// @brief Whether a form, dialog or element is valid.
    /// @return The read.
    [[nodiscard]] Handle<bool> valid() const { return author::ref<bool>(_name + ".valid"); }

    /// @brief Whether a form or dialog is ready to submit.
    /// @return The read.
    [[nodiscard]] Handle<bool> ready() const { return author::ref<bool>(_name + ".ready"); }

private:
    std::string _name;
};

/// @brief An element of the view.
/// @param name The element's id.
/// @return The element.
[[nodiscard]] inline Element element(std::string name) { return Element{std::move(name)}; }

/// @brief The dialog or drawer a command list runs in, for `close`.
struct Self {};

/// @brief The dialog or drawer a command list runs in.
inline constexpr Self self{};

/// @brief A query of a scope, without its action type: what `refetch`, `prefetch` and `optimistic` name.
class QueryRef {
public:
    /// @brief A query by name.
    /// @param name The query's name.
    explicit QueryRef(std::string name) : _name{std::move(name)} {}

    /// @brief The query's name.
    /// @return The name.
    [[nodiscard]] std::string const& name() const noexcept { return _name; }

private:
    std::string _name;
};

/// @brief A mutation of a scope, without its action type: what `signIn`, `upload` and `can` name.
class MutationRef {
public:
    /// @brief A mutation by name.
    /// @param name The mutation's name.
    explicit MutationRef(std::string name) : _name{std::move(name)} {}

    /// @brief The mutation's name.
    /// @return The name.
    [[nodiscard]] std::string const& name() const noexcept { return _name; }

private:
    std::string _name;
};

/// @brief A query declared by a scope.
/// @tparam Action The action it runs.
/// @tparam Value  The value's C++ type: the action's result for a call, the action itself for an evaluation.
// NOLINTNEXTLINE(readability-redundant-typename): MSVC needs it on a dependent default argument (C4346).
template <typename Action, typename Value = typename ::morph::model::ActionTraits<Action>::Result>
class QueryHandle {
public:
    /// @brief The value's C++ type.
    using Result = Value;

    /// @brief A query by name.
    /// @param name The query's name.
    explicit QueryHandle(std::string name) : _name{std::move(name)} {}

    /// @brief The query's name.
    /// @return The name.
    [[nodiscard]] std::string const& name() const noexcept { return _name; }

    /// @brief The query's value; the last one is kept while a refetch is in flight.
    /// @return `{"ref": name}`.
    [[nodiscard]] Handle<Result> value() const { return author::ref<Result>(_name); }

    /// @brief A field of the query's value.
    /// @tparam Class  The result type.
    /// @tparam Member The member's type.
    /// @param member The member of the result type.
    /// @return The field read.
    template <typename Class, typename Member>
        requires std::same_as<Class, detail::UnwrappedT<Result>>
    [[nodiscard]] Handle<Member> at(Member Class::* member) const {
        return value().at(member);
    }

    /// @brief Whether the query is in flight.
    /// @return `{"pending": name}`.
    [[nodiscard]] Handle<bool> pending() const { return Handle<bool>::raw(detail::single("pending", _name)); }

    /// @brief The query's error text.
    /// @return `{"error": name}`.
    [[nodiscard]] Handle<std::string> error() const {
        return Handle<std::string>::raw(detail::single("error", _name));
    }

    /// @brief The kind of the query's error.
    /// @return `{"errorKind": name}`.
    [[nodiscard]] Handle<std::string> errorKind() const {
        return Handle<std::string>::raw(detail::single("errorKind", _name));
    }

    /// @brief Whether the signed-in principal may run the query.
    /// @return `{"can": name}`.
    [[nodiscard]] Handle<bool> can() const { return Handle<bool>::raw(detail::single("can", _name)); }

    /// @brief The query without its action type.
    /// @return The reference.
    operator QueryRef() const { return QueryRef{_name}; }

private:
    std::string _name;
};

/// @brief A mutation's result: a `Handle` with no field reads.
///
/// A result is not a named read, so a field of it cannot be read in place: the screen binds it first,
/// `auto r = screen.let("r", m.result());`, and reads `r.at(...)`. The `at` overloads are deleted, so the mistake is
/// a compile error at the call rather than a refusal when the screen is registered. It converts to `Handle<T>`
/// wherever a handle is taken.
/// @tparam T The result's C++ type.
template <typename T>
class ResultHandle : public Handle<T> {
public:
    /// @brief The result read.
    /// @param handle The `{"result": name}` expression.
    explicit ResultHandle(Handle<T> const& handle) : Handle<T>{handle} {}

    /// @brief Not available: bind the result with `let` first, `auto r = screen.let("r", m.result());`, then read
    ///        `r.at(...)`.
    /// @tparam Class  The class of the member.
    /// @tparam Member The member's type.
    /// @param member The member.
    /// @return Nothing; the overload is deleted.
    template <typename Class, typename Member>
    [[nodiscard]] Handle<Member> at(Member Class::* member) const = delete;

    /// @brief Not available: bind the result with `let` first, `auto r = screen.let("r", m.result());`, then read
    ///        `r.at(...)`.
    /// @param field The field.
    /// @return Nothing; the overload is deleted.
    [[nodiscard]] Handle<Any> at(std::string_view field) const = delete;
};

/// @brief A mutation declared by a scope.
/// @tparam Action The action it runs; `::morph::model::ActionTraits<Action>::Result` is the result's type.
template <typename Action>
class MutationHandle {
public:
    /// @brief The result's C++ type.
    using Result = ::morph::model::ActionTraits<Action>::Result;

    /// @brief A mutation by name.
    /// @param name The mutation's name.
    explicit MutationHandle(std::string name) : _name{std::move(name)} {}

    /// @brief The mutation's name.
    /// @return The name.
    [[nodiscard]] std::string const& name() const noexcept { return _name; }

    /// @brief The last run's result; bind it with `let` to read its fields.
    /// @return `{"result": name}`, with no `at`.
    [[nodiscard]] ResultHandle<Result> result() const {
        return ResultHandle<Result>{Handle<Result>::raw(detail::single("result", _name))};
    }

    /// @brief Whether a run is in flight.
    /// @return `{"pending": name}`.
    [[nodiscard]] Handle<bool> pending() const { return Handle<bool>::raw(detail::single("pending", _name)); }

    /// @brief The last run's error text.
    /// @return `{"error": name}`.
    [[nodiscard]] Handle<std::string> error() const {
        return Handle<std::string>::raw(detail::single("error", _name));
    }

    /// @brief The kind of the last run's error.
    /// @return `{"errorKind": name}`.
    [[nodiscard]] Handle<std::string> errorKind() const {
        return Handle<std::string>::raw(detail::single("errorKind", _name));
    }

    /// @brief Whether the signed-in principal may run the mutation.
    /// @return `{"can": name}`.
    [[nodiscard]] Handle<bool> can() const { return Handle<bool>::raw(detail::single("can", _name)); }

    /// @brief The mutation without its action type.
    /// @return The reference.
    operator MutationRef() const { return MutationRef{_name}; }

private:
    std::string _name;
};

/// @brief One field of an action body: a member of the action, and the expression that fills it.
/// @tparam Action The action.
template <typename Action>
class BodyField {
public:
    /// @brief A field: a member pointer of the action, so a renamed field or another action's field is a compile
    ///        error, and a value that can stand for the member's type.
    /// @tparam Member The member's type.
    /// @tparam V      The value's C++ type: a handle or a constant.
    /// @param member The member.
    /// @param value  The value.
    template <typename Member, typename V>
        requires detail::Operand<V> && (detail::assignable<typename detail::ValueOf<V>::type, Member>())
    BodyField(Member Action::* member, V const& value)
        : _name{detail::memberName(member)}, _json(detail::operandJson(value)) {}

    /// @brief The field's JSON name.
    /// @return The name.
    [[nodiscard]] std::string const& name() const noexcept { return _name; }

    /// @brief The field's expression.
    /// @return The JSON.
    [[nodiscard]] Json const& json() const noexcept { return _json; }

private:
    std::string _name;
    Json _json = Json::object_t{};
};

/// @brief An action body: the action's members with expressions in place of values (spec 5 §11). C++ cannot
///        synthesise the action's struct with its members retyped, so a body lists `{&Action::member, value}` pairs.
/// @tparam Action The action.
template <typename Action>
class Body {
public:
    /// @brief An empty body; the query or mutation sends an empty object.
    Body() = default;

    /// @brief A body.
    /// @param fields The fields, in order.
    Body(std::initializer_list<BodyField<Action>> fields) : _fields{fields} {}

    /// @brief Whether the body has no fields.
    /// @return True when it has none.
    [[nodiscard]] bool empty() const noexcept { return _fields.empty(); }

    /// @brief The body's expression.
    /// @return `{"obj": {...}}`.
    [[nodiscard]] Json json() const {
        Json record = Json::object_t{};
        for (BodyField<Action> const& field : _fields) {
            detail::put(record, field.name(), field.json());
        }
        return detail::single("obj", std::move(record));
    }

private:
    std::vector<BodyField<Action>> _fields;
};

/// @brief A model alias of a scope.
/// @tparam Model The model type.
template <typename Model>
class ModelHandle {
public:
    /// @brief An alias.
    /// @param alias The alias.
    explicit ModelHandle(std::string alias) : _alias{std::move(alias)} {}

    /// @brief The alias.
    /// @return The alias.
    [[nodiscard]] std::string const& alias() const noexcept { return _alias; }

private:
    std::string _alias;
};

/// @brief How a model alias binds when no key expression is given (spec 5 §5).
enum class Instance : std::uint8_t {
    Default,  ///< `shared` when the catalog marks the model stateless, `private` otherwise.
    Private,  ///< An instance of the scope's own.
    Shared,   ///< The model's singleton.
};

/// @brief The opener's binding of an alias, for a screen opened as a dialog (`{"from": alias}`).
struct Borrow {
    /// @brief The opener's alias.
    std::string alias;
};

/// @brief A model alias's `instance`: a binding mode, a key expression, or a borrowed binding.
class InstanceSpec {
public:
    /// @brief The default binding.
    InstanceSpec() = default;

    /// @brief A binding mode.
    /// @param mode The mode.
    InstanceSpec(Instance mode)  // NOLINT(google-explicit-constructor, hicpp-explicit-conversions)
        : _mode{mode} {}

    /// @brief A borrowed binding.
    /// @param borrow The opener's alias.
    InstanceSpec(Borrow borrow)  // NOLINT(google-explicit-constructor, hicpp-explicit-conversions)
        : _borrowed{std::move(borrow.alias)} {}

    /// @brief A key expression: the shared instance the key selects. The model must be keyed.
    /// @tparam T The key's value type.
    /// @param key The key.
    template <typename T>
    InstanceSpec(Handle<T> const& key)  // NOLINT(google-explicit-constructor, hicpp-explicit-conversions)
        : _key{key} {}

    /// @brief The `instance` JSON.
    /// @return The JSON, or nothing for the default.
    [[nodiscard]] std::optional<Json> json() const {
        if (_key.present()) {
            return _key.json();
        }
        if (!_borrowed.empty()) {
            return detail::single("from", _borrowed);
        }
        if (_mode == Instance::Default) {
            return std::nullopt;
        }
        if (_mode == Instance::Private) {
            return Json(std::string{"private"});
        }
        return Json(std::string{"shared"});
    }

private:
    Instance _mode = Instance::Default;
    AnyHandle _key;
    std::string _borrowed;
};

/// @brief Options of a model alias.
struct ModelOptions {
    /// @brief How the alias binds its instance.
    InstanceSpec instance;
};

/// @brief Options of a query (spec 5 §5).
struct QueryOptions {
    /// @brief The gate; absent is always on.
    AnyHandle when;
    /// @brief How long the body must be stable before a fetch.
    std::chrono::milliseconds debounce{0};
    /// @brief The polling interval; zero does not poll.
    std::chrono::milliseconds refreshEvery{0};
    /// @brief How long a cached value counts as fresh.
    std::chrono::milliseconds staleTime{0};
};

/// @brief What a mutation run does while another is in flight.
enum class Mode : std::uint8_t {
    Exclusive,  ///< The run is refused (the default).
    Serial,     ///< Runs queue, one queue per `serialKey`.
    Latest,     ///< Every run is sent; only the newest reply applies.
    Parallel,   ///< Every run is sent; replies apply as they arrive.
};

/// @brief An `optimistic` write: the row a mutation run predicts before its reply.
struct Optimistic {
    /// @brief The query whose cached list it writes.
    std::optional<QueryRef> query;
    /// @brief The member of the query's record value that holds the list; empty when the value is the list itself.
    std::string at;
    /// @brief The row key, read against the run's body bound as `body`.
    AnyHandle key;
    /// @brief The predicted row, read against the run's body bound as `body`.
    AnyHandle with;
};

class Command;

/// @brief A command list, run as one batch.
class Commands {
public:
    /// @brief No commands.
    Commands() = default;

    /// @brief One command.
    /// @param command The command.
    Commands(Command const& command);  // NOLINT(google-explicit-constructor, hicpp-explicit-conversions)

    /// @brief Commands in order.
    /// @param commands The commands.
    Commands(std::initializer_list<Command> commands);

    /// @brief Whether the list is empty.
    /// @return True when it is.
    [[nodiscard]] bool empty() const noexcept { return _items.empty(); }

    /// @brief The list's JSON.
    /// @return An array of commands.
    [[nodiscard]] Json json() const { return Json::array_t(_items.begin(), _items.end()); }

private:
    std::vector<Json> _items;
};

/// @brief One command of `ui/1` (spec 5 §8, spec 6).
class Command {
public:
    /// @brief A command over its JSON, for a form no builder spells.
    /// @param json The command.
    // Parentheses, not braces: a braced Json from one Json may take glaze's initializer-list constructor and wrap
    // it in an array.
    explicit Command(Json json) : _json(std::move(json)) {}

    /// @brief The command's JSON.
    /// @return The JSON.
    [[nodiscard]] Json const& json() const noexcept { return _json; }

private:
    Json _json = Json::object_t{};
};

inline Commands::Commands(Command const& command) : _items{command.json()} {}

inline Commands::Commands(std::initializer_list<Command> commands) {
    _items.reserve(commands.size());
    for (Command const& command : commands) {
        _items.push_back(command.json());
    }
}

/// @brief Options of a mutation (spec 5 §5).
/// @tparam Action The action it runs.
template <typename Action>
struct MutationOptions {
    /// @brief The body; empty sends the body each `run` gives.
    Body<Action> body;
    /// @brief What a run does while another is in flight.
    Mode concurrency = Mode::Exclusive;
    /// @brief The queue key of `serial` runs.
    AnyHandle serialKey;
    /// @brief The row a run predicts before its reply.
    std::optional<Optimistic> optimistic;
    /// @brief Commands run after a success.
    Commands onSuccess;
    /// @brief Commands run after a failure.
    Commands onError;
    /// @brief Whether a run that cannot reach the server is queued offline.
    bool offlineQueue = false;
};

/// @brief Which changes a watch reacts to.
enum class Source : std::uint8_t {
    Any,   ///< Every change.
    User,  ///< Only the user's own edits.
};

/// @brief A validation across elements (spec 5 §6).
struct CheckOptions {
    /// @brief The check's id.
    std::string name;
    /// @brief The condition; `false` fails the check.
    Handle<bool> assertion;
    /// @brief The message added to each target's errors.
    AnyHandle message;
    /// @brief The elements the message attaches to.
    std::vector<Element> targets;
    /// @brief Whether a failure blocks `ready`.
    bool blocking = false;
};

class Node;

/// @brief A list of view nodes.
class Nodes {
public:
    /// @brief No nodes.
    Nodes() = default;

    /// @brief One node.
    /// @param node The node.
    Nodes(Node const& node);  // NOLINT(google-explicit-constructor, hicpp-explicit-conversions)

    /// @brief Nodes in order.
    /// @param nodes The nodes.
    Nodes(std::initializer_list<Node> nodes);

    /// @brief Nodes in order.
    /// @param nodes The nodes.
    Nodes(std::vector<Node> const& nodes);  // NOLINT(google-explicit-constructor, hicpp-explicit-conversions)

    /// @brief Whether the list is empty.
    /// @return True when it is.
    [[nodiscard]] bool empty() const noexcept { return _items.empty(); }

    /// @brief The list's JSON.
    /// @return An array of nodes.
    [[nodiscard]] Json json() const { return Json::array_t(_items.begin(), _items.end()); }

    /// @brief The catalog facts of the models and actions the nodes name.
    /// @return The facts.
    [[nodiscard]] CatalogFacts const& facts() const noexcept { return _facts; }

private:
    void add(Node const& node);

    std::vector<Json> _items;
    CatalogFacts _facts;
};

/// @brief A view node (spec 5 §9). The kind's own properties come from its builder's options; the properties every
///        node takes come from the member functions, each of which returns a copy with the property set.
class Node {
public:
    /// @brief A node over its JSON, for a kind no builder spells.
    /// @param json  The node.
    /// @param facts The catalog facts of the models and actions it names.
    explicit Node(Json json, CatalogFacts facts = {}) : _json(std::move(json)), _facts{std::move(facts)} {}

    /// @brief The node's JSON.
    /// @return The JSON.
    [[nodiscard]] Json const& json() const noexcept { return _json; }

    /// @brief The catalog facts of the models and actions the node names.
    /// @return The facts.
    [[nodiscard]] CatalogFacts const& facts() const noexcept { return _facts; }

    /// @brief A property set to an expression; an absent expression leaves the node as it is.
    /// @param key   The property.
    /// @param value The expression.
    /// @return The node with it.
    [[nodiscard]] Node with(std::string const& key, AnyHandle const& value) const {
        Node out = *this;
        if (value.present()) {
            detail::put(out._json, key, value.json());
        }
        return out;
    }

    /// @brief A property set to a plain string.
    /// @param key   The property.
    /// @param value The string.
    /// @return The node with it.
    [[nodiscard]] Node withText(std::string const& key, std::string const& value) const {
        Node out = *this;
        detail::put(out._json, key, value);
        return out;
    }

    /// @brief The element id, by which relations read its values.
    /// @param element The element.
    /// @return The node with it.
    [[nodiscard]] Node id(Element const& element) const { return withText("id", element.name()); }

    /// @brief Whether the node shows.
    /// @param condition The condition.
    /// @return The node with it.
    [[nodiscard]] Node visible(AnyHandle const& condition) const { return with("visible", condition); }

    /// @brief Whether the node is enabled.
    /// @param condition The condition.
    /// @return The node with it.
    [[nodiscard]] Node enabled(AnyHandle const& condition) const { return with("enabled", condition); }

    /// @brief A tooltip.
    /// @param text The text.
    /// @return The node with it.
    [[nodiscard]] Node tooltip(AnyHandle const& text) const { return with("tooltip", text); }

    /// @brief The width's sizing: `"content"`, `{"fixed": n}` or `{"stretch": w}`, such as `content()`, `fixed(n)`
    ///        or `stretch()` give.
    /// @param sizing The sizing.
    /// @return The node with it.
    [[nodiscard]] Node sizing(AnyHandle const& sizing) const { return with("sizing", sizing); }

    /// @brief The sizing of both dimensions, as a `{width, height}` record; an absent one is content. Literal
    ///        sizings (`content()`, `fixed(n)`, `stretch(w)`) make a literal record, which the loader checks; another
    ///        is evaluated at mount.
    /// @param width  The width's sizing.
    /// @param height The height's sizing.
    /// @return The node with it.
    [[nodiscard]] Node sizing(AnyHandle const& width, AnyHandle const& height) const {
        std::optional<Json> const literalWidth = width.present() ? detail::literalOf(width.json()) : Json{};
        std::optional<Json> const literalHeight = height.present() ? detail::literalOf(height.json()) : Json{};
        Json record = Json::object_t{};
        if (literalWidth && literalHeight) {
            if (width.present()) {
                detail::put(record, "width", *literalWidth);
            }
            if (height.present()) {
                detail::put(record, "height", *literalHeight);
            }
            return with("sizing", AnyHandle::raw(detail::single("lit", std::move(record))));
        }
        if (width.present()) {
            detail::put(record, "width", width.json());
        }
        if (height.present()) {
            detail::put(record, "height", height.json());
        }
        return with("sizing", AnyHandle::raw(detail::single("obj", std::move(record))));
    }

    /// @brief Where the node sits in its row, column or grid, in place of the container's `align`.
    /// @param align The alignment.
    /// @return The node with it.
    [[nodiscard]] Node alignSelf(Align align) const { return withText("alignSelf", detail::alignName(align)); }

    /// @brief The least width, in backend units.
    /// @param units The bound, 0 or more.
    /// @return The node with it.
    [[nodiscard]] Node minWidth(int units) const { return with("minWidth", AnyHandle{std::int64_t{units}}); }

    /// @brief The most width, in backend units.
    /// @param units The bound, 0 or more.
    /// @return The node with it.
    [[nodiscard]] Node maxWidth(int units) const { return with("maxWidth", AnyHandle{std::int64_t{units}}); }

    /// @brief The least height, in backend units.
    /// @param units The bound, 0 or more.
    /// @return The node with it.
    [[nodiscard]] Node minHeight(int units) const { return with("minHeight", AnyHandle{std::int64_t{units}}); }

    /// @brief The most height, in backend units.
    /// @param units The bound, 0 or more.
    /// @return The node with it.
    [[nodiscard]] Node maxHeight(int units) const { return with("maxHeight", AnyHandle{std::int64_t{units}}); }

    /// @brief The grid columns the node spans.
    /// @param columns The span.
    /// @return The node with it.
    [[nodiscard]] Node span(AnyHandle const& columns) const { return with("span", columns); }

    /// @brief Whether the node takes focus when it mounts.
    /// @param focus The condition.
    /// @return The node with it.
    [[nodiscard]] Node autofocus(AnyHandle const& focus) const { return with("autofocus", focus); }

    /// @brief A stable identifier for automated tests.
    /// @param name The identifier.
    /// @return The node with it.
    [[nodiscard]] Node testId(std::string const& name) const { return withText("testId", name); }

    /// @brief A named token set for the subtree.
    /// @param name The token set.
    /// @return The node with it.
    [[nodiscard]] Node surface(std::string const& name) const { return withText("surface", name); }

    /// @brief The accessible name and role.
    /// @param name The name.
    /// @param role The role; empty for the kind's own.
    /// @return The node with it.
    [[nodiscard]] Node a11y(AnyHandle const& name, std::string const& role = {}) const {
        Node out = *this;
        Json part = Json::object_t{};
        if (name.present()) {
            detail::put(part, "name", name.json());
        }
        if (!role.empty()) {
            detail::put(part, "role", role);
        }
        detail::put(out._json, "a11y", std::move(part));
        return out;
    }

    /// @brief Makes the node draggable.
    /// @param payload The payload a drop receives.
    /// @return The node with it.
    [[nodiscard]] Node drag(AnyHandle const& payload) const {
        Node out = *this;
        detail::put(out._json, "drag", detail::single("payload", payload.json()));
        return out;
    }

    /// @brief Makes the node a drop target.
    /// @param accepts The payloads it accepts.
    /// @param onDrop  The commands run with the payload as `event`.
    /// @return The node with it.
    [[nodiscard]] Node drop(AnyHandle const& accepts, Commands const& onDrop) const {
        Node out = *this;
        Json part = Json::object_t{};
        if (accepts.present()) {
            detail::put(part, "accepts", accepts.json());
        }
        detail::put(part, "onDrop", onDrop.json());
        detail::put(out._json, "drop", std::move(part));
        return out;
    }

    /// @brief A key binding.
    /// @param chord    The chord, such as `Ctrl+S`.
    /// @param commands The commands.
    /// @return The node with it.
    [[nodiscard]] Node key(std::string const& chord, Commands const& commands) const {
        Node out = *this;
        auto const entry = std::get<Json::object_t>(out._json.data).try_emplace(std::string{"keys"}, Json::object_t{});
        detail::put(entry.first->second, chord, commands.json());
        return out;
    }

    /// @brief Whether an input or text is read-only.
    /// @param condition The condition.
    /// @return The node with it.
    [[nodiscard]] Node readonly(AnyHandle const& condition) const { return with("readonly", condition); }

    /// @brief Whether an input or text is required.
    /// @param condition The condition.
    /// @return The node with it.
    [[nodiscard]] Node required(AnyHandle const& condition) const { return with("required", condition); }

    /// @brief Errors shown on an input or text.
    /// @param errors The errors.
    /// @return The node with them.
    [[nodiscard]] Node errors(AnyHandle const& errors) const { return with("errors", errors); }

    /// @brief Whether an input or text shows a value that is being recalculated.
    /// @param condition The condition.
    /// @return The node with it.
    [[nodiscard]] Node stale(AnyHandle const& condition) const { return with("stale", condition); }

private:
    Json _json = Json::object_t{};
    CatalogFacts _facts;
};

inline Nodes::Nodes(Node const& node) { add(node); }

inline Nodes::Nodes(std::initializer_list<Node> nodes) {
    _items.reserve(nodes.size());
    for (Node const& node : nodes) {
        add(node);
    }
}

inline Nodes::Nodes(std::vector<Node> const& nodes) {
    _items.reserve(nodes.size());
    for (Node const& node : nodes) {
        add(node);
    }
}

inline void Nodes::add(Node const& node) {
    _items.push_back(node.json());
    _facts.merge(node.facts());
}

namespace detail {

/// @brief A document type name for a C++ type, as `params` and `state` declare it.
/// @tparam T The C++ type.
/// @return `bool`, `int`, `string`, `decimal`, `quantity`, `timestamp`, `list<T>`, `T?`, or `record` for any
///         other class.
template <typename T>
[[nodiscard]] std::string typeName() {
    if constexpr (IsOptional<T>::value) {
        return typeName<typename T::value_type>() + "?";
    } else if constexpr (IsVector<T>::value) {
        return "list<" + typeName<typename T::value_type>() + ">";
    } else if constexpr (std::same_as<T, bool>) {
        return "bool";
    } else if constexpr (std::integral<T>) {
        return "int";
    } else if constexpr (std::same_as<T, std::string>) {
        return "string";
    } else if constexpr (std::same_as<T, math::Rational> || std::same_as<T, Decimal>) {
        return "decimal";
    } else if constexpr (std::same_as<T, Quantity>) {
        return "quantity";
    } else if constexpr (std::same_as<T, time::DateTime>) {
        return "timestamp";
    } else {
        return "record";
    }
}

}  // namespace detail

/// @brief A scope's declarations (spec 5 §3): model aliases, state, `let`, queries, mutations, watches and checks.
///        A row, a dialog, a drawer and a `switch` case declare theirs on the scope their builder hands over.
class DeclScope {
public:
    /// @brief An empty scope.
    DeclScope() = default;

    /// @brief A model alias.
    /// @tparam Model The model type; `::morph::model::ModelTraits<Model>` names it.
    /// @param alias   The alias.
    /// @param options How it binds.
    /// @return The alias's handle.
    template <typename Model>
    ModelHandle<Model> model(std::string const& alias, ModelOptions const& options = {}) {
        Json decl = Json::object_t{};
        detail::put(decl, "type", std::string{::morph::model::ModelTraits<Model>::typeId()});
        if (auto instance = options.instance.json()) {
            detail::put(decl, "instance", std::move(*instance));
        }
        detail::put(_models, alias, std::move(decl));
        _facts.model<Model>();
        return ModelHandle<Model>{alias};
    }

    /// @brief State with a declared type and no initial value; it starts `null`.
    /// @tparam T The state's type.
    /// @param name The state's name.
    /// @return The read of the state.
    template <typename T>
    Handle<T> state(std::string const& name) {
        return declareState<T>(name, {});
    }

    /// @brief State initialised from an expression; its type is `T`'s.
    /// @tparam T The state's type.
    /// @param name The state's name.
    /// @param init The initial value.
    /// @return The read of the state.
    template <typename T>
    Handle<T> state(std::string const& name, Handle<T> const& init) {
        return declareState<T>(name, init);
    }

    /// @brief State initialised from a constant; its type is the constant's.
    /// @tparam L The constant's C++ type.
    /// @param name The state's name.
    /// @param init The initial value.
    /// @return The read of the state.
    template <typename L>
        requires detail::Literal<L>
    Handle<detail::LiteralT<L>> state(std::string const& name, L const& init) {
        return declareState<detail::LiteralT<L>>(name, Handle<detail::LiteralT<L>>{init});
    }

    /// @brief A derived value (`let`).
    /// @tparam T The value type.
    /// @param name  The name.
    /// @param value The expression.
    /// @return The read of the name.
    template <typename T>
    Handle<T> let(std::string const& name, Handle<T> const& value) {
        detail::put(_lets, name, value.json());
        return author::ref<T>(name);
    }

    /// @brief A query (spec 5 §5).
    /// @tparam Action The action it runs.
    /// @tparam Model  The model type of the alias; it must run the action.
    /// @param name    The query's name.
    /// @param model   The alias it goes through.
    /// @param body    The body, which is the query's key.
    /// @param options The gate and timing.
    /// @return The query's handle.
    template <typename Action, typename Model>
        requires Runs<Model, Action>
    QueryHandle<Action> query(std::string const& name, ModelHandle<Model> const& model, Body<Action> const& body = {},
                              QueryOptions const& options = {}) {
        declareQuery<Model, Action>(name, model.alias(), "action", body, options);
        return QueryHandle<Action>{name};
    }

    /// @brief A query that evaluates an action without running it: the server returns its calculated fields and
    ///        field errors (spec 5 §7).
    /// @tparam Action The action it evaluates.
    /// @tparam Model  The model type of the alias; it must run the action.
    /// @param name    The query's name.
    /// @param model   The alias it goes through.
    /// @param body    The body, which is the query's key.
    /// @param options The gate and timing.
    /// @return The query's handle. Its value is the action body as the server reconciled it, computed fields
    ///         filled, with `errors` and `ready` beside the fields.
    template <typename Action, typename Model>
        requires Runs<Model, Action>
    QueryHandle<Action, Action> evaluate(std::string const& name, ModelHandle<Model> const& model,
                                         Body<Action> const& body = {}, QueryOptions const& options = {}) {
        declareQuery<Model, Action>(name, model.alias(), "evaluate", body, options);
        return QueryHandle<Action, Action>{name};
    }

    /// @brief A mutation (spec 5 §5).
    /// @tparam Action The action it runs.
    /// @tparam Model  The model type of the alias; it must run the action.
    /// @param name    The mutation's name.
    /// @param model   The alias it goes through.
    /// @param options Its body, concurrency, optimistic write and command lists.
    /// @return The mutation's handle.
    template <typename Action, typename Model>
        requires Runs<Model, Action>
    MutationHandle<Action> mutation(std::string const& name, ModelHandle<Model> const& model,
                                    MutationOptions<Action> const& options = {}) {
        Json decl = Json::object_t{};
        detail::put(decl, "model", model.alias());
        detail::put(decl, "action", std::string{::morph::model::ActionTraits<Action>::typeId()});
        if (!options.body.empty()) {
            detail::put(decl, "body", options.body.json());
        }
        writeMutation(decl, options.concurrency, options.serialKey, options.optimistic, options.onSuccess,
                      options.onError, options.offlineQueue);
        detail::put(_mutations, name, std::move(decl));
        _facts.action<Model, Action>();
        return MutationHandle<Action>{name};
    }

    /// @brief A cascade: commands that run when a value changes (spec 5 §6).
    /// @param watched  The value watched.
    /// @param commands The commands.
    /// @param source   Which changes run it.
    void watch(AnyHandle const& watched, Commands const& commands, Source source = Source::Any) {
        Json decl = Json::object_t{};
        detail::put(decl, "on", watched.json());
        if (source == Source::User) {
            detail::put(decl, "source", std::string{"user"});
        }
        detail::put(decl, "do", commands.json());
        _watches.push_back(std::move(decl));
    }

    /// @brief A validation across elements (spec 5 §6).
    /// @param options The check.
    void check(CheckOptions const& options) {
        Json decl = Json::object_t{};
        detail::put(decl, "id", options.name);
        detail::put(decl, "assert", options.assertion.json());
        if (options.message.present()) {
            detail::put(decl, "message", options.message.json());
        }
        if (!options.targets.empty()) {
            Json::array_t targets;
            targets.reserve(options.targets.size());
            for (Element const& target : options.targets) {
                targets.emplace_back(target.name());
            }
            detail::put(decl, "targets", std::move(targets));
        }
        if (options.blocking) {
            detail::put(decl, "blocking", true);
        }
        _checks.push_back(std::move(decl));
    }

    /// @brief Whether the scope declares nothing.
    /// @return True when it declares nothing.
    [[nodiscard]] bool empty() const {
        std::size_t const declared = detail::members(_models) + detail::members(_state) + detail::members(_lets) +
                                     detail::members(_queries) + detail::members(_mutations) + _watches.size() +
                                     _checks.size();
        return declared == 0;
    }

    /// @brief Writes the declarations into a document or a `"scope"` object, in the document's order.
    /// @param out The object to write into.
    void writeTo(Json& out) const {
        detail::putSection(out, "models", _models);
        detail::putSection(out, "state", _state);
        detail::putSection(out, "let", _lets);
        detail::putSection(out, "queries", _queries);
        detail::putSection(out, "mutations", _mutations);
        if (!_watches.empty()) {
            detail::put(out, "watch", _watches);
        }
        if (!_checks.empty()) {
            detail::put(out, "checks", _checks);
        }
    }

    /// @brief The catalog facts of the models and actions the scope names.
    /// @return The facts.
    [[nodiscard]] CatalogFacts const& facts() const noexcept { return _facts; }

protected:
    /// @brief The catalog facts, for a scope that records more.
    /// @return The facts.
    [[nodiscard]] CatalogFacts& recordedFacts() noexcept { return _facts; }

private:
    template <typename T>
    Handle<T> declareState(std::string const& name, Handle<T> const& init) {
        Json decl = Json::object_t{};
        if constexpr (!std::same_as<T, Any>) {
            detail::put(decl, "type", detail::typeName<T>());
        }
        if (init.present()) {
            detail::put(decl, "init", init.json());
        }
        detail::put(_state, name, std::move(decl));
        return author::ref<T>(name);
    }

    template <typename Model, typename Action>
    void declareQuery(std::string const& name, std::string const& alias, char const* actionKey,
                      Body<Action> const& body, QueryOptions const& options) {
        Json decl = Json::object_t{};
        detail::put(decl, "model", alias);
        detail::put(decl, actionKey, std::string{::morph::model::ActionTraits<Action>::typeId()});
        if (!body.empty()) {
            detail::put(decl, "body", body.json());
        }
        if (options.when.present()) {
            detail::put(decl, "when", options.when.json());
        }
        auto duration = [&decl](char const* key, std::chrono::milliseconds value) {
            if (value.count() != 0) {
                detail::put(decl, key, value.count());
            }
        };
        duration("debounce", options.debounce);
        duration("refreshEvery", options.refreshEvery);
        duration("staleTime", options.staleTime);
        detail::put(_queries, name, std::move(decl));
        _facts.action<Model, Action>();
    }
    static void writeMutation(Json& decl, Mode concurrency, AnyHandle const& serialKey,
                              std::optional<Optimistic> const& optimistic, Commands const& onSuccess,
                              Commands const& onError, bool offlineQueue) {
        static constexpr std::array<char const*, 4> kModes = {"exclusive", "serial", "latest", "parallel"};
        if (concurrency != Mode::Exclusive) {
            detail::put(decl, "concurrency", std::string{kModes.at(static_cast<std::size_t>(concurrency))});
        }
        if (serialKey.present()) {
            detail::put(decl, "serialKey", serialKey.json());
        }
        if (optimistic) {
            if (!optimistic->query) {
                throw AuthoringError{"an `optimistic` write names the query whose row it writes"};
            }
            Json part = Json::object_t{};
            detail::put(part, "query", optimistic->query->name());
            if (!optimistic->at.empty()) {
                detail::put(part, "at", optimistic->at);
            }
            detail::put(part, "key", optimistic->key.json());
            detail::put(part, "with", optimistic->with.json());
            detail::put(decl, "optimistic", std::move(part));
        }
        if (!onSuccess.empty()) {
            detail::put(decl, "onSuccess", onSuccess.json());
        }
        if (!onError.empty()) {
            detail::put(decl, "onError", onError.json());
        }
        if (offlineQueue) {
            detail::put(decl, "offline", std::string{"queue"});
        }
    }

    Json _models = Json::object_t{};
    Json _state = Json::object_t{};
    Json _lets = Json::object_t{};
    Json _queries = Json::object_t{};
    Json _mutations = Json::object_t{};
    Json::array_t _watches;
    Json::array_t _checks;
    CatalogFacts _facts;
};

/// @brief The scope a row of a `table` or `forEach` declares its own state, queries and model aliases on.
using RowScope = DeclScope;

/// @brief When a scope's element errors show (spec 5 §6).
enum class ShowErrors : std::uint8_t {
    Touched,  ///< Once the element has been touched.
    Submit,   ///< Once the form or dialog has been submitted.
    Always,   ///< At once.
};

/// @brief The spelling of a `ShowErrors`.
/// @param when The policy.
/// @return `touched`, `submit` or `always`.
[[nodiscard]] inline std::string showErrorsName(ShowErrors when) {
    static constexpr std::array<char const*, 3> kNames = {"touched", "submit", "always"};
    return kNames.at(static_cast<std::size_t>(when));
}

/// @brief A prop type of a component: how a value crosses into QML (spec 5 §10).
enum class PropType : std::uint8_t {
    String,  ///< Text.
    Bool,    ///< A boolean.
    Int,     ///< An integer, as text.
    Exact,   ///< An exact decimal or quantity, as text.
    Number,  ///< A double, for charts.
    Json,    ///< Structured data, exact values inside it as text.
};

/// @brief A custom or host component (spec 5 §10).
struct ComponentOptions {
    /// @brief The content hash of a delivered component, `sha256:<64 hex digits>`; empty for a host component.
    std::string qml;
    /// @brief The name of a host component the client carries; empty for a delivered one.
    std::string host;
    /// @brief The host component's version.
    std::int64_t version = 0;
    /// @brief The props and their types.
    std::vector<std::pair<std::string, PropType>> props;
    /// @brief The events and their payload types.
    std::vector<std::pair<std::string, std::string>> events;
    /// @brief The built-in subtree a renderer shows when it cannot load the component; it reads the props through
    ///        `prop(name)`.
    std::optional<Node> fallback;
};

/// @brief A declared component, for `custom` nodes.
class ComponentRef {
public:
    /// @brief A component by name.
    /// @param name The component's name.
    explicit ComponentRef(std::string name) : _name{std::move(name)} {}

    /// @brief The component's name.
    /// @return The name.
    [[nodiscard]] std::string const& name() const noexcept { return _name; }

private:
    std::string _name;
};

/// @brief A customization point (spec 5 §12b).
class Point {
public:
    /// @brief A point by name.
    /// @param name The point's name.
    explicit Point(std::string name) : _name{std::move(name)} {}

    /// @brief The point's name.
    /// @return The name.
    [[nodiscard]] std::string const& name() const noexcept { return _name; }

    /// @brief The user's choice at the point.
    /// @param path A path inside the choice; empty for the whole.
    /// @return `{"pref": "point.path"}`.
    [[nodiscard]] AnyHandle pref(std::string const& path = {}) const {
        return AnyHandle::raw(detail::single("pref", path.empty() ? _name : _name + "." + path));
    }

private:
    std::string _name;
};

/// @brief A status badge of the screen's head (`screen.status`).
struct HeadBadgeOptions {
    /// @brief The badge's text.
    AnyHandle text;
    /// @brief Its tone: `neutral`, `info`, `ok`, `warn` or `err`.
    AnyHandle tone;
    /// @brief The name of an icon.
    AnyHandle icon;
    /// @brief Whether it is shown; absent shows it.
    AnyHandle visible;
};

/// @brief A button of a screen's head (`screen.action`), or of a dialog's or drawer's footer.
struct ActionButtonOptions {
    /// @brief The action's id, unique in its list: a name.
    std::string id;
    /// @brief The label; required.
    AnyHandle label;
    /// @brief The name of an icon.
    AnyHandle icon;
    /// @brief How much it stands out; `Normal` writes no key.
    ButtonEmphasis emphasis = ButtonEmphasis::Normal;
    /// @brief Whether it can be run; absent enables it.
    AnyHandle enabled;
    /// @brief Whether it is shown; absent shows it.
    AnyHandle visible;
    /// @brief Commands run when it is clicked, in the scope its list belongs to.
    Commands onClick;
};

namespace detail {

/// @brief An expression key of a part, left out when absent.
/// @param json  The part.
/// @param key   The key.
/// @param value The expression.
inline void putPresent(Json& json, char const* key, AnyHandle const& value) {
    if (value.present()) {
        detail::put(json, key, value.json());
    }
}

/// @brief A `headBadge` part's JSON.
/// @param badge The badge.
/// @return The JSON.
[[nodiscard]] inline Json headBadge(HeadBadgeOptions const& badge) {
    Json json = Json::object_t{};
    putPresent(json, "tone", badge.tone);
    putPresent(json, "text", badge.text);
    putPresent(json, "icon", badge.icon);
    putPresent(json, "visible", badge.visible);
    return json;
}

/// @brief An `actionButton` part's JSON.
/// @param action The button.
/// @return The JSON.
[[nodiscard]] inline Json actionButton(ActionButtonOptions const& action) {
    Json json = Json::object_t{};
    detail::put(json, "id", action.id);
    putPresent(json, "label", action.label);
    putPresent(json, "icon", action.icon);
    if (action.emphasis == ButtonEmphasis::Primary) {
        detail::put(json, "emphasis", std::string{"primary"});
    } else if (action.emphasis == ButtonEmphasis::Danger) {
        detail::put(json, "emphasis", std::string{"danger"});
    }
    putPresent(json, "enabled", action.enabled);
    putPresent(json, "visible", action.visible);
    if (!action.onClick.empty()) {
        detail::put(json, "onClick", action.onClick.json());
    }
    return json;
}

/// @brief Appends @p item to the array at @p key of @p json, making the array on first use.
/// @param json The object.
/// @param key  The key.
/// @param item The item.
inline void append(Json& json, char const* key, Json item) {
    auto& object = std::get<Json::object_t>(json.data);
    Json::object_t::iterator found = object.find(key);
    if (found == object.end()) {
        detail::put(json, key, Json::array_t{});
        found = object.find(key);
    }
    std::get<Json::array_t>(found->second.data).push_back(std::move(item));
}

}  // namespace detail

/// @brief The screen's own scope: its declarations, params, lifecycle keys, components and customization points.
class Scope : public DeclScope {
public:
    /// @brief A typed param, set by navigation.
    /// @tparam T The param's type.
    /// @param name The param's name.
    /// @return The read of the param.
    template <typename T>
    Handle<T> param(std::string const& name) {
        detail::put(_params, name, detail::typeName<T>());
        return author::ref<T>(name);
    }

    /// @brief The screen's title.
    /// @param title The expression.
    void title(AnyHandle const& title) { detail::put(_head, "title", title.json()); }

    /// @brief The screen's identity: two screens with equal identities are one.
    /// @param identity The expression.
    void identity(AnyHandle const& identity) { detail::put(_head, "identity", identity.json()); }

    /// @brief Whether the screen has unsaved changes.
    /// @param dirty The condition.
    void dirty(AnyHandle const& dirty) { detail::put(_head, "dirty", dirty.json()); }

    /// @brief The head's line under the title.
    /// @param subtitle The expression.
    void subtitle(AnyHandle const& subtitle) { detail::put(_head, "subtitle", subtitle.json()); }

    /// @brief Appends a status badge to the head.
    /// @param badge The badge.
    void status(HeadBadgeOptions const& badge) { detail::append(_head, "status", detail::headBadge(badge)); }

    /// @brief Appends an action to the head, run in the screen scope.
    /// @param action The action; its id is unique among the head's actions.
    void action(ActionButtonOptions const& action) { detail::append(_head, "actions", detail::actionButton(action)); }

    /// @brief Commands run when the screen mounts.
    /// @param commands The commands.
    void onMount(Commands const& commands) { detail::put(_lifecycle, "onMount", commands.json()); }

    /// @brief Commands run when the screen unmounts.
    /// @param commands The commands.
    void onUnmount(Commands const& commands) { detail::put(_lifecycle, "onUnmount", commands.json()); }

    /// @brief Commands run when the screen is asked to close; `cancelClose()` keeps it open.
    /// @param commands The commands.
    void onClose(Commands const& commands) { detail::put(_lifecycle, "onClose", commands.json()); }

    /// @brief Commands run when the host asks the screen to save.
    /// @param commands The commands.
    void onSave(Commands const& commands) { detail::put(_lifecycle, "onSave", commands.json()); }

    /// @brief Commands run after a backend switch.
    /// @param commands The commands.
    void onBackendChange(Commands const& commands) { detail::put(_lifecycle, "onBackendChange", commands.json()); }

    /// @brief A screen-wide key binding.
    /// @param chord    The chord.
    /// @param commands The commands.
    void key(std::string const& chord, Commands const& commands) { detail::put(_keys, chord, commands.json()); }

    /// @brief When the screen's element errors show.
    /// @param when The policy.
    void showErrors(ShowErrors when) { _showErrors = showErrorsName(when); }

    /// @brief A query started before the screen mounts; its body reads only params.
    /// @param query The query.
    void prefetch(QueryRef const& query) { _prefetch.emplace_back(query.name()); }

    /// @brief A custom or host component.
    /// @param name    The component's name.
    /// @param options The component.
    /// @return The reference `custom` nodes name.
    ComponentRef component(std::string const& name, ComponentOptions const& options) {
        static constexpr std::array<char const*, 6> kPropTypes = {"string", "bool", "int", "exact", "number", "json"};
        Json decl = Json::object_t{};
        if (!options.qml.empty()) {
            detail::put(decl, "qml", options.qml);
        }
        if (!options.host.empty()) {
            detail::put(decl, "host", options.host);
            detail::put(decl, "version", options.version);
        }
        Json props = Json::object_t{};
        for (auto const& [prop, type] : options.props) {
            detail::put(props, prop, std::string{kPropTypes.at(static_cast<std::size_t>(type))});
        }
        detail::put(decl, "props", std::move(props));
        if (!options.events.empty()) {
            Json events = Json::object_t{};
            for (auto const& [event, type] : options.events) {
                detail::put(events, event, type);
            }
            detail::put(decl, "events", std::move(events));
        }
        if (options.fallback) {
            detail::put(decl, "fallback", options.fallback->json());
            recordedFacts().merge(options.fallback->facts());
        }
        detail::put(_components, name, std::move(decl));
        return ComponentRef{name};
    }

    /// @brief A `columns` point: the user reorders, hides and resizes a table's columns.
    /// @param name  The point's name.
    /// @param table The table.
    /// @return The point.
    Point columnsPoint(std::string const& name, Element const& table) {
        Json decl = Json::object_t{};
        detail::put(decl, "kind", std::string{"columns"});
        detail::put(decl, "of", table.name());
        return addPoint(name, std::move(decl));
    }

    /// @brief An `arrange` point: the user reorders and hides the children of a column or row, each of which has an
    ///        id.
    /// @param name      The point's name.
    /// @param container The column or row.
    /// @return The point.
    Point arrangePoint(std::string const& name, Element const& container) {
        Json decl = Json::object_t{};
        detail::put(decl, "kind", std::string{"arrange"});
        detail::put(decl, "of", container.name());
        return addPoint(name, std::move(decl));
    }

    /// @brief A `choice` point: one of a list of options.
    /// @param name     The point's name.
    /// @param options  The options.
    /// @param fallback The default; empty for none.
    /// @return The point.
    Point choicePoint(std::string const& name, std::vector<std::string> const& options,
                      std::string const& fallback = {}) {
        Json decl = Json::object_t{};
        detail::put(decl, "kind", std::string{"choice"});
        detail::put(decl, "options", Json::array_t(options.begin(), options.end()));
        if (!fallback.empty()) {
            detail::put(decl, "default", fallback);
        }
        return addPoint(name, std::move(decl));
    }

    /// @brief A `value` point: one typed setting.
    /// @tparam T The setting's type.
    /// @param name     The point's name.
    /// @param fallback The default.
    /// @return The point.
    template <typename T>
    Point valuePoint(std::string const& name, Value const& fallback = {}) {
        Json decl = Json::object_t{};
        detail::put(decl, "kind", std::string{"value"});
        detail::put(decl, "type", detail::typeName<T>());
        detail::put(decl, "default", valueToJson(fallback));
        return addPoint(name, std::move(decl));
    }

    /// @brief A `layout` or `saved` point.
    /// @param name The point's name.
    /// @param kind `layout` or `saved`.
    /// @return The point.
    Point point(std::string const& name, std::string const& kind) {
        Json decl = Json::object_t{};
        detail::put(decl, "kind", kind);
        return addPoint(name, std::move(decl));
    }

    /// @brief The document's JSON with a view.
    /// @param documentId The document id.
    /// @param view       The view.
    /// @return The document.
    [[nodiscard]] Json document(std::string const& documentId, Node const& view) const {
        Json out = Json::object_t{};
        detail::put(out, "vocab", std::string{kVocabulary});
        detail::put(out, "id", documentId);
        for (auto const& [key, value] : std::get<Json::object_t>(_head.data)) {
            detail::put(out, key, value);
        }
        detail::putSection(out, "params", _params);
        writeTo(out);
        for (auto const& [key, value] : std::get<Json::object_t>(_lifecycle.data)) {
            detail::put(out, key, value);
        }
        detail::putSection(out, "keys", _keys);
        if (!_showErrors.empty()) {
            detail::put(out, "showErrors", _showErrors);
        }
        if (!_prefetch.empty()) {
            detail::put(out, "prefetch", _prefetch);
        }
        detail::putSection(out, "components", _components);
        detail::putSection(out, "customize", _customize);
        detail::put(out, "view", view.json());
        return out;
    }

private:
    Point addPoint(std::string const& name, Json decl) {
        detail::put(_customize, name, std::move(decl));
        return Point{name};
    }

    Json _head = Json::object_t{};
    Json _params = Json::object_t{};
    Json _lifecycle = Json::object_t{};
    Json _keys = Json::object_t{};
    std::string _showErrors;
    Json::array_t _prefetch;
    Json _components = Json::object_t{};
    Json _customize = Json::object_t{};
};

// ── Commands ─────────────────────────────────────────────────────────────────────────────────────────────────────

namespace detail {

/// @brief A command with one key.
/// @param key   The command's key.
/// @param value The key's value.
/// @return The command.
[[nodiscard]] inline Command command(std::string_view key, Json value) {
    return Command{single(key, std::move(value))};
}

}  // namespace detail

/// @brief `set`: writes a state, an input's value or an app name.
/// @param target The state or element, as its read.
/// @param value  The value.
/// @return The command.
/// @throws AuthoringError when @p target is not a named read.
[[nodiscard]] inline Command set(AnyHandle const& target, AnyHandle const& value) {
    Json json = Json::object_t{};
    detail::put(json, "set", detail::refPath(target.json(), "`set`"));
    detail::put(json, "value", value.json());
    return Command{std::move(json)};
}

/// @brief `reset`: a state to its init, or a dialog's scope.
/// @param target The state, as its read.
/// @return The command.
/// @throws AuthoringError when @p target is not a named read.
[[nodiscard]] inline Command reset(AnyHandle const& target) {
    return detail::command("reset", detail::refPath(target.json(), "`reset`"));
}

/// @brief Options of `run`.
struct RunOptions {
    /// @brief A list to run the mutation once per element of, as one batch.
    AnyHandle each;
    /// @brief With `each`, the element's name inside the body; without it, the name the reply is bound to for the
    ///        later steps of the enclosing `seq`.
    std::string as;
};

/// @brief `run`: runs a mutation.
/// @tparam Action The mutation's action.
/// @param mutation The mutation.
/// @param body     The body; empty for the mutation's own.
/// @param options  `each` and `as`.
/// @return The command.
template <typename Action>
[[nodiscard]] Command run(MutationHandle<Action> const& mutation, Body<Action> const& body = {},
                          RunOptions const& options = {}) {
    Json json = Json::object_t{};
    detail::put(json, "run", mutation.name());
    if (!body.empty()) {
        detail::put(json, "body", body.json());
    }
    if (options.each.present()) {
        detail::put(json, "each", options.each.json());
    }
    if (!options.as.empty()) {
        detail::put(json, "as", options.as);
    }
    return Command{std::move(json)};
}

/// @brief `refetch`: refetches a query.
/// @param query The query.
/// @return The command.
[[nodiscard]] inline Command refetch(QueryRef const& query) { return detail::command("refetch", query.name()); }

/// @brief `open`: opens a dialog or drawer.
/// @param dialog The dialog.
/// @return The command.
[[nodiscard]] inline Command open(Element const& dialog) { return detail::command("open", dialog.name()); }

/// @brief `close`: closes a dialog or drawer.
/// @param dialog The dialog.
/// @return The command.
[[nodiscard]] inline Command close(Element const& dialog) { return detail::command("close", dialog.name()); }

/// @brief `close` of the dialog or drawer the command list runs in.
/// @param current `self`.
/// @return The command.
[[nodiscard]] inline Command close(Self current) {
    static_cast<void>(current);
    return detail::command("close", std::string{"self"});
}

/// @brief `focus`: moves the keyboard focus to an element.
/// @param target The element.
/// @return The command.
[[nodiscard]] inline Command focus(Element const& target) { return detail::command("focus", target.name()); }

/// @brief How `navigate` opens a screen.
enum class OpenAs : std::uint8_t {
    Default,  ///< The host's choice.
    Tab,      ///< A tab.
    Dialog,   ///< A dialog, which returns through `onResult` or `onCancel`.
    Drawer,   ///< A drawer.
    Window,   ///< A window.
};

/// @brief Options of `navigate`.
struct NavigateOptions {
    /// @brief The screen's params, a record.
    AnyHandle params;
    /// @brief How the screen opens.
    OpenAs as = OpenAs::Default;
    /// @brief Commands run with the value a dialog returns, as `event`.
    Commands onResult;
    /// @brief Commands run when a dialog is cancelled.
    Commands onCancel;
};

namespace detail {

/// @brief Writes `navigate`'s options.
/// @param json    The command.
/// @param options The options.
/// @return The command.
[[nodiscard]] inline Command navigation(Json json, NavigateOptions const& options) {
    static constexpr std::array<char const*, 5> kModes = {"", "tab", "dialog", "drawer", "window"};
    if (options.params.present()) {
        detail::put(json, "params", options.params.json());
    }
    if (options.as != OpenAs::Default) {
        detail::put(json, "as", std::string{kModes.at(static_cast<std::size_t>(options.as))});
    }
    if (!options.onResult.empty()) {
        detail::put(json, "onResult", options.onResult.json());
    }
    if (!options.onCancel.empty()) {
        detail::put(json, "onCancel", options.onCancel.json());
    }
    return Command{std::move(json)};
}

}  // namespace detail

/// @brief `navigate` to a screen by id.
/// @param screenId The screen.
/// @param options  Params, how it opens, and what runs when it returns.
/// @return The command.
[[nodiscard]] inline Command navigate(std::string const& screenId, NavigateOptions const& options = {}) {
    return detail::navigation(detail::single("navigate", screenId), options);
}

/// @brief `navigate` to a route.
/// @param route   The route expression.
/// @param options Params, how it opens, and what runs when it returns.
/// @return The command.
[[nodiscard]] inline Command navigateRoute(AnyHandle const& route, NavigateOptions const& options = {}) {
    return detail::navigation(detail::single("navigate", detail::single("route", route.json())), options);
}

/// @brief `return`: a dialog screen's result.
/// @param value The value.
/// @return The command.
[[nodiscard]] inline Command returnWith(AnyHandle const& value) { return detail::command("return", value.json()); }

/// @brief `host`: a command the application registered with the client.
/// @param name The command.
/// @param args Its arguments; absent for none.
/// @return The command.
[[nodiscard]] inline Command host(std::string const& name, AnyHandle const& args = {}) {
    Json json = Json::object_t{};
    detail::put(json, "host", name);
    if (args.present()) {
        detail::put(json, "args", args.json());
    }
    return Command{std::move(json)};
}

/// @brief `switchBackend`: switches to a named backend.
/// @param name The backend.
/// @return The command.
[[nodiscard]] inline Command switchBackend(std::string const& name) { return detail::command("switchBackend", name); }

/// @brief `resetPrefs`: resets a customization point to the document's default.
/// @param point The point.
/// @return The command.
[[nodiscard]] inline Command resetPrefs(Point const& point) { return detail::command("resetPrefs", point.name()); }

/// @brief `resetPrefs` of every point of the screen.
/// @return The command.
[[nodiscard]] inline Command resetAllPrefs() { return detail::command("resetPrefs", std::string{"all"}); }

/// @brief `setLocale`: switches the client's locale.
/// @param locale The locale, such as `de`.
/// @return The command.
[[nodiscard]] inline Command setLocale(std::string const& locale) { return detail::command("setLocale", locale); }

/// @brief The role of a notification.
enum class Role : std::uint8_t {
    Info,     ///< Information.
    Success,  ///< A success.
    Warning,  ///< A warning.
    Error,    ///< An error.
};

/// @brief `notify`: shows a notification.
/// @param message The message.
/// @param role    Its role.
/// @return The command.
[[nodiscard]] inline Command notify(AnyHandle const& message, Role role = Role::Info) {
    static constexpr std::array<char const*, 4> kRoles = {"info", "success", "warning", "error"};
    Json json = Json::object_t{};
    detail::put(json, "notify", message.json());
    if (role != Role::Info) {
        detail::put(json, "role", std::string{kRoles.at(static_cast<std::size_t>(role))});
    }
    return Command{std::move(json)};
}

/// @brief `seq`: steps in order; a failed step stops the rest.
/// @param steps The steps.
/// @return The command.
[[nodiscard]] inline Command seq(Commands const& steps) { return detail::command("seq", steps.json()); }

/// @brief `all`: steps started together.
/// @param steps The steps.
/// @return The command.
[[nodiscard]] inline Command all(Commands const& steps) { return detail::command("all", steps.json()); }

/// @brief `if`: commands chosen by a condition.
/// @param condition The condition.
/// @param then      The commands when it is true.
/// @param otherwise The commands otherwise; empty runs nothing.
/// @return The command.
[[nodiscard]] inline Command ifThen(AnyHandle const& condition, Commands const& then, Commands const& otherwise = {}) {
    Json::array_t parts{condition.json(), then.json()};
    if (!otherwise.empty()) {
        parts.push_back(otherwise.json());
    }
    return detail::command("if", std::move(parts));
}

/// @brief `cancelClose`: keeps a screen open, inside `onClose`.
/// @return The command.
[[nodiscard]] inline Command cancelClose() { return detail::command("cancelClose", true); }

/// @brief `retry`: refetches every failed query of a boundary, inside its `error`.
/// @return The command.
[[nodiscard]] inline Command retry() { return detail::command("retry", true); }

/// @brief `signIn`: runs a sign-in mutation and installs the token it returns.
/// @param mutation The mutation.
/// @return The command.
[[nodiscard]] inline Command signIn(MutationRef const& mutation) {
    return detail::command("signIn", detail::single("from", mutation.name()));
}

/// @brief `signOut`.
/// @return The command.
[[nodiscard]] inline Command signOut() { return detail::command("signOut", true); }

/// @brief Options of `upload`.
struct UploadOptions {
    /// @brief The name the reply is bound to for later steps.
    std::string as;
    /// @brief The transfer id, whose `progress`, `pending` and `error` are element values.
    std::string transfer;
};

/// @brief `upload`: sends a file through a mutation (`files/1`).
/// @param file     The file reference.
/// @param mutation The mutation.
/// @param options  `as` and the transfer id.
/// @return The command.
[[nodiscard]] inline Command upload(AnyHandle const& file, MutationRef const& mutation,
                                    UploadOptions const& options = {}) {
    Json json = Json::object_t{};
    detail::put(json, "upload", file.json());
    detail::put(json, "to", mutation.name());
    if (!options.as.empty()) {
        detail::put(json, "as", options.as);
    }
    if (!options.transfer.empty()) {
        detail::put(json, "id", options.transfer);
    }
    return Command{std::move(json)};
}

/// @brief `download` to a file the user picks.
/// @param file The file reference.
/// @param name The suggested name; absent for none.
/// @return The command.
[[nodiscard]] inline Command downloadToFile(AnyHandle const& file, AnyHandle const& name = {}) {
    Json json = Json::object_t{};
    detail::put(json, "download", file.json());
    detail::put(json, "save", std::string{"dialog"});
    if (name.present()) {
        detail::put(json, "name", name.json());
    }
    return Command{std::move(json)};
}

/// @brief `download` to a preview URL, bound to a state.
/// @param file  The file reference.
/// @param state The state the URL is written to, as its read.
/// @return The command.
/// @throws AuthoringError when @p state is not a named read.
[[nodiscard]] inline Command downloadToPreview(AnyHandle const& file, AnyHandle const& state) {
    Json json = Json::object_t{};
    detail::put(json, "download", file.json());
    detail::put(json, "to", std::string{"preview"});
    detail::put(json, "as", detail::refPath(state.json(), "a download to preview"));
    return Command{std::move(json)};
}

// ── View nodes ───────────────────────────────────────────────────────────────────────────────────────────────────

namespace detail {

/// @brief Builds a node's JSON key by key, leaving out what is absent.
class NodeWriter {
public:
    /// @brief A node of a kind.
    /// @param kind The kind.
    explicit NodeWriter(std::string_view kind) { detail::put(_json, "kind", std::string{kind}); }

    /// @brief An expression property.
    /// @param key   The key.
    /// @param value The expression.
    /// @return This writer.
    NodeWriter& put(char const* key, AnyHandle const& value) {
        if (value.present()) {
            detail::put(_json, key, value.json());
        }
        return *this;
    }

    /// @brief A command list.
    /// @param key      The key.
    /// @param commands The commands.
    /// @return This writer.
    NodeWriter& put(char const* key, Commands const& commands) {
        if (!commands.empty()) {
            detail::put(_json, key, commands.json());
        }
        return *this;
    }

    /// @brief A node list.
    /// @param key   The key.
    /// @param nodes The nodes.
    /// @return This writer.
    NodeWriter& put(char const* key, Nodes const& nodes) {
        if (!nodes.empty()) {
            detail::put(_json, key, nodes.json());
            _facts.merge(nodes.facts());
        }
        return *this;
    }

    /// @brief A plain string.
    /// @param key   The key.
    /// @param value The string; empty leaves it out.
    /// @return This writer.
    NodeWriter& text(char const* key, std::string const& value) {
        if (!value.empty()) {
            detail::put(_json, key, value);
        }
        return *this;
    }

    /// @brief Any JSON.
    /// @param key   The key.
    /// @param value The JSON.
    /// @return This writer.
    NodeWriter& raw(char const* key, Json value) {
        detail::put(_json, key, std::move(value));
        return *this;
    }

    /// @brief The declarations of the scope the node creates.
    /// @param scope The scope; one that declares nothing is left out.
    /// @return This writer.
    NodeWriter& scope(DeclScope const& scope) {
        _facts.merge(scope.facts());
        if (!scope.empty()) {
            Json decls = Json::object_t{};
            scope.writeTo(decls);
            detail::put(_json, "scope", std::move(decls));
        }
        return *this;
    }

    /// @brief Adds catalog facts.
    /// @param facts The facts.
    /// @return This writer.
    NodeWriter& facts(CatalogFacts const& facts) {
        _facts.merge(facts);
        return *this;
    }

    /// @brief The node.
    /// @return The node.
    [[nodiscard]] Node done() const { return Node{_json, _facts}; }

private:
    Json _json = Json::object_t{};
    CatalogFacts _facts;
};

}  // namespace detail

/// @brief Options of `text`.
struct TextOptions {
    /// @brief The text.
    AnyHandle text;
    /// @brief The typographic role, such as `heading`.
    AnyHandle role;
    /// @brief Whether the shown value is being recalculated.
    AnyHandle stale;
};

/// @brief A `text` node.
/// @param options The text.
/// @return The node.
[[nodiscard]] inline Node text(TextOptions const& options) {
    return detail::NodeWriter{"text"}
        .put("text", options.text)
        .put("role", options.role)
        .put("stale", options.stale)
        .done();
}

/// @brief Options of `button`.
struct ButtonOptions {
    /// @brief The label: the button's text and its accessible name.
    AnyHandle label;
    /// @brief An icon of the theme's icon map; it never names the button.
    AnyHandle icon;
    /// @brief How much it stands out; `Normal` writes no key.
    ButtonEmphasis emphasis = ButtonEmphasis::Normal;
    /// @brief Commands run on a click.
    Commands onClick;
    /// @brief Commands run on activation by keyboard.
    Commands onActivate;
    /// @brief A push button or a list entry; absent writes no key, a push button.
    std::optional<ButtonStyle> style;
    /// @brief Whether a list entry is the current one, a condition; only with `ButtonStyle::Item`, which the loader
    ///        checks.
    AnyHandle selected;
};

/// @brief A `button` node.
/// @param options The button.
/// @return The node.
[[nodiscard]] inline Node button(ButtonOptions const& options) {
    detail::NodeWriter writer{"button"};
    if (options.emphasis == ButtonEmphasis::Primary) {
        writer.put("emphasis", std::string{"primary"});
    } else if (options.emphasis == ButtonEmphasis::Danger) {
        writer.put("emphasis", std::string{"danger"});
    }
    if (options.style) {
        writer.text("style", *options.style == ButtonStyle::Item ? "item" : "push");
    }
    return std::move(writer)
        .put("label", options.label)
        .put("icon", options.icon)
        .put("selected", options.selected)
        .put("onClick", options.onClick)
        .put("onActivate", options.onActivate)
        .done();
}

/// @brief When a text input commits its draft.
enum class Commit : std::uint8_t {
    Change,  ///< On every edit.
    Blur,    ///< On loss of focus.
    Enter,   ///< On Enter.
};

/// @brief Options of `textInput`.
struct TextInputOptions {
    /// @brief The value.
    AnyHandle value;
    /// @brief The label: the accessible name.
    AnyHandle label;
    /// @brief Placeholder text.
    AnyHandle placeholder;
    /// @brief The input mode, such as `multiline` or `password`.
    AnyHandle mode;
    /// @brief An icon of the theme's icon map, drawn at the input's leading edge; empty shows none. Refused on a
    ///        multiline input.
    AnyHandle icon;
    /// @brief When the draft commits.
    std::optional<Commit> commitOn;
    /// @brief Commands run on every edit.
    Commands onChange;
    /// @brief Commands run on a commit.
    Commands onCommit;
    /// @brief Commands run on Enter in a single-line input.
    Commands onSubmit;
};

/// @brief A `textInput` node.
/// @param options The input.
/// @return The node.
[[nodiscard]] inline Node textInput(TextInputOptions const& options) {
    static constexpr std::array<char const*, 3> kCommits = {"change", "blur", "enter"};
    detail::NodeWriter node{"textInput"};
    node.put("value", options.value)
        .put("label", options.label)
        .put("placeholder", options.placeholder)
        .put("mode", options.mode)
        .put("icon", options.icon);
    if (options.commitOn) {
        node.text("commitOn", kCommits.at(static_cast<std::size_t>(*options.commitOn)));
    }
    return node.put("onChange", options.onChange)
        .put("onCommit", options.onCommit)
        .put("onSubmit", options.onSubmit)
        .done();
}

/// @brief Options of `checkbox`.
struct CheckboxOptions {
    /// @brief The label.
    AnyHandle label;
    /// @brief Whether it is checked.
    AnyHandle checked;
    /// @brief Commands run on a toggle.
    Commands onToggle;
    /// @brief Commands run on a change.
    Commands onChange;
};

/// @brief A `checkbox` node.
/// @param options The checkbox.
/// @return The node.
[[nodiscard]] inline Node checkbox(CheckboxOptions const& options) {
    return detail::NodeWriter{"checkbox"}
        .put("label", options.label)
        .put("checked", options.checked)
        .put("onToggle", options.onToggle)
        .put("onChange", options.onChange)
        .done();
}

/// @brief What a select does with a selection its new options no longer hold.
enum class OnStale : std::uint8_t {
    Clear,  ///< Clears it.
    Keep,   ///< Keeps it.
};

/// @brief Options of `select`.
struct SelectOptions {
    /// @brief The options: a list of `{key, label}` records.
    AnyHandle options;
    /// @brief The selected key.
    AnyHandle selected;
    /// @brief The label: the accessible name.
    AnyHandle label;
    /// @brief The presentation, such as `radio`.
    AnyHandle style;
    /// @brief What happens to a selection the options no longer hold.
    std::optional<OnStale> onStale;
    /// @brief Commands run with the chosen key.
    Commands onChoose;
    /// @brief Commands run on a change.
    Commands onChange;
};

/// @brief A `select` node.
/// @param options The select.
/// @return The node.
[[nodiscard]] inline Node select(SelectOptions const& options) {
    detail::NodeWriter node{"select"};
    node.put("options", options.options)
        .put("selected", options.selected)
        .put("label", options.label)
        .put("style", options.style);
    if (options.onStale) {
        node.text("onStale", *options.onStale == OnStale::Clear ? "clear" : "keep");
    }
    return node.put("onChoose", options.onChoose).put("onChange", options.onChange).done();
}

/// @brief An item of a `menu`; `items` nests a submenu.
struct MenuItem {
    /// @brief The label.
    AnyHandle label;
    /// @brief An icon.
    AnyHandle icon;
    /// @brief The shortcut shown beside it.
    std::string keys;
    /// @brief Whether a checkable item is checked.
    AnyHandle checked;
    /// @brief Whether it is enabled.
    AnyHandle enabled;
    /// @brief Commands run when it is chosen.
    Commands onClick;
    /// @brief A submenu.
    std::vector<MenuItem> items;
};

namespace detail {

/// @brief A menu item's JSON.
/// @param item The item.
/// @return The JSON.
// NOLINTNEXTLINE(misc-no-recursion): a submenu is written item by item; the nesting is the document's own.
[[nodiscard]] inline Json menuItem(MenuItem const& item) {
    Json json = Json::object_t{};
    auto put = [&json](char const* key, AnyHandle const& value) {
        if (value.present()) {
            detail::put(json, key, value.json());
        }
    };
    put("label", item.label);
    put("icon", item.icon);
    if (!item.keys.empty()) {
        detail::put(json, "keys", item.keys);
    }
    put("checked", item.checked);
    put("enabled", item.enabled);
    if (!item.onClick.empty()) {
        detail::put(json, "onClick", item.onClick.json());
    }
    if (!item.items.empty()) {
        Json::array_t items;
        items.reserve(item.items.size());
        for (MenuItem const& child : item.items) {
            items.push_back(menuItem(child));
        }
        detail::put(json, "items", std::move(items));
    }
    return json;
}

}  // namespace detail

/// @brief A `menu` node.
/// @param items The items.
/// @return The node.
[[nodiscard]] inline Node menu(std::vector<MenuItem> const& items) {
    Json::array_t array;
    array.reserve(items.size());
    for (MenuItem const& item : items) {
        array.push_back(detail::menuItem(item));
    }
    return detail::NodeWriter{"menu"}.raw("items", std::move(array)).done();
}

/// @brief Options of `column`, `row` and `grid`.
struct ContainerOptions {
    /// @brief The gap between children: a spacing step, 0 to `kMaxSpacingStep`.
    AnyHandle gap;
    /// @brief A grid's columns; adaptive layouts read `env("widthClass")`.
    AnyHandle columns;
    /// @brief Where the children sit across the axis; absent stretches them.
    std::optional<Align> align;
    /// @brief A row's or column's spare length along its axis; absent packs at the start. Not on a grid, whose
    ///        columns share its width.
    std::optional<Justify> justify;
};

namespace detail {

/// @brief A row or column with its options.
/// @param kind     `row` or `column`.
/// @param children The children.
/// @param options  The gap and the alignment.
/// @return The node.
[[nodiscard]] inline Node stackNode(char const* kind, Nodes const& children, ContainerOptions const& options) {
    NodeWriter node{kind};
    node.put("children", children).put("gap", options.gap);
    if (options.align) {
        node.text("align", alignName(*options.align));
    }
    if (options.justify) {
        node.text("justify", justifyName(*options.justify));
    }
    return node.done();
}

}  // namespace detail

/// @brief A `column` node.
/// @param children The children.
/// @param options  The gap and the alignment.
/// @return The node.
[[nodiscard]] inline Node column(Nodes const& children, ContainerOptions const& options = {}) {
    return detail::stackNode("column", children, options);
}

/// @brief A `row` node.
/// @param children The children.
/// @param options  The gap and the alignment.
/// @return The node.
[[nodiscard]] inline Node row(Nodes const& children, ContainerOptions const& options = {}) {
    return detail::stackNode("row", children, options);
}

/// @brief A `grid` node.
/// @param children The children.
/// @param options  The gap, columns and alignment; a `justify` is written, and the loader refuses it.
/// @return The node.
[[nodiscard]] inline Node grid(Nodes const& children, ContainerOptions const& options = {}) {
    detail::NodeWriter node{"grid"};
    node.put("children", children).put("gap", options.gap).put("columns", options.columns);
    if (options.align) {
        node.text("align", detail::alignName(*options.align));
    }
    if (options.justify) {
        node.text("justify", detail::justifyName(*options.justify));
    }
    return node.done();
}

/// @brief A `spacer` node.
/// @return The node.
[[nodiscard]] inline Node spacer() { return detail::NodeWriter{"spacer"}.done(); }

/// @brief Options of `panel`.
struct PanelOptions {
    /// @brief The title.
    AnyHandle title;
    /// @brief Nodes shown beside the title.
    Nodes header;
    /// @brief The padding.
    AnyHandle padding;
    /// @brief Whether the user may collapse it.
    AnyHandle collapsible;
    /// @brief Whether it is collapsed.
    AnyHandle collapsed;
    /// @brief Commands run on a collapse or expand.
    Commands onToggle;
};

/// @brief A `panel` node.
/// @param children The children.
/// @param options  The title, header and collapsing.
/// @return The node.
[[nodiscard]] inline Node panel(Nodes const& children, PanelOptions const& options = {}) {
    return detail::NodeWriter{"panel"}
        .put("title", options.title)
        .put("header", options.header)
        .put("padding", options.padding)
        .put("collapsible", options.collapsible)
        .put("collapsed", options.collapsed)
        .put("onToggle", options.onToggle)
        .put("children", children)
        .done();
}

/// @brief A `scroll` node.
/// @param children The children.
/// @param axis     The axis; absent for the kind's own.
/// @return The node.
[[nodiscard]] inline Node scroll(Nodes const& children, AnyHandle const& axis = {}) {
    return detail::NodeWriter{"scroll"}.put("axis", axis).put("children", children).done();
}

/// @brief A scope's builder: declares on the scope and returns the nodes that read it.
using ScopeBuilder = std::function<Nodes(DeclScope&)>;

/// @brief A case of a `switch` node, a scope of its own.
class Case {
public:
    /// @brief A case.
    /// @param when     The value the `switch` value is compared with.
    /// @param children The nodes shown when it matches.
    Case(AnyHandle when, Nodes children) : _when{std::move(when)}, _children{std::move(children)} {}

    /// @brief A case that declares on its own scope.
    /// @param when    The value the `switch` value is compared with.
    /// @param builder Declares on the case's scope and returns its nodes.
    Case(AnyHandle when, ScopeBuilder const& builder) : _when{std::move(when)}, _children{builder(_scope)} {}

    /// @brief The case's JSON and the facts it names.
    /// @return The part.
    [[nodiscard]] Node part() const {
        Json json = Json::object_t{};
        detail::put(json, "when", _when.json());
        CatalogFacts facts = _scope.facts();
        facts.merge(_children.facts());
        if (!_scope.empty()) {
            Json decls = Json::object_t{};
            _scope.writeTo(decls);
            detail::put(json, "scope", std::move(decls));
        }
        if (!_children.empty()) {
            detail::put(json, "children", _children.json());
        }
        return Node{std::move(json), std::move(facts)};
    }

private:
    AnyHandle _when;
    DeclScope _scope;
    Nodes _children;
};

/// @brief A `switch` node: shows the first case whose `when` equals its value.
/// @param value    The value the cases are compared with.
/// @param cases    The cases.
/// @param fallback The nodes shown when no case matches.
/// @return The node.
[[nodiscard]] inline Node switchOn(AnyHandle const& value, std::vector<Case> const& cases,
                                   Nodes const& fallback = {}) {
    detail::NodeWriter node{"switch"};
    node.put("on", value);
    Json::array_t parts;
    parts.reserve(cases.size());
    for (Case const& item : cases) {
        Node const written = item.part();
        parts.push_back(written.json());
        node.facts(written.facts());
    }
    return node.raw("cases", std::move(parts)).put("default", fallback).done();
}

/// @brief A page of a `tabs` node.
struct Tab {
    /// @brief The label.
    AnyHandle label;
    /// @brief The page's nodes.
    Nodes children;
};

/// @brief A `tabs` node.
/// @param tabs The pages.
/// @return The node.
[[nodiscard]] inline Node tabs(std::vector<Tab> const& tabs) {
    detail::NodeWriter node{"tabs"};
    Json::array_t parts;
    parts.reserve(tabs.size());
    for (Tab const& tab : tabs) {
        Json part = Json::object_t{};
        detail::put(part, "label", tab.label.json());
        detail::put(part, "children", tab.children.json());
        node.facts(tab.children.facts());
        parts.push_back(std::move(part));
    }
    return node.raw("tabs", std::move(parts)).done();
}

/// @brief Options of `dialog` and `drawer`.
struct DialogOptions {
    /// @brief The title: the accessible name.
    AnyHandle title;
    /// @brief Whether it is open; absent for `open` and `close` commands alone.
    AnyHandle open;
    /// @brief When its element errors show.
    std::optional<ShowErrors> showErrors;
    /// @brief A drawer's side, `start` or `end`.
    AnyHandle side;
    /// @brief Commands run on Enter.
    Commands defaultAction;
    /// @brief Commands run on Escape.
    Commands cancel;
    /// @brief Commands run when the user dismisses it.
    Commands onDismiss;
    /// @brief The buttons of its footer, run in its scope; each id once.
    std::vector<ActionButtonOptions> actions;
};

namespace detail {

/// @brief A dialog or drawer.
/// @param kind    `dialog` or `drawer`.
/// @param element Its element.
/// @param options Its options.
/// @param builder Declares on its scope and returns its children.
/// @return The node.
[[nodiscard]] inline Node dialogNode(std::string_view kind, Element const& element, DialogOptions const& options,
                                     ScopeBuilder const& builder) {
    DeclScope scope;
    Nodes const children = builder(scope);
    NodeWriter node{kind};
    node.text("id", element.name()).put("title", options.title).put("open", options.open);
    if (options.showErrors) {
        node.text("showErrors", showErrorsName(*options.showErrors));
    }
    if (!options.actions.empty()) {
        Json::array_t actions;
        actions.reserve(options.actions.size());
        for (ActionButtonOptions const& action : options.actions) {
            actions.push_back(actionButton(action));
        }
        node.raw("actions", std::move(actions));
    }
    return node.put("side", options.side)
        .scope(scope)
        .put("children", children)
        .put("default", options.defaultAction)
        .put("cancel", options.cancel)
        .put("onDismiss", options.onDismiss)
        .done();
}

}  // namespace detail

/// @brief A `dialog` node, a scope of its own; `open` and `close` name it by its element.
/// @param element  The dialog's element.
/// @param children The children.
/// @param options  The title and command lists.
/// @return The node.
[[nodiscard]] inline Node dialog(Element const& element, Nodes const& children, DialogOptions const& options = {}) {
    return detail::dialogNode("dialog", element, options, [&children](DeclScope&) { return children; });
}

/// @brief A `dialog` node that declares on its own scope.
/// @param element The dialog's element.
/// @param options The title and command lists.
/// @param builder Declares on the dialog's scope and returns its children.
/// @return The node.
[[nodiscard]] inline Node dialog(Element const& element, DialogOptions const& options, ScopeBuilder const& builder) {
    return detail::dialogNode("dialog", element, options, builder);
}

/// @brief A `drawer` node, a scope of its own.
/// @param element  The drawer's element.
/// @param children The children.
/// @param options  The title, side and command lists.
/// @return The node.
[[nodiscard]] inline Node drawer(Element const& element, Nodes const& children, DialogOptions const& options = {}) {
    return detail::dialogNode("drawer", element, options, [&children](DeclScope&) { return children; });
}

/// @brief A `busy` node.
/// @param active Whether it shows.
/// @param label  Its label.
/// @return The node.
[[nodiscard]] inline Node busy(AnyHandle const& active, AnyHandle const& label = {}) {
    return detail::NodeWriter{"busy"}.put("active", active).put("label", label).done();
}

/// @brief Options of `forEach`.
struct ForEachOptions {
    /// @brief The row key, read in the row scope.
    AnyHandle key;
    /// @brief The axis.
    AnyHandle axis;
    /// @brief The gap between rows: a spacing step, 0 to `kMaxSpacingStep`.
    AnyHandle gap;
};

/// @brief A `forEach` node: its children once per row, each row a scope of its own.
/// @param rows    The list.
/// @param options The key, axis and gap.
/// @param builder Declares on a row's scope and returns its nodes.
/// @return The node.
[[nodiscard]] inline Node forEach(AnyHandle const& rows, ForEachOptions const& options, ScopeBuilder const& builder) {
    DeclScope scope;
    Nodes const children = builder(scope);
    return detail::NodeWriter{"forEach"}
        .put("rows", rows)
        .put("key", options.key)
        .put("axis", options.axis)
        .put("gap", options.gap)
        .scope(scope)
        .put("children", children)
        .done();
}

/// @brief A column of a `table`.
struct TableColumn {
    /// @brief The column's id, which a `columns` customization point names.
    std::string name;
    /// @brief The header label.
    AnyHandle label;
    /// @brief The width.
    AnyHandle width;
    /// @brief The column's text role; absent writes no key, `Normal`.
    AnyHandle role;
    /// @brief The cell, read in the row scope.
    Nodes cell;
};

/// @brief Columns from cells alone, without ids or labels.
/// @param cells The cells, one per column.
/// @return The columns.
[[nodiscard]] inline std::vector<TableColumn> cells(std::vector<Node> const& cells) {
    std::vector<TableColumn> columns;
    columns.reserve(cells.size());
    for (Node const& cell : cells) {
        columns.push_back(TableColumn{.name = {}, .label = {}, .width = {}, .cell = Nodes{cell}});
    }
    return columns;
}

/// @brief How a table's selection works.
enum class Selection : std::uint8_t {
    None,      ///< No selection.
    Single,    ///< One row.
    Multiple,  ///< Several rows.
};

/// @brief Where a model-backed table sorts, filters and pages.
enum class TableMode : std::uint8_t {
    Client,  ///< In the client, over every row.
    Server,  ///< In the server, through the action's table query.
};

/// @brief Options of `table` and `dataTable`.
struct TableOptions {
    /// @brief The row key, read in the row scope.
    AnyHandle key;
    /// @brief The columns; a model-backed table derives them from the row type when none are given.
    std::vector<TableColumn> columns;
    /// @brief Whether the header row is drawn. Several tables sharing one visual header set this `false` on every
    ///        instance but the first.
    bool showHeader = true;
    /// @brief Below this many rows, every row's widget stays realized regardless of scroll position; a backend
    ///        that cannot virtualize ignores this and `virtualizeOverscan` both.
    std::size_t virtualizeThreshold = 2000;
    /// @brief How many rows beyond the visible range, on each side, keep a realized widget.
    std::size_t virtualizeOverscan = 10;
    /// @brief How the selection works.
    std::optional<Selection> selection;
    /// @brief Where a model-backed table works; ignored by a table over rows.
    std::optional<TableMode> mode;
    /// @brief The sort.
    AnyHandle sort;
    /// @brief The filters.
    AnyHandle filters;
    /// @brief A row's tone, read in the row scope.
    AnyHandle rowTone;
    /// @brief The key a linked chart highlights.
    AnyHandle highlightKey;
    /// @brief Commands run when a row is activated, in its row scope.
    Commands onActivate;
    /// @brief Shown instead of the table while its rows are an empty list, in the table's scope; empty shows the
    ///        table always.
    Nodes empty;
};

namespace detail {

/// @brief Writes a table's options after its source.
/// @param node    The table so far.
/// @param options The options.
/// @param scope   The row scope.
/// @param columns The columns.
/// @return The node.
[[nodiscard]] inline Node tableNode(NodeWriter& node, TableOptions const& options, DeclScope const& scope,
                                    std::vector<TableColumn> const& columns) {
    static constexpr std::array<char const*, 3> kSelections = {"none", "single", "multiple"};
    node.put("key", options.key);
    if (!columns.empty()) {
        Json::array_t parts;
        parts.reserve(columns.size());
        for (TableColumn const& column : columns) {
            Json part = Json::object_t{};
            if (!column.name.empty()) {
                detail::put(part, "id", column.name);
            }
            if (column.label.present()) {
                detail::put(part, "label", column.label.json());
            }
            if (column.width.present()) {
                detail::put(part, "width", column.width.json());
            }
            if (column.role.present()) {
                detail::put(part, "role", column.role.json());
            }
            if (!column.cell.empty()) {
                detail::put(part, "cell", column.cell.json());
                node.facts(column.cell.facts());
            }
            parts.push_back(std::move(part));
        }
        node.raw("columns", std::move(parts));
    }
    if (options.selection) {
        node.raw("selectionMode", std::string{kSelections.at(static_cast<std::size_t>(*options.selection))});
    }
    if (!options.showHeader) {
        node.raw("showHeader", false);
    }
    if (options.virtualizeThreshold != 2000) {
        node.raw("virtualizeThreshold", static_cast<std::int64_t>(options.virtualizeThreshold));
    }
    if (options.virtualizeOverscan != 10) {
        node.raw("virtualizeOverscan", static_cast<std::int64_t>(options.virtualizeOverscan));
    }
    return node.put("sort", options.sort)
        .put("filters", options.filters)
        .put("rowTone", options.rowTone)
        .put("highlightKey", options.highlightKey)
        .put("onActivate", options.onActivate)
        .put("empty", options.empty)
        .scope(scope)
        .done();
}

}  // namespace detail

/// @brief A `table` over a list, with the columns in its options.
/// @param rows    The rows.
/// @param options The key, columns, selection and command lists.
/// @return The node.
[[nodiscard]] inline Node table(AnyHandle const& rows, TableOptions const& options) {
    detail::NodeWriter node{"table"};
    node.put("rows", rows);
    return detail::tableNode(node, options, DeclScope{}, options.columns);
}

/// @brief A `table` over a list whose rows declare on their own scope.
/// @param rows    The rows.
/// @param options The key, selection and command lists.
/// @param builder Declares on a row's scope and returns the columns.
/// @return The node.
[[nodiscard]] inline Node table(AnyHandle const& rows, TableOptions const& options,
                                std::function<std::vector<TableColumn>(DeclScope&)> const& builder) {
    DeclScope scope;
    std::vector<TableColumn> const columns = builder(scope);
    detail::NodeWriter node{"table"};
    node.put("rows", rows);
    return detail::tableNode(node, options, scope, columns);
}

/// @brief A `table` declared from a model's list action (spec 7): the client derives its columns from the row type
///        and the table engine sorts, filters, selects and pages.
/// @tparam Action The list action.
/// @tparam Model  The model type of the alias; it must run the action.
/// @param model   The alias.
/// @param body    The action's own parameters.
/// @param options The mode, selection, columns and command lists.
/// @return The node.
template <typename Action, typename Model>
    requires Runs<Model, Action>
[[nodiscard]] Node dataTable(ModelHandle<Model> const& model, Body<Action> const& body,
                             TableOptions const& options = {}) {
    CatalogFacts facts;
    facts.action<Model, Action>();
    detail::NodeWriter node{"table"};
    node.text("model", model.alias()).text("action", std::string{::morph::model::ActionTraits<Action>::typeId()});
    if (!body.empty()) {
        node.raw("body", body.json());
    }
    if (options.mode) {
        node.raw("mode", std::string{*options.mode == TableMode::Server ? "server" : "client"});
    }
    node.facts(facts);
    return detail::tableNode(node, options, DeclScope{}, options.columns);
}

/// @brief A `table` declared from a model's list action that takes no parameters of its own.
/// @tparam Action The list action.
/// @tparam Model  The model type of the alias; it must run the action.
/// @param model   The alias.
/// @param options The mode, selection, columns and command lists.
/// @return The node.
template <typename Action, typename Model>
    requires Runs<Model, Action>
[[nodiscard]] Node dataTable(ModelHandle<Model> const& model, TableOptions const& options = {}) {
    return dataTable<Action>(model, Body<Action>{}, options);
}

/// @brief A `dateTimeInput` node.
/// @param value   The value.
/// @param label   The label.
/// @param commit  Commands run on a commit.
/// @return The node.
[[nodiscard]] inline Node dateTimeInput(AnyHandle const& value, AnyHandle const& label, Commands const& commit = {}) {
    return detail::NodeWriter{"dateTimeInput"}.put("value", value).put("label", label).put("onCommit", commit).done();
}

/// @brief Options of `dateTimeInput`.
struct DateTimeInputOptions {
    /// @brief The value.
    AnyHandle value;
    /// @brief The label: the accessible name.
    AnyHandle label;
    /// @brief The input mode, such as `date` or `time`.
    AnyHandle mode;
    /// @brief Commands run on every edit.
    Commands onChange;
    /// @brief Commands run on a commit.
    Commands onCommit;
};

/// @brief A `dateTimeInput` node.
/// @param options The input.
/// @return The node.
[[nodiscard]] inline Node dateTimeInput(DateTimeInputOptions const& options) {
    return detail::NodeWriter{"dateTimeInput"}
        .put("value", options.value)
        .put("label", options.label)
        .put("mode", options.mode)
        .put("onChange", options.onChange)
        .put("onCommit", options.onCommit)
        .done();
}

/// @brief Options of `slider`.
struct SliderOptions {
    /// @brief The value.
    AnyHandle value;
    /// @brief The label: the accessible name.
    AnyHandle label;
    /// @brief The minimum.
    AnyHandle min;
    /// @brief The maximum.
    AnyHandle max;
    /// @brief The step.
    AnyHandle step;
    /// @brief Commands run on a change.
    Commands onChange;
    /// @brief Commands run on a commit.
    Commands onCommit;
};

/// @brief A `slider` node.
/// @param options The slider.
/// @return The node.
[[nodiscard]] inline Node slider(SliderOptions const& options) {
    return detail::NodeWriter{"slider"}
        .put("value", options.value)
        .put("label", options.label)
        .put("min", options.min)
        .put("max", options.max)
        .put("step", options.step)
        .put("onChange", options.onChange)
        .put("onCommit", options.onCommit)
        .done();
}

/// @brief Options of `filePicker`.
struct FilePickerOptions {
    /// @brief The label: the accessible name.
    AnyHandle label;
    /// @brief The accepted types.
    AnyHandle accept;
    /// @brief Whether several files may be picked.
    AnyHandle multiple;
    /// @brief `open` or `save`.
    AnyHandle mode;
    /// @brief Commands run with the picked files as `event`.
    Commands onPick;
};

/// @brief A `filePicker` node.
/// @param options The picker.
/// @return The node.
[[nodiscard]] inline Node filePicker(FilePickerOptions const& options) {
    return detail::NodeWriter{"filePicker"}
        .put("label", options.label)
        .put("accept", options.accept)
        .put("multiple", options.multiple)
        .put("mode", options.mode)
        .put("onPick", options.onPick)
        .done();
}

/// @brief The action of a `banner` or `emptyState`.
struct ActionPart {
    /// @brief The label.
    AnyHandle label;
    /// @brief Commands run when it is chosen.
    Commands onClick;
};

namespace detail {

/// @brief Writes an action part.
/// @param node   The node.
/// @param action The action; absent leaves it out.
inline void actionPart(NodeWriter& node, std::optional<ActionPart> const& action) {
    if (!action) {
        return;
    }
    Json part = Json::object_t{};
    if (action->label.present()) {
        detail::put(part, "label", action->label.json());
    }
    if (!action->onClick.empty()) {
        detail::put(part, "onClick", action->onClick.json());
    }
    node.raw("action", std::move(part));
}

}  // namespace detail

/// @brief Options of `banner`.
struct BannerOptions {
    /// @brief The tone.
    AnyHandle tone;
    /// @brief The text.
    AnyHandle text;
    /// @brief Whether the user may dismiss it.
    AnyHandle dismissible;
    /// @brief Its action.
    std::optional<ActionPart> action;
    /// @brief Commands run when it is dismissed.
    Commands onDismiss;
};

/// @brief A `banner` node.
/// @param options The banner.
/// @return The node.
[[nodiscard]] inline Node banner(BannerOptions const& options) {
    detail::NodeWriter node{"banner"};
    node.put("tone", options.tone).put("text", options.text).put("dismissible", options.dismissible);
    detail::actionPart(node, options.action);
    return node.put("onDismiss", options.onDismiss).done();
}

/// @brief A `badge` node.
/// @param text The text.
/// @param tone The tone.
/// @param icon An icon.
/// @return The node.
[[nodiscard]] inline Node badge(AnyHandle const& text, AnyHandle const& tone = {}, AnyHandle const& icon = {}) {
    return detail::NodeWriter{"badge"}.put("tone", tone).put("text", text).put("icon", icon).done();
}

/// @brief A `progress` node.
/// @param value The fraction from 0 to 1; `null` is indeterminate.
/// @param label The label.
/// @return The node.
[[nodiscard]] inline Node progress(AnyHandle const& value, AnyHandle const& label = {}) {
    return detail::NodeWriter{"progress"}.put("value", value).put("label", label).done();
}

/// @brief A `steps` node.
/// @param items   The steps, `[{label, state}]`.
/// @param current The current step.
/// @return The node.
[[nodiscard]] inline Node steps(AnyHandle const& items, AnyHandle const& current = {}) {
    return detail::NodeWriter{"steps"}.put("items", items).put("current", current).done();
}

/// @brief A `keyValue` node.
/// @param items The pairs, `[{label, value}]`.
/// @return The node.
[[nodiscard]] inline Node keyValue(AnyHandle const& items) {
    return detail::NodeWriter{"keyValue"}.put("items", items).done();
}

/// @brief Options of `emptyState`.
struct EmptyStateOptions {
    /// @brief The title.
    AnyHandle title;
    /// @brief The text.
    AnyHandle text;
    /// @brief An icon.
    AnyHandle icon;
    /// @brief Its action.
    std::optional<ActionPart> action;
};

/// @brief An `emptyState` node.
/// @param options The empty state.
/// @return The node.
[[nodiscard]] inline Node emptyState(EmptyStateOptions const& options) {
    detail::NodeWriter node{"emptyState"};
    node.put("title", options.title).put("text", options.text).put("icon", options.icon);
    detail::actionPart(node, options.action);
    return node.done();
}

/// @brief A `splitter` node.
/// @param children    The panes.
/// @param orientation The orientation.
/// @param sizes       The initial sizes.
/// @return The node.
[[nodiscard]] inline Node splitter(Nodes const& children, AnyHandle const& orientation = {},
                                   AnyHandle const& sizes = {}) {
    return detail::NodeWriter{"splitter"}
        .put("orientation", orientation)
        .put("sizes", sizes)
        .put("children", children)
        .done();
}

/// @brief Options of `collapsible`.
struct CollapsibleOptions {
    /// @brief The title.
    AnyHandle title;
    /// @brief Whether it is open.
    AnyHandle open;
    /// @brief Nodes shown beside the title.
    Nodes header;
    /// @brief Commands run on a toggle.
    Commands onToggle;
};

/// @brief A `collapsible` node.
/// @param children The content.
/// @param options  The title, header and toggle.
/// @return The node.
[[nodiscard]] inline Node collapsible(Nodes const& children, CollapsibleOptions const& options = {}) {
    return detail::NodeWriter{"collapsible"}
        .put("title", options.title)
        .put("open", options.open)
        .put("header", options.header)
        .put("children", children)
        .put("onToggle", options.onToggle)
        .done();
}

/// @brief Options of `dropZone`.
struct DropZoneOptions {
    /// @brief The accepted types.
    AnyHandle accept;
    /// @brief Whether several files may be dropped.
    AnyHandle multiple;
    /// @brief Commands run with the dropped files as `event`.
    Commands onDrop;
};

/// @brief A `dropZone` node.
/// @param children The content.
/// @param options  The accepted files and the drop.
/// @return The node.
[[nodiscard]] inline Node dropZone(Nodes const& children, DropZoneOptions const& options = {}) {
    return detail::NodeWriter{"dropZone"}
        .put("accept", options.accept)
        .put("multiple", options.multiple)
        .put("children", children)
        .put("onDrop", options.onDrop)
        .done();
}

/// @brief A `customize` node: the editor of a customization point.
/// @param point The point.
/// @return The node.
[[nodiscard]] inline Node customize(Point const& point) {
    return detail::NodeWriter{"customize"}.text("point", point.name()).done();
}

/// @brief A `boundary` node: `loading` until its queries have values, `error` when one failed without one.
/// @param children The content.
/// @param loading  The nodes shown while loading.
/// @param error    The nodes shown on a failure; `ref("boundary.error")` and `retry()` are valid in them.
/// @return The node.
[[nodiscard]] inline Node boundary(Nodes const& children, Nodes const& loading = {}, Nodes const& error = {}) {
    return detail::NodeWriter{"boundary"}.put("loading", loading).put("error", error).put("children", children).done();
}

/// @brief An overlay on one field of a form (spec 6 §10), applied to the parsed form model.
struct FieldSettings {
    /// @brief Leaves the field out.
    AnyHandle omit;
    /// @brief Hides the field.
    AnyHandle hidden;
    /// @brief The label.
    AnyHandle label;
    /// @brief The display unit.
    AnyHandle unit;
    /// @brief The display places.
    AnyHandle decimals;
    /// @brief The widget.
    AnyHandle widget;
    /// @brief What an empty input is sent as.
    AnyHandle blankAs;
};

/// @brief One field of a form's `fields` overlay, by member pointer.
/// @tparam Action The form's action.
template <typename Action>
class FieldOverlay {
public:
    /// @brief An overlay.
    /// @tparam Member The member's type.
    /// @param member   The member.
    /// @param settings The overlay.
    template <typename Member>
    FieldOverlay(Member Action::* member, FieldSettings settings)
        : _name{detail::memberName(member)}, _settings{std::move(settings)} {}

    /// @brief The overlay's JSON.
    /// @return The field name and its settings.
    [[nodiscard]] std::pair<std::string, Json> json() const {
        Json settings = Json::object_t{};
        auto put = [&settings](char const* key, AnyHandle const& value) {
            if (value.present()) {
                detail::put(settings, key, value.json());
            }
        };
        put("omit", _settings.omit);
        put("hidden", _settings.hidden);
        put("label", _settings.label);
        put("unit", _settings.unit);
        put("decimals", _settings.decimals);
        put("widget", _settings.widget);
        put("blankAs", _settings.blankAs);
        return {_name, std::move(settings)};
    }

private:
    std::string _name;
    FieldSettings _settings;
};

/// @brief How a `form`, `collection` or `wizard` presents its forms: the keys every one of them takes.
struct FormPresentation {
    /// @brief A heading over the fields; on a collection or a wizard, the view's heading in place of the schema's.
    ///        Absent shows none on a form.
    AnyHandle title;
    /// @brief The explicit-mode button's label, which the status line names; absent is `Submit`.
    AnyHandle submitLabel;
    /// @brief Whether the status line shows; absent shows it.
    std::optional<bool> showStatus;
    /// @brief The columns of every group's grid, 1 to 12; absent keeps the engine's.
    std::optional<int> columns;
    /// @brief Where every cell puts its caption; absent stacks it over the control.
    std::optional<::morph::forms::FieldLayout> fieldLayout;
};

namespace detail {

/// @brief Writes @p presentation's keys on @p node.
/// @param node         The node being written.
/// @param presentation The keys.
inline void present(NodeWriter& node, FormPresentation const& presentation) {
    node.put("title", presentation.title).put("submitLabel", presentation.submitLabel);
    if (presentation.showStatus) {
        node.raw("showStatus", AnyHandle{*presentation.showStatus}.json());
    }
    if (presentation.columns) {
        node.raw("columns", AnyHandle{std::int64_t{*presentation.columns}}.json());
    }
    if (presentation.fieldLayout) {
        node.text("fieldLayout",
                  *presentation.fieldLayout == ::morph::forms::FieldLayout::Inline ? "inline" : "stacked");
    }
}

}  // namespace detail

/// @brief Options of `form`.
/// @tparam Action The form's action.
template <typename Action>
struct FormOptions {
    /// @brief The values the form starts from.
    AnyHandle prefill;
    /// @brief The identity of the record `prefill` holds: a new value of the same key keeps the fields the user
    ///        changed, a new key loads every field. Without one, only the first value keeps them.
    AnyHandle key;
    /// @brief How it submits, such as `explicit`.
    std::string submit;
    /// @brief When its errors show.
    std::optional<ShowErrors> showErrors;
    /// @brief The `fields` overlay.
    std::vector<FieldOverlay<Action>> fields;
    /// @brief Commands run on submit.
    Commands onSubmit;
    /// @brief Commands run after a successful submit.
    Commands onSuccess;
    /// @brief Commands run after a failed submit.
    Commands onError;
    /// @brief The title, button label, status line and columns.
    FormPresentation presentation{};
};

/// @brief A `form` node: a schema form of the forms engine over an action (spec 2); each field takes its label
///        from the schema or the overlay.
/// @tparam Action The action.
/// @tparam Model  The model type of the alias; it must run the action.
/// @param model   The alias.
/// @param options The prefill, overlay and command lists.
/// @return The node.
template <typename Action, typename Model>
    requires Runs<Model, Action>
[[nodiscard]] Node form(ModelHandle<Model> const& model, FormOptions<Action> const& options = {}) {
    CatalogFacts facts;
    facts.action<Model, Action>();
    detail::NodeWriter node{"form"};
    node.text("model", model.alias())
        .text("action", std::string{::morph::model::ActionTraits<Action>::typeId()})
        .put("prefill", options.prefill)
        .put("key", options.key)
        .text("submit", options.submit);
    if (options.showErrors) {
        node.text("showErrors", showErrorsName(*options.showErrors));
    }
    if (!options.fields.empty()) {
        Json overlay = Json::object_t{};
        for (FieldOverlay<Action> const& field : options.fields) {
            auto [name, settings] = field.json();
            detail::put(overlay, name, std::move(settings));
        }
        node.raw("fields", std::move(overlay));
    }
    detail::present(node, options.presentation);
    return node.put("onSubmit", options.onSubmit)
        .put("onSuccess", options.onSuccess)
        .put("onError", options.onError)
        .facts(facts)
        .done();
}

/// @brief A `collection` node: the forms engine's list screen over an action.
/// @tparam Action The action.
/// @tparam Model  The model type of the alias; it must run the action.
/// @param model        The alias.
/// @param presentation The keys that reach every form the view draws; its title replaces the view's heading.
/// @return The node.
template <typename Action, typename Model>
    requires Runs<Model, Action>
[[nodiscard]] Node collection(ModelHandle<Model> const& model, FormPresentation const& presentation = {}) {
    CatalogFacts facts;
    facts.action<Model, Action>();
    detail::NodeWriter node{"collection"};
    node.text("model", model.alias()).text("action", std::string{::morph::model::ActionTraits<Action>::typeId()});
    detail::present(node, presentation);
    return node.facts(facts).done();
}

/// @brief A `wizard` node: the forms engine's wizard over an action.
/// @tparam Action The action.
/// @tparam Model  The model type of the alias; it must run the action.
/// @param model        The alias.
/// @param presentation The keys that reach every step's form; its title replaces the view's heading.
/// @return The node.
template <typename Action, typename Model>
    requires Runs<Model, Action>
[[nodiscard]] Node wizard(ModelHandle<Model> const& model, FormPresentation const& presentation = {}) {
    CatalogFacts facts;
    facts.action<Model, Action>();
    detail::NodeWriter node{"wizard"};
    node.text("model", model.alias()).text("action", std::string{::morph::model::ActionTraits<Action>::typeId()});
    detail::present(node, presentation);
    return node.facts(facts).done();
}

/// @brief Options of `custom`.
struct CustomOptions {
    /// @brief The props, by name.
    std::vector<std::pair<std::string, AnyHandle>> props;
    /// @brief The component's events and the commands they run.
    std::vector<std::pair<std::string, Commands>> on;
};

/// @brief A `custom` node: a declared component with its props and events.
/// @param component The component.
/// @param options   The props and events.
/// @return The node.
[[nodiscard]] inline Node custom(ComponentRef const& component, CustomOptions const& options = {}) {
    detail::NodeWriter node{"custom"};
    node.text("name", component.name());
    if (!options.props.empty()) {
        Json props = Json::object_t{};
        for (auto const& [name, value] : options.props) {
            detail::put(props, name, value.json());
        }
        node.raw("props", std::move(props));
    }
    if (!options.on.empty()) {
        Json events = Json::object_t{};
        for (auto const& [name, commands] : options.on) {
            detail::put(events, name, commands.json());
        }
        node.raw("on", std::move(events));
    }
    return node.done();
}

// ── Accessibility ────────────────────────────────────────────────────────────────────────────────────────────────

namespace detail {

/// @brief Whether a node has a property that names it: present, and not the constant `""` or `null`.
/// @param node The node.
/// @param key  The property.
/// @return Whether it names the node.
[[nodiscard]] inline bool names(DocNode const& node, std::string_view key) {
    ExprPtr const value = node.prop(key);
    if (value == nullptr) {
        return false;
    }
    if (value->kind != ExprKind::Literal) {
        return true;
    }
    std::string const* const text = value->literal.asString();
    return !value->literal.isNull() && (text == nullptr || !text->empty());
}

/// @brief Whether a node has an `a11y.name` that names it.
/// @param node The node.
/// @return Whether it has.
[[nodiscard]] inline bool hasA11yName(DocNode const& node) {
    auto const parts = node.partList("a11y");
    return !parts.empty() && names(*parts.front(), "name");
}

/// @brief The accessibility rule a node breaks, if any.
/// @param node The node.
/// @return The rule, or empty.
[[nodiscard]] inline std::string brokenRule(DocNode const& node) {
    static constexpr std::array<std::string_view, 6> kInputs = {"textInput", "dateTimeInput", "slider",
                                                                "select",    "checkbox",      "filePicker"};
    std::string_view const kind = node.kind;
    if (std::ranges::find(kInputs, kind) != kInputs.end()) {
        if (names(node, "label") || hasA11yName(node)) {
            return {};
        }
        return "accessibility: a `" + node.kind + "` has a `label` or an `a11y.name`";
    }
    if (kind == "button") {
        if (names(node, "label") || hasA11yName(node)) {
            return {};
        }
        return "accessibility: a button without text has an `a11y.name`; an icon alone does not name it";
    }
    if (kind == "dialog" || kind == "drawer") {
        if (names(node, "title") || hasA11yName(node)) {
            return {};
        }
        return "accessibility: a `" + node.kind + "` has a `title` or an `a11y.name`";
    }
    if ((kind == "table" || kind == "custom") && !hasA11yName(node)) {
        return "accessibility: a `" + node.kind + "` has an `a11y.name`";
    }
    return {};
}

// NOLINTBEGIN(misc-no-recursion): the view is walked node by node; its depth is bounded by kMaxJsonDepth.

/// @brief The first accessibility rule a node or a node under it breaks.
/// @param node     The node.
/// @param document The document's id.
/// @return The refusal, or nothing.
[[nodiscard]] inline std::optional<Refusal> accessibilityOf(DocNode const& node, std::string const& document);

/// @brief The first accessibility rule the nodes of a list break.
/// @param nodes    The nodes.
/// @param document The document's id.
/// @return The refusal, or nothing.
[[nodiscard]] inline std::optional<Refusal> accessibilityOf(std::span<DocNodePtr const> nodes,
                                                            std::string const& document) {
    for (DocNodePtr const& child : nodes) {
        if (auto refusal = accessibilityOf(*child, document)) {
            return refusal;
        }
    }
    return std::nullopt;
}

inline std::optional<Refusal> accessibilityOf(DocNode const& node, std::string const& document) {
    if (std::string rule = brokenRule(node); !rule.empty()) {
        return Refusal{.document = document, .path = node.path, .reason = std::move(rule)};
    }
    for (auto const& [key, nodes] : node.children) {
        if (auto refusal = accessibilityOf(std::span<DocNodePtr const>{nodes}, document)) {
            return refusal;
        }
    }
    for (auto const& [key, parts] : node.parts) {
        for (DocNodePtr const& part : parts) {
            for (auto const& [childKey, nodes] : part->children) {
                if (auto refusal = accessibilityOf(std::span<DocNodePtr const>{nodes}, document)) {
                    return refusal;
                }
            }
        }
    }
    return std::nullopt;
}

// NOLINTEND(misc-no-recursion)

}  // namespace detail

/// @brief The accessibility rules of spec 5 §11, which registration runs after the load-time validation.
///
/// Every text input, date-time input, slider, select, checkbox and file picker has a `label` or an `a11y.name`; a
/// button without a `label`, every `custom` node and every `table` has an `a11y.name`; every `dialog` and `drawer`
/// has a `title` or an `a11y.name`. An `icon` never counts, and neither does a label or name written as the
/// constant `""` or `null`. A form field takes its field's label, so a `form` is not checked. The view and every
/// component's fallback are walked; the client does not run these rules.
/// @param document The loaded document.
/// @return The first rule broken, with the node's path, or nothing.
[[nodiscard]] inline std::optional<Refusal> checkAccessibility(Document const& document) {
    if (document.view) {
        if (auto refusal = detail::accessibilityOf(*document.view, document.id)) {
            return refusal;
        }
    }
    for (auto const& [name, component] : document.components) {
        if (component.fallback) {
            if (auto refusal = detail::accessibilityOf(*component.fallback, document.id)) {
                return refusal;
            }
        }
    }
    return std::nullopt;
}

// ── Screens and registration ─────────────────────────────────────────────────────────────────────────────────────

/// @brief A screen's emitted document: its JSON, the text that is served, and the catalog facts of its types.
struct Emitted {
    /// @brief The document.
    Json json = Json::object_t{};
    /// @brief The document as compact JSON text.
    std::string text;
    /// @brief The catalog facts of the models and actions it names.
    CatalogFacts facts;
};

/// @brief A screen definition: an id and the builder that writes the document.
class Screen {
public:
    /// @brief Declares on the screen's scope and returns the view.
    using Builder = std::function<Node(Scope&)>;

    /// @brief A screen.
    /// @param screenId The document id.
    /// @param builder  The builder.
    Screen(std::string screenId, Builder builder) : _id{std::move(screenId)}, _builder{std::move(builder)} {}

    /// @brief The document id.
    /// @return The id.
    [[nodiscard]] std::string const& id() const noexcept { return _id; }

    /// @brief Runs the builder and writes the document.
    /// @return The document, or a refusal at the root naming what the builder could not write.
    [[nodiscard]] std::expected<Emitted, Refusal> write() const {
        try {
            Scope scope;
            Node const view = _builder(scope);
            Emitted out;
            out.json = scope.document(_id, view);
            out.facts = scope.facts();
            out.facts.merge(view.facts());
            // Writing a JSON tree cannot fail; empty text would be refused by the loader as not JSON.
            out.text = glz::write_json(out.json).value_or(std::string{});
            return out;
        } catch (std::exception const& error) {
            return std::unexpected(Refusal{.document = _id, .path = "", .reason = error.what()});
        } catch (...) {
            return std::unexpected(Refusal{.document = _id,
                                           .path = "",
                                           .reason = "the screen's builder threw an exception that is not a "
                                                     "std::exception"});
        }
    }

private:
    std::string _id;
    Builder _builder;
};

/// @brief A screen with its id as a template argument, as spec 5 §11 writes it.
/// @tparam Id The document id.
/// @param builder The builder.
/// @return The screen.
template <morph::detail::FixedString Id>
[[nodiscard]] Screen screen(Screen::Builder builder) {
    return Screen{std::string{Id.view()}, std::move(builder)};
}

/// @brief A screen.
/// @param screenId The document id.
/// @param builder  The builder.
/// @return The screen.
[[nodiscard]] inline Screen screen(std::string screenId, Screen::Builder builder) {
    return Screen{std::move(screenId), std::move(builder)};
}

/// @brief A registered screen: the text that is served and the document it loads as.
struct RegisteredScreen {
    /// @brief The document id.
    std::string id;
    /// @brief The document as compact JSON text.
    std::string text;
    /// @brief The loaded document.
    Document document;
};

/// @brief The screens of an application, each validated once when it is registered.
///
/// Registration emits a screen's JSON once, loads it with `loadDocumentText` and the registry's catalog — the facts
/// of every model and action the registered screens name, the screen's own first — and then runs
/// `checkAccessibility`. A screen that fails is not registered.
class ScreenRegistry {
public:
    /// @brief Registers a screen.
    /// @param screen The screen.
    /// @return The registered screen, or the first refusal: the builder's, the load-time validation's, an
    ///         accessibility rule's, or a duplicate id.
    std::expected<RegisteredScreen const*, Refusal> add(Screen const& screen) {
        if (find(screen.id()) != nullptr) {
            return std::unexpected(
                Refusal{.document = screen.id(), .path = "/id", .reason = "a screen with this id is registered"});
        }
        auto emitted = screen.write();
        if (!emitted) {
            return std::unexpected(std::move(emitted.error()));
        }
        CatalogFacts facts = emitted->facts;
        facts.merge(_facts);
        auto loaded = loadDocumentText(emitted->text, LoadOptions{.catalog = facts.catalog()});
        if (!loaded) {
            return std::unexpected(std::move(loaded.error()));
        }
        if (auto refusal = checkAccessibility(*loaded)) {
            return std::unexpected(std::move(*refusal));
        }
        for (LoadWarning const& warning : loaded->warnings) {
            morph::log::logWarn(warning.message(loaded->id));
        }
        _facts = std::move(facts);
        _screens.push_back(std::make_unique<RegisteredScreen>(
            RegisteredScreen{.id = screen.id(), .text = std::move(emitted->text), .document = std::move(*loaded)}));
        return _screens.back().get();
    }

    /// @brief Registers a screen and keeps its refusal rather than returning it, for registration at static
    ///        initialisation, where nothing could report it; a server's test asserts `refusals()` is empty and
    ///        `lost()` is zero.
    ///
    /// Never throws, because its caller is a static initialiser, where an exception cannot be caught. A builder's
    /// own exceptions are already refusals; anything thrown while making the screen or keeping its refusal, such
    /// as an allocation failure, is counted by `lost()`.
    /// @tparam Make A callable that returns the `Screen`.
    /// @param make Makes the screen; it is called inside the registration, so its exceptions are caught too.
    template <std::invocable Make>
    void enroll(Make const& make) noexcept {
        try {
            Screen const screen = make();
            if (auto added = add(screen); !added) {
                _refusals.push_back(std::move(added.error()));
            }
        } catch (...) {
            ++_lost;
        }
    }

    /// @brief Registers a screen and keeps its refusal rather than returning it.
    /// @param screen The screen.
    void enroll(Screen const& screen) noexcept {
        enroll([&screen] { return screen; });
    }

    /// @brief How many enrolments failed without a refusal to keep, because something threw outside the builder.
    /// @return The count.
    [[nodiscard]] std::size_t lost() const noexcept { return _lost; }

    /// @brief A registered screen by id.
    /// @param screenId The id.
    /// @return The screen, or `nullptr`.
    [[nodiscard]] RegisteredScreen const* find(std::string_view screenId) const {
        for (auto const& registered : _screens) {
            if (registered->id == screenId) {
                return registered.get();
            }
        }
        return nullptr;
    }

    /// @brief How many screens are registered.
    /// @return The count.
    [[nodiscard]] std::size_t size() const noexcept { return _screens.size(); }

    /// @brief The refusals `enroll` kept.
    /// @return The refusals, in order.
    [[nodiscard]] std::vector<Refusal> const& refusals() const noexcept { return _refusals; }

    /// @brief The catalog of every model and action the registered screens name, for a local-mode client.
    /// @return The catalog.
    [[nodiscard]] Catalog catalog() const { return _facts.catalog(); }

private:
    std::vector<std::unique_ptr<RegisteredScreen>> _screens;
    std::vector<Refusal> _refusals;
    std::size_t _lost = 0;
    CatalogFacts _facts;
};

/// @brief The process's screen registry, which `MORPH_REGISTER_SCREEN` enrols into.
/// @return The registry.
[[nodiscard]] inline ScreenRegistry& screenRegistry() noexcept {
    static ScreenRegistry registry;
    return registry;
}

/// @brief Enrols a screen into the process's registry; what `MORPH_REGISTER_SCREEN` runs.
/// @tparam Make A callable that returns the `Screen`.
/// @param make Makes the screen, inside the registration.
/// @return `true`, for the static initialiser that runs it.
template <std::invocable Make>
bool enrollScreen(Make const& make) noexcept {
    screenRegistry().enroll(make);
    return true;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

}  // namespace morph::ui::author

/// @brief Concatenates two tokens after expanding them.
// NOLINTBEGIN(cppcoreguidelines-macro-usage): registration at static initialisation is a macro, as for models.
#define MORPH_UI_AUTHOR_CAT_(a, b) a##b
/// @brief Concatenates two tokens after expanding them.
#define MORPH_UI_AUTHOR_CAT(a, b) MORPH_UI_AUTHOR_CAT_(a, b)

// The registrar is a namespace-scope `const bool` initialised before main. `enrollScreen` is noexcept and keeps
// any failure as a refusal, but clang-tidy does not see through the template call and attributes a possible throw
// to every expansion site, so the check is suppressed here, once.
// NOLINTBEGIN(bugprone-throwing-static-initialization,cert-err58-cpp)
/// @brief Enrols a screen into the process's registry at static initialisation. A refusal is kept in
///        `screenRegistry().refusals()`, which a server's test asserts is empty.
/// @param SCREEN The screen: an expression, such as a call of a function that returns it, or an object defined
///               before the macro in the same translation unit.
#define MORPH_REGISTER_SCREEN(SCREEN)                                                                     \
    namespace {                                                                                           \
    [[maybe_unused]] bool const MORPH_UI_AUTHOR_CAT(morphUiScreen_,                                       \
                                                    __COUNTER__) = ::morph::ui::author::enrollScreen([] { \
        return (SCREEN);                                                                                  \
    });                                                                                                   \
    }
// NOLINTEND(bugprone-throwing-static-initialization,cert-err58-cpp)
// NOLINTEND(cppcoreguidelines-macro-usage)
