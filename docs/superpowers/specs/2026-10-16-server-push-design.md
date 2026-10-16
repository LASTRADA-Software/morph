# Server push: actions declare what they change, the server tells the GUI — design (spec 8)

The server tells every client that shows a piece of data when that data changes, so the GUI updates without
being asked. What changes is declared **once, on the action**: an action says which record types it
updates or changes, and the framework works out which cached queries, in which clients, show them. A UI
document then declares only what a screen **shows** (its queries) and what an edit **sends** (its
mutations). It no longer declares what to refresh after an action.

The same mechanism updates the GUI however the change was made: by this screen, by another screen of the
same client, by another user, or by an agent acting through the server.

## Contents

[1 Intent](#1-intent) · [2 What exists today](#2-what-exists-today) · [3 Concepts](#3-concepts) ·
[4 Declaring records and effects](#4-declaring-records-and-effects) · [5 The catalog](#5-the-catalog) ·
[6 The server-to-client channel](#6-the-server-to-client-channel) · [7 Watching](#7-watching) ·
[8 Fan-out on the server](#8-fan-out-on-the-server) · [9 The client cache](#9-the-client-cache) ·
[10 Documents](#10-documents) · [11 Agents through the server](#11-agents-through-the-server) ·
[12 Security](#12-security) · [13 Tests](#13-tests) · [14 Delivery](#14-delivery) ·
[15 Out of scope](#15-out-of-scope)

## 1. Intent

**Today**, a LIMS capture dialog declares what to refresh:

```json
"capture": { "model": "sample", "action": "Lims_CaptureConcentration", "body": { ... },
             "invalidates": ["results"] },
"results": { "model": "sample", "action": "Lims_ListResults", "body": { ... },
             "refreshOn": ["Lims_CaptureConcentration"], "refreshEvery": 5000 }
```

Every document that calls `Lims_CaptureConcentration`, and every document that lists results, has to know
about the other. A change made by another client is seen only on the next poll.

**With this design**, the action says what it changes, once, where it is registered:

```cpp
BRIDGE_REGISTER_ACTION(SampleModel, CaptureConcentration, "Lims_CaptureConcentration",
                       morph::effects::updates<ResultRow>)
```

The documents drop `invalidates`, `refreshOn` and the polling `refreshEvery`. When a capture succeeds:

- the calling client patches the new `ResultRow` into every cached query that holds it;
- the server tells every other client watching `ResultRow`, and their mounted queries showing that row refetch.

**Success criteria.**

1. A document that only shows data and sends edits updates correctly with no refresh declarations.
2. A change made by client A appears in client B's mounted screen within one push latency, with no polling.
3. An agent calling an action through the server updates every open GUI the same way.
4. The examples lose their refresh declarations, about 40 today, and their tests stay green.

## 2. What exists today

| Fact | Where |
|---|---|
| The query cache is per client session, keyed by `(model type, instance, action, canonical body)`. `invalidates`, `refreshOn` and `{"refetch"}` mark entries stale. | spec 5 §5 "The query cache" |
| `refreshOn` matches by **model type, not instance**, because two private instances of a database-backed model read the same rows. | spec 5 §5 "Queries" |
| `patch` merges a reply into a cached row found by its `id` field. `optimistic` applies the same shape before the reply. | spec 5 §5 "Mutations" |
| A change by another client is not pushed. Spec 5 §15 names a negotiated `ui-event` kind as the way to add it. | spec 5 §5, §15 |
| A bridge subscription reaches only handlers in the same process, never across connections. | `docs/spec/core/shared_instances.md` |
| `RemoteServer::handle(msg, reply, cid)` answers each request through its own `reply`. The server holds no way to send a connection a message it did not ask for. | `include/morph/core/remote.hpp` |
| Both transports track a `ConnectionId` per connection and call `closeConnection` on disconnect. | `include/morph/net/socket_server.hpp`, `src/qt/qt_websocket_server.cpp` |
| The client matches replies by `callId`. A message that answers no pending call is dropped. | `include/morph/core/wire.hpp` |
| `ActionInfo` carries `modelType`, `calculatedFields`, `secretFields`, `credential` and `listFields`. Nothing says whether an action reads or writes. | `include/morph/ui/document.hpp` |
| `BRIDGE_REGISTER_ACTION` already carries one per-action flag (`loggable`). A view's row identity is a structurally detected `rowKey`, defaulting to `"id"`. | `docs/spec/core/registry.md`, `docs/spec/forms/views.md` |

## 3. Concepts

- **Record type.** A reflected aggregate that action results carry and screens show, with a registered name
  and a key member. Examples: `ResultRow` keyed by `id`, `AccountInfo` keyed by `id`. The record type, not the
  model type, is the unit of change. One record type can be shown through several models, as when
  `CustomerModel::ListAccounts` and `AccountModel::GetAccount` both return `AccountInfo`.
- **Effect.** What a successful action did to record types. One of:
  - `updates<R>`: the result carries the new state of the `R` records it changed, identified by their keys.
  - `changes<R>`: records of `R` were created, removed or changed in a way the result does not carry.
  - nothing: the action reads.
- **Change.** What the framework derives from an effect after a successful execute: `{record, keys?}`. Keys
  are present for `updates`, taken from the result. They are absent for `changes`, which means "any record of
  this type".
- **Watch.** A client's statement of which record types its cached queries hold. It is maintained by the
  client and authorised by the server.
- **Event.** A server-to-client message carrying changes, sent to the connections watching their record types.

The unit is the record type, not the model instance, for the reason `refreshOn` already matches by model type:
two private instances of one database-backed model read the same rows.

## 4. Declaring records and effects

### Records

A record type is declared where its codec already is, in the same structural way as a view's `rowKey`:

```cpp
// Detected structurally: a member `id`, or an explicit trait.
template <> struct morph::model::RecordTraits<bank::dto::AccountInfo> {
    static constexpr std::string_view name = "Bank_Account";
    static constexpr auto key = &bank::dto::AccountInfo::id;
};
// or the macro: BRIDGE_REGISTER_RECORD(bank::dto::AccountInfo, "Bank_Account")  // key defaults to `id`
```

- The key is an integral or `std::string` member, the types `ModelKey` already admits. A record type with no
  key can only be named by `changes<R>`.
- The record registry is process-wide and filled during the registration phase, like the action registry, and
  it is checked by the same latch.

### Effects

Effects are an optional trailing argument of the registration macro. The macro is already where the framework
meets the application's types. A DTO stays free of morph types, which keeps invariant 1: the model does not
know the framework exists.

```cpp
BRIDGE_REGISTER_ACTION(AccountModel, Deposit,      "Deposit",      morph::effects::updates<AccountInfo>)
BRIDGE_REGISTER_ACTION(AccountModel, Transfer,     "Transfer",     morph::effects::updates<AccountInfo>)
BRIDGE_REGISTER_ACTION(PayeeModel,   AddPayee,     "AddPayee",     morph::effects::changes<PayeeInfo>)
BRIDGE_REGISTER_ACTION(PayeeModel,   RemovePayee,  "RemovePayee",  morph::effects::changes<PayeeInfo>)
BRIDGE_REGISTER_ACTION(PayeeModel,   ListPayees,   "ListPayees")   // reads: no effects
```

- **`updates<R>` finds the records in the result.** The result may be an `R`, a `std::vector<R>`, or an
  aggregate whose members, found by reflection, hold an `R` or a `std::vector<R>`. `Transfer` returns both
  accounts this way. A result in which none is found is a registration error, reported when the program starts,
  not when the action first runs.
- **`changes<R>` needs nothing from the result.** It covers creation and removal, whose effect on a list the
  framework cannot compute: whether a new payee belongs in a filtered list is the server's query to answer.
- **Several effects compose:** `morph::effects::updates<Order>, morph::effects::changes<OrderLine>`.
- **No declaration means the action reads, and nothing is pushed.** This default under-notifies rather than
  over-notifies. An action wrongly treated as a write would make every fetch invalidate the queries that
  showed it, and they would refetch in a loop. An action that writes and declares nothing behaves as it does
  today. §10's load-time check finds the documents that still rely on refresh declarations for it.
- **Only success has effects.** A failed action, or one that was cancelled, changes nothing.

`ActionTraits<A>` gains `effects()`, returning the list as data (`{record name, kind}`). The framework reads
effects only through that function.

## 5. The catalog

`ActionInfo` gains two facts, filled from the registry in local mode and served by `ui-catalog` remotely:

| Field | Meaning |
|---|---|
| `effects` | The action's effects: `[{record, kind: "updates" \| "changes"}]`. |
| `records` | The record types the action's **result** carries, found by the same reflection as `updates<R>`. A cached query holds exactly these. |

`records` lets a client decide, without the server, whether a cache entry holds record `R`: the entry's action's
`records` contains `R`. That is the whole link between "R changed" and "this query must update".

## 6. The server-to-client channel

The wire gains its first server-initiated message. It is a core facility, not a UI one: the core `hello`
negotiates it, and the UI layer is its first user.

- **Negotiation.** The core `hello` gains a capability list, and the client offers `events`. A server sends
  events only to connections that offered it. A client that never offers it sees today's protocol unchanged.
- **The envelope.** `kind: "event"`, `callId: 0`, `body: {topic, payload}`. The topic is a string, `changes`
  for this design. A client routes it by kind before reply matching, so an event is never mistaken for an
  orphaned reply.
- **The server side.** `RemoteServer` gains `openConnection(cid, send)`, with `closeConnection(cid)` as its pair.
  A transport registers the connection's outbound writer when the connection opens.
  `RemoteServer::post(cid, envelope)` writes through it on the transport's thread, under the same size cap
  (`kMaxEnvelopeBytes`) and the same write-failure handling as replies.
- **The client side.** `IBackend` gains `setEventHandler(fn)`, delivered on the bridge's executor. The remote
  backend calls it for each `event`. The local backend calls it in-process, with no wire: local mode gets the
  same behaviour from the same code above the backend.
- **Order.** Events on one connection arrive in the order the server posted them. No order is promised between
  an event and an unrelated reply.

## 7. Watching

A client watches the record types its cache holds, so the server sends each connection only what it can show.

- **Lifetime.** A record type is watched while at least one cache entry holds it, mounted or kept (spec 5 §5:
  for 5 minutes). The client counts references per record type. It sends `watch` when a count leaves zero and
  `unwatch` when it returns to zero. Both are core kinds with `{records: [name, ...]}` and an `ok` reply.
- **Batching.** Watch changes made in one reactive batch go out as one message, so mounting a screen with ten
  queries sends one `watch`.
- **Reconnect.** After a reconnect or a backend switch, the client sends its whole watch set again and marks
  every entry stale, as spec 6 §9 already does for a backend switch. Events missed while disconnected are not
  replayed: the stale marks cover them.
- **Granularity.** Watching is by record type, not by key. Per-key watching would need the server to hold
  every key every client shows, and keys change as lists scroll. The event carries keys, and the client filters
  them (§9).

## 8. Fan-out on the server

After an `execute` succeeds, and before its reply is written:

1. The server reads the action's effects and derives the changes: record name and, for `updates`, the keys found
   in the result.
2. It finds the connections watching those record types, **excluding the connection that made the call**, whose
   client applies the effects from its own reply (§9).
3. It filters each connection's view by authorisation (§12) and posts one `changes` event per connection.

- **Coalescing.** Changes for one connection are merged for up to 50 ms, or until 256 keys are queued, then sent
  as one event. Keys are merged per record type; a `changes` for a type absorbs its `updates` keys. One hundred
  writes in a burst are then one refetch per showing query, not one hundred.
- **Where the watch table lives.** On the server strand, next to the connection scopes `closeConnection` already
  reclaims. A closed connection drops its watches.
- **Every path that executes reaches fan-out:** the UI service, the plain `execute` kind, an offline replay, and
  an agent (§11), because fan-out sits on the dispatch path they share.

## 9. The client cache

The interpreter's query cache applies a change the same way whether it came from its own reply or from an event.

| Change | Own reply (the caller) | Event (another connection) |
|---|---|---|
| `updates R` with keys | Patch: each entry holding `R` merges the new records into the rows with those keys, using the existing `patch` merge and the catalog's `listFields` for a list inside a record. No refetch. | Each entry holding `R` whose value contains one of the keys is marked stale, and refetched if mounted. |
| `changes R` | Each entry holding `R` is marked stale, and refetched if mounted. | The same. |

- **Why other clients refetch rather than receive values.** The new values were computed for the caller. Another
  user may see different fields (`secretFields`) or may not be allowed the record at all. A refetch goes through
  the receiver's own authorised read. Pushing values to other receivers is §15's first deferred item.
- **Batching.** One event, or one reply's effects, is applied in one reactive batch, so a screen updates in one
  frame.
- **`optimistic` is unchanged.** It still shows a guess before the reply. The reply's patch then confirms or
  corrects it, exactly as a declared `patch` does today.
- **Entries no query mounts** are only marked stale. They refetch if a screen mounts on them.

## 10. Documents

With effects declared, a document needs no refresh declarations. The vocabulary changes in two steps.

**Step 1, additive within `ui/1`.** `invalidates`, `patch` and `refreshOn` stay valid and keep working, and
effects apply as well. The loader, which already has the catalog, reports:

- **redundant:** an `invalidates` or `refreshOn` that the mutation's action's effects already cover. This is a
  warning, so a document can migrate one screen at a time.
- **uncovered:** a mutation whose action declares no effects, with no `invalidates` or `patch` of its own and
  no query naming it in `refreshOn`. After it succeeds, nothing on any screen updates. This is a warning,
  because the action may genuinely change nothing that is shown. It points at the actions still to annotate.

**Step 2, removal.** Once the examples have migrated, `invalidates`, `patch` and `refreshOn` are refused at load.
A document that needs to refetch for a reason no action declares uses the existing `{"refetch": "q"}` command.
`refreshEvery` stays, for data that changes with time rather than through actions, such as a clock or an
external feed. Whether step 2 waits for a vocabulary version (`ui/2`) or happens inside `ui/1` is decided when
the examples have migrated (§14, part 8).

## 11. Agents through the server

An agent acts on the application through the same dispatch as everything else, so this design is what makes its
changes visible.

- **The MCP adapter** is a server-side component. It lists the actions a principal may run (the catalog's
  `permitted`, with their JSON shapes from the registry) as MCP tools. It runs each tool call through
  `ActionDispatcher` as that principal, with the same `authorize` and `authorizeInstance` checks, the same journal
  entry and the same `ui-evaluate` dry run as a GUI client. The agent may commit directly.
- **Fan-out treats the adapter as one more connection**, so every open GUI showing the changed records updates
  through §8 and §9.
- **Model access and keys are not part of this design.** The adapter does not call a model provider. An external
  agent (Claude Desktop, Claude Code) brings its own. Where keys would live for an assistant built into the client
  is still undecided, and it does not affect anything above.

## 12. Security

- **A watch reveals nothing by itself.** The server accepts a watch for record type `R` only when the principal
  may run at least one action whose `records` contain `R`. Otherwise it refuses with `err`, and the client logs it.
- **An event reveals that records of `R` changed, and their keys.** Keys are ids the receiver could already see
  through some action returning `R`, but not necessarily the same records. Where that matters, because a model
  filters rows per principal, a record type registered with `keys: private` is pushed without keys to principals
  other than the caller's. Every query holding `R` then refetches, which costs more and reveals nothing.
- **Values never travel to other connections** (§9), so `secretFields` and row-level filtering stay on the read
  path, where they are enforced today.
- **Rate.** Coalescing (§8) bounds events per connection to 20 a second. A connection that cannot drain its
  outbound queue past the transport's existing limit is closed, as for a slow reader of replies.

## 13. Tests

Every test below must fail if the feature does nothing, so each names the mutation that should break it.

- **Records and effects (core).** Registration extracts keys from an `R`, a `vector<R>` and a nested member. A
  result with no `R` under `updates<R>` fails registration. *Mutation: drop the reflection walk; key extraction
  tests fail.*
- **Channel (core, net, qt).** The same conformance case is run on both transports and on the local backend: a
  posted event reaches the client's handler. An event to a client that did not offer `events` is never sent.
  An event is not taken for a reply. *Mutation: route events through reply matching; the routing case fails.*
- **Fan-out (core).** Two connections, both watching `R`. An `updates<R>` execute from A posts to B and not to A.
  An unwatched type posts nothing, and a burst of 100 coalesces to one event. *Mutation: remove the origin
  exclusion; A receives its own event.*
- **Cache (ui).** A recording-backend screen with a list query and no refresh declarations. An own reply patches
  the row without a fetch, and the fetch counter stays flat. An event marks it stale and refetches it.
  *Mutation: skip the cache step; the shown row keeps its old value.*
- **End to end (examples).** One rung runs two clients against one server. Client A's mutation updates client B's
  mounted screen with no `refreshOn` and no `refreshEvery` in either document. *Control: unregister the action's
  effect; B does not update.*

## 14. Delivery

Each part lands as its own pull request to `master`. Parts with no dependency between them can run side by side.

| # | Part | Layer | Needs |
|---|---|---|---|
| 1 | Records and effects: `RecordTraits`, `BRIDGE_REGISTER_RECORD`, `morph::effects`, `ActionTraits::effects()`, change derivation from a result | core | none |
| 2 | Server-to-client channel: core `hello` capabilities, `event` kind, `openConnection` and `post`, both transports, `setEventHandler`, in-process delivery in the local backend | core, net, qt | none |
| 3 | Catalog: `ActionInfo::effects` and `records`, local lookup and `ui-catalog` | ui | 1 |
| 4 | Watching and fan-out: `watch` and `unwatch` kinds, the watch table, origin exclusion, coalescing, authorisation (§12) | core | 1, 2 |
| 5 | Client cache: apply effects from own replies and from events; watch reference counting; reconnect | ui | 2, 3 (4 for its remote tests) |
| 6 | Document checks: redundant and uncovered warnings; spec 5 §5 updated | ui | 3, 5 |
| 7 | Examples: declare records and effects in the seven rungs that use refresh declarations, remove those declarations, and add the two-client end-to-end test | examples | 5, 6 |
| 8 | Removal: refuse `invalidates`, `patch` and `refreshOn` at load, in `ui/1` or as `ui/2` (decided then) | ui | 7 |
| 9 | MCP adapter: catalog actions as tools, run as a principal, through fan-out | net, ui | 4 |

**Ready now:** parts 1 and 2. They are independent, and both are core. **Natural pairs:** 1 ∥ 2, then 3 ∥ 4.
Part 9 can run beside anything after part 4. Part 5 runs beside part 4: the local backend delivers events
in-process, so its local tests need no server fan-out, and only its remote tests wait for part 4.

## 15. Out of scope

- **Pushing values to other connections.** Receivers refetch through their own authorised read. Sending the new
  record, filtered per receiver, would save that round trip. It needs per-receiver field filtering on the server,
  and it is the first follow-up once refetch traffic is measured.
- **Per-key and per-field watching.** Watching is by record type, and the event carries keys. Field-level effects
  (`updates<R>` naming fields) would let a client skip refetches for fields a screen does not show. That is
  deferred until a measurement shows refetches cost something.
- **Effects computed at run time** beyond the records found in the result, such as an explicit change list a model
  returns. `updates<R>` over a reflected result covers the known cases, `Transfer` included.
- **An assistant built into the client, and where its model keys live.** §11 covers agents acting through the
  server only.
- **Server push for anything other than changes:** progress of a long action, notifications. The channel (§6) has
  topics so they can be added without another negotiation, but none is defined here.
