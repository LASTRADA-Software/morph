# Keeping every screen current: records and effects

When an action changes data, every screen that shows that data refreshes: on the client that ran it, on every other
connected client, and when an AI agent runs it over MCP. Two things make that happen, and neither of them is in a
screen.

- **A record** names a result type that is shared data, such as a bookmark, an account or a sample, and says how to
  tell its rows apart.
- **An effect** declares, beside an action's registration, which records the action changes.

The server tells every connection that is showing a changed record, and each client refreshes what it holds. A
screen only says what it shows and what an edit sends.

This guide is for people and for coding agents. It covers what you write, how to choose an effect, what refreshes
when, every error you can hit and its fix, and how to test it. The authoritative reference is in
[`docs/spec/`](spec/): see [Where the rules live](#where-the-rules-live).

## Contents

- [The three things you write](#the-three-things-you-write)
- [Choosing an effect](#choosing-an-effect)
- [Records](#records)
- [What refreshes, and when](#what-refreshes-and-when)
- [Screens](#screens)
- [Checklist for a new or changed action](#checklist-for-a-new-or-changed-action)
- [Errors and warnings, and their fixes](#errors-and-warnings-and-their-fixes)
- [Testing](#testing)
- [Agents over MCP](#agents-over-mcp)
- [Seeing it in the explorer](#seeing-it-in-the-explorer)
- [Limits](#limits)
- [Where the rules live](#where-the-rules-live)

## The three things you write

The examples are from [`examples/bookmarks`](../examples/bookmarks).

**1. Records, once per result type, beside the DTO.** The macro is in `<morph/core/records.hpp>`.

```cpp
// bookmark_dto.hpp: a bookmark shown two ways, as a list row and as the full detail view
BRIDGE_REGISTER_RECORD(bookmarks::BookmarkSummary, "Bookmarks_Bookmark", ::morph::record::privateKeys)
BRIDGE_REGISTER_RECORD(bookmarks::BookmarkView, "Bookmarks_BookmarkDetail", ::morph::record::privateKeys)

// tag_dto.hpp
BRIDGE_REGISTER_RECORD(bookmarks::TagSummary, "Bookmarks_Tag", ::morph::record::privateKeys)
```

The key is the member named `id` unless you say otherwise. `privateKeys` keeps one user's row keys away from other
users' clients.

**2. Effects, beside each action that changes something.** The macro is in `<morph/core/effects.hpp>`.

```cpp
// tag_model.hpp: renaming a tag renames it on every bookmark that carries it
BRIDGE_REGISTER_ACTION(bookmarks::TagModel, bookmarks::RenameTag, "RenameTag")
BRIDGE_REGISTER_EFFECTS(bookmarks::RenameTag, ::morph::effects::changes<bookmarks::TagSummary>,
                        ::morph::effects::changes<bookmarks::BookmarkSummary>,
                        ::morph::effects::changes<bookmarks::BookmarkView>)

// bookmark_model.hpp: the reply is the edited bookmark's full view, so an open detail view is patched in place
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::EditBookmark, "EditBookmark")
BRIDGE_REGISTER_EFFECTS(bookmarks::EditBookmark, ::morph::effects::updates<bookmarks::BookmarkView>,
                        ::morph::effects::changes<bookmarks::BookmarkSummary>,
                        ::morph::effects::changes<bookmarks::TagSummary>)

// A read declares nothing. An action a screen runs as a mutation but that changes nothing says so:
BRIDGE_REGISTER_EFFECTS(bookmarks::ExportBookmarks, ::morph::effects::none)
```

**3. Screens that say only what they show and what they send.**

```cpp
auto const tagModel = screen.model<TagModel>("tagModel", {.instance = ui::Instance::Private});
auto const tags = screen.query<ListTags>("tags", tagModel, {});
auto const picked = ui::element("tagList");
return ui::row({
    ui::table(tags.value().at(&ListTagsResult::tags), {.key = ui::row("id"), /* columns */}).id(picked),
    ui::form<RenameTag>(tagModel, {.prefill = ui::obj({{"id", ui::call("first", {picked.selection()})}}),
                                   .fields = {{&RenameTag::id, {.hidden = true}}}}),
});
```

Nothing here says what to refresh. When the rename succeeds, this table refetches, and so does every other screen
holding tags, bookmark rows or a bookmark's detail, on every connected client.

A screen written as JSON is the same:

```json
{ "vocab": "ui/1", "id": "bookmarks.tags",
  "models": { "tagModel": { "type": "TagModel", "instance": "private" } },
  "queries": { "tags": { "model": "tagModel", "action": "ListTags" } },
  "mutations": { "rename": { "model": "tagModel", "action": "RenameTag" } },
  "view": { "kind": "text", "text": "…" } }
```

## Choosing an effect

Ask what the action does to the data, and what its reply carries.

| The action… | Declare | What happens |
|---|---|---|
| changes rows of `R`, and its reply holds those rows with their keys | `updates<R>` | The caller's screens show the reply's rows at once, before any fetch. Every watching client, the caller included, then refetches the entries holding one of those keys; for a `privateKeys` record, other principals refetch every entry holding `R`. |
| changes rows of `R`, but its reply does not hold them (a delete, a bulk edit, an import, a reply of `Ack` or `bool`) | `changes<R>` | Every cache entry holding an `R`, on every watching client, is marked stale and refetched. |
| changes the state of the model instance it ran on, not a shared record | `changesInstance` | On the caller, entries read through that instance refresh. A keyed shared instance's change also reaches the other clients watching it; a private instance's change reaches only the connection that holds it. |
| changes nothing, but a screen runs it as a mutation (open, attach, export) | `none` | Nothing refreshes, and the document checks stay quiet. |
| only reads | nothing | It is a read. A screen runs it as a query. |

Rules of thumb:

- **List every record the action changes**, not only the one in its name. Renaming a tag changes the bookmarks that
  carry it. Archiving a bookmark changes the tag counts.
- **Prefer `updates<R>` when the reply really is the new row.** The caller's screens show it without waiting for a
  fetch, and other clients refetch only the entries holding the changed keys. If the reply is a different shape of the
  same data, such as a full view while the lists show a summary, declare `updates` for the
  shape the reply holds and `changes` for the others, as `EditBookmark` does.
- **When unsure, declare `changes<R>`.** It refetches more than it needs to, but it never misses a change.
- **`updates<R>` and `changes<R>` together mean the whole type**: the change is reported without keys.
- Declare each action's effects in **one** place. A header included by several translation units is still one
  declaration; a second, different declaration for the same action is refused.

## Records

```cpp
BRIDGE_REGISTER_RECORD(Type, "WireName", options...)
```

| Option | Meaning |
|---|---|
| (none) | The key is the member glaze reflects as `id`. A type with no `id` member and no `key<>` has no key, like `keyless`. |
| `::morph::record::key<&T::member>` | The key is another member. A strong id (`WrappedModelKey`) works as well as a raw integer or string. |
| `::morph::record::keyless` | The record has no key: it can only be changed as a whole type (`changes<R>`), never `updates<R>`. |
| `::morph::record::derivedFrom<S...>` | A change to any `S` is a change to this record. For a report or a summary computed from other records. A derived record must be keyless. |
| `::morph::record::privateKeys` | Row keys reach only the principal who made the change. Every other watching principal is told the whole type changed, without keys. Use it when a key alone would reveal something, such as which accounts exist. |

**What an action's result holds.** The registration of each action walks its result type and records the paths at
which it holds each record type. The result type itself counts, at the path `""`: `EditBookmark` returns a
`BookmarkView`, so it holds `Bookmarks_BookmarkDetail`. The walk looks through `std::vector`, `std::optional` and
`std::map`, so a `ListBookmarksResult` holding `std::vector<BookmarkSummary> bookmarks` holds `Bookmarks_Bookmark` at
`bookmarks`. It stops at the first record on each path, so a record's own members are not searched for further
records. `morph::effects::recordsOf("ListBookmarks")` answers what an action holds, with each record's key field and
paths, and the explorer shows the same.

A derived record, from [`examples/bank`](../examples/bank):

```cpp
// A spending report sums the ledger, so whatever adds a transaction changes every report.
BRIDGE_REGISTER_RECORD(bank::dto::SpendingReport, "Bank_SpendingReport", ::morph::record::keyless,
                       ::morph::record::derivedFrom<bank::dto::TxnInfo>)
```

Where to put the macros: `BRIDGE_REGISTER_RECORD` beside the DTO, and `BRIDGE_REGISTER_EFFECTS` directly under the
action's `BRIDGE_REGISTER_ACTION`. A header is fine for both: every translation unit that includes it registers the
same declaration again, which is a no-op, and one that does not include it still reads the same effects. Wire names
follow the application's prefix, as `Bookmarks_Bookmark` and `Bank_Account` do. Under
`MORPH_CLIENT_ONLY` both expand to nothing.

## What refreshes, and when

1. **On the caller,** as soon as the reply arrives, the reply's effects are applied to its own cache.
    - `updates<R>`: rows of `R` in every cached query holding them are replaced, by the record's key, at the paths the
      catalog gives, so the screen shows the new values before any fetch. The server's event for the same change then
      refetches the entries holding those keys, as on every other client.
    - `changes<R>` and `changesInstance`: the entries holding the record, or read through the instance, are marked
      stale. A mounted query on a stale entry refetches.
    - This covers mutations and the submits of forms, collections and wizards alike.
2. **On the server,** a successful execute derives its changes from the declared effects and the result. It sends
   them to every connection watching a changed record, the caller's included, after re-checking that each watcher
   may still see it.
3. **On every other client,** the change is applied the same way, filtered by its keys when it carries them.

The client watches the records of what its cache holds, and sends the watch before the first fetch, so no change is
missed in between. You never write a watch. After a reconnect the client watches everything again and refetches what
it shows, since it may have missed changes while it was away.

A fetch that is in flight when a change arrives fetches once more when it settles, because it may have read the data
before the change. Changes are coalesced per connection for 50 ms (`ServerConfig::coalesceWindow`), or
until 256 keys and topics are queued. A reader that falls behind, with more than 1 MiB unread, is sent no more events
until it is below 256 KiB again; its replies are never held back. Then it gets one `resync`, which refreshes everything
it watches.

## Screens

What a screen declares:

- **Queries** read. A query whose action declares effects is refused: it would change data every time it fetched.
  An `evaluate` query runs no handler, so it may name any action.
- **Mutations** run actions. They declare `body`, `concurrency`, `serialKey`, `optimistic`, `onSuccess`, `onError`
  and `offline`, and nothing about refreshing.
- **`optimistic`** writes a predicted row into a query's cached list as soon as the run starts, and rolls it back on
  failure. It is for instant feedback and is independent of effects.
- **`refetch`** (`ui::refetch(query)`, or `{"refetch": "q"}`) refetches a query for a reason no action can declare.
- **`refreshEvery`** polls, for data that changes with time rather than through actions, such as a clock or an
  external feed.

What a screen may not declare: `invalidates`, `patch` and `refreshOn`. A document that names one is refused, pointing
at the key. They would restate, per screen, what the action's effects already say once for every client, and the two
would drift apart. The C++ builders offer none of them, so authored screens cannot name them.

## Checklist for a new or changed action

Work through it every time you add an action or change what one does. A coding agent should treat it as required.

1. **Does the action change persistent data?** If not, declare nothing: it is a read. If a screen runs it as a
   mutation anyway, declare `none`.
2. **Which record types does it change?** Include the knock-on ones: counts, summaries and other views of the same
   rows. Each needs a `BRIDGE_REGISTER_RECORD` (`<morph/core/records.hpp>`).
3. **For each, does the reply hold the changed rows with their keys?** It does when the result type is the record, or
   holds it through members, `std::vector`, `std::optional` or `std::map`, and the record has a key. Then
   `updates<R>`. Otherwise `changes<R>`. When unsure, `morph::effects::recordsOf("Action")` says what it holds.
4. **Is a record computed from others?** Register it with `keyless` and `derivedFrom<S...>` instead of adding it to
   every action's effects.
5. **Would a key alone leak something to another user?** Add `privateKeys` to the record.
6. **Write `BRIDGE_REGISTER_EFFECTS` (`<morph/core/effects.hpp>`) directly under the action's
   `BRIDGE_REGISTER_ACTION`.**
7. **Remove any refresh the screens did by hand**: an `onSuccess` refetch that the effects now cover.
8. **Build and run the screen tests.** A bad declaration fails when the server starts. Check that the bundle loads
   with no warnings ([Testing](#testing)).

## Errors and warnings, and their fixes

**When the server starts.** Records and effects are validated together on the first read of the registries: in
`RemoteServer`'s constructor, or in local mode when the first screen is registered. A problem throws
`morph::model::EffectsRegistrationError`, whose message lists every problem, one per line.

| Message (abridged) | Fix |
|---|---|
| ``action `A` declares updates on record `R`, which has no key`` | Give `R` a key (an `id` member, or `key<&R::member>`), or declare `changes<R>`. |
| ``action `A` declares updates on record `R`, but its result holds no `R` `` | The reply does not carry the rows: declare `changes<R>`. |
| ``action `A` declares changes<T>, which is not a registered record`` (or `updates<T>`; `T` is the compiler's type name) | Register `T` with `BRIDGE_REGISTER_RECORD`, or name the record type you meant. |
| ``action `A` declares different effects in two registrations`` | Keep one `BRIDGE_REGISTER_EFFECTS` per action. |
| ``action `A` declares none together with other effects`` | `none` stands alone. Drop it, or drop the others. |
| ``record `D` is derived from other records but has a key`` | A derived record is keyless: add `keyless` and remove the key. |
| ``record `D` is derived from S, which is not a registered record`` | Register `S`, or fix the `derivedFrom` type. |
| ``record `D` is derived from itself: D -> … -> D`` | Break the `derivedFrom` cycle. |
| ``record name `N` is registered for two types`` | Give each record type its own wire name. |
| ``type T is registered under two record names`` | Register each type once, under one name. |
| ``record `N` is registered twice with different options`` | The same record is registered with different options in two places, such as two headers. Keep one registration. |
| ``action `A` is registered by two action types; give each its own id`` | Two action types share an id in `BRIDGE_REGISTER_ACTION`. |

**When it compiles.** A record with two `key<>` options, with both `keyless` and `key<>`, or whose key or `id` member is
not a `ModelKey`, fails a `static_assert`. So does an effects declaration that is empty or names something other than
`updates<R>`, `changes<R>`, `changesInstance` or `none`.

**When a screen loads.** These come from `ui::loadDocument`, which also runs when a screen is registered on the
server, so a refused screen shows up in the server's own tests.

| Message (abridged) | Fix |
|---|---|
| ``a query reads, so its action declares no effects; `A` declares …`` | Run `A` as a mutation, or, if it really only reads, remove its effects. |
| ``a document does not declare `invalidates` `` (or `patch`, or `refreshOn`) | Delete the key. Declare the action's effects instead, or use `refetch` for a refresh no action declares. |

**Warnings.** A warning never refuses a document. Registration logs each one at `warn`.

| Warning | Fix |
|---|---|
| ``nothing refreshes after `m` succeeds: `A` declares no effects, and no `refetch` follows it`` | Declare `A`'s effects, or `effects::none` if it changes nothing. |
| A `watch` whose `on` reads a query and whose `do` runs a mutation with effects | Another client's change would refetch the query, and the watch would run the mutation again, possibly in a loop between clients. Add `"source": "user"` so it runs only on the user's own edits. |

## Testing

- **The bundle loads cleanly.** Each rung has a test that loads every screen against the catalog the server serves
  and expects no refusal and no warning
  ([`examples/common/testkit/bundle_warnings.hpp`](../examples/common/testkit/bundle_warnings.hpp)):

  ```cpp
  auto const bundle = bank::screens::bundle();
  REQUIRE(bundle.has_value());
  CHECK(morph::examples::testing::bundleLoadFindings(**bundle).empty());
  ```

- **Effects are declared.** Read them back in a unit test with `morph::effects::effectsOf("RenameTag")`, see what a
  result holds with `morph::effects::recordsOf("ListBookmarks")`, or derive a change from a sample result with
  `morph::effects::deriveChanges(actionId, resultJson, ranOn)`.
- **Two clients see each other.**
  [`tests/net/test_push_two_clients.cpp`](../tests/net/test_push_two_clients.cpp) runs two sessions against one server
  over a real socket: client A's mutation refreshes client B's mounted screen, with no refresh declared anywhere.
- **Check that a test can fail.** Remove one effect and confirm that the screen test which depends on it fails. A
  passing test proves nothing until you have seen it fail.

## Agents over MCP

An AI agent acts on the application through the same server, on a connection of its own. Its calls get every check a
GUI's calls get, and every open screen watching what it changes refreshes. `morph::ui::McpAdapter` lists one tool
per action the agent's principal may run, plus an `evaluate` dry run. `serveStdio` serves it over MCP's stdio
transport.

Bank's server serves one with `--mcp-stdio`:

```sh
claude mcp add bank -e MORPH_MCP_PRINCIPAL=alice \
    -e BANK_DB="DRIVER=SQLite3;Database=$PWD/bank.db" -e BANK_PORT=0 \
    -- ./build/examples/bank/ladder_bank_server --mcp-stdio
```

See [`examples/bank/README.md`](../examples/bank/README.md) for the full setup, and
[`docs/spec/ui/agent.md`](spec/ui/agent.md) for the adapter.

## Seeing it in the explorer

The app explorer's facts for an action show its effects (`changes Bank_Account`, `updates Lims_Result`,
`changes its instance`, `none`, or `reads`) and the records its result holds, each with its key field and paths. Use
it to check what a screen will refresh before wiring one up.

## Limits

- **More than 4096 changed keys** from one execute are reported as a whole-type change (`kMaxChangeKeys`).
- **A connection watches at most 4096 topics** (`wire::kMaxWatchTopics`). Past that, a new topic is refused, and the
  client is not told which. Its entries then refresh only from that client's own replies, on a `resync` and on
  reconnect.
- **`updates<R>` cannot move a row to a new key.** If an action replaces a row under a new key, declare `changes<R>`,
  as lims's re-capture does.
- **Effects are declared, not computed.** An action that changes different records depending on its input declares
  all of them.
- **A keyed row without its key member** in a reply is skipped when the changed keys are collected.

## Where the rules live

| Question | Reference |
|---|---|
| Exactly what the record and effects macros accept and refuse | [`docs/spec/core/registry.md`](spec/core/registry.md), "Record registry" and "Effects" |
| The `watch`, `unwatch` and `event` wire kinds | [`docs/spec/core/wire.md`](spec/core/wire.md), "The `event` kind" |
| Fan-out, coalescing, the backlog and re-authorisation on the server | [`docs/spec/core/backend.md`](spec/core/backend.md), "Fan-out" |
| How the client cache applies replies and events | [`docs/spec/ui/interpreter.md`](spec/ui/interpreter.md), "Queries and the cache" |
| The document rules and warnings | [`docs/spec/ui/document.md`](spec/ui/document.md) |
| The MCP adapter | [`docs/spec/ui/agent.md`](spec/ui/agent.md) |
| The design, and why it is shaped this way | [`docs/superpowers/specs/2026-10-16-server-push-design.md`](superpowers/specs/2026-10-16-server-push-design.md) |
