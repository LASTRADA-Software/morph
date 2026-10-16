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
| `refreshOn` matches by **model type, not instance**, because two private instances of a database-backed model read the same rows. | spec 5 §5 "Queries" |
| `patch` merges a reply into a cached row found by its `id` field. `optimistic` applies the same shape before the reply. `latest` applies only the newest reply. | spec 5 §5 "Mutations" |
| Every success the client sees, from a mutation or from a form, collection or wizard submit, is reported through `session.actionSucceeded(typeId, action)`. That call drives `refreshOn`. | `include/morph/ui/interpreter.hpp`, `ScreenCore::aliasSubmits` |
| A change made by another client is not pushed. Spec 5 §15 names a `ui-event` kind negotiated by `ui-hello` as the way to add it. | spec 5 §5, §15 |
| A bridge subscription reaches only handlers in the same process, never across connections. | `docs/spec/core/shared_instances.md` |
| `RemoteServer::handle(msg, reply, cid)` answers each request through its own `reply`. `openConnection()` returns a scope id. The server holds no way to send a connection a message it did not ask for. | `include/morph/core/remote.hpp` |
| `execute` replies are written from the model's strand, and a timed-out `execute` is answered `err "timeout"` while the model may still finish. | `include/morph/core/remote.hpp` |
| The server stamps the verified principal onto **each envelope**. A connection has no principal of its own. | `include/morph/core/remote.hpp`, `docs/spec/core/backend.md` |
| Both transports track a `ConnectionId` per connection and call `closeConnection` on disconnect. Neither caps its outbound queue. | `include/morph/net/socket_server.hpp`, `include/morph/net/detail/ws_connection.hpp`, `src/qt/qt_websocket_server.cpp`, `docs/spec/security.md` |
| The client matches replies by `callId`. A message that answers no pending call is dropped. | `include/morph/core/wire.hpp` |
| The core `hello` request carries only `protocolVersion`, and the generic UI client opens with `ui-hello`, not `hello`. | `include/morph/core/wire.hpp`, `include/morph/ui/service_client.hpp` |
| `ActionInfo` carries `modelType`, `calculatedFields`, `secretFields`, `credential` and `listFields`. Nothing says whether an action reads or writes. | `include/morph/ui/document.hpp` |
| `BRIDGE_REGISTER_ACTION` picks its form by argument count, and its fourth argument is `Loggable`. | `include/morph/core/registry.hpp` |
| A key may be raw (integral or `std::string`) or a strong id wrapping one (`WrappedModelKey`). | `include/morph/core/model_key.hpp` |
| Offline replay runs as reconnect → activate → bind → replay. | `docs/spec/offline/offline.md` |
| Within a vocabulary version, changes are additive. | spec 5 §12 "Versions" |
| The examples hold 21 `invalidates` and 13 `refreshOn` occurrences outside their tests. | `grep` over `examples/`, at `a55374f5` |

## 3. Concepts

- **Record type.** A reflected aggregate that action results carry and screens show, with a registered name and
  optionally a key member: `ResultView` keyed by its `ResultId`, `AccountInfo` keyed by `id`. A record type shown
  through several models is one record type: `CustomerModel::ListAccounts` and `AccountModel::GetAccount` both
  return `AccountInfo`.
- **Derived record type.** A record computed from others: ledger's `GetBudgetReportResult` from transactions. It
  is keyless, and it declares what it is derived from (§4), so a change to its sources reaches it without every
  writer naming it.
- **Effect.** What a successful action did. One of:
  - `updates<R>`: the result carries the new state of the `R` records it changed, identified by their keys.
  - `changes<R>`: records of `R` were created, removed or changed in a way the result does not carry.
  - `changesInstance`: the action changed the state of the model instance it ran on, such as a working copy that
    only that instance's queries read.
  - nothing: the action reads.
- **Change.** What the framework derives from an effect after a successful execute: a record topic
  `{record, keys?}`, or an instance topic `{model type, instance}`.
- **Watch.** A client's statement of which topics its cached queries hold, authorised by the server.
- **Event.** A server-to-client message carrying changes, sent to the connections watching their topics.

The record type is the unit for shared data. It cannot be the model instance, for the reason `refreshOn` already
matches by model type: two private instances of one database-backed model read the same rows. The instance is the
unit only for state that lives in the instance, which is what `changesInstance` names.

## 4. Declaring records and effects

### Records

```cpp
BRIDGE_REGISTER_RECORD(bank::dto::AccountInfo, "Bank_Account")                // key: member `id`
BRIDGE_REGISTER_RECORD(lims::ResultView, "Lims_Result")                        // key: `id`, a ResultId
BRIDGE_REGISTER_RECORD(ledger::dto::GetBudgetReportResult, "Ledger_BudgetReport",
                       morph::record::keyless, morph::record::derivedFrom<ledger::dto::TransactionEntryInfo>)
```

- **The key** defaults to a member named `id`, as a view's `rowKey` defaults to `"id"`. It may be any `ModelKey`,
  raw or wrapped. On the wire it is the raw scalar, as `ModelKey` already encodes it. `morph::record::key<&T::m>`
  names another member, and `morph::record::keyless` declares none.
- **`derivedFrom<S...>`** says that a change to any `S` is a change to this record type. Fan-out expands it once,
  when the change is derived (§8). A derived type is keyless, because a key-level change to a source does not say
  which derived rows it moved.
- The record registry is process-wide and filled during the registration phase, like the action registry, and the
  same latch checks it. `derivedFrom` cycles are refused at registration.

### Effects

Effects are declared by their own macro, beside the action's registration. This leaves `BRIDGE_REGISTER_ACTION`'s
argument-count forms untouched, including `Loggable` as the fourth argument and the `_FOR_CLIENT` forms. It also
keeps every DTO free of morph types, which preserves the rule that a model does not know the framework exists.

```cpp
BRIDGE_REGISTER_ACTION(TransactionModel, Deposit,  "Deposit")
BRIDGE_REGISTER_EFFECTS(Deposit,  morph::effects::changes<TxnInfo>, morph::effects::changes<AccountInfo>)
BRIDGE_REGISTER_EFFECTS(Transfer, morph::effects::changes<TxnInfo>, morph::effects::changes<AccountInfo>)
BRIDGE_REGISTER_EFFECTS(AddPayee, morph::effects::changes<PayeeInfo>)
BRIDGE_REGISTER_EFFECTS(RemovePayee, morph::effects::changes<PayeeInfo>)
// An action that edits one row and returns it, such as a hypothetical EditPayee returning PayeeInfo:
// BRIDGE_REGISTER_EFFECTS(EditPayee, morph::effects::updates<PayeeInfo>)
BRIDGE_REGISTER_EFFECTS(LoadSample, morph::effects::changesInstance)
// ListPayees, GetAccount: no effects, they read.
```

- **`BRIDGE_REGISTER_EFFECTS(A, effects...)`** specialises `morph::model::ActionEffects<A>`. `ActionTraits<A>`
  gains `effects()`, which returns the list as data (`{kind, record?}`), empty when `A` has no specialisation.
  The framework reads effects only through `effects()`.
- **`updates<R>` finds the records in the result.** The result may be an `R`, a `std::vector<R>`, or an aggregate
  whose members, found by reflection, hold an `R` or a `std::vector<R>`. If none is found, registration fails
  when the program starts, not when the action first runs. `R` must have a key. Bank's `Transfer` returns two
  balances (`TransferResult{fromBalanceMinor, toBalanceMinor}`), not two `AccountInfo`s, so it declares
  `changes<AccountInfo>`.
- **`updates<R>` only changes rows that exist.** A key that no cached entry holds is not added to any list, because
  whether it belongs there is the server's query to answer. An action that may create a record declares
  `changes<R>`.
- **`changes<R>` needs nothing from the result.** It covers creation, removal, and changes the result does not
  carry.
- **`changesInstance`** means the instance's own state changed. It reaches the caller's entries through the same
  instance (§9), and, for a shared instance, other clients attached to that instance (§7).
- **Several effects compose,** as above.
- **No declaration means the action reads, and nothing is pushed.** A read is safe by construction, because a
  query may only run an action with no effects (§10). An action that writes and declares nothing behaves as it
  does today, and §10's checks point at it.
- **Only success has effects.** A failed action or a cancelled one changes nothing.

### What each action's result holds

Registration also records, per action, the record types its **result** holds, found by the same reflection as
`updates<R>`: `records(A)`. A cached query holds exactly the records of its action. `ListResults` returns
`ListResultsResult{results: vector<ResultView>}`, so its entries hold `Lims_Result`.

## 5. The catalog

`ActionInfo` gains two facts. Local mode fills them from the registry; remotely, `ui-catalog` serves them, like the
existing ones.

| Field | Meaning |
|---|---|
| `effects` | `[{kind: "updates" \| "changes" \| "changesInstance", record?}]`. |
| `records` | The record types the action's result holds (§4), each with whether it is keyed. |

`records` lets the client decide, without asking the server, whether a cache entry holds record `R`: the entry's
action's `records` contain `R`. `derivedFrom` is never sent. The server expands it before a change leaves (§8), so a
client only ever matches the record types its entries hold.

An `evaluate` cache entry (`ui-evaluate`, spec 5 §7) holds no records and is never watched: an evaluation depends on
its body, not on stored data.

## 6. The server-to-client channel

The wire gains its first server-initiated message.

- **Negotiation in `ui-hello`.** `HelloRequest` gains `events: true` and `HelloReply` gains `events: bool`. Both are
  additive optional keys. The generic client already opens with `ui-hello`, on the same connection as its executes,
  and spec 5 §15 names it as the place to negotiate this. A server sends events only on a connection whose
  `ui-hello` offered them and that it accepted. A client that never offers sees today's protocol unchanged.
  Negotiation is per connection, so a reconnect negotiates again.
- **The envelope.** `kind: "event"`, `callId: 0`, `body: {topic, payload}`. For this design the topic is
  `changes`. A client routes it by kind before reply matching, so an event is never taken for an orphaned reply.
- **Writing to a connection.** `RemoteServer` gains `setConnectionWriter(cid, write)`, the transport's writer for a
  connection already opened with `openConnection()`, and `post(cid, envelope)`. `post` is callable from any thread.
  It hands the encoded envelope to the writer, which queues it on the transport's own thread, as replies are
  queued today. The size cap (`kMaxEnvelopeBytes`) applies. A write failure drops the connection's watches,
  as a disconnect does.
- **The outbound cap.** Events are queued per connection, up to 256 pending events or 1 MiB. On overflow the queue
  is replaced by one `resync` event, meaning "everything you watch may have changed", and the client marks every
  watched entry stale. The connection is not closed, and replies are not affected. This is the only outbound limit
  the transports gain.
- **The client side.** `IBackend` gains `setEventHandler(fn)`, delivered on the bridge's executor:
  - The remote backends call it for each `event`: the Qt WebSocket backend, the `net` socket backend, and
    `SimulatedRemoteBackend`, which the core transport tests use.
  - The local backend calls it in-process, with no wire, so local mode gets the same behaviour from the same code
    above the backend.
- **Order.** Events on one connection arrive in the order the server posted them. No order is promised between an
  event and any reply.

## 7. Watching

A client watches the topics its cache holds, so the server sends each connection only what it can show.

- **Topics.**
  - A record type, for every record its entries hold.
  - `(model type, instance key)` for every entry read through a **shared** instance whose action holds no
    records, which is the case for working-copy reads.
  - A private instance is never a topic, because no other connection can reach it.
- **Lifetime.** A topic is watched while at least one cache entry holds it, mounted or kept (spec 5 §5: for
  5 minutes). The client counts references per topic. It sends `watch` when a count leaves zero and `unwatch` when it
  returns to zero. Both are UI service kinds (`ui-watch`, `ui-unwatch`) with `{records: [...], instances: [...]}`,
  and an `ok` reply that lists any topic refused (§12).
- **Watch before fetch.** Watch changes made in one reactive batch are sent as one message, **before** the
  fetches of that batch, on the same connection. The server handles them in arrival order on its strand. A write
  that commits after the fetch has read therefore finds the watch in place, and one that commits before it is in
  the fetched value. Neither is missed.
- **Reconnect.** After a reconnect or a backend switch, the client negotiates again and sends its whole watch set
  before anything else, then marks every entry stale, as spec 6 §9 already does for a backend switch. Events missed
  while disconnected are not replayed: the stale marks cover them. Offline replay comes after the watch set, so its
  changes arrive as events too (§9).
- **Granularity.** Record topics are per type, not per key. Per-key watching would make the server track every key
  every client shows, and those keys change as lists scroll. The event carries keys, and the client filters on them
  (§9).

## 8. Fan-out on the server

Fan-out sits in `RemoteServer`'s execute path (`dispatchExecute`), which every executing message shares: the
`execute` kind, offline replays (which arrive as executes), and the agent adapter (§11). The UI service has no
execute path of its own (`ui-evaluate` never executes), so it needs nothing.

After an `execute` succeeds on the model's strand:

1. The model strand derives the changes: the effects' record topics, with keys read from the result for `updates`,
   expanded through `derivedFrom`; and, for `changesInstance` on a shared instance, its instance topic. It posts the
   changes to the server strand. The reply is written as it is today, unaffected.
2. On the server strand, the changes are matched against the watch table. **The calling connection is included.**
   It also applies the effects from its reply (§9), and a second refetch request for the same entry is deduplicated
   by the cache. Including it covers every case in which the caller never sees its own reply: an execute that timed
   out on the client but succeeded on the server, a reply that `latest` discarded, and a replay whose reply went to
   the offline worker.
3. Each matching watch is re-authorised with the principal stored on it (§12), and the connection's coalescer
   queues the change.

- **Coalescing.** Changes for one connection are merged for up to 50 ms, or until 256 keys are queued, then sent as
  one event. Keys are merged per record type, a `changes` absorbs that type's `updates` keys, and instance topics are
  deduplicated. A burst of one hundred writes is then one refetch per showing query, not one hundred.
- **The watch table** lives on the server strand, next to the connection scopes `closeConnection` already reclaims.
  A closed connection drops its watches.

## 9. The client cache

The interpreter's query cache applies a change the same way whether it came from a reply or from an event.

| Change | From a reply (the caller's own) | From an event |
|---|---|---|
| `updates R` with keys | Patch: each entry holding `R` merges the new records into the rows with those keys, using the existing `patch` merge and the catalog's `listFields` for a list inside a record. No refetch. | Each entry holding `R` whose value contains one of the keys is marked stale, and refetched if mounted. |
| `changes R` | Each entry holding `R` is marked stale, and refetched if mounted. | The same. |
| `changesInstance` | Each entry read through the same instance is marked stale, and refetched if mounted. | The same, for the shared instance named. |
| `resync` | none | Every watched entry is marked stale, and refetched if mounted. |

- **Every reply applies its effects.** The single hook is `session.actionSucceeded`, which every success already
  goes through: mutations, and form, collection and wizard submits (`aliasSubmits`). It gains the reply's JSON, so
  `updates` can patch. A `latest` run whose reply is discarded still applies its effects; only its value is
  dropped. An offline replay's reply reaches the offline worker, not a mutation. Its effects reach the cache as an
  event, because the caller is not excluded (§8).
- **Why other clients refetch rather than receive values.** The new values were computed for the caller. Another
  user may see different fields (`secretFields`) or may not be allowed the record at all. A refetch goes through the
  receiver's own authorised read. Pushing values to other receivers is the first item in §15.
- **Duplicates are harmless.** The caller sees each change twice, once from its reply and once as an event. Marking
  an entry that is already stale, or already refetching, is a no-op, so a duplicate costs nothing.
- **Batching.** One event, or one reply's effects, is applied in one reactive batch, so a screen updates in one
  frame.
- **`optimistic` is unchanged.** It still shows a guess before the reply, and the reply's patch confirms or corrects
  it, exactly as a declared `patch` does today.
- **Entries no query mounts** are only marked stale. They refetch if a screen mounts on them.

## 10. Documents

With effects declared, a document needs no refresh declarations.

**At load, in `ui/1`, additively.** The loader already has the catalog, and it reports:

- **refused: a query whose action declares effects.** A query must read. This is what makes the default safe: two
  clients cannot refetch each other in a ring, because a fetch never produces a change. It is new in what it
  refuses, so it applies only to actions that declare effects, which no existing action does.
- **warning, redundant:** an `invalidates`, `patch` or `refreshOn` that the mutation's action's effects already
  cover, so a document can migrate one screen at a time.
- **warning, uncovered:** a mutation whose action declares no effects, with no `invalidates` or `patch` of its own
  and no query naming it in `refreshOn`. After it succeeds, nothing on any screen updates. It may genuinely change
  nothing that is shown, so this is only a warning, but it points at actions still to annotate.

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

- **The MCP adapter** is a server-side component. It opens a connection scope with `openConnection()` for each
  agent session, and sends each tool call through `RemoteServer::handle(msg, reply, cid)` as an ordinary `execute`.
  That keeps every check the GUI gets: authentication and the principal, `authorize` and `authorizeInstance`,
  limits, the journal, cancellation and the in-flight count. It lists the actions the principal may run (the
  catalog's `permitted`, with their JSON shapes) as MCP tools, and it offers `ui-evaluate` as a dry run. The agent
  may commit directly.
- **Fan-out treats the agent's connection like any other,** so every open GUI showing the changed records updates
  through §8 and §9.
- **Model access and keys are not part of this design.** The adapter does not call a model provider. An external
  agent (Claude Desktop, Claude Code) brings its own. Where keys would live for an assistant built into the client
  is still undecided, and it does not affect anything above.

## 12. Security

- **Each watch carries a principal.** A connection has no principal of its own: the server verifies one on every
  envelope. The watch table therefore stores, with each watch, the verified principal of the `ui-watch` envelope
  that created it.
  - **A later envelope on the connection with a different verified principal drops all of the connection's
    watches.** That covers signing in as someone else, and a client that keeps old watches across a sign-out. The
    client's cache is already cleared on a principal change (spec 5 §5), and it watches again.
  - Fan-out re-authorises each watch at send time, with its stored principal and that token's expiry. An expired
    token drops the watch. The client watches again with its next envelope, which carries a fresh token.
- **What a watch may name.**
  - A record type: only when the principal may run at least one action whose `records` contain it. Otherwise the
    `ok` reply lists it as refused, and the client logs it.
  - An instance topic: only when `authorizeInstance` admits the principal to that instance.
- **An event reveals that records of `R` changed, and their keys.** Keys are ids of records the receiver could
  list through some action returning `R`, but not necessarily the same ones. For a model that filters rows per
  principal, the record type is registered with `morph::record::privateKeys`. Its changes then reach principals other
  than the caller's without keys, and every entry holding `R` refetches. That costs more and reveals nothing.
- **Values never travel to other connections** (§9), so `secretFields` and row-level filtering stay on the read path,
  where they are enforced today.
- **Volume.** Coalescing bounds events to 20 a second per connection, and the outbound cap (§6) bounds what a slow
  reader can queue. The query-must-read rule (§10) removes the only way the design could feed itself.

## 13. Tests

Every test below must fail if the feature does nothing, so each names the change that should break it.

- **Records and effects (core).**
  - Registration extracts raw and wrapped keys from an `R`, a `vector<R>` and a nested member.
  - `updates<R>` over a result with no `R`, and a `derivedFrom` cycle, both fail registration.
  - `records(A)` lists the result's record types.
  - *Mutation: drop the reflection walk; key extraction fails.*
- **Channel (core, net, qt).**
  - One conformance case runs on both transports, on `SimulatedRemoteBackend` and on the local backend: a posted
    event reaches the handler.
  - A connection that did not offer `events` receives none.
  - An event is not taken for a reply.
  - Overflow produces exactly one `resync`.
  - *Mutation: route events through reply matching; the routing case fails.*
- **Watching and fan-out (core).**
  - Two connections watch `R`, and an execute from A posts to both.
  - An unwatched type posts nothing, and a burst of 100 is one event.
  - A watch made under principal P is dropped by an envelope under Q.
  - A watch on a type the principal cannot read is refused.
  - An execute that times out at the client but succeeds still reaches the caller as an event.
  - *Mutation: skip re-authorisation; the principal case fails.*
- **Client cache (ui).** A recording-backend screen with a list query and no refresh declarations:
  - An own `updates` reply patches the row without a fetch, and the fetch counter stays flat.
  - A `changes` event refetches it.
  - A submit (not a mutation) applies its effects.
  - A discarded `latest` reply still applies its effects.
  - `changesInstance` refetches only entries of that instance.
  - *Mutation: skip the cache step in `actionSucceeded`; the shown row keeps its old value.*
- **Documents (ui).** A query whose action declares effects is refused at load. A redundant `invalidates` warns. An
  uncovered mutation warns.
- **End to end (examples).** One rung runs two clients against one server. Client A's mutation updates client B's
  mounted screen, with no `refreshOn` and no `refreshEvery` in either document. *Control: remove the action's
  `BRIDGE_REGISTER_EFFECTS`; B does not update.*

## 14. Delivery

Each part lands as its own pull request to `master`. Parts with no dependency between them can run side by side.
Each part updates the `docs/spec/` pages named in its row, and keeps every new public symbol's Doxygen complete.

| # | Part | Layer | Needs | Spec pages |
|---|---|---|---|---|
| 1 | Records and effects: `BRIDGE_REGISTER_RECORD`, keys raw and wrapped, `keyless`, `derivedFrom`, `privateKeys`; `BRIDGE_REGISTER_EFFECTS`, `ActionEffects`, `ActionTraits::effects()`; `records(A)`; deriving changes from a result | core | none | `core/registry.md` |
| 2 | Server-to-client channel: the `event` kind, `setConnectionWriter` and `post`, the outbound cap and `resync`, both transports, `SimulatedRemoteBackend`, `setEventHandler`, in-process delivery in the local backend | core, net, qt | none | `core/wire.md`, `core/backend.md`, `security.md` |
| 3 | Catalog and negotiation: `ActionInfo::effects` and `records`, local lookup and `ui-catalog`; `events` in `ui-hello` | ui | 1 | `ui/service.md`, `ui/document.md` |
| 4 | Watching and fan-out: `ui-watch` and `ui-unwatch`, the watch table with principals, fan-out in `dispatchExecute`, `derivedFrom` expansion, coalescing, re-authorisation, `privateKeys` | core, ui | 1, 2, 3 | `core/backend.md`, `ui/service.md`, `security.md` |
| 5 | Client cache: `actionSucceeded` carries the reply and applies effects; events and `resync`; topic reference counting; watch before fetch; reconnect order | ui | 2, 3 (4 for its remote tests) | `ui/interpreter.md`, spec 5 §5 |
| 6 | Document checks: query-must-read refused; redundant and uncovered warnings | ui | 3, 5 | `ui/document.md` |
| 7 | Examples: records and effects in the seven rungs that use refresh declarations, their declarations removed in `ui/1` documents where the warnings say so, and the two-client end-to-end test | examples | 5, 6 | the rungs' READMEs |
| 8 | `ui/2`: remove `invalidates`, `patch` and `refreshOn`; `optimistic` keeps its own shape; the authoring layer and golden files emit `ui/2`; the server keeps serving `ui/1` | ui | 7 | `ui/document.md`, `ui/author.md`, spec 5 §5 and §12 |
| 9 | MCP adapter: a connection scope per agent session, tools from `permitted`, calls through `RemoteServer::handle` | net, ui | 4 | a new `ui/agent.md` |

**Ready now:** parts 1 and 2. They are independent: part 1 is `registry.hpp` and a new record header, part 2 is
`wire.hpp`, `remote.hpp`, `backend.hpp` and the transports. **Then:** part 3, then parts 4 and 5 side by side. Part 5
uses the local backend's in-process delivery for its tests until part 4 lands. Part 9 can run beside anything after
part 4.

## 15. Out of scope

- **Pushing values to other connections.** Receivers refetch through their own authorised read. Sending the new
  record, filtered per receiver, would save that round trip. It needs per-receiver field filtering on the server,
  and it is the first follow-up once refetch traffic has been measured.
- **Per-key and per-field watching.** Watching is by record type, and the event carries keys. Field-level effects
  would let a client skip refetches for fields a screen does not show. That is deferred until a measurement shows
  refetches cost something.
- **Effects computed at run time,** such as a change list a model returns. Declared effects with `derivedFrom` cover
  the examples' cases. A run-time list would have to keep the rule that a model does not know the framework exists.
- **Push for non-UI clients.** Negotiation is in `ui-hello`. A plain `Client` that never sends it receives no events.
  Offering events in the core `hello` would need that request to carry capabilities, which it does not today.
- **An assistant built into the client, and where its model keys live.** §11 covers agents acting through the server
  only.
- **Push for anything other than changes:** progress of a long action, notifications. The `event` envelope has a
  topic, so they can be added without another negotiation, but none is defined here.
