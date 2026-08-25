// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cassert>
#include <concepts>
#include <cstdint>
#include <functional>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/util/rational.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "../forms/forms.hpp"
#include "model.hpp"
#include "payload_schema.hpp"

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

/// @brief Hash functor for `std::pair<std::string, std::string>` keys used by registries.
/// Shared between `ActionDispatcher` (server-side dispatch) and `ActionExecuteRegistry`
/// (client-side generic-execute) since their key types are structurally identical.
struct PairKeyHash {
    /// @brief Combines the hashes of `key.first` and `key.second`.
    /// @param key The pair to hash.
    /// @return The combined hash value.
    [[nodiscard]] std::size_t operator()(const std::pair<std::string, std::string>& key) const noexcept {
        std::size_t seed = std::hash<std::string>{}(key.first);
        // NOLINTNEXTLINE(readability-magic-numbers, cppcoreguidelines-avoid-magic-numbers)
        seed ^= std::hash<std::string>{}(key.second) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }
};

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

/// @brief Thrown when a decoded action fails `ActionValidator<Action>::ready(...)`
///        on a server-side or local execution path, before `Model::execute` runs.
///
/// Raised by `morph::model::detail::ActionDispatcher::registerAction`'s runner
/// (the server dispatch path used by `RemoteServer`, every remote and Qt
/// WebSocket topology) and by `Bridge::executeVia`'s `localOp` (the in-process
/// path used by `LocalBackend`) — the two execution sites an in-progress action
/// reaches without first passing through the reactive `set<>` gate
/// (`BridgeHandler::tryFireImpl`) or the type-erased `executeJson` gate
/// (`ActionExecuteRegistry::registerAction`), both of which already enforce
/// `ready()` themselves. Deriving from `std::runtime_error` means existing
/// `catch (const std::exception&)` handling — e.g. `RemoteServer::dispatchExecute`'s
/// strand catch, which turns any thrown exception into an `err` reply — keeps
/// working unchanged; callers that care can `catch`/`dynamic_cast` the specific
/// type instead.
struct ValidationError : std::runtime_error {
    /// @brief Constructs the error with a message of the form
    ///        `"action failed validation: <modelType>/<actionType>"`.
    /// @param modelType  `ModelTraits<Model>::typeId()` of the target model.
    /// @param actionType `ActionTraits<Action>::typeId()` of the rejected action.
    ValidationError(std::string_view modelType, std::string_view actionType)
        : std::runtime_error("action failed validation: " + std::string{modelType} + "/" + std::string{actionType}) {}
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

/// @brief Concept satisfied by `ActionTraits<A>` specialisations that expose a
///        `payloadSchema()` static member.
///
/// Both `BRIDGE_REGISTER_ACTION` and `BRIDGE_REGISTER_ACTION_FOR_CLIENT`
/// generate one; a hand-written `ActionTraits` does not have to.
template <typename A>
concept HasPayloadSchema = requires {
    { ActionTraits<A>::payloadSchema() } -> std::convertible_to<std::string_view>;
};

/// @brief Returns `ActionTraits<A>::payloadSchema()` if the specialisation
///        provides one, otherwise the empty string.
///
/// **The fingerprint is a property of the codec, not of the action type.** The
/// macro-generated codecs read and write through Glaze's reflection, so their
/// JSON shape *is* the struct's reflected shape and
/// `morph::model::payloadFingerprint` describes it exactly. A hand-written
/// `ActionTraits` may map its struct to entirely different JSON -- it may not
/// even be a reflectable type (`test_client_execute_deadline.cpp` registers an
/// anonymous-namespace struct, which Glaze's traditional reflection cannot name
/// at all) -- so deriving a "shape" from the struct would describe something
/// that never reaches the journal.
///
/// Empty means *unstamped*: entries for such an action are recorded without a
/// fingerprint and replay treats them exactly as every build before the
/// fingerprint existed did (see `journal::UnstampedPayloadPolicy`). A
/// hand-written specialisation that wants the guarantee opts in by defining its
/// own `payloadSchema()`.
///
/// @tparam A Action type.
/// @return The fingerprint, or `""` when the specialisation provides none.
template <typename A>
inline std::string actionPayloadSchema() {
    if constexpr (HasPayloadSchema<A>) {
        return std::string{ActionTraits<A>::payloadSchema()};
    } else {
        return {};
    }
}

/// @brief Exception thrown when JSON serialisation or deserialisation fails.
struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// @brief Write options for an action's/result's own JSON body: identical to
///        `morph::wire::detail::EscapingWriteOpts`, applied one layer down.
///
/// The envelope codec already escapes ASCII control bytes (see
/// `docs/spec/core/wire.md`, "Control bytes in string fields"); the action and
/// result bodies it carries are serialized here, separately, and need exactly
/// the same treatment for exactly the same two reasons. With the option off,
/// a raw `0x00`–`0x1F` in any caller-supplied string field of an action makes
/// the body invalid JSON that the peer's own reader rejects — and, when the
/// same string also contains an escaped character, glaze's chunked fast path
/// silently rewrites such a byte as two `0x00`s, destroying the payload in a
/// way that still decodes. Action bodies are pure caller data (a paste's
/// content, a message, a filename), so this is if anything more exposed than
/// the envelope was.
///
/// Deliberately duplicated rather than reused from `morph::wire`: the action
/// codec belongs to the model layer and must not acquire a dependency on the
/// transport layer's header just to share a four-line option struct.
///
/// Applies to writing only — glaze's reader already accepts `\\uXXXX`.
struct EscapingWriteOpts : glz::opts {
    /// @brief Emit control bytes as `\\uXXXX` rather than raw.
    // NOLINTNEXTLINE(readability-identifier-naming) — the name is glaze's, not ours; the option is matched by name.
    bool escape_control_characters = true;
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
    /// actually owns @p holder. Before `Model::execute` runs, the runner reconciles
    /// any `Quantity` fields to their declared precision
    /// (`morph::forms::reconcileDeclaredPrecision`), overwrites any declared
    /// computed fields from their inputs (`morph::forms::recomputeAll`,
    /// `forms.hpp`), and enforces `ActionValidator<Action>::ready(action)`,
    /// throwing `ValidationError` when it returns `false` — the same checks the
    /// client bridge dispatch path (`ActionExecuteRegistry::registerAction`,
    /// `bridge.hpp`) already performs. If a `journal::IActionLog` is attached to
    /// @p holder (via `IModelHolder::attachActionLog`) and `Action` is loggable
    /// (the default), the executed action is recorded automatically -- on a
    /// successful `Model::execute` with `outcome = Outcome::Succeeded` and the
    /// JSON result, and equally on a thrown `std::exception` (a validation
    /// failure, a rejected write) with `outcome = Outcome::Failed` and
    /// `error = what()`, `result` empty. Either way the exception (if any)
    /// propagates unchanged after the entry is recorded.
    ///
    /// Every recorded entry is stamped with `payloadFingerprint<Action>()` in
    /// `LogEntry::schema`, and the same fingerprint is filed under
    /// `(modelId, actionId)` for `schemaFor()` to hand back to the type-erased
    /// replay path. That pair is what lets `journal::replay()` notice that the
    /// shape which wrote an entry is not the shape it is about to decode it
    /// with -- see `docs/spec/journal/journal.md`, "Payload schema fingerprint".
    /// @throws ValidationError if the decoded action fails `ActionValidator<Action>::ready`.
    template <typename Model, typename Action>
    void registerAction(std::string_view modelId, std::string_view actionId) {
        Key const key{std::string{modelId}, std::string{actionId}};
        _runners[key] = [](IModelHolder& holder, std::string_view payloadJson) {
            auto action = ActionTraits<Action>::fromJson(payloadJson);
            // Retag any Quantity fields to their declared precision so a
            // hand-built wire payload matches the schema's advertised
            // x-decimalPlaces, exactly as the client bridge dispatch path
            // (ActionExecuteRegistry::registerAction, bridge.hpp) already does.
            // No-op for actions with no Quantity members. See
            // docs/spec/forms/forms.md.
            ::morph::forms::reconcileDeclaredPrecision(action);
            // Pre-decode wire validation seam: reject a Quantity field whose
            // engaged value falls outside its unit's declared bounds
            // (UnitTraits<E>::bounds), before the action's own validate()
            // (a business-rule check) ever runs. No-op for actions with no
            // Quantity members, or whose units declare no bounds(). See
            // docs/spec/forms/forms.md, "Pre-decode wire validation".
            ::morph::forms::enforceQuantityBounds(action);
            // Overwrite any computed fields from their declared inputs. This is
            // the true server-side execution site for every remote and Qt
            // WebSocket topology (RemoteServer -> ActionDispatcher::dispatch) --
            // the one path a hand-built wire envelope reaches directly,
            // bypassing every client-side gate. A tampered computed value on
            // the wire is discarded here, before the validator check and
            // Model::execute run. No-op for actions with no computedFields. See
            // docs/spec/forms/forms.md.
            ::morph::forms::recomputeAll(action);
            // Enforce the action's validator on the server dispatch path — the
            // one path an untrusted remote client can drive directly with a
            // hand-built envelope, bypassing the client-side gates
            // (BridgeHandler::set<>'s tryFireImpl and
            // ActionExecuteRegistry::registerAction). ActionValidator<Action>::ready
            // auto-detects a `bool validate() const` member and defaults to
            // `true` for actions with no validator, so this is a no-op for
            // unvalidated actions (zero behavior change) and a hard gate for
            // validated ones. The exception propagates out of this lambda to
            // ActionDispatcher::dispatch's caller (RemoteServer::dispatchExecute's
            // strand catch turns it into an `err` reply).
            if (!ActionValidator<Action>::ready(action)) {
                throw ValidationError{ModelTraits<Model>::typeId(), ActionTraits<Action>::typeId()};
            }
            auto& model = holder.template into<Model>();
            // Both the success and failure paths below record a journal entry
            // (when a log is attached and Action is loggable) so a rejected or
            // throwing execution -- a validation failure, a lost connection, a
            // rejected write -- still leaves an audit trail, not silence. The
            // exception is rethrown unchanged either way; only the outcome
            // shape differs. See docs/spec/journal/journal.md, "Outcome".
            try {
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
                            .schema = detail::actionPayloadSchema<Action>(),
                            .result = resultJson,
                            .outcome = ::morph::journal::Outcome::Succeeded,
                            .error = {},
                            .principal = {},
                            .timestampMs = 0,
                        });
                    }
                }
                return resultJson;
            } catch (const std::exception& exc [[maybe_unused]]) {
                if constexpr (detail::actionLoggable<Action>() == Loggable::Yes) {
                    if (holder.hasActionLog()) {
                        holder.recordIfAttached(::morph::journal::LogEntry{
                            .seq = 0,
                            .modelType = std::string{ModelTraits<Model>::typeId()},
                            .entityKey = {},
                            .actionType = std::string{ActionTraits<Action>::typeId()},
                            .payload = std::string{payloadJson},
                            .schema = detail::actionPayloadSchema<Action>(),
                            .result = {},
                            .outcome = ::morph::journal::Outcome::Failed,
                            .error = exc.what(),
                            .principal = {},
                            .timestampMs = 0,
                        });
                    }
                }
                throw;
            }
        };
        _coalesce[key] = ActionLogPolicy<Action>::coalesce;
        _schema[key] = detail::actionPayloadSchema<Action>();
    }

    /// @brief Dispatches an action against @p holder and returns the JSON-encoded result.
    std::string dispatch(std::string_view modelId, std::string_view actionId, IModelHolder& holder,
                         std::string_view payload) {
        Key const key{std::string{modelId}, std::string{actionId}};
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

    /// @brief Returns the payload fingerprint `(modelId, actionId)` was
    ///        registered with, or an empty string if the pair is unregistered.
    ///
    /// This is the type-erased half of the journal's payload-evolution check:
    /// `registerAction` knows the concrete `Action` and can compute
    /// `payloadFingerprint<Action>()`; `journal::replay()` sees only strings on
    /// a `LogEntry` and needs to ask for the current build's fingerprint by id.
    /// The empty return is not an error -- an entry naming an unregistered
    /// action fails at `dispatch()` with "unknown action" a moment later, which
    /// is the better diagnostic for that case.
    ///
    /// @param modelId  Model type-id.
    /// @param actionId Action type-id.
    /// @return The registered fingerprint, or `""` if the pair is unregistered.
    [[nodiscard]] std::string schemaFor(std::string_view modelId, std::string_view actionId) const {
        auto iter = _schema.find(Key{std::string{modelId}, std::string{actionId}});
        return iter == _schema.end() ? std::string{} : iter->second;
    }

    /// @brief Returns the process-level singleton dispatcher.
    static ActionDispatcher& instance() { return defaultDispatcher(); }

private:
    using Key = std::pair<std::string, std::string>;
    std::unordered_map<Key, Runner, PairKeyHash> _runners;
    std::unordered_map<Key, bool, PairKeyHash> _coalesce;
    std::unordered_map<Key, std::string, PairKeyHash> _schema;
};

/// @brief Registry that creates `IModelHolder` instances by string type-id.
///
/// Used by `RemoteServer` to instantiate models on demand from incoming
/// `"register"` messages.
class ModelRegistryFactory {
public:
    /// @brief Registers a default-construction factory for `Model` under @p modelId.
    ///
    /// Equivalent to `registerModel<Model>(modelId, [] { return
    /// ModelFactory::create<Model>(); })` — the plain default-construction path
    /// every `BRIDGE_REGISTER_MODEL` invocation uses.
    /// @param modelId String id to register the factory under.
    template <typename Model>
    void registerModel(std::string_view modelId) {
        _factories.insert_or_assign(std::string{modelId}, [] { return ModelFactory::create<Model>(); });
    }

    /// @brief Registers a custom construction hook for `Model` under @p modelId.
    ///
    /// The per-instance dependency-injection seam for registry-constructed
    /// (`Socket`-mode / `RemoteServer`) models — the equivalent of
    /// `Bridge::HandlerBinding::modelFactory` for the client-side `Local`-mode
    /// path. @p factory runs once per incoming `"register"` message (and once
    /// per fresh shared-instance creation) and may capture arbitrary
    /// per-instance dependencies (an injectable clock, a secondary log handle,
    /// a feature flag) that `Model`'s default constructor cannot reach — this
    /// is also the seam a model whose only constructor takes arguments (i.e.
    /// is not default-constructible at all) needs in order to be registered
    /// for remote/socket instantiation in the first place.
    ///
    /// @p factory returns an owning pointer to a freshly built holder (e.g.
    /// `std::make_unique<ModelHolder<Model>>(...)`), not a bare `Model` — the
    /// caller controls construction end-to-end, including passing
    /// constructor arguments `ModelHolder`'s forwarding constructor accepts.
    /// This overload does **not** auto-attach the process-wide default action
    /// log the way the default-construction overload does: a caller supplying
    /// its own factory is assumed to attach whatever log/identity it needs
    /// (via `IModelHolder::attachActionLog`) inside the closure, or to rely on
    /// `RemoteServer`'s `LogProvider` doing so afterward, exactly as the
    /// default path allows.
    ///
    /// @tparam Model   Concrete model type. Used only to assert (debug builds)
    ///                 that @p factory's returned holder actually reports
    ///                 `typeid(Model)` — @p factory is otherwise free to build
    ///                 the holder however it likes, so nothing *prevents* a
    ///                 mismatched `Model`/@p factory pairing at compile time.
    /// @tparam Factory Callable returning an owning pointer convertible to
    ///                 `std::unique_ptr<IModelHolder>`; deduced from @p factory.
    /// @param modelId  String id to register the factory under.
    /// @param factory  Callable that constructs and returns a fresh holder.
    template <typename Model, typename Factory>
        requires std::invocable<Factory> &&
                 std::convertible_to<std::invoke_result_t<Factory>, std::unique_ptr<IModelHolder>>
    void registerModel(std::string_view modelId, Factory factory) {
        _factories.insert_or_assign(std::string{modelId}, [factory = std::move(factory)]() mutable {
            std::unique_ptr<IModelHolder> holder{factory()};
            assert(!holder ||
                   (holder->type() == std::type_index(typeid(Model)) &&
                    "registerModel<Model>(modelId, factory): factory returned a holder for a different type"));
            return holder;
        });
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
///
/// HARD REQUIREMENT: `BRIDGE_REGISTER_ACTION_4` calls this function unconditionally, but only
/// declares it here — it does NOT define it. Any translation unit that invokes
/// `BRIDGE_REGISTER_ACTION` MUST include `<morph/core/bridge.hpp>` (directly or transitively)
/// somewhere in the same translation unit, or the build will fail to link with an unresolved
/// external symbol for this function.
template <typename Model, typename Action>
bool registerActionExecutorOnce(std::string_view modelId, std::string_view actionId) noexcept;

}  // namespace detail

}  // namespace morph::model

// NOLINTBEGIN(bugprone-macro-parentheses)

// Keying the generated registrar names on `__COUNTER__` (rather than on the type spelling)
// keeps them valid identifiers regardless of how `M`/`A` are written. A namespace-qualified
// or template type (e.g. `app::models::Report`) would otherwise be pasted directly into the
// identifier, producing invalid tokens like `bridge_model_reg_app::models::Report`.
// `__LINE__` was tried first, but it is only unique *within a single physical file*: two
// headers that each invoke this macro on the same line number (e.g. `card_model.hpp:38` and
// `payment_model.hpp:38`) produce the same identifier once both are `#include`d into one
// translation unit, and since C++ unnamed namespaces are per-TU (not per-file), that is a
// hard redefinition error. `__COUNTER__` increments monotonically for the entire translation
// unit regardless of which file expands it, so it cannot collide this way. The extra
// indirection (`_CAT` calling `_CAT_`) is required so `__COUNTER__` is expanded to its numeric
// value before the paste, rather than being pasted as the literal text "__COUNTER__".
#define BRIDGE_DETAIL_CAT_(a, b) a##b
#define BRIDGE_DETAIL_CAT(a, b) BRIDGE_DETAIL_CAT_(a, b)

/// @brief Suppresses the two registrars whose bodies reference a model's
///        constructor/`execute()` definitions (`registerModelOnce`,
///        `registerActionOnce`), for a pure client that dispatches every
///        action to a remote peer and never constructs a model locally.
///
/// A model's constructor and `Model::execute` are ordinary functions the
/// compiler emits a call to inside these registrars' lambda bodies, whether
/// or not that lambda is ever invoked at runtime — so a plain client-only
/// build still forces the linker to resolve them, pulling in implementations
/// (a database driver, a native UI framework, an OS-specific API) the client
/// target may have no link path for at all, and will never call regardless.
/// The third registrar (`registerActionExecutorOnce`, `bridge.hpp`) routes
/// through `Bridge::executeVia` -> `IBackend::execute` (fully abstract) and
/// needs only the action's JSON codecs plus `Model::execute`'s *declaration*
/// (for `ActionTraits::Result`, via `decltype`) — never its definition — so
/// it is unaffected and always emitted.
///
/// Defined on the `morph` target's INTERFACE via the `MORPH_CLIENT_ONLY`
/// CMake option, never per translation unit: two TUs disagreeing on whether a
/// model registers itself would violate ODR. See docs/spec/core/registry.md,
/// "MORPH_CLIENT_ONLY".
///
/// @warning NEVER define this for a process that hosts models (a server, or
/// any `Bridge` running `LocalBackend`) — it silently registers nothing, and
/// the model fails at runtime with "unknown model type" rather than at
/// compile/link time.
#ifdef MORPH_CLIENT_ONLY
#define MORPH_DETAIL_REGISTER_MODEL_LOCAL(M, NAME)
#define MORPH_DETAIL_REGISTER_ACTION_LOCAL(M, A, NAME)
#else
// clang-format off
// Hand-aligned: clang-format pulls the short registerModelOnce() call up onto the
// BRIDGE_DETAIL_CAT line and then breaks *inside* the token-paste invocation, which
// also splits the pair apart visually -- MORPH_DETAIL_REGISTER_ACTION_LOCAL has a
// longer right-hand side and so survives untouched. Keep the two parallel by hand.
#define MORPH_DETAIL_REGISTER_MODEL_LOCAL(M, NAME)                                                       \
    namespace {                                                                                          \
    [[maybe_unused]] const bool BRIDGE_DETAIL_CAT(bridge_model_reg_, __COUNTER__) =                      \
        morph::model::detail::registerModelOnce<M>(NAME);                                                \
    }
#define MORPH_DETAIL_REGISTER_ACTION_LOCAL(M, A, NAME)                                                   \
    namespace {                                                                                          \
    [[maybe_unused]] const bool BRIDGE_DETAIL_CAT(bridge_action_reg_, __COUNTER__) =                     \
        morph::model::detail::registerActionOnce<M, A>(morph::model::ModelTraits<M>::typeId(), NAME);    \
    }
// clang-format on
#endif

/// @brief Registers model type @p M with the string id @p NAME.
///
/// Specialises `morph::model::ModelTraits<M>` and registers a factory with the
/// process-level `ModelRegistryFactory` at static-init time — unless
/// `MORPH_CLIENT_ONLY` is defined, in which case that registrar is suppressed
/// (see `MORPH_DETAIL_REGISTER_MODEL_LOCAL`'s doc comment above).
///
/// @param M    Concrete model type.
/// @param NAME String literal used as the type-id.
// clang-format off -- public macro surface: hand-aligned on purpose.
// These definitions are the framework's documented API; contributors read them
// as reference, and the continuation backslashes line up so the body is legible
// as a block. Leaving them to the formatter means any unrelated edit nearby
// re-wraps the whole definition, and in one case it broke a token-paste
// invocation apart. Freeze them; realign by hand if a body changes.
#define BRIDGE_REGISTER_MODEL(M, NAME)                                       \
    template <>                                                              \
    struct morph::model::ModelTraits<M> {                                    \
        static constexpr std::string_view typeId() noexcept { return NAME; } \
    };                                                                       \
    MORPH_DETAIL_REGISTER_MODEL_LOCAL(M, NAME)
// clang-format on

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
/// HARD REQUIREMENT: this macro's expansion unconditionally calls
/// `morph::model::detail::registerActionExecutorOnce<M, A>`, which is only *declared* in
/// `registry.hpp` and *defined* in `<morph/core/bridge.hpp>`. Every translation unit that invokes
/// `BRIDGE_REGISTER_ACTION` MUST include `<morph/core/bridge.hpp>` (directly or transitively) in
/// that same translation unit, or the build will fail to link with an unresolved external
/// symbol for `registerActionExecutorOnce<M, A>`.
///
/// @param M    Concrete model type that handles the action.
/// @param A    Concrete action type.
/// @param NAME String literal used as the action type-id.
/// @param ...  Optional: a `morph::model::Loggable` value (defaults to `Loggable::Yes`).
// NOLINTBEGIN(cppcoreguidelines-macro-usage) — registration macros are the intended public API
// clang-format off -- public macro surface: hand-aligned on purpose.
// These definitions are the framework's documented API; contributors read them
// as reference, and the continuation backslashes line up so the body is legible
// as a block. Leaving them to the formatter means any unrelated edit nearby
// re-wraps the whole definition, and in one case it broke a token-paste
// invocation apart. Freeze them; realign by hand if a body changes.
#define BRIDGE_REGISTER_ACTION(...)                                                              \
    BRIDGE_REGISTER_ACTION_PICK(__VA_ARGS__, BRIDGE_REGISTER_ACTION_4, BRIDGE_REGISTER_ACTION_3) \
    (__VA_ARGS__)
// clang-format on

/// @brief Registers action type @p A (for model @p M) the same way
///        `BRIDGE_REGISTER_ACTION` does, but names the action's result type
///        @p RESULT explicitly instead of deducing it from
///        `decltype(std::declval<M&>().execute(std::declval<A>()))`.
///
/// `BRIDGE_REGISTER_ACTION`'s `Result` deduction requires `M` to be a
/// **complete type with `execute(A)` declared** at the point of registration
/// — ordinarily the model's own header, which a client must then `#include`
/// (and everything that header transitively pulls in — a persistence mixin's
/// database-driver headers, for a model backed by one) just to compute
/// `ActionTraits<A>::Result`, even under `MORPH_CLIENT_ONLY`, where the
/// client never constructs `M` or calls `M::execute` at all (see
/// `docs/spec/core/registry.md`, "MORPH_CLIENT_ONLY").
///
/// A pure client names @p RESULT directly instead, so @p M can be a
/// lightweight, declaration-only facade type — forward-declared or declaring
/// no members at all — carrying none of the real model's persistence
/// surface. @p M still needs `BRIDGE_REGISTER_MODEL(M, ...)` (for
/// `ModelTraits<M>::typeId()`, itself needing no completeness either) and is
/// used only as `BridgeHandler<M>`'s template tag and dispatch-registry key;
/// nothing on the path a `MORPH_CLIENT_ONLY` build actually calls
/// (`BridgeHandler<M>::execute<A>()`/`executeJson`, routed through
/// `Bridge::executeVia`) reads a member of `M`, so `M` need not be complete
/// at the call site at all.
///
/// A build that does **not** define `MORPH_CLIENT_ONLY` may also use this
/// macro — the emitted `ActionTraits<A>` specialisation is identical either
/// way — but gets none of the header-avoidance benefit unless `M` really is
/// left incomplete, since something in that same link must still define the
/// real model to satisfy `MORPH_DETAIL_REGISTER_MODEL_LOCAL`/
/// `MORPH_DETAIL_REGISTER_ACTION_LOCAL`'s registrars (suppressed only under
/// `MORPH_CLIENT_ONLY`).
///
/// @warning @p RESULT must name the exact same type `BRIDGE_REGISTER_ACTION`
/// would have deduced from the real model's `M::execute(A)`. Nothing checks
/// this — a mismatch is a silent JSON (de)serialisation bug (wrong shape
/// on the wire), not a compile error, since `resultToJson`/`resultFromJson`
/// below are instantiated against whatever @p RESULT names, and the server
/// side (registered separately, from the real model's own header, via the
/// plain `BRIDGE_REGISTER_ACTION`) still serialises the type `M::execute`
/// actually returns.
///
/// @param M      Model type -- may be a declaration-only facade under
///               `MORPH_CLIENT_ONLY`; must be the real, complete model type
///               otherwise (see above).
/// @param A      Concrete action type.
/// @param RESULT The action's result type, named explicitly.
/// @param NAME   String literal used as the action type-id.
/// @param ...    Optional: a `morph::model::Loggable` value (defaults to `Loggable::Yes`).
// clang-format off -- public macro surface: hand-aligned on purpose.
// These definitions are the framework's documented API; contributors read them
// as reference, and the continuation backslashes line up so the body is legible
// as a block. Leaving them to the formatter means any unrelated edit nearby
// re-wraps the whole definition, and in one case it broke a token-paste
// invocation apart. Freeze them; realign by hand if a body changes.
#define BRIDGE_REGISTER_ACTION_FOR_CLIENT(...)                                               \
    BRIDGE_REGISTER_ACTION_FOR_CLIENT_PICK(__VA_ARGS__, BRIDGE_REGISTER_ACTION_FOR_CLIENT_5, \
                                           BRIDGE_REGISTER_ACTION_FOR_CLIENT_4)              \
    (__VA_ARGS__)
// clang-format on

/// @cond detail
#define BRIDGE_REGISTER_ACTION_FOR_CLIENT_PICK(_1, _2, _3, _4, _5, NAME, ...) NAME

// clang-format off -- public macro surface: hand-aligned on purpose.
// These definitions are the framework's documented API; contributors read them
// as reference, and the continuation backslashes line up so the body is legible
// as a block. Leaving them to the formatter means any unrelated edit nearby
// re-wraps the whole definition, and in one case it broke a token-paste
// invocation apart. Freeze them; realign by hand if a body changes.
#define BRIDGE_REGISTER_ACTION_FOR_CLIENT_4(M, A, RESULT, NAME) \
    BRIDGE_REGISTER_ACTION_FOR_CLIENT_5(M, A, RESULT, NAME, ::morph::model::Loggable::Yes)
// clang-format on

// clang-format off -- public macro surface: hand-aligned on purpose.
// These definitions are the framework's documented API; contributors read them
// as reference, and the continuation backslashes line up so the body is legible
// as a block. Leaving them to the formatter means any unrelated edit nearby
// re-wraps the whole definition, and in one case it broke a token-paste
// invocation apart. Freeze them; realign by hand if a body changes.
#define BRIDGE_REGISTER_ACTION_FOR_CLIENT_5(M, A, RESULT, NAME, LOGGABLE)                                      \
    template <>                                                                                                \
    struct morph::model::ActionTraits<A> {                                                                     \
        using Result = RESULT;                                                                                 \
        static constexpr std::string_view typeId() { return NAME; }                                            \
        static constexpr ::morph::model::Loggable loggable = (LOGGABLE);                                       \
        /* Structural fingerprint of the JSON shape the codecs below read and write.  */                       \
        /* Stamped on every journal entry and checked on replay -- see                */                       \
        /* morph::model::payloadFingerprint and docs/spec/journal/journal.md.          */                      \
        static const std::string& payloadSchema() { return ::morph::model::payloadFingerprint<A>(); }          \
        static std::string toJson(const A& action) {                                                           \
            std::string out;                                                                                   \
            /* EscapingWriteOpts, not write_json: a raw control byte in any     */                             \
            /* caller-supplied string field would otherwise produce a body the  */                             \
            /* peer's reader rejects, or be silently mangled by glaze's chunked */                             \
            /* fast path — see its doc comment in registry.hpp.                 */                             \
            if (auto errCode = glz::write<::morph::model::detail::EscapingWriteOpts{}>(action, out)) {         \
                throw morph::model::detail::ParseError{glz::format_error(errCode, out)};                       \
            }                                                                                                  \
            return out;                                                                                        \
        }                                                                                                      \
        static A fromJson(std::string_view jsonStr) {                                                          \
            A action{};                                                                                        \
            /* null_terminated=false: jsonStr is a caller-supplied view with no      */                        \
            /* guaranteed trailing '\0' (e.g. an execute envelope's `body`) — see    */                        \
            /* the identical fix + rationale on morph::wire::decode (wire.hpp).      */                        \
            static constexpr glz::opts kLenientRead{.null_terminated = false, .error_on_unknown_keys = false}; \
            /* The codec boundary for an action payload: morph::wire carries `body` as                         \
               an opaque string and never parses it, so this is the first and only                             \
               place a Rational inside it is decoded. A Rational decode cannot fail --                         \
               it clamps what it cannot represent -- so {"num":5,"den":0,"dp":2} would                         \
               otherwise arrive as a plausible 5/1 that no model-level validate() could                        \
               recognise as altered. Deciding that a silently-altered payload is a                             \
               protocol violation belongs here, where we know the bytes came off a wire. */                    \
            ::morph::math::WireClampScope clampedRationals;                                                    \
            if (auto errCode = glz::read<kLenientRead>(action, jsonStr)) {                                     \
                throw morph::model::detail::ParseError{glz::format_error(errCode, jsonStr)};                   \
            }                                                                                                  \
            if (clampedRationals.clamped() != 0) {                                                             \
                throw morph::model::detail::ParseError{                                                        \
                    "action body contains a Rational that cannot be represented exactly"};                     \
            }                                                                                                  \
            return action;                                                                                     \
        }                                                                                                      \
        static std::string resultToJson(const Result& result) {                                                \
            std::string out;                                                                                   \
            /* EscapingWriteOpts: see toJson() above — a result body carries    */                             \
            /* caller data back (a paste's content, a fetched record) and needs */                             \
            /* the identical treatment.                                          */                            \
            if (auto errCode = glz::write<::morph::model::detail::EscapingWriteOpts{}>(result, out)) {         \
                throw morph::model::detail::ParseError{glz::format_error(errCode, out)};                       \
            }                                                                                                  \
            return out;                                                                                        \
        }                                                                                                      \
        static Result resultFromJson(std::string_view jsonStr) {                                               \
            Result result{};                                                                                   \
            /* null_terminated=false: see the identical fix on fromJson() above. */                            \
            static constexpr glz::opts kLenientRead{.null_terminated = false, .error_on_unknown_keys = false}; \
            if (auto errCode = glz::read<kLenientRead>(result, jsonStr)) {                                     \
                throw morph::model::detail::ParseError{glz::format_error(errCode, jsonStr)};                   \
            }                                                                                                  \
            return result;                                                                                     \
        }                                                                                                      \
    };                                                                                                         \
    MORPH_DETAIL_REGISTER_ACTION_LOCAL(M, A, NAME)                                                             \
    namespace {                                                                                                \
    [[maybe_unused]] const bool BRIDGE_DETAIL_CAT(bridge_action_exec_reg_, __COUNTER__) =                      \
        morph::model::detail::registerActionExecutorOnce<M, A>(morph::model::ModelTraits<M>::typeId(), NAME);  \
    }
// clang-format on
/// @endcond

/// @cond detail
#define BRIDGE_REGISTER_ACTION_PICK(_1, _2, _3, _4, NAME, ...) NAME

#define BRIDGE_REGISTER_ACTION_3(M, A, NAME) BRIDGE_REGISTER_ACTION_4(M, A, NAME, ::morph::model::Loggable::Yes)

// clang-format off -- public macro surface: hand-aligned on purpose.
// These definitions are the framework's documented API; contributors read them
// as reference, and the continuation backslashes line up so the body is legible
// as a block. Leaving them to the formatter means any unrelated edit nearby
// re-wraps the whole definition, and in one case it broke a token-paste
// invocation apart. Freeze them; realign by hand if a body changes.
#define BRIDGE_REGISTER_ACTION_4(M, A, NAME, LOGGABLE)                                                         \
    template <>                                                                                                \
    struct morph::model::ActionTraits<A> {                                                                     \
        using Result = decltype(std::declval<M&>().execute(std::declval<A>()));                                \
        static constexpr std::string_view typeId() { return NAME; }                                            \
        static constexpr ::morph::model::Loggable loggable = (LOGGABLE);                                       \
        /* Structural fingerprint of the JSON shape the codecs below read and write.  */                       \
        /* Stamped on every journal entry and checked on replay -- see                */                       \
        /* morph::model::payloadFingerprint and docs/spec/journal/journal.md.          */                      \
        static const std::string& payloadSchema() { return ::morph::model::payloadFingerprint<A>(); }          \
        static std::string toJson(const A& action) {                                                           \
            std::string out;                                                                                   \
            /* EscapingWriteOpts, not write_json: a raw control byte in any     */                             \
            /* caller-supplied string field would otherwise produce a body the  */                             \
            /* peer's reader rejects, or be silently mangled by glaze's chunked */                             \
            /* fast path — see its doc comment in registry.hpp.                 */                             \
            if (auto errCode = glz::write<::morph::model::detail::EscapingWriteOpts{}>(action, out)) {         \
                throw morph::model::detail::ParseError{glz::format_error(errCode, out)};                       \
            }                                                                                                  \
            return out;                                                                                        \
        }                                                                                                      \
        static A fromJson(std::string_view jsonStr) {                                                          \
            A action{};                                                                                        \
            /* null_terminated=false: jsonStr is a caller-supplied view with no      */                        \
            /* guaranteed trailing '\0' (e.g. an execute envelope's `body`) — see    */                        \
            /* the identical fix + rationale on morph::wire::decode (wire.hpp).      */                        \
            static constexpr glz::opts kLenientRead{.null_terminated = false, .error_on_unknown_keys = false}; \
            /* The codec boundary for an action payload: morph::wire carries `body` as                         \
               an opaque string and never parses it, so this is the first and only                             \
               place a Rational inside it is decoded. A Rational decode cannot fail --                         \
               it clamps what it cannot represent -- so {"num":5,"den":0,"dp":2} would                         \
               otherwise arrive as a plausible 5/1 that no model-level validate() could                        \
               recognise as altered. Deciding that a silently-altered payload is a                             \
               protocol violation belongs here, where we know the bytes came off a wire. */                    \
            ::morph::math::WireClampScope clampedRationals;                                                    \
            if (auto errCode = glz::read<kLenientRead>(action, jsonStr)) {                                     \
                throw morph::model::detail::ParseError{glz::format_error(errCode, jsonStr)};                   \
            }                                                                                                  \
            if (clampedRationals.clamped() != 0) {                                                             \
                throw morph::model::detail::ParseError{                                                        \
                    "action body contains a Rational that cannot be represented exactly"};                     \
            }                                                                                                  \
            return action;                                                                                     \
        }                                                                                                      \
        static std::string resultToJson(const Result& result) {                                                \
            std::string out;                                                                                   \
            /* EscapingWriteOpts: see toJson() above — a result body carries    */                             \
            /* caller data back (a paste's content, a fetched record) and needs */                             \
            /* the identical treatment.                                          */                            \
            if (auto errCode = glz::write<::morph::model::detail::EscapingWriteOpts{}>(result, out)) {         \
                throw morph::model::detail::ParseError{glz::format_error(errCode, out)};                       \
            }                                                                                                  \
            return out;                                                                                        \
        }                                                                                                      \
        static Result resultFromJson(std::string_view jsonStr) {                                               \
            Result result{};                                                                                   \
            /* null_terminated=false: see the identical fix on fromJson() above. */                            \
            static constexpr glz::opts kLenientRead{.null_terminated = false, .error_on_unknown_keys = false}; \
            if (auto errCode = glz::read<kLenientRead>(result, jsonStr)) {                                     \
                throw morph::model::detail::ParseError{glz::format_error(errCode, jsonStr)};                   \
            }                                                                                                  \
            return result;                                                                                     \
        }                                                                                                      \
    };                                                                                                         \
    MORPH_DETAIL_REGISTER_ACTION_LOCAL(M, A, NAME)                                                             \
    namespace {                                                                                                \
    [[maybe_unused]] const bool BRIDGE_DETAIL_CAT(bridge_action_exec_reg_, __COUNTER__) =                      \
        morph::model::detail::registerActionExecutorOnce<M, A>(morph::model::ModelTraits<M>::typeId(), NAME);  \
    }
// clang-format on
/// @endcond

/// @brief Registers a readiness predicate for action @p A.
///
/// Specialises `morph::model::ActionValidator<A>` so that `BridgeHandler::set<...>`
/// only fires `model.execute(draft)` when the predicate returns `true`.
///
/// @param A  Concrete action type.
/// @param FN Callable `bool(const A&)`.
// clang-format off -- public macro surface: hand-aligned on purpose.
// These definitions are the framework's documented API; contributors read them
// as reference, and the continuation backslashes line up so the body is legible
// as a block. Leaving them to the formatter means any unrelated edit nearby
// re-wraps the whole definition, and in one case it broke a token-paste
// invocation apart. Freeze them; realign by hand if a body changes.
#define BRIDGE_REGISTER_VALIDATOR(A, FN)                            \
    template <>                                                     \
    struct morph::model::ActionValidator<A> {                       \
        static bool ready(const A& action) { return (FN)(action); } \
    };
// clang-format on
// NOLINTEND(cppcoreguidelines-macro-usage)
// NOLINTEND(bugprone-macro-parentheses)
