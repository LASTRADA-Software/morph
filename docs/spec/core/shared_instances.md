# Keyed, shareable model instances — design

## Contents

- [The gap this closes](#the-gap-this-closes)
- [What morph already has](#what-morph-already-has)
- [Declaring a primary key](#declaring-a-primary-key)
- [Where the key comes from](#where-the-key-comes-from)
- [`AllowShared` — opting a handler into sharing](#allowshared--opting-a-handler-into-sharing)
- [Re-pointing, not re-keying](#re-pointing-not-re-keying)
- [The instance directory](#the-instance-directory)
- [Enumerating live instances](#enumerating-live-instances)
- [Wire protocol changes](#wire-protocol-changes)
- [Ownership and authorization](#ownership-and-authorization)
- [Lifetime and the A7 connection-scope change](#lifetime-and-the-a7-connection-scope-change)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Failure modes](#failure-modes)
- [Limitations](#limitations)
- [Non-goals](#non-goals)
- [Cross-references](#cross-references)

## The gap this closes

`Bridge::registerHandler<Model>()` unconditionally constructs a fresh
`HandlerBinding` and calls `registerModelWithContext(...)`. Every
`BridgeHandler` is therefore a new model instance, always. There is no way to
name an instance, no way to ask for one that already exists, and no way to find
out which ones are alive.

The cost is visible in `examples/bank`: five controllers each construct a
`BridgeHandler<bank::AccountModel>`, so the desktop GUI holds five
`AccountModel` instances — and, because each model lazily opens its own
`Lightweight::DataMapper`, five SQLite connections — for what is logically one
thing.

Once models hold state ([the bank example](../../../examples/bank/README.md))
the problem stops being wasteful and starts being wrong: five instances of
account 42 are five divergent copies of that account's balance.

## What morph already has

Most of the mechanism is present and only needs connecting.

- **A stable per-instance identity already exists.** `HandlerBinding::contextKey`
  is documented as "stable identity of this model instance (e.g. an account
  id)" and already travels in the `register` wire envelope
  ([wire.md](wire.md)). It is used **only** for journal entity keys
  and server-side log attachment; `wire.md`'s design-decision table states
  explicitly that `contextKey` plays no part in instance routing. The vocabulary
  is there; the routing is not.
- **Structural trait detection is an established pattern.**
  [views.md](../forms/views.md) detects `kind`, `query`, `title`, `rowKey`
  and friends "via a `requires`-expression, not inheritance or a marker base".
  `KeyedModel` is detected the same way, over either the deduced
  `ModelKeyTraits<M>` or an explicit nested alias.
- **Per-instance authorization exists.** `IAuthorizer::authorizeInstance` is
  consulted on every `execute` and every `deregister`, carrying the instance id
  and its recorded owner ([session.md](../session/session.md)).
- **One strand per instance** already gives a shared instance the serialisation
  it needs; sharing an instance changes nothing about how its actions run.

## Declaring a primary key

**A model's own class body says nothing about keys.** One line beside the
registrations the author is already writing designates the action that defines
the key, and the key's *type* is deduced from the member it names:

```cpp
class AccountModel {                       // a plain C++ class — unchanged
public:
    dto::AccountInfo execute(const dto::GetAccount&);
    dto::CommandResult execute(const dto::CloseAccount&);
};

BRIDGE_REGISTER_MODEL (AccountModel, "AccountModel")
BRIDGE_REGISTER_ACTION(AccountModel, GetAccount,   "GetAccount")
BRIDGE_REGISTER_ACTION(AccountModel, CloseAccount, "CloseAccount")

BRIDGE_MODEL_KEY(AccountModel, GetAccount, &GetAccount::id);  // key type deduced: std::int64_t
BRIDGE_KEY_FROM(CloseAccount, &CloseAccount::id);             // also carries it
```

`BRIDGE_MODEL_KEY` appears **once per model** — it is an explicit
specialisation of `ModelKeyTraits<Model>` and cannot be repeated. Every *other*
action naming the same entity uses `BRIDGE_KEY_FROM`, which records only that
the action carries the key. Most models need just the one line, because most
have a single loader action and the rest are keyless.

A key type must be one morph can carry on the wire and use as a map key: an
integral type or `std::string`.

A model may still declare `using PrimaryKey = …` in its own body, and that wins
over the deduced type — *infer by default, declare to override*, the same rule
the rest of `morph::forms` follows. It is useful only when the key type differs
from the field's type.

### Attachment is automatic

A handler names a key exactly once, in a keyed action; everything after it
follows the handler:

```cpp
BridgeHandler<AccountModel, AllowShared> first{bridge, gui}, second{bridge, gui};

first .execute(GetAccount{.id = 32});   // no instance for 32 → constructs one
second.execute(GetAccount{.id = 32});   // 32 is live → attaches, constructs nothing

first .execute(Deposit{.amountMinor = 100});  // keyless → instance 32
second.execute(GetBalance{});                 // keyless → instance 32, sees the 100
```

## Where the key comes from

A keyed action declares which of its fields carries the key. Different actions
spell it differently, which is why the declaration is per action rather than per
model — one of them additionally defines the model's key type:

```cpp
BRIDGE_MODEL_KEY(AccountModel, GetAccount, &GetAccount::id)  // defines + carries
BRIDGE_KEY_FROM(Deposit,      &Deposit::accountId)           // carries
BRIDGE_KEY_FROM(CloseAccount, &CloseAccount::id)             // carries
```

An action that *creates* the entity has no key to carry — it produces one. Such
an action sources the key from its **result**, exactly as a database insert
returns its generated primary key:

```cpp
BRIDGE_MODEL_KEY_FROM_RESULT(CustomerModel, OpenAccount, &dto::AccountInfo::id)
```

Actions with neither declaration are **keyless**, and most actions are: they run
on whichever instance the handler is currently attached to and say nothing about
identity. This is the common case, not the exception — a stateful model's whole
point is that its actions operate on state the instance already holds.

A handler may also attach explicitly, without going through an action:

```cpp
handler.attach(42);
```

## `AllowShared` — opting a handler into sharing

Sharing eligibility is a **static** property of the handler, expressed as a
template parameter. The primary key is discovered **dynamically**, from
whichever action first supplies one.

```cpp
BridgeHandler<AccountModel, AllowShared> a1{bridge, gui};   // no primary yet
BridgeHandler<AccountModel, AllowShared> a2{bridge, gui};   // no primary yet
BridgeHandler<AccountModel>              a3{bridge, gui};   // opts out

a1.execute(GetAccount{.id = 42});   // sets primary 42 → modelId 1
a2.execute(GetAccount{.id = 42});   // primary 42 already live → attaches to modelId 1
a3.execute(GetAccount{.id = 42});   // sets primary 42 → modelId 2, deliberately separate
```

`BridgeHandler<Model>` — the spelling every existing call site uses — is
`BridgeHandler<Model, NoSharing>`, and behaves byte-for-byte as it does today:
its own instance, never entered into the directory, never handed to anyone
else. This is what keeps the feature backward compatible.

Because a shared handler starts with no primary, **it registers nothing at
construction**. The `register` envelope is deferred until the handler knows
which instance it wants. A handler that only ever runs keyless actions gets a
private instance created on first execute; see
[Failure modes](#failure-modes).

## Re-pointing, not re-keying

A primary is **not** write-once. A handler already attached to account 42 may
execute a keyed action naming 43:

```cpp
a1.execute(GetAccount{.id = 42});   // attached to the instance for 42
a1.execute(GetAccount{.id = 43});   // now attached to the instance for 43
```

This **re-points the handler** to the instance holding key 43, creating that
instance if it does not exist. The instance for 42 is untouched — it keeps its
identity and its state, and survives if any other handler is still attached.

Instances never mutate their own identity. That is a deliberate simplification
and it is what makes the rule total: because a key always maps to exactly one
instance and an instance never changes key, there is no collision case to
resolve, no merge semantics to define, and no window in which the directory
disagrees with itself. Re-pointing gives the account-switching behaviour a GUI
actually wants, without any of that.

Re-pointing a handler does **not** cancel its in-flight calls. A call dispatched
against instance 1 completes against instance 1 and delivers its result
normally; only subsequent calls go to the new instance.

## The instance directory

The directory lives **server-side**, in `RemoteServer`, so instances are
reusable across clients. Two connections that attach to key 42 reach the same
instance and see each other's state — that is the point of (a), and it is what
distinguishes this from a client-side handle cache.

The directory maps `(modelTypeId, primaryKey) → ModelId`, held under the same
`_regMtx` that guards `_models`/`_owners`, so directory membership can never
desync from instance existence — the same invariant the connection-scope map
already maintains ([backend.md](backend.md), "Connection scopes").

Only instances created by an `AllowShared` handler are entered. A plain
handler's instance is invisible to the directory and unreachable by key.

In local mode (`LocalBackend`) the directory lives in the backend rather than
the server, with identical semantics. The call site is unchanged between the
two, as morph requires everywhere.

## Enumerating live instances

```cpp
handler.instances()                       // Completion<std::vector<AccountModel::PrimaryKey>>
    .then([](std::vector<std::int64_t> keys) { /* {42, 43, 71} */ });
```

**This must be asynchronous.** The directory is server state, so in remote mode
answering it is a round trip. morph's core rule is that a call site is identical
local and remote, so the local implementation returns an already-resolved
`Completion` rather than the API returning a bare `std::vector` that only works
in-process.

The result is a snapshot, not a live view, and it is stale the moment it
arrives — another client may attach or release between the reply being built
and the callback running. Callers must treat a returned key as "was live
recently", never as a guarantee that a subsequent `attach` finds the same
instance.

Only *shared* instances are enumerable. Plain handlers' instances are absent by
construction.

## Wire protocol changes

Four additive changes. All are compatible with the additive-only evolution
policy in [wire.md](wire.md), and the lenient decoding that A6
established means an older peer ignores what it does not understand.

- **`register` grows `primary` and `shared`.** `primary` is the key as a string
  (integral keys are decimal-encoded); `shared` is a bool. A `register` with
  `shared: true` is a *register-or-attach*: the server returns the existing
  `ModelId` for that `(typeId, primary)` if one is live, otherwise creates one
  and enters it in the directory. `shared: false` or absent is today's
  behaviour exactly.
- **A new `attach` request.** Re-points an existing binding at a different
  primary without tearing down and recreating it, returning the target
  `ModelId`. Semantically a `deregister` + `register` pair, made atomic so a
  re-pointing handler cannot lose its slot to `LimitPolicy::maxLiveModels` in
  between.
- **A new `assign` request.** Files an already-live, still-anonymous `modelId`
  under a primary key, in place. This is what makes a result-sourced key work
  without losing state: an action that creates its own entity runs on a
  not-yet-keyed instance, and only the reply carries the generated key, so the
  instance the action ran on is promoted rather than abandoned for a fresh
  one. Promotion only ever applies to a still-anonymous instance: the
  existing holder of a key always wins (promoting onto a taken key is a
  silent no-op, never a displacement), and an instance that already holds a
  *different* real key is left exactly where it is (also a silent no-op) —
  instances never change key, so `assign` never reaches for one still in use
  elsewhere.
- **A new `instances` request.** Takes a model type id, replies with the live
  primary keys for it. Subject to `authorize` like any other request; see below.

`contextKey` keeps its current meaning and is **not** overloaded as the primary.
The two coincide in practice — a keyed model will normally set `contextKey` to
its primary so journal entries carry the entity key — but conflating them would
silently change behaviour for anyone already setting `contextKey` for journal
purposes, which the framework's opt-in discipline forbids.

## Ownership and authorization

`RemoteServer` records an `ownerPrincipal` for each instance at register time
and consults `authorizeInstance` on every execute, with the documented typical
policy being `ownerPrincipal.empty() || ownerPrincipal == ctx.principal`
([session.md](../session/session.md)).

Under that policy, a second client attaching to an instance the first client
created would be **rejected**. Cross-client sharing and per-instance ownership
are therefore mutually exclusive, and the design says so rather than letting an
authorizer silently defeat the feature:

- **A shared instance is ownerless.** Its `ownerPrincipal` is empty, so the
  standard policy admits every principal, and gating access to it is the job of
  `authorize` (per model type and action) or of the model itself.
- **`authorizeRegister` still gates creation.** An authorizer that refuses
  `register` for a model type refuses it whether or not the request is shared.
- **`attach` and `assign` are gated by `authorizeRegister` too**, the same
  hook `register` uses. Filing an instance into the directory — whether by
  creating it (`register`, `attach`) or by promoting one already live
  (`assign`) — is bounds-checked identically; there is no path that reaches
  the directory without it.
- **`instances` is gated by `authorize`** for the model type with an empty
  action id, so an authorizer can refuse enumeration without refusing use. It
  leaks the set of live keys to anyone permitted to call it, which is a
  meaningful disclosure for key spaces that are themselves sensitive — the
  security spec must call this out.

An application that needs per-instance ownership on a shared model must enforce
it inside the model, from `Context::principal`, which is the same advice
`security.md` already gives for security-critical checks.

## Lifetime and the A7 connection-scope change

This changed shipped A7 behaviour, which nothing in the §A–§E program did. It was
unavoidable.

`closeConnection(cid)` used to erase every model recorded in `cid`'s scope. With
cross-client sharing that would destroy an instance another live client is still
attached to, so a scope entry is a **reference**, not ownership:

- Each attach — from any connection — increments an instance's attach count.
- `deregister`, handler destruction, and `closeConnection` each decrement.
- The instance is destroyed when the count reaches zero, at which point it
  leaves the directory.
- `closeConnection` remains idempotent and still bypasses `IAuthorizer`; it
  decrements once per scope entry regardless of how many handlers a single
  connection had attached.

Unshared instances have exactly one attacher by construction, so their lifetime
is unchanged: count reaches zero on the same event that erases them today.

An optional idle grace period (keep a zero-count shared instance alive for *n*
seconds in case another client re-attaches) is **not** part of this work.
Default behaviour is immediate destruction at zero.

`LimitPolicy::maxLiveModels` counts instances, not attachments, so sharing
strictly reduces pressure on it.

## API reference

| Symbol | Signature | Meaning |
|---|---|---|
| `BRIDGE_MODEL_KEY(M, A, &A::f)` | macro | Designates `A` as the action defining `M`'s key, and deduces the key type from `f`. Once per model. |
| `Model::PrimaryKey` | optional nested alias | Overrides the deduced key type. Integral or `std::string`. |
| `morph::bridge::AllowShared` | tag type | Second template argument of `BridgeHandler`. Opts the handler into the directory. |
| `morph::bridge::NoSharing` | tag type | The default. Today's isolated-instance behaviour. |
| `BRIDGE_KEY_FROM(A, &A::field)` | macro | Declares that a further action `A` also carries the key. |
| `BRIDGE_MODEL_KEY_FROM_RESULT(M, A, &R::field)` | macro | As `BRIDGE_MODEL_KEY`, but the key comes from `A`'s *result*. |
| `BRIDGE_KEY_FROM_RESULT(A, &R::field)` | macro | A further creating action whose result establishes the key. |
| `handler.attach(key)` | `void` | Attaches (or re-points) without executing an action. |
| `handler.primary()` | `std::optional<PrimaryKey>` | The handler's current primary; empty if unattached. |
| `handler.instances()` | `Completion<std::vector<PrimaryKey>>` | Snapshot of live shared keys for this model type. |

## Design decisions

- **Sharing is static, identity is dynamic.** Whether a handler *may* share is a
  compile-time property, so it is visible at the declaration and cannot vary per
  call. Which instance it shares is a runtime property, because that is what the
  user is choosing at runtime.
- **The key type is deduced, not restated.** Writing it in the model class as
  well as in the action field would be the same fact in two places, free to
  drift. `BRIDGE_MODEL_KEY` reads it off the member pointer it is already given,
  which is why a keyed model's class body is indistinguishable from an unkeyed
  one's.
- **The directory is server-side.** A client-side cache would solve the bank's
  five-connections problem but not the one that matters — two clients diverging
  on the same entity. Server-side is the whole reason this needs wire changes.
- **Instances never change key.** Re-pointing the handler achieves the same user
  goal with none of the collision, merge, or directory-consistency cases.
- **`contextKey` is not reused as the primary.** It has a shipped meaning;
  overloading it would change behaviour for existing users silently.
- **Shared instances are ownerless.** The alternative — teaching
  `authorizeInstance` about a set of owners — makes a simple, shipped, verified
  hook substantially more complex to serve a case the model layer can handle.
- **`instances()` is async even locally.** Local/remote call-site symmetry is
  the framework's most load-bearing promise; a synchronous convenience overload
  would be the first place it broke.

## Failure modes

- **A shared handler that runs a keyless action while unattached** gets a
  private instance with no primary, which can never enter the directory. It is
  then an `AllowShared` handler that shares nothing. The recommended discipline
  is *keyed action, or `attach`, first*; the implementation should make this
  observable — `primary()` returning empty after a successful execute is the
  signal — rather than silently degrading.
- **Attaching to a key whose entity does not exist.** The directory happily
  creates an instance; hydration fails inside the model and the action completes
  through `onError`. The instance must not be left in the directory in a
  half-hydrated state, so a failed *first* action on a freshly created shared
  instance releases it.
- **`instances()` raced against `attach`.** Documented as inherent: the snapshot
  is stale on arrival. An `attach` to a key from a stale list is not an error —
  it simply creates the instance again.
- **Re-pointing with calls in flight.** In-flight calls complete against the old
  instance. A caller that assumes `.then` runs against the newly attached
  instance is wrong; the spec must state the ordering explicitly.
- **`closeConnection` under-counting.** If a connection attaches the same
  instance from two handlers, the scope must record two references, or closing
  it leaks one. This is the main correctness risk in the A7 change and needs a
  dedicated test.

## Limitations

- **No activation on demand from persistence.** An instance exists because a
  handler asked for it, not because the key exists in a database. There is no
  "the instance always exists, virtually" guarantee.
- **No idle deactivation.** Zero attachments destroys immediately; there is no
  keep-alive, no LRU, no eviction policy.
- **No key derived from the session principal.** A per-user model must be
  attached explicitly after login.
- **One key per instance.** No secondary keys, no alternate indexes, no
  querying the directory by anything but model type.
- **Enumeration is per model type and unfiltered.** No paging, no predicate; a
  model type with very many live instances returns all of them.
- **`instances()` discloses live keys** to any principal `authorize` admits.

## Non-goals

- **Not virtual actors.** No perpetual existence, no placement, no clustering,
  no activation-on-message-to-a-cold-key. The directory tracks what exists; it
  does not conjure it.
- **No cross-instance transactions.** Two shared instances are two strands; an
  operation spanning both is a domain concern, as it is today.
- **No state persistence provider.** What an instance holds and how it is loaded
  is entirely the model's business — see
  [the bank example](../../../examples/bank/README.md).

## Cross-references

- [the bank example](../../../examples/bank/README.md) — the demonstrator; a key
  identifies nothing until models hold state.
- [bridge.md's subscription semantics](bridge.md#subscription-semantics) — how attached handlers
  learn that a shared instance changed.
- [bridge.md](bridge.md) — `HandlerBinding`, `contextKey`,
  `registerHandler`, and `switchBackend`'s re-registration path.
- [backend.md](backend.md) — `RemoteServer`, connection scopes
  (A7), and `LimitPolicy`.
- [wire.md](wire.md) — the envelope, additive evolution, and the
  `contextKey`-vs-`modelId` decision this proposal preserves.
- [session.md](../session/session.md) — `authorizeInstance`,
  `authorizeRegister`, and the recorded owner principal.
- [security.md](../security.md) — the per-instance ownership hook and the
  trust boundary the ownerless-shared-instance decision sits inside.
