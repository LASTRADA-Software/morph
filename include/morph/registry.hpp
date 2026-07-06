// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <concepts>
#include <cstdint>
#include <functional>
#include <glaze/glaze.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "model.hpp"

namespace morph::model {

/// @brief Traits specialisation that maps a model type to its string type-id.
///
/// Users must specialise this (or use `BRIDGE_REGISTER_MODEL`) before the model
/// can be registered with a backend.
/// @tparam Model Concrete model type.
template <typename Model>
struct ModelTraits;

/// @brief Traits specialisation that maps an action type to its id, JSON codec,
///        and result type.
///
/// Users must specialise this (or use `BRIDGE_REGISTER_ACTION`).
/// @tparam Action Concrete action type.
template <typename Action>
struct ActionTraits;

namespace detail {

/// @brief Concept satisfied by actions that expose a `bool validate() const` member.
///
/// Drives the default `ActionValidator::ready(...)` path: if the action supplies
/// a `validate()` method, it is called automatically; otherwise readiness defaults
/// to `true` (matching the existing one-shot semantics).
template <typename A>
concept HasValidate = requires(const A& act) {
    { act.validate() } -> std::convertible_to<bool>;
};

}  // namespace detail

/// @brief Per-action validator used by `BridgeHandler::set<...>` to decide
///        whether the in-progress draft is ready to execute.
///
/// Resolution order (highest-priority first):
///   1. **Explicit specialisation** of `ActionValidator<Action>` — typically via
///      `BRIDGE_REGISTER_VALIDATOR(Action, fn)`. Wins over everything.
///   2. **`bool validate() const`** member on `Action` — picked up automatically.
///      Co-locates the predicate with the data it inspects; preferred when you
///      own the action type.
///   3. **Default** — returns `true`, matching one-shot semantics (the first
///      `set<>` lands and the action fires).
///
/// Validation is intentionally a property of the **action**, not the model:
/// different actions on the same model have different readiness requirements,
/// and keeping the predicate next to the action keeps the GUI side oblivious
/// to model internals.
///
/// @par Example — member function
/// @code
/// struct FormAction {
///     double a;
///     double b;
///     double c;
///     bool validate() const { return a != 0.0 && b != 0.0 && c != 0.0; }
/// };
/// // No macro needed; ActionValidator<FormAction>::ready calls .validate() automatically.
/// @endcode
///
/// @tparam Action Concrete action type.
template <typename Action>
struct ActionValidator {
    /// @brief Returns `true` if the action is in a state that should be executed.
    ///
    /// Auto-detects a `bool validate() const` member on @p action via the
    /// `morph::model::detail::HasValidate` concept. Falls back to `true`.
    ///
    /// @param action Draft action whose readiness is being checked.
    /// @return `true` if the action should fire, `false` to keep collecting fields.
    static constexpr bool ready(const Action& action) {
        if constexpr (detail::HasValidate<Action>) {
            return action.validate();
        } else {
            (void)action;
            return true;
        }
    }
};

/// @brief Whether an action's executions are recorded to an attached action log.
///
/// A strong type instead of a bare `bool` so registration call sites read as
/// intent (`Loggable::No`) rather than an unexplained `false`.
enum class Loggable : std::uint8_t { No, Yes };

/// @brief Per-action policy deciding how repeated executions are checkpointed
///        into a durable action log.
///
/// Deliberately minimal for now — only `coalesce` exists, and it has no
/// registration macro yet. Specialise directly for the rare action where only
/// the latest occurrence should survive a checkpoint (e.g. a form-field edit
/// fired repeatedly via `BridgeHandler::set<...>`); every other action defaults
/// to `false`, meaning every execution is treated as a distinct, permanent fact
/// (the right default for anything resembling a business event).
/// @tparam Action Concrete action type.
template <typename Action>
struct ActionLogPolicy {
    /// @brief If `true`, a checkpoint keeps only the most recent entry per
    ///        `(modelType, entityKey, actionType)`. If `false` (default), every
    ///        entry survives.
    static constexpr bool coalesce = false;
};

namespace detail {

/// @brief Concept satisfied by `ActionTraits<A>` specialisations that expose a
///        `Loggable loggable` static member.
///
/// Lets `actionLoggable()` default to `Loggable::Yes` for hand-written
/// `ActionTraits` specialisations (as used in tests) that predate this member,
/// instead of requiring every existing specialisation to be updated.
template <typename A>
concept HasLoggableFlag = requires {
    { ActionTraits<A>::loggable } -> std::convertible_to<Loggable>;
};

/// @brief Returns `ActionTraits<A>::loggable` if present, otherwise `Loggable::Yes`.
///
/// Logging every action by default (opt out via the macro's 4th argument) means
/// new actions are captured automatically; only actions that are known to be
/// pure queries need to opt out explicitly.
template <typename A>
constexpr Loggable actionLoggable() {
    if constexpr (HasLoggableFlag<A>) {
        return ActionTraits<A>::loggable;
    } else {
        return Loggable::Yes;
    }
}

/// @brief Exception thrown when JSON serialisation or deserialisation fails.
struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Forward declarations so instance() methods inside the classes can reference them.
inline class ActionDispatcher& defaultDispatcher();
inline class ModelRegistryFactory& defaultRegistry();

/// @brief Registry that maps (modelId, actionId) pairs to type-erased runner functions.
///
/// Used by `RemoteServer` to dispatch incoming JSON requests without knowing the
/// concrete model or action types at the call site.
class ActionDispatcher {
public:
    /// @brief Type-erased action runner: deserialises, executes, and serialises the result.
    using Runner = std::function<std::string(IModelHolder&, std::string_view)>;

    /// @brief Registers a runner for `(Model, Action)` under the given string ids.
    ///
    /// This is the single execution site used by `RemoteServer` (every remote and
    /// Qt WebSocket topology) — `Model::execute()` runs here, on whichever process
    /// actually owns @p holder. If a `journal::IActionLog` is attached to @p holder
    /// (via `IModelHolder::attachActionLog`) and `Action` is loggable (the default),
    /// the executed action is recorded automatically after it succeeds.
    template <typename Model, typename Action>
    void registerAction(std::string_view modelId, std::string_view actionId) {
        Key key{std::string{modelId}, std::string{actionId}};
        _runners[key] = [](IModelHolder& holder, std::string_view payloadJson) {
            auto action = ActionTraits<Action>::fromJson(payloadJson);
            auto& model = holder.template into<Model>();
            auto result = model.execute(action);
            auto resultJson = ActionTraits<Action>::resultToJson(result);
            if constexpr (detail::actionLoggable<Action>() == Loggable::Yes) {
                if (holder.hasActionLog()) {
                    // entityKey/principal/timestampMs are filled in by recordIfAttached.
                    holder.recordIfAttached(::morph::journal::LogEntry{
                        .seq = 0,
                        .modelType = std::string{ModelTraits<Model>::typeId()},
                        .entityKey = {},
                        .actionType = std::string{ActionTraits<Action>::typeId()},
                        .payload = std::string{payloadJson},
                        .result = resultJson,
                        .principal = {},
                        .timestampMs = 0,
                    });
                }
            }
            return resultJson;
        };
        _coalesce[key] = ActionLogPolicy<Action>::coalesce;
    }

    /// @brief Dispatches an action against @p holder and returns the JSON-encoded result.
    std::string dispatch(std::string_view modelId, std::string_view actionId, IModelHolder& holder,
                         std::string_view payload) {
        Key key{std::string{modelId}, std::string{actionId}};
        auto iter = _runners.find(key);
        if (iter == _runners.end()) {
            throw std::runtime_error("unknown action: " + key.first + "/" + key.second);
        }
        return iter->second(holder, payload);
    }

    /// @brief Returns whether `(modelId, actionId)` was registered with
    ///        `ActionLogPolicy<Action>::coalesce == true`.
    ///
    /// Used by `journal::SessionLog::checkpoint()` to decide, from the type-erased
    /// `LogEntry` stream, which entries collapse to their latest occurrence.
    /// Unknown pairs (never registered) default to `false` — every entry kept.
    [[nodiscard]] bool coalesce(std::string_view modelId, std::string_view actionId) const {
        auto iter = _coalesce.find(Key{std::string{modelId}, std::string{actionId}});
        return iter != _coalesce.end() && iter->second;
    }

    /// @brief Returns the process-level singleton dispatcher.
    static ActionDispatcher& instance() { return defaultDispatcher(); }

private:
    using Key = std::pair<std::string, std::string>;
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            std::size_t seed = std::hash<std::string>{}(key.first);
            seed ^= std::hash<std::string>{}(key.second) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
    };
    std::unordered_map<Key, Runner, KeyHash> _runners;
    std::unordered_map<Key, bool, KeyHash> _coalesce;
};

/// @brief Registry that creates `IModelHolder` instances by string type-id.
///
/// Used by `RemoteServer` to instantiate models on demand from incoming
/// `"register"` messages.
class ModelRegistryFactory {
public:
    /// @brief Registers a default-construction factory for `Model` under @p modelId.
    template <typename Model>
    void registerModel(std::string_view modelId) {
        _factories.insert_or_assign(std::string{modelId}, [] { return ModelFactory::create<Model>(); });
    }

    /// @brief Creates a new model holder for the type registered under @p modelId.
    std::unique_ptr<IModelHolder> create(std::string_view modelId) {
        auto iter = _factories.find(std::string{modelId});
        if (iter == _factories.end()) {
            throw std::runtime_error("unknown model type: " + std::string{modelId});
        }
        return iter->second();
    }

    /// @brief Returns the process-level singleton registry.
    static ModelRegistryFactory& instance() { return defaultRegistry(); }

private:
    std::unordered_map<std::string, std::function<std::unique_ptr<IModelHolder>()>> _factories;
};

/// @brief Returns the process-level `ActionDispatcher` singleton.
inline ActionDispatcher& defaultDispatcher() {
    static ActionDispatcher inst;
    return inst;
}

/// @brief Returns the process-level `ModelRegistryFactory` singleton.
inline ModelRegistryFactory& defaultRegistry() {
    static ModelRegistryFactory inst;
    return inst;
}

/// @brief Static-init helper for `BRIDGE_REGISTER_MODEL`.
template <typename Model>
inline bool registerModelOnce(std::string_view modelId) noexcept {
    ModelRegistryFactory::instance().registerModel<Model>(modelId);
    return true;
}

/// @brief Static-init helper for `BRIDGE_REGISTER_ACTION`.
template <typename Model, typename Action>
inline bool registerActionOnce(std::string_view modelId, std::string_view actionId) noexcept {
    ActionDispatcher::instance().registerAction<Model, Action>(modelId, actionId);
    return true;
}

/// @brief Static-init helper for `BRIDGE_REGISTER_ACTION`'s generic-execute registration.
///
/// Forward declaration only; the definition is in `bridge.hpp` (after `ActionExecuteRegistry`
/// is visible) to avoid a `registry.hpp` -> `bridge.hpp` include cycle. `bridge.hpp` already
/// includes `registry.hpp`, not the other way round.
template <typename Model, typename Action>
bool registerActionExecutorOnce(std::string_view modelId, std::string_view actionId) noexcept;

}  // namespace detail

}  // namespace morph::model

// NOLINTBEGIN(bugprone-macro-parentheses)

/// @brief Registers model type @p M with the string id @p NAME.
///
/// Specialises `morph::model::ModelTraits<M>` and registers a factory with the
/// process-level `ModelRegistryFactory` at static-init time.
///
/// @param M    Concrete model type.
/// @param NAME String literal used as the type-id.
#define BRIDGE_REGISTER_MODEL(M, NAME)                                         \
    template <>                                                                \
    struct morph::model::ModelTraits<M> {                                      \
        static constexpr std::string_view typeId() noexcept { return NAME; }   \
    };                                                                         \
    namespace {                                                                \
    [[maybe_unused]] const bool bridge_model_reg_##M =                         \
        morph::model::detail::registerModelOnce<M>(NAME);                      \
    }

/// @brief Registers action type @p A (for model @p M) with the string id @p NAME.
///
/// Specialises `morph::model::ActionTraits<A>` with JSON codec functions and
/// registers the action with the process-level `ActionDispatcher` at static-init time.
/// `BRIDGE_REGISTER_MODEL(M, ...)` must be called before this macro.
///
/// An optional 4th argument overrides whether executions of @p A are recorded to
/// an attached action log (see `IModelHolder::attachActionLog`); omitted, it
/// defaults to `morph::model::Loggable::Yes`, so every action is captured unless
/// explicitly opted out — typically for pure queries:
/// @code
/// BRIDGE_REGISTER_ACTION(AccountModel, Deposit,    "Deposit")                            // logged
/// BRIDGE_REGISTER_ACTION(AccountModel, GetAccount, "GetAccount", morph::model::Loggable::No)  // opt out
/// @endcode
///
/// @param M    Concrete model type that handles the action.
/// @param A    Concrete action type.
/// @param NAME String literal used as the action type-id.
/// @param ...  Optional: a `morph::model::Loggable` value (defaults to `Loggable::Yes`).
#define BRIDGE_REGISTER_ACTION(...)                                                                     \
    BRIDGE_REGISTER_ACTION_PICK(__VA_ARGS__, BRIDGE_REGISTER_ACTION_4, BRIDGE_REGISTER_ACTION_3)         \
    (__VA_ARGS__)

/// @cond detail
#define BRIDGE_REGISTER_ACTION_PICK(_1, _2, _3, _4, NAME, ...) NAME

#define BRIDGE_REGISTER_ACTION_3(M, A, NAME) BRIDGE_REGISTER_ACTION_4(M, A, NAME, ::morph::model::Loggable::Yes)

#define BRIDGE_REGISTER_ACTION_4(M, A, NAME, LOGGABLE)                                                   \
    template <>                                                                                          \
    struct morph::model::ActionTraits<A> {                                                               \
        using Result = decltype(std::declval<M&>().execute(std::declval<A>()));                          \
        static constexpr std::string_view typeId() { return NAME; }                                      \
        static constexpr ::morph::model::Loggable loggable = (LOGGABLE);                                 \
        static std::string toJson(const A& action) {                                                     \
            std::string out;                                                                             \
            if (auto errCode = glz::write_json(action, out)) {                                           \
                throw morph::model::detail::ParseError{glz::format_error(errCode, out)};                 \
            }                                                                                            \
            return out;                                                                                  \
        }                                                                                                \
        static A fromJson(std::string_view jsonStr) {                                                    \
            A action{};                                                                                  \
            if (auto errCode = glz::read_json(action, jsonStr)) {                                        \
                throw morph::model::detail::ParseError{glz::format_error(errCode, jsonStr)};             \
            }                                                                                            \
            return action;                                                                               \
        }                                                                                                \
        static std::string resultToJson(const Result& result) {                                          \
            std::string out;                                                                             \
            if (auto errCode = glz::write_json(result, out)) {                                           \
                throw morph::model::detail::ParseError{glz::format_error(errCode, out)};                 \
            }                                                                                            \
            return out;                                                                                  \
        }                                                                                                \
        static Result resultFromJson(std::string_view jsonStr) {                                         \
            Result result{};                                                                             \
            if (auto errCode = glz::read_json(result, jsonStr)) {                                        \
                throw morph::model::detail::ParseError{glz::format_error(errCode, jsonStr)};             \
            }                                                                                            \
            return result;                                                                               \
        }                                                                                                \
    };                                                                                                   \
    namespace {                                                                                          \
    [[maybe_unused]] const bool bridge_action_reg_##M##_##A =                                            \
        morph::model::detail::registerActionOnce<M, A>(morph::model::ModelTraits<M>::typeId(), NAME);    \
    [[maybe_unused]] const bool bridge_action_exec_reg_##M##_##A =                                       \
        morph::model::detail::registerActionExecutorOnce<M, A>(morph::model::ModelTraits<M>::typeId(), NAME); \
    }
/// @endcond

/// @brief Registers a readiness predicate for action @p A.
///
/// Specialises `morph::model::ActionValidator<A>` so that `BridgeHandler::set<...>`
/// only fires `model.execute(draft)` when the predicate returns `true`.
///
/// @param A  Concrete action type.
/// @param FN Callable `bool(const A&)`.
#define BRIDGE_REGISTER_VALIDATOR(A, FN)                                       \
    template <>                                                                \
    struct morph::model::ActionValidator<A> {                                  \
        static bool ready(const A& action) { return (FN)(action); }            \
    };
// NOLINTEND(bugprone-macro-parentheses)
