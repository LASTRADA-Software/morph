# Locality: how a local call differs from a remote one

morph's headline claim is that a call site works "without caring whether the
model runs in this process or across a socket". That claim is defensible — the
local path already obeys the remote path's asynchronous, fallible contract, and
there is no distributed object graph to leak — but it is not *absolute*, and the
exceptions were previously scattered across `backend.md`, `registry.md`,
`security.md`, `forms.md` and the README, with no page collecting them.

This is that page. It is the reference for anyone deciding whether a model can
be moved across the wire unchanged, and for anyone debugging a behaviour that
appears only on one side.

`docs/spec/core/backend.md` remains the per-backend API reference; this document
only tabulates the *differences*, and links there for depth.

## The delta

| # | Difference | Local (`LocalBackend`) | Remote (`RemoteServer` + a transport) | Depth |
|---|---|---|---|---|
| 1 | **Model construction** | Runs the caller's factory closure, so a model needs no default constructor and no registration to be *constructed*. | Constructs through `ModelRegistryFactory` from the string type id (`_registry.create(env.typeId)`), so the model must be default-constructible **and** carry `BRIDGE_REGISTER_MODEL`. | [backend.md](backend.md), Limitations; [registry.md](registry.md) |
| 2 | **Authorization** | Does not exist. There is no authorizer on this path at all. | `IAuthorizer` is consulted on every kind — `authenticate`, `authorizeRegister`, `authorize`, `authorizeInstance`. | [security.md](../security.md) |
| 3 | **Exception type** | The concrete exception the model threw reaches the caller. | Types do not cross the wire. The server serialises a message into an `err` envelope and the client raises a generic `std::runtime_error`. Catch by type only on the local path. | [registry.md](registry.md) |
| 4 | **Error text** | Wording produced in-process. | Differs verbatim from the local wording for the same underlying failure. | [backend.md](backend.md), Failure modes |
| 5 | **Quantity wire validation** | `reconcileDeclaredPrecision` and `enforceQuantityBounds` do **not** run: no JSON is involved, so `Quantity` fields keep whatever precision the caller constructed them with. | Both run on the decode path, before the validator. | [forms.md](../forms/forms.md), "Pre-decode wire validation" |
| 6 | **Computed fields and validators** | `recomputeAll` and `ActionValidator<A>::ready` **do** run — this is deliberately *not* a locality difference. | Same. | [forms.md](../forms/forms.md) |
| 7 | **Payload reference semantics** | The model receives the caller's own object. | The payload is a JSON round-trip, so the model receives a reconstruction. Anything not part of the serialised shape does not survive. | Implied by the wire model |
| 8 | **Failure set** | Cannot produce transport failures. | Adds failures with no local analogue — `DisconnectedError` (raised only from a transport backend), `TimeoutError`, and the server's `err "server busy"`. | [backend.md](backend.md) |
| 9 | **Registration cost** | A map insert. | A blocking round-trip (a nested `QEventLoop`, or a condvar park). | [backend.md](backend.md), "Asynchronous registration" |
| 10 | **Instance lifetime** | Reclaimed only when the handler goes out of scope. | Additionally reclaimed by connection close: a `register` is attributed to a connection scope, and `closeConnection(cid)` reclaims everything in it. | [backend.md](backend.md), "Connection scopes" |
| 11 | **Subscriptions** | `subscribe<R>` fans out to subscribers on the same `Bridge`. | Does not cross the wire — the string `subscribe` does not occur in `remote.hpp` at all. A remote peer's mutation notifies nobody locally. | [bridge.md](bridge.md) |
| 12 | **Thread context and latency** | One strand hop. | Differs per transport; `backend.md` carries three separate tables. | [backend.md](backend.md), "Thread context" |
| 13 | **`onBackendChanged()`** | The only backend that implements it. Fires when the process goes *offline* — i.e. when switching away to another backend. | Never fires. `SimulatedRemoteBackend`, `SocketBackend` and `QtWebSocketBackend` all define `notifyBackendChanged()` as an empty body, so it never fires on *reconnect* to a remote backend. | [backend.md](backend.md) |

## Reading row 13

Row 13 is the one most likely to be misread, because the name suggests a
reconnect notification and it is not one.

`IBackend::notifyBackendChanged()` is pure virtual, and exactly one
implementation has a body: `LocalBackend`'s. The three remote backends override
it as `{}`. `Bridge` calls it on the *incoming* backend when the active backend
is switched, so the sequence that actually delivers a callback is
"remote → local" — going offline. The sequence a reader usually wants,
"local → remote" on reconnect, delivers nothing, because the incoming backend's
implementation is empty.

An offline-queue replay seam built on this hook therefore fires when
connectivity is *lost*, not when it is regained.

## What this page does not change

Nothing here is a defect list. Rows 1, 2, 9, 10 and 13 are deliberate design
consequences of there being a real process boundary; rows 3, 4, 7 and 8 are
what serialisation costs. Row 6 is included precisely because it is *not* a
difference — the two paths were aligned on purpose, and a future change that
silently unaligned them would be a regression.

The rows worth treating as open questions are 5 (two validations that run on
one path only) and 13 (a hook whose name does not describe when it fires).
