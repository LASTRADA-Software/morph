// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <typeinfo>

#include "../journal/action_log.hpp"
#include "../session/session.hpp"
#include "strand.hpp"

namespace morph::model::detail {

template <typename Model>
struct ModelHolder;

// ── Backend-change notification interface ─────────────────────────────────────

/// @brief Optional interface for models that need to react to backend switches.
///
/// Implemented automatically by `ModelHolder<M>` when `M` declares
/// `void onBackendChanged()`. `Bridge::switchBackend()` discovers this
/// capability via `dynamic_cast` without coupling to the concrete type.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
struct IBackendChangedSink {
    virtual ~IBackendChangedSink() = default;

    /// @brief Called after the bridge has switched to a new backend and
    ///        re-registered all handlers on it.
    virtual void onBackendChanged() = 0;
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

// ── Concept ───────────────────────────────────────────────────────────────────

/// @brief Concept satisfied by model types that expose `void onBackendChanged()`.
template <typename M>
concept BackendChangedNotifiable = requires(M& model) {
    { model.onBackendChanged() } -> std::same_as<void>;
};

/// @brief Concept satisfied by model types that expose a model-level
///        `attachActionLog(std::shared_ptr<IActionLog>, std::string)`.
///
/// A small number of keyed/shared models (e.g. `kanban::BoardModel`) cannot
/// rely on `IModelHolder::recordIfAttached`'s auto-append alone (that
/// mechanism serves plain fire-after-success journaling; a model that needs
/// to read its own log back, e.g. for an activity-stream view, needs the
/// *same* `IActionLog` instance available on `Model` itself, not only on the
/// type-erased holder wrapping it) and so declares its own `attachActionLog`
/// method with this exact shape, purely by structural convention — there is
/// no shared base class or interface a model opts into. This concept is what
/// lets `ModelHolder<Model>::onActionLogAttached` forward to it generically,
/// the same "detect the hook structurally, forward only if present" shape
/// `BackendChangedNotifiable`/`BackendChangedMixin` already establish for
/// `onBackendChanged()`.
template <typename M>
concept ModelLevelActionLogAttachable =
    requires(M& model, std::shared_ptr<::morph::journal::IActionLog> log, std::string key) {
        { model.attachActionLog(log, key) } -> std::same_as<void>;
    };

// ── Conditional mixin ─────────────────────────────────────────────────────────

/// @brief Empty base when `M` does not declare `onBackendChanged()`.
template <typename M, bool = BackendChangedNotifiable<M>>
struct BackendChangedMixin {};

/// @brief Specialisation that wires `IBackendChangedSink` to `M::onBackendChanged()`.
template <typename M>
struct BackendChangedMixin<M, true> : IBackendChangedSink {
    /// @brief Forwards the virtual call to the concrete model instance.
    void onBackendChanged() override { static_cast<ModelHolder<M>*>(this)->model.onBackendChanged(); }
};

// ── Core type-erasure ─────────────────────────────────────────────────────────

/// @brief Type-erased wrapper that owns a single model instance.
///
/// Used internally by backends to store heterogeneous models in a single map.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
struct IModelHolder {
    virtual ~IModelHolder() = default;

    /// @brief Returns the `std::type_index` of the concrete model type.
    [[nodiscard]] virtual std::type_index type() const noexcept = 0;

    /// @brief Returns `true` if the concrete model type declares `void onBackendChanged()`.
    ///
    /// Answered by `ModelHolder<Model>` from the `BackendChangedNotifiable<Model>`
    /// concept — a compile-time constant per concrete `Model` — so a backend can
    /// learn this once, at registration time, instead of `dynamic_cast`ing every
    /// holder on every backend switch. See `LocalBackend::registerModel` /
    /// `LocalBackend::notifyBackendChanged` (`backend.hpp`).
    [[nodiscard]] virtual bool isBackendChangeAware() const noexcept = 0;

    /// @brief Forwards to the concrete model's `onBackendChanged()`, if it declared one.
    ///
    /// Default implementation is a no-op, for holders whose model is not
    /// backend-change-aware. `ModelHolder<Model>` overrides this to call
    /// `Model::onBackendChanged()` when `BackendChangedNotifiable<Model>` holds.
    /// Called by `LocalBackend::notifyBackendChanged` through this base-class
    /// virtual — no `dynamic_cast`, no RTTI dependency on this path.
    virtual void onBackendChanged() {}

    /// @brief Down-casts to a concrete `Model` reference.
    ///
    /// @tparam Model The expected concrete type.
    /// @throws std::bad_cast if the stored type is not `Model`.
    template <typename Model>
    Model& into() {
        if (type() != std::type_index(typeid(Model))) {
            throw std::bad_cast{};
        }
        return static_cast<ModelHolder<Model>*>(this)->model;
    }

    /// @brief Attaches a durable action log and this instance's stable identity.
    ///
    /// Set once, typically from the same custom `HandlerBinding::modelFactory`
    /// closure already used to inject other dependencies, or from
    /// `RemoteServer::LogProvider` for a registry-constructed remote instance
    /// (see `docs/spec/journal/journal.md`, "Attaching a log to remote
    /// instances"). @p contextKey is stamped onto every `LogEntry` this
    /// instance produces (e.g. an account id) so log entries are identifiable
    /// without parsing `payload`/`result` JSON.
    ///
    /// Also forwards @p log/@p contextKey to the wrapped model's own
    /// `attachActionLog(log, contextKey)`, if it declares one matching
    /// `ModelLevelActionLogAttachable` — see `onActionLogAttached`'s doc
    /// comment for why this second call exists alongside the holder's own
    /// `_actionLog`/`_contextKey` state below.
    /// @param log        Sink entries are forwarded to. Pass a `SessionLog` to also
    ///                   get undo/checkpoint support.
    /// @param contextKey Stable identity of this model instance.
    void attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string contextKey) {
        onActionLogAttached(log, contextKey);
        _actionLog = std::move(log);
        _contextKey = std::move(contextKey);
    }

    /// @brief Returns `true` if an action log is attached to this instance.
    [[nodiscard]] bool hasActionLog() const noexcept { return static_cast<bool>(_actionLog); }

    /// @brief Marks this instance as outbox-managed: the model records its own
    ///        `LogEntry` (inside its own store's transaction) and relays it via
    ///        `journal::OutboxRelay`, so `recordIfAttached` must stop
    ///        auto-appending for it — otherwise the framework's normal
    ///        fire-after-success append would double-log the same action.
    ///
    /// Independent of `attachActionLog`/`hasActionLog()`: those keep reporting
    /// whatever log is attached and whether one is attached, respectively; this
    /// flag only changes whether `recordIfAttached` actually forwards to it.
    /// Defaults to `false` — every instance auto-appends exactly as before
    /// unless a model explicitly opts in.
    /// @param outboxManaged `true` to suppress `recordIfAttached`; `false` to
    ///        restore the ordinary fire-after-success behavior.
    void setOutboxManaged(bool outboxManaged) noexcept { _outboxManaged = outboxManaged; }

    /// @brief Returns `true` if `setOutboxManaged(true)` was called on this instance.
    [[nodiscard]] bool isOutboxManaged() const noexcept { return _outboxManaged; }

    /// @brief Records @p entry if a log is attached and this instance is not
    ///        outbox-managed; no-op otherwise.
    ///
    /// Called automatically by the two places `Model::execute()` is actually
    /// invoked (`ActionDispatcher`'s runner and `Bridge::executeVia`'s local
    /// op) — model code and application code never call this directly.
    /// Overwrites `entityKey`, `principal`, and `timestampMs` on @p entry;
    /// callers only need to fill `modelType`, `actionType`, `payload`, `result`.
    /// A no-op if no log is attached, **or** if `setOutboxManaged(true)` was
    /// called on this instance (the model records its own entry elsewhere).
    /// @param entry Entry to record; `seq` is assigned by the attached sink.
    void recordIfAttached(::morph::journal::LogEntry entry) {
        if (!_actionLog || _outboxManaged) {
            return;
        }
        entry.entityKey = _contextKey;
        if (const auto* ctx = ::morph::session::current()) {
            entry.principal = ctx->principal;
        }
        entry.timestampMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        _actionLog->append(std::move(entry));
    }

protected:
    /// @brief Forwards @p log/@p contextKey to the wrapped model's own
    ///        `attachActionLog`, if it declares one. No-op default, for the
    ///        overwhelming majority of models that only ever rely on
    ///        `recordIfAttached`'s auto-append and have no model-level state
    ///        of their own to keep a log reference in.
    ///
    /// `ModelHolder<Model>` overrides this exactly like `onBackendChanged()`
    /// (see `BackendChangedMixin`): detected structurally via
    /// `ModelLevelActionLogAttachable<Model>`, not through a shared
    /// interface a model opts into by inheritance. This closes the gap a
    /// registry-constructed remote instance would otherwise have — before
    /// this hook existed, `RemoteServer::LogProvider`'s `holder->
    /// attachActionLog(log, contextKey)` populated only this holder's own
    /// `_actionLog`/`_contextKey` (used by `recordIfAttached`'s auto-append),
    /// never a `Model`-level `_log`-shaped member a model reads back from
    /// itself (e.g. for an activity-stream view): the model instance
    /// `into<Model>()` down-casts to is default-constructed by `ModelFactory::
    /// create<Model>()` and never otherwise touched. A model with no
    /// `attachActionLog` of its own is entirely unaffected — this call
    /// resolves to the base's no-op body for it, same as before this hook
    /// was added.
    /// @param log        Sink entries are forwarded to.
    /// @param contextKey Stable identity of this model instance.
    virtual void onActionLogAttached(const std::shared_ptr<::morph::journal::IActionLog>& log,
                                     const std::string& contextKey) {
        (void)log;
        (void)contextKey;
    }

private:
    std::shared_ptr<::morph::journal::IActionLog> _actionLog;
    std::string _contextKey;
    bool _outboxManaged{false};
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

/// @brief Concrete holder that stores a `Model` by value.
///
/// Inherits `BackendChangedMixin<Model>` so that backend-change notifications
/// are forwarded automatically when `Model` opts in.
template <typename Model>
struct ModelHolder : IModelHolder, BackendChangedMixin<Model> {
    /// @brief The owned model instance.
    Model model;

    /// @brief Constructs the model by forwarding all arguments.
    template <typename... Args>
    explicit ModelHolder(Args&&... args) : model{std::forward<Args>(args)...} {}

    /// @brief Returns `typeid(Model)` wrapped in a `std::type_index`.
    [[nodiscard]] std::type_index type() const noexcept override { return typeid(Model); }

    /// @brief Returns `BackendChangedNotifiable<Model>` — a compile-time constant.
    [[nodiscard]] bool isBackendChangeAware() const noexcept override { return BackendChangedNotifiable<Model>; }

    /// @brief Forwards to `Model::onBackendChanged()` iff `Model` declared one; no-op otherwise.
    ///
    /// This single definition is the final overrider for two unrelated base
    /// virtuals: `IModelHolder::onBackendChanged` (always), and — when `Model`
    /// is notifiable — `IBackendChangedSink::onBackendChanged`, reached via
    /// `BackendChangedMixin<Model, true>`. That mixin's own forwarding body
    /// becomes unreachable (shadowed by this more-derived override) but is left
    /// as-is; both call paths (`IModelHolder*` and `dynamic_cast<IBackendChangedSink*>`)
    /// land here.
    void onBackendChanged() override {
        if constexpr (BackendChangedNotifiable<Model>) {
            model.onBackendChanged();
        }
    }

protected:
    /// @brief Forwards to `Model::attachActionLog(log, contextKey)` iff `Model`
    ///        declared one matching `ModelLevelActionLogAttachable`; no-op otherwise.
    void onActionLogAttached(const std::shared_ptr<::morph::journal::IActionLog>& log,
                             const std::string& contextKey) override {
        if constexpr (ModelLevelActionLogAttachable<Model>) {
            model.attachActionLog(log, contextKey);
        }
    }
};

/// @brief Factory that creates default-constructed `ModelHolder<Model>` instances.
class ModelFactory {
public:
    /// @brief Creates a new `ModelHolder<Model>` on the heap.
    ///
    /// If a process-wide default action log is installed (see
    /// `morph::journal::setActionLog`), it is attached to the new holder
    /// automatically (with an empty `entityKey`) — this is the single
    /// construction path behind every ordinary model registration, local or
    /// remote, which is what makes "set the log once in `main()`" work
    /// uniformly across topologies. Callers that need a specific instance
    /// identity call `attachActionLog` again afterward to override it.
    /// @tparam Model The model type to instantiate.
    /// @return Owning pointer to the new holder.
    template <typename Model>
    static std::unique_ptr<IModelHolder> create() {
        auto holder = std::make_unique<ModelHolder<Model>>();
        if (auto log = ::morph::journal::defaultActionLog()) {
            holder->attachActionLog(std::move(log), {});
        }
        return holder;
    }
};

}  // namespace morph::model::detail
