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
| A successful fetch clears an entry's `stale` mark unconditionally, and a refetch is skipped while a fetch is pending. | `include/morph/ui/interpreter.hpp` (`settleSuccess`, `refetchCurrent`) |
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
| The examples hold 21 `invalidates` and 13 `refreshOn` occurrences outside their tests, in seven rungs. | `grep` over `examples/`, at `a55374f5` |

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

### Records

```cpp
BRIDGE_REGISTER_RECORD(bank::dto::AccountInfo, "Bank_Account")                // key: member `id`
BRIDGE_REGISTER_RECORD(lims::ResultView, "Lims_Result")                        // key: `id`, a ResultId
BRIDGE_REGISTER_RECORD(ledger::dto::GetBudgetReportResult, "Ledger_BudgetReport",
                       morph::record::keyless, morph::record::derivedFrom<ledger::dto::TransactionEntryInfo>)
```

- **The key** defaults to a member named `id`, the field the interpreter's `patch` already identifies rows by. It
  may be any `ModelKey`, raw or wrapped, and on the wire it is the raw scalar, as `ModelKey` already encodes it.
  `morph::record::key<&T::m>` names another member, and `morph::record::keyless` declares none.
- **`derivedFrom<S...>`** says that a change to any `S` is a change to this record type. Fan-out expands it when it
  derives a change (§8). A derived type is keyless, because a key-level change to a source does not say which
  derived rows moved.
- **`morph::record::privateKeys`** marks a type whose keys must not reach other principals (§12).
- `derivedFrom` cycles, and two registrations of one type or one name, are refused when the registration phase
  closes.

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
  `ActionTraits<A>::typeId()`, the effects as data (`{kind, record type?}`). It must appear where `ActionTraits<A>`
  is visible: after `BRIDGE_REGISTER_ACTION`, or after `BRIDGE_DECLARE_ACTION` in the same header. It defines no
  specialisation, so a translation unit that does not see it is not inconsistent with one that does.
- **Client-only builds** (`MORPH_CLIENT_ONLY`) compile the registrars too; they hold no model code. A pure remote
  client does not consult them: it takes effects and records from `ui-catalog` (§5), as it takes every other
  action fact.
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

Each action's existing registration also records a reflection walker over its `Result` type, which needs no record
to be registered yet. When the registration phase closes, the walker's type identities are matched against the
record registry, giving `records(A)`. For each record, it gives the **paths** at which the result holds it and
whether it is keyed. A cached query holds exactly the records of its action. `ListResults` returns
`ListResultsResult{results: vector<ResultView>}`, so its entries hold `Lims_Result` at path `results`.

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
  connection already opened with `openConnection()`, and `post(cid, envelope)`.
  - `post` is callable from any thread. It hands the encoded envelope to the writer, which queues it on the
    transport's own thread, as replies are queued today.
  - The size cap (`kMaxEnvelopeBytes`) applies.
  - A write failure drops the connection's watches, as a disconnect does.
- **The outbound cap.** Events are queued per connection, up to 256 pending events or 1 MiB.
  - On overflow, the queue is replaced by one `resync` event, meaning "everything you watch may have changed", and
    the client marks every watched entry stale.
  - Overflow itself closes nothing. A transport's own rules still apply: the `net` writer closes a peer that
    stalls past `sendTimeout`, as it does for replies.
  - This is the only outbound limit the transports gain.
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
  - `(model type, instance id)` for every entry read through a **keyed shared** instance whose action holds no
    records, which is the case for working-copy reads. The instance id is the binding's `currentId`.
  - A private instance needs no watch: its owner is known to the server (§8). A keyless `"shared"` alias names a
    stateless model, which has no instance state to change.
- **The kinds.** `watch` and `unwatch` are **core** kinds, handled by `RemoteServer` on its strand with the
  connection's `cid`, because the watch table belongs to the connection and fan-out reads it in the execute path.
  - Core can validate them: after part 1, records are a core registry, and instance topics are checked with
    `authorizeInstance`.
  - The body is `{records: [...], instances: [{modelType, instanceId}]}`. The `ok` reply lists any topic refused
    (§12).
  - The UI layer sends them; it does not handle them.
- **Lifetime.** A topic is watched while at least one cache entry holds it, mounted or kept (spec 5 §5: for
  5 minutes). The client counts references per topic. It sends `watch` when a count leaves zero and `unwatch` when it
  returns to zero.
- **Watch before fetch.** Watch changes made in one reactive batch are sent as one message, **before** the fetches
  of that batch, on the same connection. The server handles `watch` on its strand when the message arrives, and an
  `execute` is admitted on that same strand before it is posted to the model strand. The watch is therefore in the
  table before the fetch reads anything:
  - a write that commits before the fetch reads is in the fetched value;
  - one that commits after produces an event, which §9 keeps even if the fetch is still in flight.
- **Reconnect.** The watch set is resent in the `bindContext` step of the offline reconnect sequence (`activate` →
  `bindContext` → `replay`), after `ui-hello` has negotiated again and before the offline queue replays. The client
  then marks every entry stale, as spec 6 §9 already does for a backend switch. Events missed while disconnected are
  not replayed; the stale marks cover them. The replay's own changes arrive as events (§8).
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
| `resync` | none | Every watched entry is marked stale, and refetched if mounted. |

- **A change during a fetch is kept.** A stale mark on an entry whose fetch is pending sets `refetchAfter`. When the
  fetch settles, the entry stays stale and fetches once more, even if it succeeded. Otherwise the success would clear
  the mark, and a change committed after the fetch read would be lost. This replaces today's unconditional
  `stale = false` on settle, and it is what makes the watch-before-fetch argument in §7 hold.
- **Every reply applies its effects.** The single hook is `session.actionSucceeded`, which every success already
  goes through: mutations, and form, collection and wizard submits (`aliasSubmits`). It gains the reply's JSON, so
  `updates` can patch. A `latest` run whose reply is discarded still applies its effects; only its value is
  dropped. An offline replay's reply reaches the offline worker, not a mutation. Its effects reach the cache as an
  event, because the caller is not excluded (§8).
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
- **warning, watch feedback:** a document `watch` whose `on` reads a query's value, whose `source` is `any` (the
  default), and whose `do` runs a mutation with effects. Another client's change refetches the query, the watch
  fires, and the mutation's change reaches the other client, which may do the same. Such a watch should use
  `"source": "user"`. The loader cannot prove a ring, so this is a warning, and the claim above is limited to
  queries.
- **warning, redundant:** an `invalidates`, `patch` or `refreshOn` that the mutation's action's effects already
  cover, so a document can migrate one screen at a time.
- **warning, uncovered:** a mutation whose action declares nothing (not even `none`), with no `invalidates` or
  `patch` of its own and no query naming it in `refreshOn`. After it succeeds, nothing on any screen updates.
  `effects::none` silences it for actions that attach or open, such as `OpenSample` and `OpenBoard`.

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
    watches.** The comparison is made in `stampVerifiedPrincipal`, which every kind passes through, given the
    envelope's `cid`. That covers signing in as someone else, and a client that keeps old watches across a sign-out.
    The client's cache is already cleared on a principal change (spec 5 §5), and it watches again.
  - **Fan-out re-authorises each watch at send time.** It runs `authenticate` on the stored context, which fails for
    an expired token, then `authorize` for the action that produced the change, or `authorizeInstance` for an
    instance topic. A failure drops the watch. The client watches again with its next envelope, which carries a
    fresh token.
- **What a watch may name.**
  - A record type: only when the principal may run at least one action whose `records` contain it. Otherwise the
    `ok` reply lists it as refused, and the client logs it.
  - An instance topic: only when `authorizeInstance` admits the principal to that instance.
- **An event reveals that records of `R` changed, and their keys.** The keys are ids of records the receiver could
  list through some action returning `R`, but not necessarily the same ones. For a model that filters rows per
  principal, the record type is registered with `morph::record::privateKeys`. Its changes then reach principals
  other than the caller's without keys, and every entry holding `R` refetches. That costs more and reveals nothing.
- **Values never travel to other connections** (§9), so `secretFields` and row-level filtering stay on the read path,
  where they are enforced today.
- **Volume.**
  - Coalescing bounds events to 20 a second per connection.
  - The outbound cap (§6) bounds what a slow reader can queue.
  - The query-must-read rule (§10) removes the way a fetch could feed itself.

## 13. Tests

Every test below must fail if the feature does nothing, so each names the change that should break it.

- **Records and effects (core).**
  - Registration extracts raw and wrapped keys, and their paths, from an `R`, a `vector<R>` and a nested member.
  - Registration order across translation units does not change `records(A)`.
  - `updates<R>` over a result with no `R`, `updates<R>` on a keyless `R`, and a `derivedFrom` cycle all fail when
    the phase closes.
  - A translation unit that does not see `BRIDGE_REGISTER_EFFECTS` compiles and links with one that does.
  - *Mutation: drop the reflection walk; key extraction fails.*
- **Channel (core, net, qt).**
  - One conformance case runs on both transports and on `SimulatedRemoteBackend`: a posted event reaches the
    handler.
  - A connection that did not offer `events` receives none.
  - An event is not taken for a reply.
  - Overflow produces exactly one `resync`.
  - *Mutation: route events through reply matching; the routing case fails.*
- **Watching and fan-out (core).**
  - Two connections watch `R`, and an execute from A posts to both.
  - An unwatched type posts nothing, and a burst of 100 is one event.
  - A watch made under principal P is dropped by an envelope under Q.
  - A watch whose token has expired is dropped at send time.
  - A watch on a type the principal cannot read is refused.
  - A `changesInstance` on a private instance reaches only its owning connection.
  - An execute that times out at the client but succeeds still reaches the caller as an event.
  - *Mutation: skip re-authorisation; the principal case fails.*
- **Client cache (ui).** A recording-backend screen with a list query and no refresh declarations:
  - An own `updates` reply patches the row, matched by a non-`id` key field, without a fetch, and the fetch counter
    stays flat.
  - A `changes` event, delivered through the local backend's test seam, refetches it.
  - A change marked while a fetch is pending causes exactly one more fetch after it settles.
  - A submit (not a mutation) applies its effects.
  - A discarded `latest` reply still applies its effects.
  - `changesInstance` refetches only entries of that instance.
  - *Mutation: restore the unconditional `stale = false`; the mid-flight case fails.*
- **Documents (ui).**
  - A query whose action declares effects is refused at load.
  - A redundant `invalidates` warns.
  - An uncovered mutation warns, and `effects::none` silences it.
  - The watch-feedback shape warns.
- **End to end.**
  - A harness in `tests/` runs two generic-client sessions over the `net` transport against one server.
  - Client A's mutation updates client B's mounted screen, with no `refreshOn` and no `refreshEvery` in either
    document.
  - *Control: remove the action's `BRIDGE_REGISTER_EFFECTS`; B does not update.*

## 14. Delivery

Each part lands as its own pull request to `master`. Parts with no dependency between them can run side by side.
Each part updates the `docs/spec/` pages named in its row, and keeps every new public symbol's Doxygen complete.

| # | Part | Layer | Needs | Spec pages |
|---|---|---|---|---|
| 1 | **Records and effects.** `BRIDGE_REGISTER_RECORD`, raw and wrapped keys, `keyless`, `derivedFrom`, `privateKeys`; `BRIDGE_REGISTER_EFFECTS` as a registrar; the result walker in the action registration; matching when the phase closes; `records(A)` with paths; deriving changes from a result | core | none | `core/registry.md` |
| 2 | **Server-to-client channel.** The `event` kind, `setConnectionWriter` and `post`, the outbound cap and `resync`, both transports, `SimulatedRemoteBackend`, `setEventHandler`, the local backend's `deliverEvent` seam | core, net, qt | none | `core/wire.md`, `core/backend.md`, `security.md` |
| 3 | **Catalog and wire shapes.** `ActionInfo::effects` and `records` (key field, paths), local lookup and `ui-catalog`; `events` in `ui-hello`; the `watch`/`unwatch` and `changes` body shapes, defined here so parts 4 and 5 share them | ui, core (`wire.hpp` shapes) | 1 | `ui/service.md`, `ui/document.md`, `core/wire.md` |
| 4 | **Watching and fan-out.** Core `watch`/`unwatch` kinds in `RemoteServer` with the `cid`; the watch table with session contexts; the principal comparison in `stampVerifiedPrincipal`; fan-out in `dispatchExecute`, with private instances to their owner; `derivedFrom` expansion, coalescing, re-authorisation, `privateKeys` | core | 1, 2, 3 | `core/backend.md`, `core/shared_instances.md`, `security.md` |
| 5 | **Client cache.** `actionSucceeded` carries the reply and applies effects; patch by key field and path; `refetchAfter`; events and `resync`; topic reference counting; watch before fetch; the watch resend in `ReconnectCoordinator`'s `bindContext` | ui, offline glue | 2, 3 (4 for its remote tests) | `ui/interpreter.md`, `offline/offline.md`, `ui/host.md`, spec 5 §5 |
| 6 | **Document checks.** Query-must-read refused; watch-feedback, redundant and uncovered warnings | ui | 3, 5 | `ui/document.md` |
| 7 | **End-to-end harness and examples.** The two-session harness in `tests/`. Records and effects in the seven rungs that use refresh declarations (`none` on attach-style actions), with their declarations removed in `ui/1` documents where the warnings say so | tests, examples | 5, 6 | the rungs' READMEs, `testing_strategy.md` |
| 8 | **`ui/2`.** Remove `invalidates`, `patch` and `refreshOn`; `optimistic` keeps its own shape; the authoring layer and golden files emit `ui/2`; the server keeps serving `ui/1` | ui | 7 | `ui/document.md`, `ui/author.md`, spec 5 §5 and §12 |
| 9 | **MCP adapter.** A connection scope per agent session, tools from `permitted`, calls through `RemoteServer::handle` | net, ui | 4 | a new `ui/agent.md` |

**Ready now: parts 1 and 2.** They are independent. Part 1 is `registry.hpp` and a new records header. Part 2 is
`wire.hpp`'s event kind, `remote.hpp`'s writer and `post`, `backend.hpp` and the transports.

**Then part 3, then parts 4 and 5 side by side.**
- They share only the shapes part 3 defines.
- Part 4 is in `remote.hpp`; part 5 is in the interpreter and the offline glue.
- Part 5 tests its event path through the local backend's seam until part 4 lands.

Part 9 can run beside anything after part 4.

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
- **Push for non-UI clients.** Negotiation is in `ui-hello`. A plain `Client` that never sends it receives no events.
  The core `hello` reply already lists capabilities, but its request carries none, so a plain client has no way to
  offer events. Adding that is a separate change to `hello`.
- **An assistant built into the client, and where its model keys live.** §11 covers agents acting through the server
  only.
- **Push for anything other than changes:** progress of a long action, notifications. The `event` envelope has a
  topic, so they can be added without another negotiation, but none is defined here.
