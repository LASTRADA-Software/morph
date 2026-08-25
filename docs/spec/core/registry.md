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
    - [Control bytes in action and result bodies](#control-bytes-in-action-and-result-bodies)
    - [`payload_schema.hpp` — the payload fingerprint](#payload_schemahpp--the-payload-fingerprint)
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
- [`MORPH_CLIENT_ONLY` — suppressing model-owning registrars](#morph_client_only--suppressing-model-owning-registrars)
  - [`BRIDGE_REGISTER_ACTION_FOR_CLIENT` — a header seam for `MORPH_CLIENT_ONLY`](#bridge_register_action_for_client--a-header-seam-for-morph_client_only)
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

### What the macros emit

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

### Header placement is legal

Placing a `BRIDGE_REGISTER_*` invocation in a header included by many `.cpp`
files is **well-formed**, and is the ladder's standard practice — 40 headers under
`examples/` invoke a `BRIDGE_REGISTER_*` macro.

This document previously called it an ODR violation. That legal claim was
false. Item (1) is an explicit specialisation — a class definition — and
[basic.def.odr] expressly *permits* a class to be defined in more than one
translation unit when the definitions are token-identical, which a shared
header `#include`d unchanged guarantees. It is the same rule that makes any
ordinary header-defined class legal; no special exception is being threaded.
The claim was checked empirically as well as read from the standard: a two-TU
reproduction (a header invoking `BRIDGE_REGISTER_MODEL`/`BRIDGE_REGISTER_ACTION`,
included by `tu_a.cpp` and `tu_b.cpp`, both resolving the model through
`ModelRegistryFactory`) compiles, links and runs clean with **zero
diagnostics** on Clang 22.1.8 and GCC 15.3.0, and on Clang 22.1.3 and
MSVC 19.51 in morph#231.

Item (2) is what actually differs, and the cost is small. The anonymous
namespace makes each initialiser object internal to its TU, so the model is
registered once per *including* TU rather than once per program. That is
redundant work, not a defect: `registerModelOnce` forwards to
`ModelRegistryFactory::registerModel`, which does `insert_or_assign`, so the
repeat calls reassign an equivalent factory closure over the same key. The
observable end state is identical.

So choose placement on ordinary grounds — a header when several TUs need the
registration (see
[`BRIDGE_REGISTER_ACTION_FOR_CLIENT`](#bridge_register_action_for_client--a-header-seam-for-morph_client_only),
which prescribes exactly that), a `.cpp` when only one does and you would
rather not pay N static initialisers. What is *not* optional is that the
registration reaches the link: see the next section.

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

### Remotely instantiated models must be default-constructible — unless registered with a custom factory

`ModelRegistryFactory::create` reaches models registered via
`BRIDGE_REGISTER_MODEL` (i.e. via the single-argument
`registerModel<Model>(modelId)`) through `ModelFactory::create<Model>()`,
which does `std::make_unique<ModelHolder<Model>>()` with **no constructor
arguments** (see `model.hpp`). Any model registered that way — the ordinary,
common case — must therefore be **default-constructible**. A model with no
accessible default constructor still compiles the macro (which only needs
`ModelTraits`) but fails to compile the factory instantiation.

This is no longer the only registry-constructed path, though: the
two-argument `registerModel<Model>(modelId, factory)` overload (see
[`ModelRegistryFactory`](#modelregistryfactory) below) lets `factory` build
the holder however it likes — including calling a non-default constructor —
so a model that needs injected dependencies (previously reachable only
through a custom `Bridge::HandlerBinding::modelFactory` closure, and therefore
only via `Local`-mode/in-process registration) can now be registered for
`Socket`-mode/remote instantiation too, by calling
`ModelRegistryFactory::instance().registerModel<Model>(modelId, factory)`
directly instead of (or in addition to, last-write-wins) relying on
`BRIDGE_REGISTER_MODEL`'s default-construction registrar.

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

Both macros also generate a fifth member, `static const std::string&
payloadSchema()`, returning `morph::model::payloadFingerprint<A>()` — a
structural fingerprint of the JSON shape the four codec functions read and
write. It is stamped on every journal entry recorded for the action and
compared on replay, which is what lets `journal::replay()` notice that the
shape which wrote an entry is not the shape it is about to decode it with. See
[payload_schema.hpp](#payload_schemahpp--the-payload-fingerprint) below and
[journal.md, "Payload schema fingerprint"](../journal/journal.md#payload-schema-fingerprint).

A **hand-written** `ActionTraits` need not provide it. Such a specialisation
may map its struct to entirely different JSON than reflection would — or may
name a type Glaze cannot reflect at all (`tests/test_client_execute_deadline.cpp`
registers an anonymous-namespace struct, which has no linkage and so cannot be
reflected traditionally) — so deriving a shape from the struct would describe
something that never reaches the journal. `detail::actionPayloadSchema<A>()`
returns the empty string for it, entries are recorded unstamped, and replay
treats them exactly as every build before the fingerprint existed did. Opting
in is a matter of defining the member.

### `payload_schema.hpp` — the payload fingerprint

```cpp
namespace morph::model {
inline constexpr std::uint32_t kPayloadFingerprintScheme = 1;
template <typename A> const std::string& payloadShapeString();  // "(count:i4,state:s)"
template <typename A> const std::string& payloadFingerprint();  // "1:8c38bd160c0cf832"
}
```

`payloadShapeString<A>()` renders `A`'s JSON shape as a compact string — one
tag per member, key-sorted, recursing into reflected aggregates.
`payloadFingerprint<A>()` is the FNV-1a digest of that rendering, prefixed with
the scheme version so a future build that computes fingerprints differently can
tell a scheme change from a payload change. Both are memoised per type in a
function-local static.

Every tag is derived from a `std::` type trait or from Glaze's reflected key
strings — never from a compiler-spelled type name — so builds of the same
sources on different compilers, standard libraries, or platforms agree on the
digest. The grammar, the deliberate order-insensitivity, and the full list of
what the fingerprint cannot see are documented once, in
[journal.md](../journal/journal.md#payload-schema-fingerprint), where the
consequences live.

#### Control bytes in action and result bodies

`toJson`/`resultToJson` write with `detail::EscapingWriteOpts`, a `glz::opts`
refinement that turns on glaze's `escape_control_characters` — the same
treatment, and for the same two reasons, that `wire::encode` already applies
to the envelope (see wire.md, "Control bytes in string fields"). With the
option off, an ASCII control byte (U+0000–U+001F) in any caller-supplied
string field of an action or result:

- **produces invalid output** — RFC 8259 requires those code points to be
  escaped, and glaze's own reader enforces it, so the peer's `fromJson` throws
  a `ParseError` on a body its own peer just wrote; and
- **can be silently corrupted** — with a `\` or `"` earlier in the same
  string, glaze's chunked fast path writes such a byte out as two `0x00`
  bytes, and the result still decodes.

Action bodies are pure caller data (a paste's content, a chat message, a
filename), so this is at least as exposed as the envelope was. Escaping is
lossless in both directions; the read side needs no counterpart, since glaze's
reader already accepts `\uXXXX`.

`morph::model::detail::EscapingWriteOpts` deliberately duplicates
`morph::wire::detail::EscapingWriteOpts` rather than reusing it: the action
codec belongs to the model layer and must not acquire a dependency on the
transport layer's header to share a four-line option struct.

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

A common `validate()` body composes `morph::forms::allRulesSatisfied(*this)`
(an action's declared cross-field rules, [forms.md](../forms/forms.md)) with
`morph::forms::allRequiredEngaged(*this)` (per-field required-ness). Neither
requires any change to `ActionValidator`/`HasValidate` — both are ordinary
`bool`-returning calls inside `validate()`, picked up the same way any other
`validate()` body is.

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
(`morph::forms::reconcileDeclaredPrecision`, an exact re-rounding of the value
and not just a retag) before the `ready()` check, so a hand-built wire payload's
`Quantity` values match the schema's advertised `x-decimalPlaces` the same way
the client bridge dispatch path already normalises them (see
[forms.md](../forms/forms.md)). Immediately after
reconciliation, `morph::forms::enforceQuantityBounds` rejects any `Quantity`
field whose engaged value falls outside its unit's declared bounds
(`UnitTraits<E>::bounds`), throwing `QuantityDecodeError` before the `ready()`
check — a no-op for actions with no `Quantity` members, or whose units
declare no `bounds()` (see [forms.md](../forms/forms.md), "Pre-decode wire
validation"). `Bridge::executeVia`'s `localOp` does not reconcile precision or
enforce bounds — that path never decodes JSON, so there is no wire `dp` or
wire value to check against.

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

### `fromJson` is the codec boundary for an action payload

`morph::wire` carries an execute envelope's `body` as an opaque `std::string`
and never parses it — `wire.hpp` states this directly ("payload smuggled
*inside* `body` is invisible to any structural/depth check"). So
`ActionTraits<A>::fromJson` is the first and only place the body's contents are
decoded into typed fields, which makes it the layer responsible for what a
malformed payload means.

That matters for values whose decode **cannot fail**.
[`morph::math::Rational`](../util/rational.md) is the case in point: `setWire`
clamps what it cannot represent rather than rejecting, so
`{"num":5,"den":0,"dp":2}` would otherwise arrive as a perfectly plausible
`5/1`. A model's own `validate()` runs *after* the decode and has nothing left
to notice — the value looks fine by then.

`fromJson` therefore wraps its `glz::read` in a `morph::math::WireClampScope`
and throws `ParseError` if anything was clamped. `Rational` reports the fact;
this layer decides it is a protocol violation, because this is the layer that
knows the bytes came off a wire. A local caller constructing the same value in
code is unaffected.

Note that a *non-canonical but representable* value is accepted: `4/8` reduces
to `1/2`, and reduction is canonicalisation, not clamping — the value survives
intact.

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
    [[nodiscard]] virtual bool isBackendChangeAware() const noexcept = 0;
    virtual void onBackendChanged() {}
    template <typename Model> Model& into();
    void attachActionLog(std::shared_ptr<::morph::journal::IActionLog>, std::string contextKey);
    bool hasActionLog() const noexcept;
    void recordIfAttached(LogEntry entry);
    void setOutboxManaged(bool outboxManaged) noexcept;
    [[nodiscard]] bool isOutboxManaged() const noexcept;
};
```

- `isBackendChangeAware()` / `onBackendChanged()` are the compile-time-known
  backend-change-notification capability, exposed as base-class virtuals so a
  backend can query and invoke it without `dynamic_cast`. `ModelHolder<M>`
  answers `isBackendChangeAware()` from the `BackendChangedNotifiable<M>`
  concept and forwards `onBackendChanged()` to `M::onBackendChanged()` only
  when that concept holds; the base default is a no-op. See
  [backend.md](backend.md#localbackend--in-process-execution) for how
  `LocalBackend` uses this.
- `into<Model>()` down-casts to a concrete `Model&`; throws `std::bad_cast` on
  mismatch.
- `attachActionLog` sets the durable log sink and the instance's stable identity
  (stamped onto every `LogEntry`), then calls the protected virtual
  `onActionLogAttached(log, contextKey)` (base default: no-op) before storing
  either. `ModelHolder<Model>` overrides this to forward to
  `Model::attachActionLog(log, contextKey)` when `Model` structurally
  satisfies `ModelLevelActionLogAttachable` (`morph/core/model.hpp`) — the
  same "detect the hook structurally, forward only if present" shape
  `onBackendChanged()`/`BackendChangedMixin` use below. This is what lets a
  model that keeps its own model-level `IActionLog` reference (to read its
  own history back later, e.g. an activity-stream view) receive the same log
  instance a registry-constructed, remote/keyed attach populates the holder
  with — see [journal.md's "Attaching a log to remote
  instances"](../journal/journal.md#attaching-a-log-to-remote-instances). A
  model with no `attachActionLog` of its own is unaffected: the hook resolves
  to the base's no-op body for it.
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
    bool isBackendChangeAware() const noexcept override;
    void onBackendChanged() override;
  protected:
    void onActionLogAttached(const std::shared_ptr<::morph::journal::IActionLog>&,
                             const std::string& contextKey) override;
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

Optional interface for models that need to react to backend switches.
`ModelHolder<M>` inherits `BackendChangedMixin<M>` which conditionally derives
from `IBackendChangedSink` when `M` declares `void onBackendChanged()` (detected
by the `BackendChangedNotifiable<M>` concept) — this remains available to
anyone holding an `IModelHolder*` who wants to `dynamic_cast` to it directly.
`LocalBackend`, however, does not: it discovers and invokes the same capability
through `IModelHolder::isBackendChangeAware()` / `IModelHolder::onBackendChanged()`
— two base-class virtuals `ModelHolder<M>` answers from the same
`BackendChangedNotifiable<M>` concept — so its `notifyBackendChanged()` sweep
needs no RTTI and visits only models that opted in. See
[backend.md](backend.md#localbackend--in-process-execution).

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
    std::string schemaFor(std::string_view modelId, std::string_view actionId) const;
    static ActionDispatcher& instance();
};
```

- `registerAction` registers a runner that deserialises, reconciles any
  `Quantity` fields to their declared precision, rejects any `Quantity` field
  outside its unit's declared bounds (`morph::forms::enforceQuantityBounds`,
  throwing `QuantityDecodeError`; a no-op for actions with no `Quantity`
  members or whose units declare no `bounds()`), overwrites any declared
  computed fields from their inputs (`morph::forms::recomputeAll`,
  [forms.md](../forms/forms.md) — a no-op for actions with no
  `computedFields`; runs after precision reconciliation and bounds
  enforcement and before the validator check, so the validator sees the
  authoritative computed value), enforces `ActionValidator<Action>::ready(action)` (throwing `ValidationError`
  on `false`, before `Model::execute` runs), then calls `Model::execute(action)`
  inside a `try`/`catch (const std::exception&)`: on success it serialises the
  result and records a `LogEntry` with `outcome = Outcome::Succeeded` (when
  loggable and a log is attached); on a throw it records `outcome =
  Outcome::Failed` (`error = exc.what()`, `result` empty) for the same actions
  and rethrows unchanged, so callers see the same exception as before — the
  journal entry is a side effect, not a change to error propagation. Mirrors
  `Bridge::executeVia`'s `localOp` (`bridge.md`) for `LocalBackend`. See
  [journal.md, "Outcome"](../journal/journal.md#logentry--one-recorded-action-execution)
  for the full field/replay semantics. Every recorded entry — success or
  failure — is stamped with `detail::actionPayloadSchema<Action>()` in
  `LogEntry::schema`, and the same value is filed under `(modelId, actionId)`
  for `schemaFor()`.
- `dispatch` looks up the runner and invokes it; throws `std::runtime_error` for
  unknown pairs.
- `coalesce` returns the `ActionLogPolicy<Action>::coalesce` value for the pair;
  unknown pairs default to `false`.
- `schemaFor` returns the payload fingerprint the pair was registered with, or
  the empty string for an unregistered pair (or one whose `ActionTraits` is
  hand-written and supplies no `payloadSchema()`). This is the type-erased half
  of the journal's payload-evolution check: `registerAction` knows the concrete
  `Action` and can compute the fingerprint, while `journal::replay()` sees only
  the strings on a `LogEntry` and has to ask for it by id. The empty return is
  not an error — an entry naming an unregistered action fails at `dispatch()`
  with "unknown action" a moment later, which is the better diagnostic for that
  case.

### `ModelRegistryFactory`

Creates `IModelHolder` instances by string type-id. Used by `RemoteServer` to
instantiate models on demand from incoming `"register"` messages.

```cpp
class ModelRegistryFactory {
    template <typename Model>
    void registerModel(std::string_view modelId);

    template <typename Model, typename Factory>
        requires std::invocable<Factory> &&
                 std::convertible_to<std::invoke_result_t<Factory>, std::unique_ptr<IModelHolder>>
    void registerModel(std::string_view modelId, Factory factory);

    std::unique_ptr<IModelHolder> create(std::string_view modelId);
    static ModelRegistryFactory& instance();
};
```

- `create` throws `std::runtime_error` for unknown model types.
- The single-argument `registerModel<Model>(modelId)` registers the plain
  default-construction path — equivalent to
  `registerModel<Model>(modelId, [] { return ModelFactory::create<Model>(); })`
  — and is what `BRIDGE_REGISTER_MODEL` always uses.
- The two-argument overload is the **per-instance dependency-injection seam
  for registry-constructed (`Socket`-mode) models** — the equivalent, for
  `RemoteServer`'s registry path, of `Bridge::HandlerBinding::modelFactory` for
  the client-side `Local`-mode path. `factory` runs once per `create(modelId)`
  call (i.e. once per incoming `"register"` request, and once per fresh
  shared-instance creation — see `acquireSharedInstance`, `remote.hpp`), and
  may capture and hand the model constructor arbitrary per-instance
  dependencies an ordinary default constructor cannot reach: an injectable
  clock (see `docs/spec/util/datetime.md`'s `now()` override seam for the
  complementary, constructor-free path to the same goal), a secondary log
  handle, a feature flag. It is also the **only** way to register a model
  whose constructor takes arguments at all — such a model has no accessible
  default constructor, so the single-argument overload cannot compile against
  it (see [Remotely instantiated models must be default-constructible — unless
  registered with a custom factory](#remotely-instantiated-models-must-be-default-constructible--unless-registered-with-a-custom-factory)).
  `factory` returns an owning pointer convertible to
  `std::unique_ptr<IModelHolder>` (e.g.
  `std::make_unique<ModelHolder<Model>>(...)`) — the caller controls
  construction end-to-end, including which `ModelHolder<Model>` constructor
  overload runs. Unlike the default-construction overload, this one does
  **not** auto-attach the process-wide default action log
  (`morph::journal::defaultActionLog()`); a caller supplying its own factory is
  assumed to attach whatever log/identity it needs inside the closure via
  `IModelHolder::attachActionLog`, or to rely on `RemoteServer`'s
  `LogProvider` doing so afterward, exactly as the default path already
  allows. Two registrations for the same `modelId` still silently
  last-write-wins, as for the single-argument overload (see
  [Failure modes](#failure-modes)) — registering a custom factory under an id
  that a `BRIDGE_REGISTER_MODEL(Model, id)` invocation already claimed
  overwrites that default-construction factory, and vice versa, whichever
  static-init/runtime call happens last.

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
class is declared in `morph/core/bridge.hpp` in namespace `morph::bridge` — it depends
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

Both macros below name their generated anonymous-namespace variable by pasting
a fixed prefix onto `__COUNTER__` (via the two-level `BRIDGE_DETAIL_CAT`/
`BRIDGE_DETAIL_CAT_` indirection needed to force macro expansion before the
paste), not onto the spelling of `M`/`A`. Pasting the type directly (the
original approach) breaks for namespace-qualified or template types —
`app::models::Report` pastes `::` into the identifier. `__LINE__` was tried as
a replacement key but is only unique *within a single physical file*; two
different headers that each invoke one of these macros on the same line
number produce the same identifier once both are transitively `#include`d
into one translation unit, which is a hard redefinition error because C++
unnamed namespaces are per-TU, not per-file. `__COUNTER__` increments
monotonically across the whole translation unit regardless of which file
expands it, so it cannot collide this way.

### `BRIDGE_REGISTER_MODEL(M, NAME)`

Specialises `ModelTraits<M>` and registers a factory at static-init time.

```cpp
BRIDGE_REGISTER_MODEL(AccountModel, "Account")
```

Expands to:
- `template <> struct morph::model::ModelTraits<M> { static constexpr std::string_view typeId() noexcept { return NAME; } };`
- Unless `MORPH_CLIENT_ONLY` is defined: a `[[maybe_unused]] const bool` in an
  anonymous namespace (internal linkage, no explicit `static`) that calls
  `detail::registerModelOnce<M>(NAME)`. See
  [`MORPH_CLIENT_ONLY`](#morph_client_only--suppressing-model-owning-registrars).

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
  `resultToJson` use `glz::write<detail::EscapingWriteOpts{}>` (see
  ["Control bytes in action and result bodies"](#control-bytes-in-action-and-result-bodies));
  `fromJson`/`resultFromJson` use
  `glz::read<glz::opts{.error_on_unknown_keys = false}>` — the same
  forward-compatibility convention `wire::decode` uses (see wire.md,
  "Action-evolution policy") — so an older-compiled action struct silently
  ignores an additive field a newer peer sent.
- Unless `MORPH_CLIENT_ONLY` is defined: a `[[maybe_unused]] const bool` in an
  anonymous namespace calling
  `detail::registerActionOnce<M, A>(morph::model::ModelTraits<M>::typeId(), NAME)`
  (the model-id argument is the model's registered `typeId()`, not a raw string).
  See [`MORPH_CLIENT_ONLY`](#morph_client_only--suppressing-model-owning-registrars).
- A `[[maybe_unused]] const bool` in an anonymous namespace calling
  `detail::registerActionExecutorOnce<M, A>(morph::model::ModelTraits<M>::typeId(), NAME)`
  — always emitted, `MORPH_CLIENT_ONLY` or not.

**Hard requirement:** Every translation unit invoking `BRIDGE_REGISTER_ACTION`
must include `<morph/core/bridge.hpp>` (directly or transitively) because
`registerActionExecutorOnce` is only defined there. Without it, the link fails
with an unresolved external symbol.

A client-side alternative, `BRIDGE_REGISTER_ACTION_FOR_CLIENT`, avoids the
`Result`-deduction step that forces `M` to be a complete type — see
[`BRIDGE_REGISTER_ACTION_FOR_CLIENT` — a header seam for
`MORPH_CLIENT_ONLY`](#bridge_register_action_for_client--a-header-seam-for-morph_client_only)
below.

### `BRIDGE_REGISTER_VALIDATOR(A, FN)`

Specialises `ActionValidator<A>` with a custom predicate.

```cpp
BRIDGE_REGISTER_VALIDATOR(FormAction, [](const FormAction& a) {
    return a.a != 0.0 && a.b != 0.0 && a.c != 0.0;
})
```

Expands to `template <> struct morph::model::ActionValidator<A> { static bool ready(const A& action) { return (FN)(action); } };`.

## `MORPH_CLIENT_ONLY` — suppressing model-owning registrars

A pure client — one that dispatches every action to a remote peer and never
constructs a model locally — has no use for two of the three registrars
`BRIDGE_REGISTER_MODEL`/`BRIDGE_REGISTER_ACTION` normally emit:

- `registerModelOnce<M>` stores a factory (`[] { return ModelFactory::create<M>(); }`)
  in the process-level `ModelRegistryFactory`, used by `LocalBackend`/`RemoteServer`
  to construct a live instance.
- `registerActionOnce<M, A>` stores a runner in the process-level
  `ActionDispatcher` that calls `Model::execute(...)` directly on a live
  holder — the server-side dispatch path `RemoteServer` uses.

Both are ordinary functions the compiler must fully compile into the stored
closure regardless of whether that closure is ever *invoked* at runtime —
so even a build that never constructs a model locally still forces the
linker to resolve the model's constructor and `execute()` bodies, pulling in
whatever those depend on (a database driver, a native UI framework, an
OS-specific API) — dependencies a client target may have no link path for at
all (a browser/WASM build in particular), and will never call regardless.

Defining `MORPH_CLIENT_ONLY` (via the CMake option of the same name, which
adds it to the `morph` target's `INTERFACE` compile definitions) suppresses
both. `BRIDGE_REGISTER_MODEL`/`BRIDGE_REGISTER_ACTION` still specialise
`ModelTraits<M>`/`ActionTraits<A>` (type-ids, JSON codecs) exactly as before —
only the two registrar bodies above disappear.

**The third registrar needed the same treatment, contrary to first
appearances.** `registerActionExecutorOnce<M, A>` routes through
`BridgeHandler<Model>::execute<Action>()` → `Bridge::executeVia<Model, Action>`
— and `executeVia` unconditionally constructs an `ActionCall::localOp` closure
that calls `Model::execute(...)` directly, *regardless of which backend ends
up installed at runtime* (only `LocalBackend::execute` ever actually invokes
`call.localOp`; every remote backend ignores it). That closure is compiled
into `executeVia`'s instantiation the moment any code calls
`BridgeHandler<Model>::execute<Action>()` — which the type-erased
`ActionExecuteRegistry` executor `registerActionExecutorOnce` installs
*also* does, internally, to serve `executeJson`. So merely suppressing the
first two registrars is not sufficient to make a client-only build link if it
uses `BridgeHandler::execute<Action>()` (the typed API) or `executeJson` (the
type-erased API) at all — both routes reach the same `model.execute(...)`
call inside `executeVia`.

`Bridge::executeVia`'s `localOp` closure is therefore itself gated on
`MORPH_CLIENT_ONLY` (`bridge.hpp`): under the macro, the closure throws
`std::logic_error` instead of calling `Model::execute`, so nothing in the
compiled program ever references its definition. This is *not* a
per-registration-site choice like the two macros above — it lives inside
`executeVia` itself, compiled once per `(Model, Action)` instantiation,
consistently for the whole link (exactly the "carried on the interface, not
per-consumer" requirement below).

**Confirmed empirically** (see `tests/compile_checks/client_only_no_model_link.cpp`
and the `try_compile()` probes in `tests/CMakeLists.txt`): a model whose
constructor and `execute()` are declared but never defined anywhere in the
link succeeds when built with `MORPH_CLIENT_ONLY` defined, and fails to link
(both symbols genuinely referenced) when built without it.

**Must be carried on the `morph` target's `INTERFACE`, never per-consumer.**
The macro changes which registrars a model header emits; two translation
units disagreeing about it — one linking `registerModelOnce`'s closure, the
other not — would be an ODR violation (the closures wouldn't even have the
same instantiated members). The `MORPH_CLIENT_ONLY` CMake option sets it via
`target_compile_definitions(morph INTERFACE MORPH_CLIENT_ONLY)`, so every
consumer of the `morph::morph` target sees the identical definition.

**Must never be defined for a process that hosts models.** A server, or any
`Bridge` running `LocalBackend`, would silently register nothing:
`ModelRegistryFactory::create`/`ActionDispatcher::dispatch` would fail at
*runtime* with `"unknown model type"` — far from the actual cause — rather
than failing to compile or link. `Bridge::executeVia`'s `localOp` closure
throwing `std::logic_error` if ever reached is a second line of defence for
exactly this mistake: a `MORPH_CLIENT_ONLY` build that somehow still ends up
running `LocalBackend` gets a clear, immediate diagnostic instead of a
"model not found" red herring.

Off by default: the standard build (and every existing consumer) is unaffected.

### `BRIDGE_REGISTER_ACTION_FOR_CLIENT` — a header seam for `MORPH_CLIENT_ONLY`

`MORPH_CLIENT_ONLY` removes the *link* dependency on a model's implementation
(above), but not the *header* dependency: `BRIDGE_REGISTER_ACTION`'s `Result`
type is `decltype(std::declval<M&>().execute(std::declval<A>()))`, so `M` must
be a **complete type with `execute(A)` declared** at the exact point the macro
is invoked — ordinarily the model's own header. A pure client that never
constructs `M` still has to `#include` that header (and everything it pulls
in transitively — a persistence mixin's database-driver headers, for a model
backed by one) purely to let this `decltype` resolve, even though a
`MORPH_CLIENT_ONLY` build never calls `Model::execute` at all (`executeVia`'s
`localOp` throws instead, per the previous section). A WASM/browser client has
no include path for a native database client library at all, so this is a hard
build blocker, not merely extra compile weight.

`BRIDGE_REGISTER_ACTION_FOR_CLIENT(M, A, RESULT, NAME, ...)` closes this seam:
it emits the exact same `ActionTraits<A>` specialisation as
`BRIDGE_REGISTER_ACTION`, except `Result` is the explicitly-named @p RESULT
type argument instead of a `decltype`-deduced one. `M` is then used only as
`BridgeHandler<M>`'s template tag and `ActionExecuteRegistry`'s dispatch key —
both routes call only `ModelTraits<M>::typeId()` (needs the trait
specialisation, not `M`'s completeness) and, under `MORPH_CLIENT_ONLY`, never
reach `holder.into<M>()`/`M::execute(...)` (gated out inside `executeVia`, see
above) — so `M` may be **forward-declared and never defined** anywhere in the
client's link. A client's model header therefore reduces to one forward
declaration plus the two registration macros; the real, complete model
(inheriting whatever persistence mixin it needs) lives only in the
server-side translation unit that actually owns it.

```cpp
// client_only_model.hpp -- the ENTIRE client-visible surface for RecordModel,
// under MORPH_CLIENT_ONLY. No database-driver header, no ORM mixin, in sight.
struct RecordModel;  // forward declaration only -- never defined here

struct RecordMeasurement { /* ...fields... */ };
struct RecordMeasurementResult { /* ...fields... */ };

BRIDGE_REGISTER_MODEL(RecordModel, "RecordModel")
BRIDGE_REGISTER_ACTION_FOR_CLIENT(RecordModel, RecordMeasurement, RecordMeasurementResult, "RecordMeasurement")
```

**`M` being incomplete constrains *which* `BridgeHandler<M>` constructor a
client may use.** `BridgeHandler<M>`'s default constructor
(`BridgeHandler(Bridge&, IExecutor*)`) calls `Bridge::registerHandler<M>()`,
which unconditionally builds `[] { return ModelFactory::create<M>(); }` — and
`ModelFactory::create<M>` default-constructs `M` by value, requiring `M`
complete. Using it here would silently reintroduce the exact completeness
requirement this macro exists to avoid. The client must instead use the
**pre-built-binding constructor**
(`BridgeHandler(Bridge&, IExecutor*, shared_ptr<HandlerBinding>)`) with a
`modelFactory` that is never actually invoked in a `MORPH_CLIENT_ONLY` process
(no `LocalBackend` exists to call it — see `Bridge::executeVia`'s
`MORPH_CLIENT_ONLY` guard, above):

```cpp
auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
binding->typeId = std::string{morph::model::ModelTraits<RecordModel>::typeId()};
binding->modelFactory = [] -> std::unique_ptr<morph::model::detail::IModelHolder> {
    throw std::logic_error("client-only: RecordModel has no local instance");
};
morph::bridge::BridgeHandler<RecordModel> handler{bridge, guiExec, std::move(binding)};

// Dispatch generically -- executeJson never touches RecordModel's definition.
handler.executeJson("RecordMeasurement", bodyJson);
```

**`@p RESULT` is not checked against the real model's `execute` return
type — this is the one thing the macro cannot verify.** A mismatch is a
silent JSON-shape bug (the client (de)serialises the wrong shape on the wire),
not a compile error: nothing here compares against the server-side
registration, which still uses the plain `BRIDGE_REGISTER_ACTION` from the
real model's own header and therefore still deduces `Result` correctly from
`Model::execute`'s actual return type. Keeping the two declarations in sync is
the caller's responsibility, same as any hand-written wire contract.

Usable with or without `MORPH_CLIENT_ONLY` defined — the emitted
`ActionTraits<A>` is identical either way — but the header-avoidance benefit
only materialises when `M` is genuinely left incomplete at the client's
registration site *and* `MORPH_CLIENT_ONLY` is defined (so
`MORPH_DETAIL_REGISTER_MODEL_LOCAL`/`MORPH_DETAIL_REGISTER_ACTION_LOCAL`'s
registrars, which do need `M` complete, are suppressed for that build).

**Confirmed empirically**
(`tests/compile_checks/client_only_facade_no_model_header.cpp`, run via
`try_run()` in `tests/CMakeLists.txt`): `ClientOnlyFacadeModel` is
forward-declared and never defined anywhere in that probe's link; it compiles,
links, and round-trips `ClientOnlyFacadeAction` through `toJson`/`fromJson`
correctly under `MORPH_CLIENT_ONLY`.

## API reference

### Traits and policies

| Symbol | Kind | Purpose |
|---|---|---|
| `ModelTraits<M>` | class template | **Customisation point.** Maps model type to `std::string_view typeId()`. |
| `ActionTraits<A>` | class template | **Customisation point.** Maps action type to id, JSON codec, result type, `Loggable`, and (macro-generated only) `payloadSchema()`. |
| `ActionValidator<A>` | class template | **Customisation point.** `static bool ready(const A&)` — built-in detection of `bool validate() const`, overridable via specialisation. |
| `ValidationError` | exception type | Thrown by `ActionDispatcher::registerAction`'s runner and `Bridge::executeVia`'s `localOp` when `ActionValidator<A>::ready` returns `false`. `std::runtime_error` subclass carrying `"action failed validation: <modelType>/<actionType>"`. |
| `ActionLogPolicy<A>` | class template | **Customisation point.** `static constexpr bool coalesce = false` — checkpoint coalescing policy. |
| `payloadFingerprint<A>()` | function template | `payload_schema.hpp`. Structural fingerprint of `A`'s JSON shape, `"<scheme>:<16 hex digits>"`. Companion: `payloadShapeString<A>()`, the human-readable rendering behind it. |
| `Loggable` | enum | `{ No, Yes }` — strong boolean for action loggability. |

### Concepts (detail)

| Symbol | Purpose |
|---|---|
| `HasValidate<A>` | `true` when `A` exposes `bool validate() const`. |
| `HasLoggableFlag<A>` | `true` when `ActionTraits<A>` exposes `static constexpr Loggable loggable`. |
| `HasPayloadSchema<A>` | `true` when `ActionTraits<A>` exposes `payloadSchema()`. Both registration macros generate one; a hand-written specialisation need not. |
| `BackendChangedNotifiable<M>` | `true` when `M` exposes `void onBackendChanged()`. |

### Type-erased model infrastructure

| Symbol | Kind | Purpose |
|---|---|---|
| `IModelHolder` | abstract class | Type-erased model owner with an action log slot, an outbox-managed opt-out flag, and a compile-time-answered backend-change-awareness bit. |
| `ModelHolder<M>` | class template | Concrete holder storing `M` by value; conditionally inherits `IBackendChangedSink`; answers `isBackendChangeAware()`/`onBackendChanged()` from `BackendChangedNotifiable<M>`. |
| `ModelFactory` | class | `static create<M>()` — default-constructs `ModelHolder<M>` and attaches the process-wide default log. |
| `IBackendChangedSink` | abstract class | Optional interface for backend-switch notification; reachable via `dynamic_cast`, though `LocalBackend` itself dispatches through `IModelHolder`'s virtuals instead. |
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
| `BRIDGE_REGISTER_ACTION` | `(M, A, NAME, ...)` | `ActionTraits<A>` specialisation + static-init dispatcher and executor registration. Optional 4th arg: `Loggable`. `Result` deduced from `decltype(M::execute(A))`, requiring `M` complete. |
| `BRIDGE_REGISTER_ACTION_FOR_CLIENT` | `(M, A, RESULT, NAME, ...)` | Same as `BRIDGE_REGISTER_ACTION`, except `Result` is the explicitly-named `RESULT` argument — `M` may be forward-declared only. See ["a header seam for `MORPH_CLIENT_ONLY`"](#bridge_register_action_for_client--a-header-seam-for-morph_client_only). |
| `BRIDGE_REGISTER_VALIDATOR` | `(A, FN)` | `ActionValidator<A>` specialisation + custom predicate. |

### Detail helpers

| Symbol | Purpose |
|---|---|
| `PairKeyHash` | Hash functor for `std::pair<std::string, std::string>` keys used by `ActionDispatcher` and `ActionExecuteRegistry`. |
| `actionLoggable<A>()` | Returns `ActionTraits<A>::loggable` if present, else `Loggable::Yes`. |
| `actionPayloadSchema<A>()` | Returns `ActionTraits<A>::payloadSchema()` if present, else `""` (unstamped). |
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
  `ActionExecuteRegistry` class itself (declared in `<morph/core/bridge.hpp>`, not
  `registry.hpp`). Explains the **hard `#include <morph/core/bridge.hpp>` requirement**
  for any TU using `BRIDGE_REGISTER_ACTION` (`registerActionExecutorOnce` is only
  *defined* there), the parallel executor path this spec's
  `ActionExecuteRegistry` section summarises, and `Bridge::executeVia`'s
  `localOp`, which enforces the same `ValidationError` gate as this spec's
  `ActionDispatcher::registerAction` for the local execution path and performs
  the same computed-field recompute (`morph::forms::recomputeAll`) for it.
- **[forms.md](../forms/forms.md)** — `allRequiredEngaged`; the closed
  cross-field rule vocabulary (`allRulesSatisfied`, `x-rules`) that composes
  into `validate()` alongside it; and `computed`/`computeList`/`recomputeAll`,
  both invoked by `ActionDispatcher::registerAction`'s runner before
  `Model::execute`.
- **[journal.md](../journal/journal.md)** — `IActionLog`, `LogEntry`, `SessionLog`,
  checkpoint coalescing, and `ScopedActionLog`. Explains how the runner's
  `recordIfAttached` call and `ActionLogPolicy<Action>::coalesce` feed the
  durable log, and provides the scoped-install pattern the registries lack. Also
  `LogEntry::idempotencyKey` and `journal::OutboxRelay`, the transactional
  outbox this spec's `setOutboxManaged`/`isOutboxManaged` opt-out enables — see
  [Transactional outbox (opt-in)](../journal/journal.md#transactional-outbox-opt-in).
- **[backend.md](backend.md)** — backends store `IModelHolder`s in a single map
  and drive backend-change notification via `IModelHolder::isBackendChangeAware()`/
  `onBackendChanged()` (in turn backed by `BackendChangedMixin`/`IBackendChangedSink`);
  the model instances created by `ModelRegistryFactory` land here.
- **[security.md](../security.md)** — the `session::current()` principal stamped onto
  every logged entry by `recordIfAttached`, and the trust boundary of the
  string-keyed remote dispatch surface.
- **Error handling** — the `detail::ParseError` / `std::runtime_error` taxonomy
  the registries raise (see [Failure modes](#failure-modes)); glaze codec errors
  originate in the `ActionTraits` JSON functions.
- **Concurrency and lifetimes** — the static-init-then-read-only discipline in
  [Thread safety](#thread-safety) and the process-lifetime singleton ownership in
  [Limitations](#limitations).