# The model registration system — design

`morph::model` uses a trait-plus-singleton-registry pattern to map C++ model
and action types to string identifiers at static-init time, so that remote
frontends and schema-driven GUIs can discover, instantiate, and execute models
without knowing their concrete types.

## Contents

- [Overview](#overview)
- [Registration rules and invariants](#registration-rules-and-invariants)
- [Customisation traits](#customisation-traits)
  - [ModelTraits](#modeltraits)
  - [ActionTraits](#actiontraits)
- [Validation and logging policy](#validation-and-logging-policy)
  - [ActionValidator](#actionvalidator)
  - [ValidationError](#validationerror)
  - [Loggable](#loggable)
  - [ActionLogPolicy](#actionlogpolicy)
- [Type-erased holders and factory](#type-erased-holders-and-factory)
  - [IModelHolder](#imodelholder)
  - [ModelHolder](#modelholder)
  - [ModelFactory](#modelfactory)
- [Singleton registries](#singleton-registries)
  - [ActionDispatcher](#actiondispatcher)
  - [ModelRegistryFactory](#modelregistryfactory)
  - [ActionExecuteRegistry](#actionexecuteregistry)
- [Registration macros](#registration-macros)
  - [BRIDGE_REGISTER_MODEL](#bridge_register_model)
  - [BRIDGE_REGISTER_ACTION](#bridge_register_action)
  - [BRIDGE_REGISTER_VALIDATOR](#bridge_register_validator)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Thread safety](#thread-safety)
- [Failure modes](#failure-modes)
- [Limitations](#limitations)
- [Cross-references](#cross-references)

## Overview

Every model and action type that participates in morph's remote or schema-driven
infrastructure must be registered with a string id. The registration system
provides:

- **Traits** — `ModelTraits<M>` and `ActionTraits<A>` that map types to string
  ids and JSON codecs. Users specialise them directly or use macros.
- **Validators** — `ActionValidator<A>` that decides whether a partially-built
  action draft is ready to execute, enforced on every dispatch path: the
  reactive `set<>` path, the type-erased `executeJson` path, the server dispatch
  runner (`ActionDispatcher::registerAction`), and the local `Bridge::executeVia`
  path — the last two throw `ValidationError` on a `false` result instead of
  running `Model::execute`.
- **Logging policy** — `ActionLogPolicy<A>` and `Loggable` that control whether
  an action's executions are recorded and how duplicates are coalesced.
- **Type-erased holders** — `IModelHolder` / `ModelHolder<M>` that own a model
  instance and carry an optional action log attachment.
- **Singleton registries** — `ActionDispatcher` (server-side dispatch),
  `ModelRegistryFactory` (model instantiation by string id), and
  `ActionExecuteRegistry` (client/schema-driven generic execute).
- **Macros** — `BRIDGE_REGISTER_MODEL`, `BRIDGE_REGISTER_ACTION`,
  `BRIDGE_REGISTER_VALIDATOR` that specialise traits and register into
  singletons at static-init time.

## Registration rules and invariants

Registration is not a runtime call the application makes; it is a side effect of
static initialisation of file-scope objects the macros emit. That machinery only
works if a handful of invariants hold. **Read this section before adding a model
or action to any target other than a single executable** — the most common
failure (a model that silently never registers) is a linking problem, not a code
problem, and produces no diagnostic.

### One macro invocation per translation unit

`BRIDGE_REGISTER_MODEL` and `BRIDGE_REGISTER_ACTION` each emit two things at
namespace scope:

1. an **explicit template specialisation** — `ModelTraits<M>` or `ActionTraits<A>`
   — which has external visibility to the type system; and
2. one or more **file-scope initialiser objects** (`[[maybe_unused]] const bool`
   in an anonymous namespace) whose initialisation runs
   `registerModelOnce` / `registerActionOnce` / `registerActionExecutorOnce`.

`BRIDGE_REGISTER_VALIDATOR` emits **only** item 1 — an `ActionValidator<A>`
specialisation. It performs no static-init registration (there is no singleton
of validators; `ActionValidator` is consulted purely by template lookup).

Because of (1), a macro must be invoked in **exactly one translation unit**.
Placing a `BRIDGE_REGISTER_*` invocation in a header that is included by more
than one `.cpp` defines the same explicit specialisation in multiple translation
units — an **ODR violation** (ill-formed, no diagnostic required). The anonymous
namespace makes the *initialiser* objects internal to each TU, so it does not
save you: it would register the model once per including TU while the duplicated
specialisation remains ill-formed. Register each type in one `.cpp` (or a header
that is guaranteed to be compiled into exactly one TU).

### Static initialisation only fires in linked translation units

Static-init "guarantees registrations are live before `main()`" (see
[Design decisions](#design-decisions)) **only for translation units the linker
actually keeps**. The initialiser object is never referenced by name from
application code — nothing has a symbolic dependency on it. Consequences:

- **Object files / whole executables**: an object file linked directly into an
  executable contributes its static initialisers, so registration works with no
  extra ceremony. This is the common case and the one the design optimises for.
- **Static libraries (`.a` / `.lib`)**: a linker pulls in an archive member
  **only if some symbol in it is already referenced**. A registration-only TU
  exposes no referenced symbol, so the linker drops the member and the
  initialiser never runs — the model or action **silently never registers**, and
  the first symptom is a runtime `std::runtime_error` ("unknown model type" /
  "unknown action") far from the cause. Force the member to be retained:
  `--whole-archive` / `-Wl,--whole-archive` (GNU/LLD), `-force_load` (Apple ld),
  `/WHOLEARCHIVE` (MSVC), or CMake's `$<LINK_LIBRARY:WHOLE_ARCHIVE,...>`. Prefer
  linking registration TUs into the executable's own object set when practical.
- **Dynamically loaded modules (`dlopen`/`LoadLibrary`)**: registrations run when
  the module is loaded, i.e. **after `main` has started**, not before. Any code
  path that could dispatch/create a plugin's model must run after the module is
  loaded; there is no ordering guarantee relative to other TUs' static init.

### Remotely instantiated models must be default-constructible

`ModelRegistryFactory::create` reaches models through
`ModelFactory::create<Model>()`, which does `std::make_unique<ModelHolder<Model>>()`
with **no constructor arguments** (see `model.hpp`). Any model registered with
`BRIDGE_REGISTER_MODEL` that will be instantiated by string id from a remote
`"register"` message must therefore be **default-constructible**. A model with no
accessible default constructor still compiles the macro (which only needs
`ModelTraits`) but fails to compile the factory instantiation. Models that need
injected dependencies are instead constructed by a custom
`HandlerBinding::modelFactory` closure and are not reachable through the string-id
factory path.

## Customisation traits

### `ModelTraits<Model>`

Maps a concrete model type to its string type-id. Must be specialised (or
`BRIDGE_REGISTER_MODEL` used) before the model can be registered. The
default is a **forward declaration** — using it without a specialisation
is an incomplete-type error.

```cpp
template <typename Model>
struct ModelTraits;  // forward — specialize or use BRIDGE_REGISTER_MODEL
```

### `ActionTraits<Action>`

Maps a concrete action type to its string id, JSON codec, result type, and
optional logging flag. The `BRIDGE_REGISTER_ACTION` macro generates a full
specialisation. Hand-written specialisations (used in tests) predate the
`loggable` member; the framework defaults to `Loggable::Yes` when it is absent
(see `detail::actionLoggable()`). The default is a **forward declaration**.

```cpp
template <typename Action>
struct ActionTraits;  // forward — specialize or use BRIDGE_REGISTER_ACTION
```

All four JSON functions throw `detail::ParseError` (a `std::runtime_error`
subclass) on glaze encode/decode failure.

## Validation and logging policy

### `ActionValidator<Action>`

Decides whether an in-progress action draft is ready to execute. Resolution
order (highest priority first):

1. **Explicit specialisation** — via `BRIDGE_REGISTER_VALIDATOR(Action, fn)`.
2. **`bool validate() const` member** on `Action` — auto-detected via the
   `detail::HasValidate` concept.
3. **Default** — returns `true` (one-shot semantics: first `set<>` lands and
   the action fires).

Validation is a property of the action, not the model: different actions on the
same model have different readiness requirements.

```cpp
template <typename Action>
struct ActionValidator {
    static constexpr bool ready(const Action& action);
};
```

### `ValidationError`

Thrown by the two execution sites that receive an action without first passing
through a client-side readiness gate: `ActionDispatcher::registerAction`'s
runner (the server dispatch path `RemoteServer` uses on every remote and Qt
WebSocket topology) and `Bridge::executeVia`'s `localOp` (the in-process path
`LocalBackend` uses). Both call `ActionValidator<Action>::ready(action)`
immediately before `Model::execute` and throw `ValidationError` on `false`,
instead of executing the action:

```cpp
struct ValidationError : std::runtime_error {
    ValidationError(std::string_view modelType, std::string_view actionType);
    // what(): "action failed validation: <modelType>/<actionType>"
};
```

`ActionDispatcher::registerAction`'s runner additionally reconciles every
`Quantity` field of the decoded action to its declared precision
(`morph::forms::reconcileDeclaredPrecision`) before the `ready()` check, so a
hand-built wire payload's `Quantity` values match the schema's advertised
`x-decimalPlaces` the same way the client bridge dispatch path already
normalises them (see [forms.md](../forms/forms.md)). `Bridge::executeVia`'s
`localOp` does not reconcile precision — that path never decodes JSON, so
there is no wire `dp` to reconcile against.

`ValidationError` derives from `std::runtime_error`, so it is caught by
existing generic `catch (const std::exception&)` handling on both paths
without any special-casing: `LocalBackend::execute`'s strand `catch (...)`
(`backend.hpp`) forwards it into the `Completion`'s `onError` with the concrete
type intact; `RemoteServer::dispatchExecute`'s strand `catch (const
std::exception&)` (`remote.hpp`) turns it into an ordinary `err` reply carrying
`exc.what()` and the `callId` — the client's `Completion` resolves through
`onError` with a generic `std::runtime_error` carrying that message (the
concrete type does not cross the wire).

Actions with no validator are unaffected on both paths: `ActionValidator<A>::ready`
defaults to `true` when neither a `bool validate() const` member nor a
`BRIDGE_REGISTER_VALIDATOR` specialisation exists, so this is backward
compatible.

`ValidationError` is **not** an authorization mechanism — see
[security.md](../security.md) for that separate concern.

### `Loggable`

A strong enum avoiding bare `bool` arguments at registration sites:

```cpp
enum class Loggable : std::uint8_t { No, Yes };
```

### `ActionLogPolicy<Action>`

Controls how repeated executions are checkpointed into a durable action log.
Only `coalesce` exists; every other action defaults to `false` (every execution
treated as a distinct fact).

```cpp
template <typename Action>
struct ActionLogPolicy {
    static constexpr bool coalesce = false;
};
```

When `coalesce` is `true`, a checkpoint keeps only the most recent entry per
`(modelType, entityKey, actionType)` triple.

## Type-erased holders and factory

### `IModelHolder`

Type-erased wrapper that owns a single model instance. Used by backends to store
heterogeneous models in a single map. Declared in `morph::model::detail` (an
implementation type — backends hold it, application code never names it).

```cpp
// namespace morph::model::detail
struct IModelHolder {
    virtual ~IModelHolder() = default;
    [[nodiscard]] virtual std::type_index type() const noexcept = 0;
    template <typename Model> Model& into();
    void attachActionLog(std::shared_ptr<::morph::journal::IActionLog>, std::string contextKey);
    bool hasActionLog() const noexcept;
    void recordIfAttached(LogEntry entry);
    void setOutboxManaged(bool outboxManaged) noexcept;
    [[nodiscard]] bool isOutboxManaged() const noexcept;
};
```

- `into<Model>()` down-casts to a concrete `Model&`; throws `std::bad_cast` on
  mismatch.
- `attachActionLog` sets the durable log sink and the instance's stable identity
  (stamped onto every `LogEntry`).
- `recordIfAttached` is called automatically by `ActionDispatcher`'s runner and
  `Bridge::executeVia` — model code never calls it directly. It fills
  `entityKey`, `principal` (from `session::current()`), and `timestampMs` on the
  entry before forwarding. It is also a no-op when `isOutboxManaged()` is
  `true` — see [journal.md's transactional outbox section](../journal/journal.md#transactional-outbox-opt-in).
- `setOutboxManaged(true)` marks this instance as managing its own outbox log
  write, so `recordIfAttached` stops auto-appending for it; `hasActionLog()` is
  unaffected. Defaults to `false`.

### `ModelHolder<Model>`

Concrete holder that stores a `Model` by value. Inherits `BackendChangedMixin`
so that backend-change notifications are forwarded automatically when `Model`
declares `void onBackendChanged()`.

```cpp
template <typename Model>
struct ModelHolder : IModelHolder, BackendChangedMixin<Model> {
    Model model;
    template <typename... Args> explicit ModelHolder(Args&&... args);
    std::type_index type() const noexcept override;
};
```

### `ModelFactory`

Creates default-constructed `ModelHolder<Model>` instances. If a process-wide
default action log is installed (via `morph::journal::setActionLog`), it is
attached to the new holder automatically (with an empty `entityKey`). This is the
single construction path behind every ordinary model registration, making "set
the log once in `main()`" work uniformly across topologies.

```cpp
class ModelFactory {
    template <typename Model>
    static std::unique_ptr<IModelHolder> create();
};
```

### `IBackendChangedSink` and `BackendChangedMixin`

Optional interface for models that need to react to backend switches. `Bridge::switchBackend`
discovers this capability via `dynamic_cast`. `ModelHolder<M>` inherits
`BackendChangedMixin<M>` which conditionally derives from `IBackendChangedSink`
when `M` declares `void onBackendChanged()` (detected by the
`BackendChangedNotifiable<M>` concept).

## Singleton registries

`ActionDispatcher` and `ModelRegistryFactory` are both declared in
`registry.hpp` in namespace `morph::model::detail`. `ActionExecuteRegistry` lives
elsewhere — see its section below.

### `ActionDispatcher`

Maps `(modelId, actionId)` pairs to type-erased runner functions. Used by
`RemoteServer` to dispatch incoming JSON requests.

```cpp
class ActionDispatcher {
    using Runner = std::function<std::string(IModelHolder&, std::string_view)>;
    template <typename Model, typename Action>
    void registerAction(std::string_view modelId, std::string_view actionId);
    std::string dispatch(std::string_view modelId, std::string_view actionId,
                         IModelHolder& holder, std::string_view payload);
    bool coalesce(std::string_view modelId, std::string_view actionId) const;
    static ActionDispatcher& instance();
};
```

- `registerAction` registers a runner that deserialises, reconciles any
  `Quantity` fields to their declared precision, enforces
  `ActionValidator<Action>::ready(action)` (throwing `ValidationError` on
  `false`, before `Model::execute` runs), executes via `Model::execute(action)`,
  serialises the result, and records to the attached action log when the
  action is loggable and a log is attached.
- `dispatch` looks up the runner and invokes it; throws `std::runtime_error` for
  unknown pairs.
- `coalesce` returns the `ActionLogPolicy<Action>::coalesce` value for the pair;
  unknown pairs default to `false`.

### `ModelRegistryFactory`

Creates `IModelHolder` instances by string type-id. Used by `RemoteServer` to
instantiate models on demand from incoming `"register"` messages.

```cpp
class ModelRegistryFactory {
    template <typename Model>
    void registerModel(std::string_view modelId);
    std::unique_ptr<IModelHolder> create(std::string_view modelId);
    static ModelRegistryFactory& instance();
};
```

- `create` throws `std::runtime_error` for unknown model types.

#### Instance identity and per-instance authorization

`ModelRegistryFactory` maps a **string type-id** to a fresh holder; it has no
notion of a per-*instance* id or owner. The numeric **instance id** that
addresses a live holder is assigned separately by `RemoteServer` from a single
sequential counter (`_nextId`), and those ids are therefore guessable across
tenants. To keep a caller from `execute`/`deregister`-ing an instance it did not
create, `RemoteServer` records an **owner principal** for each instance at
`register` time — the *verified* identity of the register call
(`IAuthorizer::authenticate`), not the client's raw claim — and consults the
optional `IAuthorizer::authorizeInstance(ctx, modelType, actionType, modelId,
ownerPrincipal)` hook on every `execute` and `deregister`. The hook **defaults
to allow**, so this registry's type-keyed behaviour is unchanged unless a
deployer installs an authorizer that overrides it. The type registry maps type
ids only; instance ownership lives one layer up in `RemoteServer`. See
[session.md](../session/session.md) and [security.md](../security.md).

### `ActionExecuteRegistry`

Type-erased, JSON-in/JSON-out execute path for actions whose concrete C++ type
is only known by its registered string id at the call site (e.g. a schema-driven
GUI). Populated automatically by `BRIDGE_REGISTER_ACTION`. Every entry calls
through the real `BridgeHandler<Model>::execute<Action>()`, so sessions, backend
switches, and completions behave exactly as for hand-written call sites.

Unlike `ActionDispatcher` and `ModelRegistryFactory` (which live in
`morph::model::detail` in `registry.hpp`), the whole `ActionExecuteRegistry`
class is declared in `morph/bridge.hpp` in namespace `morph::bridge` — it depends
on `BridgeHandler`, which `registry.hpp` cannot see. `Completion` here is
`morph::async::Completion`.

```cpp
class ActionExecuteRegistry {  // namespace morph::bridge, declared in bridge.hpp
    using Executor = std::function<::morph::async::Completion<std::string>(void*, std::string_view)>;
    template <typename Model, typename Action>
    void registerAction(std::string_view modelId, std::string_view actionId);
    [[nodiscard]] ::morph::async::Completion<std::string> execute(
        std::string_view modelId, std::string_view actionId,
        void* handler, std::string_view bodyJson) const;
    static ActionExecuteRegistry& instance();
};
```

- `registerAction` is only *declared* in the class body; its definition is
  out-of-line in `bridge.hpp` (after `BridgeHandler` is fully defined) so the
  executor can safely cast the `void*` handler and call its methods. `execute`
  and `instance()` are defined inline in `bridge.hpp`.
- `execute` throws `std::runtime_error` for unknown pairs.

### Static-init helpers

Three `detail` functions serve as static-init helpers that the macros call:

| Function | Purpose |
|---|---|
| `registerModelOnce<Model>(modelId)` | Registers a model factory with `ModelRegistryFactory::instance()`. Returns `true` so it can be assigned to a `const bool` in an anonymous namespace. |
| `registerActionOnce<Model, Action>(modelId, actionId)` | Registers a runner with `ActionDispatcher::instance()`. Returns `true`. |
| `registerActionExecutorOnce<Model, Action>(modelId, actionId)` | Registers with `ActionExecuteRegistry::instance()`. Only declared in `registry.hpp`; defined in `bridge.hpp` to avoid a `registry.hpp` → `bridge.hpp` include cycle. |

The process-level singletons are returned by `defaultDispatcher()` and
`defaultRegistry()` (both are `inline` functions with function-local `static`
variables).

## Registration macros

### `BRIDGE_REGISTER_MODEL(M, NAME)`

Specialises `ModelTraits<M>` and registers a factory at static-init time.

```cpp
BRIDGE_REGISTER_MODEL(AccountModel, "Account")
```

Expands to:
- `template <> struct morph::model::ModelTraits<M> { static constexpr std::string_view typeId() noexcept { return NAME; } };`
- A `[[maybe_unused]] const bool` in an anonymous namespace (internal linkage,
  no explicit `static`) that calls `detail::registerModelOnce<M>(NAME)`.

### `BRIDGE_REGISTER_ACTION(M, A, NAME, ...)`

Variadic macro accepting 3 or 4 arguments. The 4-argument form accepts an
optional `Loggable` value (defaults to `Loggable::Yes`).

```cpp
BRIDGE_REGISTER_ACTION(AccountModel, Deposit, "Deposit")
BRIDGE_REGISTER_ACTION(AccountModel, GetAccount, "GetAccount", morph::model::Loggable::No)
```

Expands to:
- `template <> struct morph::model::ActionTraits<A>` with `Result` deduced from
  `decltype(std::declval<M&>().execute(std::declval<A>()))`, a
  `static constexpr std::string_view typeId()` (no `noexcept`, unlike
  `ModelTraits::typeId()`), a `static constexpr Loggable loggable`, and four JSON
  codec functions (each throwing `detail::ParseError` on failure): `toJson`/
  `resultToJson` use `glz::write_json`; `fromJson`/`resultFromJson` use
  `glz::read<glz::opts{.error_on_unknown_keys = false}>` — the same
  forward-compatibility convention `wire::decode` uses (see wire.md,
  "Action-evolution policy") — so an older-compiled action struct silently
  ignores an additive field a newer peer sent.
- A `[[maybe_unused]] const bool` in an anonymous namespace calling
  `detail::registerActionOnce<M, A>(morph::model::ModelTraits<M>::typeId(), NAME)`
  (the model-id argument is the model's registered `typeId()`, not a raw string).
- A `[[maybe_unused]] const bool` in an anonymous namespace calling
  `detail::registerActionExecutorOnce<M, A>(morph::model::ModelTraits<M>::typeId(), NAME)`.

**Hard requirement:** Every translation unit invoking `BRIDGE_REGISTER_ACTION`
must include `<morph/bridge.hpp>` (directly or transitively) because
`registerActionExecutorOnce` is only defined there. Without it, the link fails
with an unresolved external symbol.

### `BRIDGE_REGISTER_VALIDATOR(A, FN)`

Specialises `ActionValidator<A>` with a custom predicate.

```cpp
BRIDGE_REGISTER_VALIDATOR(FormAction, [](const FormAction& a) {
    return a.a != 0.0 && a.b != 0.0 && a.c != 0.0;
})
```

Expands to `template <> struct morph::model::ActionValidator<A> { static bool ready(const A& action) { return (FN)(action); } };`.

## API reference

### Traits and policies

| Symbol | Kind | Purpose |
|---|---|---|
| `ModelTraits<M>` | class template | **Customisation point.** Maps model type to `std::string_view typeId()`. |
| `ActionTraits<A>` | class template | **Customisation point.** Maps action type to id, JSON codec, result type, and `Loggable`. |
| `ActionValidator<A>` | class template | **Customisation point.** `static bool ready(const A&)` — built-in detection of `bool validate() const`, overridable via specialisation. |
| `ValidationError` | exception type | Thrown by `ActionDispatcher::registerAction`'s runner and `Bridge::executeVia`'s `localOp` when `ActionValidator<A>::ready` returns `false`. `std::runtime_error` subclass carrying `"action failed validation: <modelType>/<actionType>"`. |
| `ActionLogPolicy<A>` | class template | **Customisation point.** `static constexpr bool coalesce = false` — checkpoint coalescing policy. |
| `Loggable` | enum | `{ No, Yes }` — strong boolean for action loggability. |

### Concepts (detail)

| Symbol | Purpose |
|---|---|
| `HasValidate<A>` | `true` when `A` exposes `bool validate() const`. |
| `HasLoggableFlag<A>` | `true` when `ActionTraits<A>` exposes `static constexpr Loggable loggable`. |
| `BackendChangedNotifiable<M>` | `true` when `M` exposes `void onBackendChanged()`. |

### Type-erased model infrastructure

| Symbol | Kind | Purpose |
|---|---|---|
| `IModelHolder` | abstract class | Type-erased model owner with an action log slot and an outbox-managed opt-out flag. |
| `ModelHolder<M>` | class template | Concrete holder storing `M` by value; conditionally inherits `IBackendChangedSink`. |
| `ModelFactory` | class | `static create<M>()` — default-constructs `ModelHolder<M>` and attaches the process-wide default log. |
| `IBackendChangedSink` | abstract class | Optional interface for backend-switch notification, discovered via `dynamic_cast`. |
| `BackendChangedMixin<M>` | class template | Conditionally inherits `IBackendChangedSink` when `M` has `onBackendChanged()`. |

### Singleton registries

| Symbol | Kind | Purpose |
|---|---|---|
| `ActionDispatcher` | class | Maps `(modelId, actionId)` → type-erased runner; server-side dispatch. |
| `ModelRegistryFactory` | class | Maps `modelId` → factory; server-side model instantiation. |
| `ActionExecuteRegistry` | class | Maps `(modelId, actionId)` → type-erased executor through `BridgeHandler`; client/schema-driven execute. |

### Macros

| Macro | Arguments | Generates |
|---|---|---|
| `BRIDGE_REGISTER_MODEL` | `(M, NAME)` | `ModelTraits<M>` specialisation + static-init factory registration. |
| `BRIDGE_REGISTER_ACTION` | `(M, A, NAME, ...)` | `ActionTraits<A>` specialisation + static-init dispatcher and executor registration. Optional 4th arg: `Loggable`. |
| `BRIDGE_REGISTER_VALIDATOR` | `(A, FN)` | `ActionValidator<A>` specialisation + custom predicate. |

### Detail helpers

| Symbol | Purpose |
|---|---|
| `PairKeyHash` | Hash functor for `std::pair<std::string, std::string>` keys used by `ActionDispatcher` and `ActionExecuteRegistry`. |
| `actionLoggable<A>()` | Returns `ActionTraits<A>::loggable` if present, else `Loggable::Yes`. |
| `ParseError` | `std::runtime_error` subclass thrown on JSON codec failure. |
| `registerModelOnce<M>(id)` | Static-init helper; returns `true`. |
| `registerActionOnce<M, A>(modelId, actionId)` | Static-init helper; returns `true`. |
| `registerActionExecutorOnce<M, A>(modelId, actionId)` | Static-init helper; only declared in `registry.hpp`, defined in `bridge.hpp`. |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Trait-based registration | **Template specialisation + static-init guards** | Users never manage registry lifecycle; a macro or a hand-written specialisation is all that's needed. Static-init guarantees registrations are live before any `main()` code runs. |
| Singleton registries | **Function-local `static` in `inline` functions** | Process-level singletons with no header-level `static` ordering issues; `inline` avoids ODR violations across translation units. |
| Two registries for action execution | **`ActionDispatcher` (server) vs `ActionExecuteRegistry` (client)** | `ActionDispatcher` calls `Model::execute()` directly on an owned `IModelHolder` — the server-side path. `ActionExecuteRegistry` goes through `BridgeHandler<Model>` — sessions, backend switches, and completions work identically to hand-written call sites. Both are populated by the same macro. |
| `registerActionExecutorOnce` forward-declared in `registry.hpp` | **Defined in `bridge.hpp`** | Avoids a `registry.hpp` → `bridge.hpp` include cycle. `bridge.hpp` already includes `registry.hpp`. The cost: every translation unit that uses `BRIDGE_REGISTER_ACTION` must also include `bridge.hpp` or the link fails. |
| `HasLoggableFlag` backward compatibility | **Defaults to `Loggable::Yes` when `loggable` is absent** | Hand-written `ActionTraits` specialisations in tests predate the member; forcing them to add it would be churn. The default of `Yes` also means new actions are captured automatically — only pure queries opt out. |
| Action validation is a property of the action | **`ActionValidator<Action>`, not `ActionValidator<Model, Action>`** | Different actions on the same model have different readiness requirements; keeping the predicate next to the action keeps the GUI side oblivious to model internals. |
| `Loggable` is a strong enum | **`Loggable::No` / `Loggable::Yes`**, not bare `bool` | Registration call sites read as intent rather than an unexplained `false`. |
| `ModelFactory::create` attaches the default log | **Single construction path for all topologies** | "Set the log once in `main()`" works uniformly across local and remote topologies. Callers that need a specific identity call `attachActionLog` again afterward. |
| `setOutboxManaged` opt-out | **Suppress `recordIfAttached`, not `hasActionLog()`** | A store-backed model that logs inside its own transaction (see `journal.md`'s transactional outbox) must stop the framework's auto-append without losing "a log is attached" as a fact holders can still query. |
| `coalesce` defaults to `false` | **Every execution is a distinct, permanent fact** | The right default for anything resembling a business event. Only actions where only the latest occurrence should survive a checkpoint (e.g. a form-field edit fired repeatedly via `BridgeHandler::set`) opt in. |

## Thread safety

All three registries — `ActionDispatcher`, `ModelRegistryFactory`, and
`ActionExecuteRegistry` — are backed by plain `std::unordered_map` members with
**no mutex, no atomic, and no other synchronisation**. This is deliberate and
safe *only because of the registration model*:

- **Writes happen during static initialisation**, which runs single-threaded
  before `main()` (the process has not spawned worker threads yet). Every
  `registerAction` / `registerModel` mutation therefore happens-before any code
  that could observe the map concurrently.
- **After `main()` begins the maps are read-only.** `dispatch`, `create`, and
  `coalesce` only ever call `find` on an already-populated map — concurrent
  reads of a `const`-in-practice `unordered_map` are data-race-free. This is what
  lets `RemoteServer` and the bridge dispatch/create/coalesce from arbitrary
  threads without locking.

The corollary is a hard constraint: **runtime registration is not thread-safe.**
Calling `registerAction` / `registerModel` after threads are running — e.g. from
a `dlopen`ed module loaded on a worker thread while another thread is
dispatching — races the map's internals against concurrent `find` calls and is
undefined behaviour. Load and register plugin modules from a single thread,
quiesced with respect to dispatch, before exposing them.

## Failure modes

| Situation | Behaviour | Where |
|---|---|---|
| Two registrations for the same `(modelId, actionId)` (or same `modelId`) | **Silent last-write-wins.** `ActionDispatcher::registerAction` does `_runners[key] = ...` and `_coalesce[key] = ...`; `ModelRegistryFactory::registerModel` does `insert_or_assign`. No diagnostic; the surviving entry is whichever initialiser ran last, and static-init order across TUs is unspecified. | `registry.hpp` |
| Two **distinct C++ types** registered under one string id | Same silent overwrite — the string id, not the type, is the key. The second type's runner/factory shadows the first. This is the collision hazard behind the string-vocabulary limitation below. | `registry.hpp` |
| `dispatch` / `execute` with an unknown `(modelId, actionId)` | Throws `std::runtime_error` **at runtime** — `"unknown action: …"` from `ActionDispatcher::dispatch`, `"unknown action for executeJson: …"` from `ActionExecuteRegistry::execute`. The string-keyed remote path has **no compile-time completeness check** — a pair that was never registered is only discovered when a request for it arrives. | `ActionDispatcher::dispatch`, `ActionExecuteRegistry::execute` |
| `dispatch` when the decoded action fails `ActionValidator<Action>::ready(...)` | Throws `morph::model::ValidationError` (a `std::runtime_error` subclass) **before** `Model::execute` runs — the action is never executed. Actions with no validator (the common case) are unaffected: `ready()` defaults to `true`. | `ActionDispatcher::registerAction`'s runner |
| `create` with an unknown model id | Throws `std::runtime_error("unknown model type: …")` at runtime. | `ModelRegistryFactory::create` |
| `coalesce` for an unknown pair | Does **not** throw — defaults to `false` (every entry kept). | `ActionDispatcher::coalesce` |
| Allocation failure inside a `register*Once` helper during static init | `registerModelOnce` / `registerActionOnce` (and `registerActionExecutorOnce`) are declared `noexcept` yet allocate (they build `std::string` keys and grow the map). An OOM there raises an exception through a `noexcept` boundary, which calls `std::terminate` — the process aborts during static init. | `registry.hpp` |

Note the asymmetry the design accepts intentionally: the **typed local path**
(`BridgeHandler::execute<Action>()`, `Model::execute(action)`) is checked by the
compiler — an unregistered or misspelled action is a build error — whereas the
**string-keyed remote/schema path** through these registries defers every id
resolution to runtime. Registration correctness for the remote surface is a
testing obligation, not a compile-time guarantee.

## Limitations

- **String type-ids are an unversioned, un-namespaced global protocol
  vocabulary.** Every `NAME` passed to `BRIDGE_REGISTER_MODEL` /
  `BRIDGE_REGISTER_ACTION` lives in one flat namespace shared across the whole
  process and, implicitly, across the wire with every peer. There is no version
  tag, no module qualifier, and — per [Failure modes](#failure-modes) — collisions
  are silent last-write-wins. Two independently developed subsystems that both
  register `"Update"` will clobber each other with no diagnostic. Mitigations the
  design does **not** yet enforce but should be adopted by convention: an
  **id-namespacing convention** (e.g. `"bank.Account"` / `"bank.Account.Deposit"`
  prefixes per subsystem) to make collisions structurally unlikely, and a
  **startup self-check** that iterates the intended `(model, action)` set and
  asserts each is present before serving traffic — there is otherwise **no
  compile-time guarantee that every remotely executed pair was actually
  registered** (registration is a static-init side effect that can be silently
  dropped; see [Registration rules and invariants](#registration-rules-and-invariants)).
- **Global mutable singletons with no teardown or reset.** `defaultDispatcher()`
  and `defaultRegistry()` (and `ActionExecuteRegistry::instance()`) are
  function-local `static`s that live for the whole process and expose no clear /
  reset. This hurts test isolation: registrations accumulate across test cases in
  one binary, one test cannot register a fake model without leaking it into the
  next, and last-write-wins means test ordering can change behaviour. Contrast
  `journal::ScopedActionLog`, which deliberately provides scoped install/restore
  for exactly this reason; the registries have no equivalent.
- **Per-call heap allocation on the hot path.** Every `dispatch`, `create`,
  `coalesce`, and `execute` constructs a `std::string` (or a
  `std::pair<std::string, std::string>`) key from its `string_view` arguments
  purely to probe the map — an allocation per lookup on what is the request hot
  path. Heterogeneous lookup (a transparent hash/equality over `string_view`,
  C++20 `unordered_map` `find` with `is_transparent`) would remove the
  allocation entirely; the maps are keyed on owning `std::string` today.
- **The `ActionDispatcher` / `ActionExecuteRegistry` split can silently
  diverge.** A single `BRIDGE_REGISTER_ACTION` populates both registries (one
  initialiser each). But they are independent maps consulted by different
  code paths — `ActionDispatcher` on the `RemoteServer` server path,
  `ActionExecuteRegistry` on the `BridgeHandler` schema-driven path. If only one
  registration fires (e.g. a hand-written `ActionTraits` specialisation that
  registers a runner but skips the executor, or a partial refactor), one path
  works and the other throws "unknown action" for the *same* logical action, with
  no signal that the two are meant to stay in lockstep.

## Cross-references

- **[bridge.md](bridge.md)** — defines `BridgeHandler<Model>`, `Bridge`, and the
  `ActionExecuteRegistry` class itself (declared in `<morph/bridge.hpp>`, not
  `registry.hpp`). Explains the **hard `#include <morph/bridge.hpp>` requirement**
  for any TU using `BRIDGE_REGISTER_ACTION` (`registerActionExecutorOnce` is only
  *defined* there), the parallel executor path this spec's
  `ActionExecuteRegistry` section summarises, and `Bridge::executeVia`'s
  `localOp`, which enforces the same `ValidationError` gate as this spec's
  `ActionDispatcher::registerAction` for the local execution path.
- **[journal.md](../journal/journal.md)** — `IActionLog`, `LogEntry`, `SessionLog`,
  checkpoint coalescing, and `ScopedActionLog`. Explains how the runner's
  `recordIfAttached` call and `ActionLogPolicy<Action>::coalesce` feed the
  durable log, and provides the scoped-install pattern the registries lack. Also
  `LogEntry::idempotencyKey` and `journal::OutboxRelay`, the transactional
  outbox this spec's `setOutboxManaged`/`isOutboxManaged` opt-out enables — see
  [Transactional outbox (opt-in)](../journal/journal.md#transactional-outbox-opt-in).
- **[backend.md](backend.md)** — backends store `IModelHolder`s in a single map
  and drive `IBackendChangedSink` / `BackendChangedMixin`; the model instances
  created by `ModelRegistryFactory` land here.
- **[security.md](../security.md)** — the `session::current()` principal stamped onto
  every logged entry by `recordIfAttached`, and the trust boundary of the
  string-keyed remote dispatch surface.
- **Error handling** — the `detail::ParseError` / `std::runtime_error` taxonomy
  the registries raise (see [Failure modes](#failure-modes)); glaze codec errors
  originate in the `ActionTraits` JSON functions.
- **Concurrency and lifetimes** — the static-init-then-read-only discipline in
  [Thread safety](#thread-safety) and the process-lifetime singleton ownership in
  [Limitations](#limitations).