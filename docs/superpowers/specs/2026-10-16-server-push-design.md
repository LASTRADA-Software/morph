# Server push: actions declare what they change, the server tells the GUI — design (spec 8)

The server tells every client that shows a piece of data when that data changes, so the GUI updates without
being asked. What changes is declared **once, next to the action's registration**: an action says which
record types it updates or changes, or that it changed its own instance's state. The framework then works out
which cached queries, in which clients, show that data. A UI document declares only what a screen **shows**
(its queries) and what an edit **sends** (its mutations). It no longer declares what to refresh after an action.

The same mechanism updates the GUI however the change was made: by this screen, by another screen of the
same client, by another user, by an offline replay, or by an agent acting through the server.

## Contents

[1 Intent](#1-intent) · [2 What exists today](#2-what-exists-today) · [3 Concepts](#3-concepts) ·
[4 Declaring records and effects](#4-declaring-records-and-effects) · [5 The catalog](#5-the-catalog) ·
[6 The server-to-client channel](#6-the-server-to-client-channel) · [7 Watching](#7-watching) ·
[8 Fan-out on the server](#8-fan-out-on-the-server) · [9 The client cache](#9-the-client-cache) ·
[10 Documents](#10-documents) · [11 Agents through the server](#11-agents-through-the-server) ·
[12 Security](#12-security) · [13 Tests](#13-tests) · [14 Delivery](#14-delivery) ·
[15 Out of scope](#15-out-of-scope)

## 1. Intent

**Today**, LIMS's sample-entry screen declares, query by query, which actions refresh it, and each mutation
declares which queries it invalidates (`examples/lims/screens/lims_screens.cpp`):

```cpp
auto const header = screen.query<GetHeader>("header", sample, {},
    {.when = ready, .refreshOn = sample.actions<HeaderSection, SelectProject, SaveSample>()});
auto const load = screen.mutation<LoadSample>("load", sample,
    {.body = {{&LoadSample::id, sampleId}}, .invalidates = {header, series, measure}, ...});
```

Every document that calls an action and every document that shows what it changes has to know about the
other. A change made by another client is seen only on that client's next poll.

**With this design**, each action's effects are declared once, beside its registration:

```cpp
BRIDGE_REGISTER_EFFECTS(CaptureConcentration, morph::effects::changes<ResultView>)
BRIDGE_REGISTER_EFFECTS(LoadSample,           morph::effects::changesInstance)
```

The documents drop `refreshOn`, `invalidates` and the polling `refreshEvery`. When a capture succeeds:

- the calling client marks every cached query holding `ResultView` stale, and the mounted ones refetch;
- the server tells every other client watching `ResultView`, and theirs do the same.

When `LoadSample` succeeds, only the caller's queries through that same private instance refetch, because no
other client can see that working copy.

**Success criteria.**

1. A document that only shows data and sends edits updates correctly with no refresh declarations.
2. A change made by client A appears in client B's mounted screen within one push latency, with no polling.
3. An agent calling an action through the server updates every open GUI the same way.
4. The examples lose their refresh declarations, and their unit and GUI tests stay green.

## 2. What exists today

| Fact | Where |
|---|---|
| The query cache is per client session, keyed by `(model type, instance, action, canonical body)`. `invalidates`, `refreshOn` and `{"refetch"}` mark entries stale. | spec 5 §5 "The query cache"; `include/morph/ui/interpreter.hpp` (the cache key) |
| A successful fetch clears an entry's `stale` mark unconditionally, and a refetch is skipped while a fetch is pending. | `include/morph/ui/interpreter.hpp` (`settleReply`, `refetchCurrent`) |
| `refreshOn` matches by **model type, not instance**, because two private instances of a database-backed model read the same rows. | spec 5 §5 "Queries" |
| An alias is `"private"`, keyless `"shared"` (a stateless model), or keyed by an instance expression. | spec 5 §5 "Models" |
| `patch` merges a reply into a cached row found by its `id` field (`rowId`). `optimistic` applies the same shape before the reply. `latest` applies only the newest reply. | spec 5 §5 "Mutations"; `include/morph/ui/interpreter.hpp` |
| Every success the client sees, from a mutation or from a form, collection or wizard submit, is reported through `session.actionSucceeded(typeId, action)`, and that call drives `refreshOn`. | `include/morph/ui/interpreter.hpp`, `ScreenCore::aliasSubmits` |
| A binding holds the server's id for the instance it is bound to (`currentId`). | `docs/spec/core/bridge.md` |
| A change made by another client is not pushed. Spec 5 §15 names a `ui-event` kind negotiated by `ui-hello` as the way to add it. | spec 5 §5, §15 |
| A bridge subscription reaches only handlers in the same process, never across connections. | `docs/spec/core/shared_instances.md` |
| `RemoteServer::handle(msg, reply, cid)` answers each request through its own `reply`, and `openConnection()` returns a scope id that owns the connection's registered models. The server holds no way to send a connection a message it did not ask for. | `include/morph/core/remote.hpp` |
| An `IEnvelopeService` (the UI service) handles the kinds core does not know, through `handle(request, authorizer)`, which receives no connection id. | `include/morph/core/remote.hpp` |
| `execute` replies are written from the model's strand, and a timed-out `execute` is answered `err "timeout"` while the model may still finish. | `include/morph/core/remote.hpp` |
| The server stamps the verified principal onto **each envelope**. A connection has no principal of its own, and `IAuthorizer::authenticate` returns only the principal, with no expiry. | `include/morph/core/remote.hpp`, `include/morph/session/session.hpp` |
| Both transports track a `ConnectionId` per connection and call `closeConnection` on disconnect. Neither caps its outbound queue, and the `net` writer closes a peer that stalls past `sendTimeout`. | `include/morph/net/socket_server.hpp`, `include/morph/net/detail/ws_connection.hpp`, `src/qt/qt_websocket_server.cpp`, `docs/spec/security.md` |
| The client matches replies by `callId`. A message that answers no pending call is dropped. | `include/morph/core/wire.hpp` |
| The core `hello` request carries only `protocolVersion`. Its reply carries capabilities. The generic UI client opens with `ui-hello`, not `hello`. | `include/morph/core/wire.hpp`, `include/morph/core/remote.hpp`, `include/morph/ui/service_client.hpp` |
| `ActionInfo` carries `modelType`, `calculatedFields`, `secretFields`, `credential` and `listFields`. Nothing says whether an action reads or writes. | `include/morph/ui/document.hpp` |
| `BRIDGE_REGISTER_ACTION` is a full specialisation of `ActionTraits<A>`, chosen by argument count, with `Loggable` as its fourth argument. Per-action facts added later use their own trait or registry: `BRIDGE_REGISTER_VALIDATOR` specialises `ActionValidator<A>`. | `include/morph/core/registry.hpp` |
| A key may be raw (integral or `std::string`) or a strong id wrapping one (`WrappedModelKey`). | `include/morph/core/model_key.hpp` |
| Offline replay runs as reconnect → activate → `bindContext` → `replay`. | `docs/spec/offline/offline.md`, `include/morph/offline/reconnect_coordinator.hpp` |
| Within a vocabulary version, changes are additive. | spec 5 §12 "Versions" |
| The registration latch closes on the first read of a process-level registry only in a debug build. A release build closes it only when the application calls `closeRegistrationPhase()`, which is `noexcept`. | `include/morph/core/registry.hpp` |
| The app explorer shows, for one action, its catalog facts (`explorer_facts`): whether it sends a credential, its calculated, secret and list fields, and its payload fingerprint. | `include/morph/ui/explorer.hpp`, `docs/spec/ui/explorer.md` |
| The ladder has three rungs: bookmarks, bank and lims (`examples/rungs.txt`). Outside their tests they hold 9 `invalidates` (bookmarks 2, bank 5, lims 2), 5 `refreshOn` and 1 `patch` (lims), and no `refreshEvery`. Bank registers its actions in two model sets, `examples/bank/include` and `examples/bank/memory/include`. | `grep` over `examples/`, at `e70025cc` |

## 3. Concepts

- **Record type.** A reflected aggregate that action results carry and screens show, with a registered name and
  optionally a key member: `ResultView` keyed by its `ResultId`, `AccountInfo` keyed by `id`. A record type shown
  through several models is one record type: `CustomerModel::ListAccounts` and `AccountModel::GetAccount` both
  return `AccountInfo`.
- **Derived record type.** A record computed from others: bank's `SpendingReport` from transactions (`TxnInfo`). It
  is keyless, and it declares what it is derived from (§4), so a change to its sources reaches it without every
  writer naming it.
- **Effect.** What a successful action did. One of:
  - `updates<R>`: the result carries the new state of the `R` records it changed, identified by their keys.
  - `changes<R>`: records of `R` were created, removed or changed in a way the result does not carry.
  - `changesInstance`: the action changed the state of the model instance it ran on, such as a working copy that
    only that instance's queries read.
  - `none`: the action changes nothing that is shown. This is stated explicitly, for an action that attaches or
    opens and would otherwise draw a warning (§10).
  - nothing declared: the action reads.
- **Change.** What the framework derives from an effect after a successful execute: a record topic
  `{record, keys?}`, or an instance topic `{model type, instance id}`.
- **Watch.** A client's statement of which topics its cached queries hold, authorised by the server.
- **Event.** A server-to-client message carrying changes, sent to the connections whose topics they match.

The record type is the unit for shared data. It cannot be the model instance, for the reason `refreshOn` already
matches by model type: two private instances of one database-backed model read the same rows. The instance is the
unit only for state that lives in the instance, which is what `changesInstance` names.

## 4. Declaring records and effects

Both are runtime registries, filled by static registrars during the registration phase and checked by its latch.
Neither specialises a trait that `BRIDGE_REGISTER_ACTION` has already used.
- The action's `ActionTraits<A>` is unchanged. Its inline members are already defined when the action is
  registered, so a later specialisation that they read would be ill-formed.
- In a header split between `BRIDGE_DECLARE_ACTION` and `BRIDGE_REGISTER_ACTION_SOURCE`, one translation unit
  would see a different trait than another.

Records and effects are matched by **runtime type identity** (`std::type_index`) when the registration phase
closes. The order of `BRIDGE_REGISTER_RECORD`, `BRIDGE_REGISTER_ACTION` and `BRIDGE_REGISTER_EFFECTS` across
translation units therefore does not matter.

**When matching runs.** The latch cannot run it: a release build closes the latch only when the application asks,
and `closeRegistrationPhase()` is `noexcept`. Matching therefore runs once (`std::call_once`), on the first read of
the record or effects registry, which also closes the latch as any registry read does. `RemoteServer`'s constructor
and the catalog facts (`ui::CatalogFacts::action`, which local mode and the server's `ui-catalog` both build from)
each make that read, so a server or a local client fails at startup. A
failure throws `morph::model::EffectsRegistrationError` (a `std::logic_error`) naming every problem found, not only
the first, and every later read throws the same error. "Refused when the phase closes" below means this.

### Records

```cpp
BRIDGE_REGISTER_RECORD(bank::dto::AccountInfo, "Bank_Account")                // key: member `id`
BRIDGE_REGISTER_RECORD(lims::ResultView, "Lims_Result")                        // key: `id`, a ResultId
BRIDGE_REGISTER_RECORD(bank::dto::TxnInfo, "Bank_Transaction")                 // key: member `id`
BRIDGE_REGISTER_RECORD(bank::dto::SpendingReport, "Bank_SpendingReport",
                       morph::record::keyless, morph::record::derivedFrom<bank::dto::TxnInfo>)
```

- **The key** defaults to a member named `id`, the field the interpreter's `patch` already identifies rows by. It
  may be any `ModelKey`, raw or wrapped, and on the wire it is the raw scalar, as `ModelKey` already encodes it.
  `morph::record::key<&T::m>` names another member, and `morph::record::keyless` declares none.
- **`derivedFrom<S...>`** says that a change to any `S` is a change to this record type. Fan-out expands it when it
  derives a change (§8). A derived type is keyless, because a key-level change to a source does not say which
  derived rows moved.
- **`morph::record::privateKeys`** marks a type whose keys must not reach other principals (§12).
- `derivedFrom` cycles, a `derivedFrom` source that is not a registered record, and conflicting registrations (one
  type under two names, one name on two types, or one type with different options) are refused when the registration
  phase closes. An identical registration repeated, as a
  record registered in a header gives once per translation unit, is one registration.

### Effects

```cpp
BRIDGE_REGISTER_ACTION(TransactionModel, Deposit,  "Deposit")
BRIDGE_REGISTER_EFFECTS(Deposit,  morph::effects::changes<TxnInfo>, morph::effects::changes<AccountInfo>)
BRIDGE_REGISTER_EFFECTS(Transfer, morph::effects::changes<TxnInfo>, morph::effects::changes<AccountInfo>)
BRIDGE_REGISTER_EFFECTS(AddPayee, morph::effects::changes<PayeeInfo>)
BRIDGE_REGISTER_EFFECTS(RemovePayee, morph::effects::changes<PayeeInfo>)
// An action that edits one row and returns it, such as a hypothetical EditPayee returning PayeeInfo:
// BRIDGE_REGISTER_EFFECTS(EditPayee, morph::effects::updates<PayeeInfo>)
BRIDGE_REGISTER_EFFECTS(LoadSample, morph::effects::changesInstance)
BRIDGE_REGISTER_EFFECTS(OpenSample, morph::effects::none)
// ListPayees, GetAccount: nothing declared, they read.
```

- **`BRIDGE_REGISTER_EFFECTS(A, effects...)`** expands to a static registrar that records, under
  `ActionTraits<A>::typeId()`, the effects as data (`{kind, record type?}`).
  - It must appear where `ActionTraits<A>` is visible. Its place is beside the action's registration: after
    `BRIDGE_REGISTER_ACTION`, or after `BRIDGE_REGISTER_ACTION_SOURCE` in the `.cpp`, for an action split with
    `BRIDGE_DECLARE_ACTION`. In a header it would run once per translation unit, which is the static-initialisation
    cost `BRIDGE_DECLARE_ACTION` exists to avoid.
  - It is idempotent per action, like `registerActionOnce`: registering the same effects twice is a no-op. Registering
    different effects for one action is refused when the phase closes.
  - It defines no specialisation, so a translation unit that does not see it is not inconsistent with one that does.
- **Client-only builds** (`MORPH_CLIENT_ONLY`) compile the registrars out, as they do every action registrar. A
  pure remote client takes effects and records from `ui-catalog` (§5), as it takes every other action fact.
- **`updates<R>` finds the records in the result.** The result may be an `R`, a `std::vector<R>`, or an aggregate
  whose members, found by reflection, hold an `R` or a `std::vector<R>`. `R` must have a key. If no `R` is found,
  or `R` has no key, startup fails when the registration phase closes, not when the action first runs. Bank's
  `Transfer` returns two balances (`TransferResult{fromBalanceMinor, toBalanceMinor}`), not two `AccountInfo`s, so
  it declares `changes<AccountInfo>`.
- **`updates<R>` only changes rows that exist.** A key that no cached entry holds is not added to any list, because
  whether it belongs there is the server's query to answer. An action that may create a record declares
  `changes<R>`.
- **`changes<R>` needs nothing from the result.** It covers creation, removal, and changes the result does not
  carry.
- **`changesInstance`** means the state of the instance the action ran on changed:
  - the caller's entries through that instance refetch (§9);
  - for a private instance, the event also goes to the connection that owns it (§8);
  - for a keyed shared instance, it also goes to the other clients watching that instance (§7).
- **Several effects compose,** as above.
- **Nothing declared means the action reads, and nothing is pushed.** A query may only run such an action (§10). An
  action that writes and declares nothing behaves as it does today, and §10's warnings point at it.
- **Only success has effects.** A failed action or a cancelled one changes nothing.

### What each action's result holds

Each action's existing registration (`BRIDGE_DETAIL_ACTION_REGISTRARS`, which every form of the macro runs) also
records a reflection walker over `ActionTraits<A>::Result`. That is the unwrapped type, so a `Task<R>` handler
contributes `R`. The walker needs no record to be registered yet. When the registration phase closes, the walker's
type identities are matched against the record registry, giving `records(A)`. For each record, it gives the
**paths** at which the result holds it and whether it is keyed. A cached query holds exactly the records of its
action. `ListResults` returns `ListResultsResult{results: vector<ResultView>}`, so its entries hold `Lims_Result` at
path `results`.

- **What the walker visits.**
  - The result itself; a `std::vector`, `std::optional` or `std::map` value of a visited type.
  - The members of a reflectable aggregate. Their JSON names are taken from `glz::reflect`'s keys, which are the
    names the codec writes.
  - It recurses into members, depth-first, so a record inside a record (a row list inside a header) has a path such
    as `header.rows`. It stops at a registered record, so a record's own members are not searched for further
    records. It stops at a type already on its path, so recursive types end.
- **What it skips:** `void`, scalars, strings, and any type it cannot reflect: a type whose `glz::meta` replaces its
  codec (`custom_read`/`custom_write`, or a `value` mapping), or an anonymous-namespace struct. A `glz::meta` that only
  renames members is reflected, and its renamed keys are the paths. A skipped type contributes no records. An action whose result
  is skipped holds none. `updates<R>` on such an action fails when the phase closes, because `R` is not found.
- **The registrars are compiled out of client-only builds.** `MORPH_CLIENT_ONLY` empties every registrar, these
  included. That is consistent with a pure remote client taking these facts from `ui-catalog`.

## 5. The catalog

`ActionInfo` gains two facts. Local mode fills them from the registry; remotely, `ui-catalog` serves them, like the
existing ones.

| Field | Meaning |
|---|---|
| `effects` | `[{kind: "updates" \| "changes" \| "changesInstance" \| "none", record?}]`. |
| `records` | `[{record, keyField?, paths: [...]}]`: the record types the action's result holds, the key member's wire name when keyed, and the JSON paths at which the result holds them (`""` for the result itself, `"results"` for a member list). |

From `records` the client can decide, without asking the server, whether a cache entry holds record `R`, where its
rows are, and which field identifies them. The patch therefore matches rows by the record's key field, not only by
`id`. `derivedFrom` is never sent. The server expands it before a change leaves (§8), so a client only ever matches
the record types its entries hold.

An `evaluate` cache entry (`ui-evaluate`, spec 5 §7) holds no records and is never watched: an evaluation depends on
its body, not on stored data.

**The app explorer shows both.** Its Facts section (`explorer_facts`) gains two rows from the same `ActionInfo`:
"effects", listing each effect as `changes Bank_Account`, `updates Lims_Result`, `changes its instance` or `none`,
and "reads" when nothing is declared; and "records", listing each record with its key field and paths, such as
`Lims_Result by id at results`. A person deciding why a screen did or did not update reads the answer there, without
the source.

## 6. The server-to-client channel

The wire gains its first server-initiated message.

- **Negotiation in `ui-hello`.** `HelloRequest` gains `events: true` and `HelloReply` gains `events: bool`. Both are
  additive optional keys. The generic client already opens with `ui-hello`, on the same connection as its executes,
  and spec 5 §15 names it as the place to negotiate this. A server sends events only on a connection whose
  `ui-hello` offered them and that it accepted. A client that never offers sees today's protocol unchanged.
  Negotiation is per connection, so a reconnect negotiates again.
- **How core learns the result.** `ui-hello` is a UI service kind, and `IEnvelopeService::handle` receives no
  connection id. It gains a third parameter, `ConnectionFacts&`, with a default overload forwarding to today's
  two-parameter form. `RemoteServer` passes the facts of the envelope's `cid`. The UI service sets
  `facts.events = true` when it accepts the offer. Core reads the flag at fan-out. Core never parses a `ui-*` body.
  - A message handled without a connection (`handle(msg, reply)`, `cid` 0) gets throwaway facts with
    `events = false`.
  - `IEnvelopeService` keeps its two-parameter `handle` pure. The three-parameter one is a non-pure virtual whose
    default calls it. A service that overrides only one of the two
    overloads adds `using IEnvelopeService::handle;`, as the `-Woverloaded-virtual` build requires; one that
    overrides both needs none.
- **The envelope.** `kind: "event"`, `callId: 0`, `body: {topic, payload}`. For this design the topic is
  `changes`. A client routes it by kind before reply matching, so an event is never taken for an orphaned reply.
- **Writing to a connection.** `RemoteServer` gains `setConnectionWriter(cid, writer)`, which takes the transport's
  `ConnectionWriter{write, backlog}` for a connection already opened with `openConnection()`, `post(cid, envelope)`,
  and `connectionDrained(cid)`.
  - `post` is callable from any thread. It hands the encoded envelope to the writer, which queues it on the
    transport's own thread, as replies are queued today.
  - The size cap (`kMaxEnvelopeBytes`) applies.
  - A write failure drops the connection's watches, as a disconnect does.
- **The outbound bound.** The server cannot see what a transport still holds unsent: replies and events are handed
  to the transport's outbox and forgotten. The design bounds what it can see, and names the transport's rule for the
  rest.
  - **Each transport counts its backlog.** Writes are asynchronous: the `net` writer posts the frame to its loop,
    and Qt queues it to the socket's thread. Neither queue can be read from the server strand. So each transport
    keeps a per-connection atomic byte counter:
    - incremented when its writer accepts a frame, a reply or an event;
    - decremented on the loop or socket thread once the frame is written (the `net` `outbox` emptying, Qt's
      `bytesWritten`).

    `setConnectionWriter(cid, writer)` hands the server the writer and a read of that counter, as
    `ConnectionWriter{write, backlog}`. When the counter falls below 256 KiB, the transport calls
    `RemoteServer::connectionDrained(cid)`; the server holds no callback into the transport.
    - The counter counts the bytes of the encoded envelopes handed to the writer, and is decremented by the same
      amount when that frame has been written. It never uses a transport's framed byte counts, so it cannot drift.
    - `connectionDrained` is called on the transport's thread. It only posts to the server strand, where the owed
      `resync` is sent and the flag cleared.
  - **Events, not replies, give way.** When `backlog()` exceeds 1 MiB, the server stops posting events to that
    connection and records that it owes it a `resync`. When the transport calls `connectionDrained`, the server sends
    one `resync` event, meaning "everything you watch may have changed", and the client marks every watched entry
    stale.
    The drain call, not a later write, triggers it, so an idle client that only watches still receives it.
    Replies are never held back.
  - **A peer that stops reading** is closed by the transport's own rule: the `net` writer closes a peer that stalls
    past `sendTimeout`, as it does today for replies. The Qt transport gains the same timeout in part 2, so both
    behave alike.
- **The client side.** `IBackend` gains `setEventHandler(fn)`, delivered on the bridge's executor:
  - the Qt WebSocket backend, the `net` socket backend and `SimulatedRemoteBackend`, which the core transport tests
    use, call it for each `event`;
  - the local backend has no server and produces no events in production. Local mode is covered by the effects
    every reply applies (§9). The local backend exposes `deliverEvent(envelope)` as a test seam, so the client
    cache's event path is tested without a server.
- **Order.** Events on one connection arrive in the order the server posted them. No order is promised between an
  event and any reply.

## 7. Watching

A client watches the topics its cache holds, so the server sends each connection only what it can show.

- **Topics.**
  - A record type, for every record its entries hold.
  - `(model type, primary key)` for every entry read through a **keyed shared** instance whose action holds no
    records, which is the case for working-copy reads. The primary key is the one the alias binds with: the
    interpreter already binds a keyed alias as `bind(type, Shared, key)` from its token, so the key is known before
    the first fetch. The server resolves it through its instance directory (`DirectoryKey{typeId, primary}`). The
    binding's `currentId` cannot serve here, because it is 0 until that first fetch has bound the alias.
  - **An instance watch is stored by key, not resolved.** The watch is sent before the batch's fetches, so the
    instance may not exist on the server yet: no client has attached it. The server therefore stores the topic as
    `(modelType, primary)` and never resolves it to an instance id. At fan-out, it matches the executing instance's
    directory key (`Instance::sharedKey`), which a keyed instance holds from the moment it is keyed: at construction,
    or when an `assign` promotes it. An instance that never comes to exist simply
    never matches.
  - A private instance needs no watch: its owner is known to the server (§8). A keyless `"shared"` alias names a
    stateless model, which has no instance state to change.
- **The kinds.** The `watch` and `unwatch` wire kinds (unrelated to a document's `watch` declarations) are **core**
  kinds, handled by `RemoteServer` on its strand with the
  connection's `cid`, because the watch table belongs to the connection and fan-out reads it in the execute path.
  - Core can validate them: after part 1, records are a core registry, and instance topics are checked with
    `authorizeInstance`.
  - The body is `{records: [...], instances: [{modelType, primary}], replace?}`. Without `replace` it adds topics.
    `unwatch` removes them. The `ok` reply lists any topic refused
    (§12).
  - The UI layer sends them; it does not handle them.
- **Lifetime.** A topic is watched while at least one cache entry holds it, mounted or kept (spec 5 §5: for
  5 minutes). The client counts references per topic. It sends `watch` when a count leaves zero and `unwatch` when it
  returns to zero.
  The server may also drop a watch on its own (§12): it then sends a `resync`, and the client answers every `resync`
  by sending its whole watch set again with `replace: true`, as it does after a reconnect.
- **Watch before fetch.** Watch changes made in one reactive batch are sent as one message, **before** the fetches
  of that batch, on the same connection. The server handles `watch` on its strand when the message arrives, and an
  `execute` is admitted on that same strand before it is posted to the model strand. The watch is therefore in the
  table before the fetch reads anything:
  - a write that commits before the fetch reads is in the fetched value;
  - one that commits after produces an event, which §9 keeps even if the fetch is still in flight.
- **Reconnect.** Both transports reconnect on their own, so the UI layer cannot rely on being told by an offline
  coordinator.
  - **Who is told: library seams, not a particular source.** In the library, `AppSource` (`ui/frontend.hpp`) is an
    interface. The concrete remote source that builds the backend lives in the generic client
    (`examples/common/app`), as does the shell that builds the `Session`. So the library names the seams, and a
    source and a shell only connect them:
    - **`AppSource::onConnected(listener)`**, a new virtual with an empty default for local sources. The source calls
      every listener on the owner after each successful connect, the first included. A remote source implements it
      by installing the backend's `setConnectHandler`, which runs on the transport's thread, and posting to the
      owner.
    - **The source owns the backend's single `setConnectHandler` slot.** Anything else that wants connects, such as a
      connection-status indicator, subscribes through `onConnected`, so the slot is never taken twice.
    - **`Session::reconnected()`**, new. The shell that builds the `Session` registers it with
      `source.onConnected(...)`. That is one line in whichever shell builds the session; in this repository, the
      generic client's shell.
    - **The `watch` verb.** `Session` owns the cache, so it does the topic reference counting, and it needs its own
      route to the server: `SessionOptions` gains a `watcher`, a functor `(body) -> Completion<std::string>`. The
      remote source builds it with a new `watcherOver(exchange, currentSession, cbExec)`, as it builds the evaluator,
      because the source holds the exchange. The shell sets `sessionOptions.watcher = source.watcher()`. The watcher
      takes `currentSession`, a function returning the source's session **now**, rather than a session fixed when it
      is built.
      - When `SessionOptions::watcher` is unset, as for a local source with no `RemoteServer`, the session still
        counts topics and sends nothing. Local mode is covered by the effects every reply applies (§9). A `watch` is sent with whatever session the user is signed in with at that moment.
      Requests through the exchange are not stamped with the backend's session, so an unsigned `watch` would be
      refused (§12). `Dispatcher` is unchanged, so its test doubles need nothing.
    - The Qt WebSocket backend already fires `setConnectHandler`. The `net` `SocketBackend` does not override it
      today; part 2 adds the override, fired from `onConnected()` on its loop, the first connect included.
  - **What happens on each connect,** on the owner:
    1. on a **reconnect** (not the first connect, whose `ui-hello` `open()` has already filed), the remote source's
       own listener sends `ui-hello` again, so the new connection negotiates `events`, then calls the other
       listeners, `Session::reconnected()` among them. It sends only the `ui-hello` handshake, not a bundle reload:
       the documents did not change because the connection dropped;
    2. `reconnected()` sends the whole watch set;
    3. `reconnected()` marks every entry stale, as spec 6 §9 already does for a backend switch. Entries with a fetch in flight get
       `refetchAfter`.
  - **What it does not promise is order, and why that is safe.** What the transports do today:
    - An `execute` made while disconnected is rejected at once with `DisconnectedError`, and one in flight at the
      drop is cancelled. No fetch is queued across an outage.
    - `request()` envelopes made while disconnected (the `ui-*` kinds, and `watch`/`unwatch` changes) are held, and
      replayed when the connection opens, before the posted handler can run.
    - Executes the owner files **between the connect and `reconnected()` running** (a user's mutation, a
      `refreshEvery` tick, a timed replay of queued mutations, calls released by the bridge's rebinds) reach the server before the new `ui-hello` and
      watch set. A fetch among them may read before a change commits that produces no event for this client.

    Correctness rests on step 3. Every entry is stale, and an entry whose fetch is in flight gets `refetchAfter`.
    Every refetch that follows is filed after the watch set, on the same ordered channel: `request()` and `execute()`
    share one queue per connection on both transports. A change committed before such a refetch reads is in its
    value, and one committed after produces an event. Nothing is missed for good.
  - **The full watch set replaces.** The `watch` sent in step 2 carries `replace: true`: the connection's watches
    become exactly that set. Watch changes held during the outage and replayed ahead of it are therefore superseded.
    The server accepts `watch` before `ui-hello` on a connection; it stores the watches but posts events only once
    `events` is negotiated.
  - An offline replay through `ReconnectCoordinator` runs after this, and its changes reach the cache as events (§8).
    Events missed while disconnected are not replayed; the stale marks cover them.
- **Granularity.** Record topics are per type, not per key. Per-key watching would make the server track every key
  every client shows, and those keys change as lists scroll. The event carries keys, and the client filters on them
  (§9).

## 8. Fan-out on the server

Fan-out sits in `RemoteServer`'s execute path (`dispatchExecute`), which every executing message shares: the
`execute` kind, offline replays (which arrive as executes), and the agent adapter (§11). The UI service has no
execute path of its own (`ui-evaluate` never executes), so it needs nothing.

After an `execute` succeeds on the model's strand:

1. **The model strand derives the changes.**
   - Record topics come from the effects, with keys read from the result for `updates`, and are expanded through
     `derivedFrom`.
   - For `changesInstance`, the topic is the instance the action ran on.

   It posts the changes to the server strand. The reply is written as it is today, unaffected.
2. **The server strand finds the recipients.**
   - Record and keyed-shared-instance topics go to every watching connection, **the calling connection included**.
   - A private instance's topic goes only to the connection whose scope owns it.

   Including the caller covers the cases in which it never sees its own reply: an execute that timed out on the
   client but succeeded on the server, a reply that `latest` discarded, and a replay whose reply went to the offline
   worker.
3. **Each recipient watch is re-authorised** with the session context stored on it (§12), and the connection's
   coalescer queues the change.

- **Coalescing.** Changes for one connection are merged for up to 50 ms, or until 256 keys are queued, then sent as
  one event.
  - Keys are merged per record type, and a `changes` absorbs that type's `updates` keys.
  - Instance topics are deduplicated.
  - A burst of one hundred writes is then one refetch per showing query, not one hundred.
- **The watch table** lives on the server strand, next to the connection scopes `closeConnection` already reclaims.
  A closed connection drops its watches.

## 9. The client cache

The interpreter's query cache applies a change the same way whether it came from a reply or from an event.

| Change | From a reply (the caller's own) | From an event |
|---|---|---|
| `updates R` with keys | Patch: each entry holding `R` merges the new records into the rows with those keys, at the paths and by the key field the catalog gives (§5). It uses the existing `patch` merge, and the catalog's `listFields` for a list inside a record. No refetch. | Each entry holding `R` whose value contains one of the keys is marked stale, and refetched if mounted. |
| `changes R` | Each entry holding `R` is marked stale, and refetched if mounted. | The same. |
| `changesInstance` | Each entry read through the same instance is marked stale, and refetched if mounted. | The same, for the instance named: the private one this connection owns, or a keyed shared one it watches. |
| `resync` | none | Every watched entry is marked stale, and refetched if mounted. The whole watch set is sent again with `replace: true` (§12). |

- **A change during a fetch is kept.** A stale mark on an entry whose fetch is pending sets `refetchAfter`. When the
  fetch settles, the entry stays stale and fetches once more, even if it succeeded. Otherwise the success would clear
  the mark, and a change committed after the fetch read would be lost. This replaces today's unconditional
  `stale = false` on settle, and it is what makes the watch-before-fetch argument in §7 hold.
- **A patch on a fetching entry also refetches.** A reply's `updates` patch that lands on an entry whose fetch is
  pending is applied and also sets `refetchAfter`. Otherwise that fetch, which may have read before the write
  committed, would settle over the patch. Remotely the caller's duplicate event would repair this too, but local
  mode has no events, so the patch path must not rely on one.
- **Every reply applies its effects.** The single hook is `session.actionSucceeded`, which every success already
  goes through: mutations, and form, collection and wizard submits (`aliasSubmits`). It gains the reply's JSON, so
  `updates` can patch.
  - **A `latest` run whose reply is discarded** applies its effects as marks only: each `updates` is treated as
    `changes`. Its values are older than the reply a newer run already applied, so it must never patch.
  - **An offline replay's reply** reaches the offline worker, not a mutation. Its effects reach the cache as an event,
    because the caller is not excluded (§8).
- **Duplicates cost at most one extra fetch.** The caller sees each change twice: once from its reply and once as an
  event.
  - On an idle entry, the second mark lands on an entry already stale or already fetching. While fetching, it sets
    `refetchAfter` (above) and costs one extra fetch.
  - That fetch is the price of never losing a change. Telling a duplicate from a genuine second change would need the
    event to name the call it came from, and §15 defers that.
- **Why other clients refetch rather than receive values.** The new values were computed for the caller. Another
  user may see different fields (`secretFields`) or may not be allowed the record at all. A refetch goes through the
  receiver's own authorised read. Pushing values to other receivers is the first item in §15.
- **Batching.** One event, or one reply's effects, is applied in one reactive batch, so a screen updates in one
  frame.
- **`optimistic` is unchanged.** It still shows a guess before the reply, and the reply's patch confirms or corrects
  it, exactly as a declared `patch` does today.
- **Entries no query mounts** are only marked stale. They refetch if a screen mounts on them.

## 10. Documents

With effects declared, a document needs no refresh declarations.

**At load, in `ui/1`, additively.** The loader already has the catalog, and it reports:

- **refused: a query whose action declares effects** (anything other than nothing declared). A query must read, so a
  fetch never produces a change and two clients cannot refetch each other through their queries. It is new in what
  it refuses, so it applies only to actions that declare effects, which no existing action does.
- **warning, watch feedback:** a `watch` declaration (spec 5 §6, not the `watch` wire kind of §7) whose `on` reads a query's value, whose `source` is `any` (the
  default), and whose `do` runs a mutation with effects. Another client's change refetches the query, the watch
  fires, and the mutation's change reaches the other client, which may do the same. Such a watch should use
  `"source": "user"`. The loader cannot prove a ring, so this is a warning, and the claim above is limited to
  queries.
- **warning, redundant:** an `invalidates`, `patch` or `refreshOn` that the mutation's action's effects already
  cover, so a document can migrate one screen at a time.
- **warning, uncovered:** a mutation whose action declares nothing (not even `none`), with no `invalidates` or
  `patch` of its own and no query naming it in `refreshOn`. After it succeeds, nothing on any screen updates.
  `effects::none` silences it for actions that attach or open, such as lims's `OpenSample`.

**In `ui/2`, the removal.** `invalidates`, `patch` and `refreshOn` are removed from mutations and queries. Removing
keys is not additive, so it is a new vocabulary version under spec 5 §12. A server that keeps `ui/1` documents for
older clients keeps them working.

- `optimistic` stays, and keeps the `{query, key, with}` shape it has today, now as its own definition rather than
  `patch`'s.
- A document that must refetch for a reason no action declares uses the existing `{"refetch": "q"}` command.
- `refreshEvery` stays, for data that changes with time rather than through actions, such as a clock or an external
  feed.

**The C++ authoring layer** (`include/morph/ui/author.hpp`) gains nothing for effects, because they are declared
beside the action, not in a document. It drops `.invalidates`, `.patch` and `.refreshOn` when it emits `ui/2`, and
its golden files (`tests/ui/golden/`) change with it.

## 11. Agents through the server

An agent acts on the application through the same dispatch as everything else, so this design is what makes its
changes visible.

- **The MCP adapter** is a server-side component.
  - It opens a connection scope with `openConnection()` for each agent session, and sends each tool call through
    `RemoteServer::handle(msg, reply, cid)` as an ordinary `execute`.
  - That keeps every check the GUI gets: authentication and the principal, `authorize` and `authorizeInstance`,
    limits, the journal, cancellation and the in-flight count.
  - It lists the actions the principal may run (the catalog's `permitted`, with their JSON shapes) as MCP tools, and
    offers `ui-evaluate` as a dry run.
  - The agent may commit directly.
- **The adapter's shape.**
  - `morph::ui::McpAdapter` (`include/morph/ui/agent.hpp`) speaks MCP's JSON-RPC 2.0 and knows no transport:
    `handle(message)` takes one JSON-RPC message and completes with its reply, or with nothing for a notification.
    It answers `initialize`, `tools/list` and `tools/call`, and refuses every other method with JSON-RPC error
    `-32601`.
  - `serveStdio(adapter, in, out)` drives it over newline-delimited streams, which is MCP's stdio transport. An
    agent such as Claude Code launches the server process with an option that turns it on (bank's server:
    `--mcp-stdio`), and the process serves its WebSocket clients as before. An HTTP transport is out of scope (§15).
  - **Identity.** The deployer constructs the adapter with the `session::Context` its envelopes carry, for example
    from a token in the environment. The server authenticates it on every envelope, as it does a GUI's.
  - **Tools.** One tool per permitted action, named by the action id, whose `inputSchema` is the action's schema
    (`schemas`). A keyed model's tool takes the instance key as the argument `instance`, and the adapter attaches
    the shared instance by it. Any other model runs on one private instance per model type, registered on the
    adapter's connection at first use. A tool named `evaluate`, taking `{action, body}`, is the dry run.
  - A tool call's reply is the action's result JSON as text content, or its `err` message with `isError: true`.
- **Fan-out treats the agent's connection like any other,** so every open GUI showing the changed records updates
  through §8 and §9.
- **Model access and keys are not part of this design.** The adapter does not call a model provider. An external
  agent (Claude Desktop, Claude Code) brings its own. Where keys would live for an assistant built into the client
  is still undecided, and it does not affect anything above.

## 12. Security

- **Each watch carries a session context.** A connection has no principal of its own: the server verifies one on
  every envelope. The watch table therefore stores, with each watch, the session context (principal and token) of the
  `watch` envelope that created it, and the verified principal.
  - **A later envelope on the connection with a different verified principal drops all of the connection's
    watches.** The comparison is made in `stampVerifiedPrincipal`, given the envelope's `cid`. Every kind that
    authorises passes through it; `hello`, which carries no session, does not.
  - **The baseline** is the verified principal of the first stamped envelope after the connection opens. An envelope
    whose token fails `authenticate` is stamped with no principal, so it counts as different: the watches drop, and
    the connection is sent one `resync` (below). That covers signing in as someone else, and a client that keeps old
    watches across a sign-out. The client's cache is already cleared on a principal change (spec 5 §5).
  - **Fan-out re-authorises each watch at send time, by the rule that admitted it.**
    - It runs `authenticate` on the stored context, which fails for an expired token.
    - For a record topic, it then requires again that the principal may run some action whose `records` hold the
      type, the same rule as below. It does not check the action that wrote the change: a role that may list
      results but not capture them still receives the change.
    - For an instance topic, it re-runs the admission check for instance topics below.

    A failure drops the watch, and the connection is sent one `resync` (below). The "may run some action holding
    `R`" answer is cached per principal and token, so a policy change reaches an existing watch when its stored
    token stops verifying or the topic is watched again.
  - **A watch the server drops is answered with a `resync`.** Whenever the server drops any of a connection's
    watches other than by that connection's own `unwatch` or `replace` (a principal change, or a re-authorisation
    failure at fan-out), it sends that connection one `resync` event: one per envelope or fan-out, however many
    watches went, only when `events` is negotiated, and through the backlog hold-back (§6), so a connection held
    back gets the one `resync` its drain owes. The client answers **every** `resync` by marking its watched entries
    stale and sending its whole watch set again with `replace: true`; a topic still refused comes back in the
    `WatchReply`. A close and a write failure send nothing: there is no connection left to tell.
- **What a watch may name.**
  - A record type: only when the principal may run at least one action whose `records` contain it. Otherwise the
    `ok` reply lists it as refused, and the client logs it.
  - An instance topic: only when `authorizeInstance(ctx, modelType, {}, 0, "")` admits the principal. The instance
    may not exist yet, so the call carries no instance id. A shared instance is recorded with an empty owner, so
    ownership gives no protection here. What protects the topic is the same `authorize` rule as for an execute on
    that model type: the principal must be allowed some action of `modelType`. The record rule above is applied in
    the same way. An event on an instance topic carries no data, only "refetch", so it reveals no more than the
    principal could learn by fetching.
- **An event reveals that records of `R` changed, and their keys.** The keys are ids of records the receiver could
  list through some action returning `R`, but not necessarily the same ones. For a model that filters rows per
  principal, the record type is registered with `morph::record::privateKeys`. Its changes then reach principals
  other than the caller's without keys, and every entry holding `R` refetches. That costs more and reveals nothing.
- **Values never travel to other connections** (§9), so `secretFields` and row-level filtering stay on the read path,
  where they are enforced today.
- **Volume.**
  - Coalescing bounds a connection to at most 20 timer flushes a second, plus one event per 256 keys or topics it
    is sent.
  - The backlog bound (§6) holds events back from a slow reader and replaces them with one `resync`, and the
    transport's `sendTimeout` closes a reader that stops.
  - The query-must-read rule (§10) removes the way a fetch could feed itself.

## 13. Tests

Every test below must fail if the feature does nothing, so each names the change that should break it.

- **Records and effects (core).**
  - Registration extracts raw and wrapped keys, and their paths, from an `R`, a `vector<R>` and a nested member.
  - Registration order across translation units does not change `records(A)`.
  - The walker finds a record nested inside a record, skips a type it cannot reflect, and ends on a recursive type.
  - Registering the same effects twice is a no-op; different effects for one action fail when the phase closes.
  - `updates<R>` over a result with no `R`, `updates<R>` on a keyless `R`, and a `derivedFrom` cycle all fail when
    the phase closes.
  - A translation unit that does not see `BRIDGE_REGISTER_EFFECTS` compiles and links with one that does.
  - *Mutation: drop the reflection walk; key extraction fails.*
- **Channel (core, net, qt).**
  - One conformance case runs on both transports and on `SimulatedRemoteBackend`: a posted event reaches the
    handler.
  - Both remote backends fire the connect handler on the first connect and on every reconnect.
  - A connection that did not offer `events` receives none.
  - An event is not taken for a reply.
  - A backlog over 1 MiB holds events back, and exactly one `resync` follows when the transport reports the drain,
    on a connection that sends nothing in between. Replies are not held back.
  - *Mutation: route events through reply matching; the routing case fails.*
- **Watching and fan-out (core).**
  - Two connections watch `R`, and an execute from A posts to both.
  - An unwatched type posts nothing, and a burst of 100 is one event.
  - A watch made under principal P is dropped by an envelope under Q.
  - A watch whose token has expired is dropped at send time.
  - A watch on a type the principal cannot read is refused.
  - A principal who may list `R` but not run the writer still receives the change.
  - An instance topic named by primary key, watched before any client attached the instance, is stored, and an
    execute on the instance created later reaches the watcher.
  - A `changesInstance` on a private instance reaches only its owning connection.
  - An execute that times out at the client but succeeds still reaches the caller as an event.
  - *Mutation: skip re-authorisation; the principal case fails.*
- **Client cache (ui).** A recording-backend screen with a list query and no refresh declarations:
  - An own `updates` reply patches the row, matched by a non-`id` key field, without a fetch, and the fetch counter
    stays flat.
  - A `changes` event, delivered through the local backend's test seam, refetches it.
  - A change marked while a fetch is pending causes exactly one more fetch after it settles.
  - A submit (not a mutation) applies its effects.
  - A discarded `latest` reply marks entries stale and patches nothing, so a newer reply's row is kept.
  - A reply's `updates` patch on an entry whose fetch is pending causes one more fetch, in local mode.
  - After the transport reconnects, the client sends `ui-hello` and its watch set again before any refetch that
    `reconnected()` files. Every entry is marked stale, and events arrive on the new connection.
  - A fetch filed after the connect but before `reconnected()` runs, and settling after it, refetches once.
  - Watch changes held during the outage and replayed before the full set are superseded by it (`replace: true`).
  - `changesInstance` refetches only entries of that instance.
  - *Mutation: restore the unconditional `stale = false`; the mid-flight case fails.*
- **Documents (ui).**
  - A query whose action declares effects is refused at load.
  - A redundant `invalidates` warns.
  - An uncovered mutation warns, and `effects::none` silences it.
  - The watch-feedback shape warns.
- **Explorer (ui).** An action with `changes<R>` shows `changes R` in `explorer_facts`, and one with nothing
  declared shows `reads`. *Mutation: drop the effects row; the case fails.*
- **Agents (ui).**
  - `tools/list` lists exactly the permitted actions, plus `evaluate`.
  - A `tools/call` of a writer through the adapter reaches a GUI connection watching its record as an event.
  - A call the authoriser refuses answers `isError: true` and posts nothing.
  - *Control: the adapter's connection is not watched by anyone; no event is posted.*
- **End to end.**
  - A harness in `tests/` runs two generic-client sessions over the `net` transport against one server.
  - Client A's mutation updates client B's mounted screen, with no `refreshOn` and no `refreshEvery` in either
    document.
  - *Control: remove the action's `BRIDGE_REGISTER_EFFECTS`; B does not update.*

## 14. Delivery

All nine parts land in **one pull request**, the one that carries this design, in the order below. Each part is a
run of commits that leaves the branch building, with its unit and GUI tests green, so a bisect never lands on a
broken commit and review can read the branch part by part. Each part updates the `docs/spec/` pages named in its
row, and keeps every new public symbol's Doxygen complete. The implementation plan is
`docs/superpowers/plans/2026-10-18-server-push.md`.

| # | Part | Layer | Needs | Spec pages |
|---|---|---|---|---|
| 1 | **Records and effects.** `BRIDGE_REGISTER_RECORD`, raw and wrapped keys, `keyless`, `derivedFrom`, `privateKeys`; `BRIDGE_REGISTER_EFFECTS` as a registrar; the result walker in the action registration; matching when the phase closes; `records(A)` with paths; deriving changes from a result | core | none | `core/registry.md` |
| 2 | **Server-to-client channel.** The `event` kind; `setConnectionWriter(cid, writer)` and `connectionDrained(cid)` with a per-connection atomic byte counter in each transport; `post`; holding events back and `resync` on drain; `sendTimeout` for the Qt transport; `SocketBackend::setConnectHandler`, fired in `onConnected()` after the held requests are flushed and before the reconnect handler is posted; `SimulatedRemoteBackend`; `setEventHandler`; the local backend's `deliverEvent` seam | core, net, qt | none | `core/wire.md`, `core/backend.md`, `security.md` |
| 3 | **Catalog and wire shapes.** `ActionInfo::effects` and `records` (key field, paths), local lookup and `ui-catalog`; `events` in `ui-hello`, and `ConnectionFacts` on `IEnvelopeService::handle`; the `watch`/`unwatch` and `changes` body shapes, defined here so parts 4 and 5 share them | ui, core (`wire.hpp` shapes, `remote.hpp` `ConnectionFacts`) | 1, 2 | `ui/service.md`, `ui/document.md`, `core/wire.md`, spec 5 §15 (the push kind is core `event`, not `ui-event`) |
| 4 | **Watching and fan-out.** Core `watch`/`unwatch` kinds in `RemoteServer` with the `cid`; the watch table with session contexts; the principal comparison in `stampVerifiedPrincipal`; fan-out in `dispatchExecute`, with private instances to their owner; `derivedFrom` expansion, coalescing, re-authorisation, `privateKeys` | core | 1, 2, 3 | `core/backend.md`, `core/shared_instances.md`, `security.md` |
| 5 | **Client cache and reconnect seams.** `actionSucceeded` carries the reply and applies effects; patch by key field and path; `refetchAfter`, set by a stale mark or a patch on a fetching entry; discarded `latest` replies as marks; events and `resync`; topic reference counting; watch before fetch; `Session::reconnected()`; `AppSource::onConnected` with an empty default; `watcherOver` and `SessionOptions::watcher`; the generic client's remote source and shell wired to them (`examples/common/app`) | ui, examples/common | 2, 3 (4 for its remote tests) | `ui/interpreter.md`, `ui/frontend.md`, `ui/service.md`, `offline/offline.md`, spec 5 §5 |
| 6 | **Document checks.** Query-must-read refused; watch-feedback, redundant and uncovered warnings | ui | 3 | `ui/document.md` |
| 7 | **End-to-end harness, examples and the explorer.** The two-session harness in `tests/`. Records and effects in the three rungs: bookmarks, bank (both model sets) and lims, with `none` on attach-style actions; their 9 `invalidates`, 5 `refreshOn` and 1 `patch` removed from `ui/1` documents where the warnings say so. The explorer's effects and records rows | tests, examples, ui | 4, 5, 6 | the rungs' READMEs, `testing_strategy.md`, `ui/explorer.md` |
| 8 | **`ui/2`.** Remove `invalidates`, `patch` and `refreshOn`; `optimistic` keeps its own shape; the authoring layer and golden files emit `ui/2`; the server keeps serving `ui/1` | ui | 7 | `ui/document.md`, `ui/author.md`, spec 5 §5 and §12 |
| 9 | **MCP adapter.** `McpAdapter` over a connection scope, tools from `permitted`, calls through `RemoteServer::handle`, `serveStdio`, bank's `--mcp-stdio` | ui, examples | 4 | a new `ui/agent.md` |

**Order in the branch:** 1, 2, 3, 4, 5, 6, 7, 8, 9. Parts 1 and 2 are independent and 6 needs only 3, but one
branch is written in one order. Part 9 comes after 8 so that the agent test runs against the examples' final
documents.

## 15. Out of scope

- **Pushing values to other connections.** Receivers refetch through their own authorised read. Sending the new
  record, filtered per receiver, would save that round trip. It needs per-receiver field filtering on the server,
  and it is the first follow-up once refetch traffic has been measured.
- **Suppressing the caller's duplicate.** An event could name the call that caused it, so the caller skips the change
  it already applied from the reply. That saves the one extra fetch §9 accepts, and is deferred until it is measured.
- **Per-key and per-field watching.** Watching is by record type, and the event carries keys. Field-level effects
  would let a client skip refetches for fields a screen does not show. That is deferred until a measurement shows
  refetches cost something.
- **Effects computed at run time,** such as a change list a model returns. Declared effects with `derivedFrom` cover
  the examples' cases. A run-time list would have to keep the rule that a model does not know the framework exists.
- **An HTTP transport for the MCP adapter.** The adapter is transport-free and is served over stdio (§11). MCP's
  streamable HTTP transport would let an agent reach a running server it did not launch.
- **Push for non-UI clients.** Negotiation is in `ui-hello`. A plain `Client` that never sends it receives no events.
  The core `hello` reply already lists capabilities, but its request carries none, so a plain client has no way to
  offer events. Adding that is a separate change to `hello`.
- **An assistant built into the client, and where its model keys live.** §11 covers agents acting through the server
  only.
- **Push for anything other than changes:** progress of a long action, notifications. The `event` envelope has a
  topic, so they can be added without another negotiation, but none is defined here.
