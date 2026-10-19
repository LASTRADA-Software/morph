# Getting started: building an app on morph

This is the document that comes before the others. It builds one small thing
end to end and explains why each piece exists, using
[`examples/bookmarks`](../examples/bookmarks) — the smallest application on
morph's [application ladder](../examples/LADDER.md) — as the worked example.

Read this first, then
[`docs/ARCHITECTURE.md`](ARCHITECTURE.md) for the narrative map, then
[`docs/spec/`](spec/) for the authoritative per-subsystem reference. Those two
are precise and assume you already know which subsystem you need; this one
assumes you do not.

**What you should be able to do at the end:** write a model, register it, call
it from a UI, move it behind a socket without touching the call site, and say
which spec file answers your next question.

**What this is not.** It is not a spec — where this document and a file under
[`docs/spec/`](spec/) disagree, the spec wins. It is also not a survey: the
journal and the offline queue are named here only where a first app trips
over them. Sessions get a little more room, because bookmarks is multi-user
and every one of its actions reads the caller's principal.

## Contents

- [1. What morph is for](#1-what-morph-is-for)
- [2. The whole seam in one file](#2-the-whole-seam-in-one-file)
- [3. Building and running bookmarks](#3-building-and-running-bookmarks)
- [4. The model is the application](#4-the-model-is-the-application)
- [5. Strong ids, DTOs, and what `validate()` is for](#5-strong-ids-dtos-and-what-validate-is-for)
- [6. Persistence](#6-persistence)
- [7. Registration, and what it buys](#7-registration-and-what-it-buys)
- [8. The bridge: `Completion<T>` and the executor](#8-the-bridge-completiont-and-the-executor)
- [9. Screens on top](#9-screens-on-top)
- [10. Forms from the compiled action type](#10-forms-from-the-compiled-action-type)
- [11. Testing](#11-testing)
- [12. The payoff: making it remote](#12-the-payoff-making-it-remote)
- [13. What morph does not do](#13-what-morph-does-not-do)
- [14. Where to go next](#14-where-to-go-next)

---

## 1. What morph is for

morph is a typed, asynchronous bridge between a GUI thread and business-object
models. You write the model as **plain, single-threaded C++** with typed
action structs; the framework owns the concurrency (one *strand* per model
instance, so a model author never touches a mutex), the marshalling (results
arrive on the executor you chose, typically your UI event loop) and the
transport (the same registered action travels a JSON wire protocol to a remote
server with no change to the model or the call site).

That last clause is the reason for most of the shapes in this document.
**Deployment transparency is the design goal**: a model may live in this
process or behind a socket, and the code that calls it is identical either way.
Everything that looks like ceremony — why a result is a `Completion<T>` rather
than a return value, why every action type has a string id, why callbacks
always arrive through an executor — is that goal being paid for.

State it early because it explains the rest.

## 2. The whole seam in one file

Before any persistence, GUI or transport, here are all four moving parts with
nothing else around them. This is
[`examples/concepts/getting_started.cpp`](../examples/concepts/getting_started.cpp),
which is compiled and run by CI, so it cannot drift from what you read here.

**The actions and results are plain structs.** No base class, no macro, no
serialisation code — Glaze reflects the members.

```cpp
struct GsCreatePaste {
    std::string content;
    std::string syntax;

    [[nodiscard]] bool validate() const { return !content.empty() && !syntax.empty(); }
};

struct GsCreatePasteResult {
    std::string id;
};

struct GsGetPaste {
    std::string id;
};

struct GsPasteView {
    std::string id;
    std::string content;
    std::string syntax;
};
```

**The model is plain, single-threaded C++.** It knows nothing about morph: no
executor, no mutex, no bridge, no transport. Failures are thrown.

```cpp
class GsPasteModel {
public:
    GsCreatePasteResult execute(const GsCreatePaste& action) {
        const std::string id = "paste-" + std::to_string(_pastes.size() + 1);
        _pastes[id] = GsPasteView{.id = id, .content = action.content, .syntax = action.syntax};
        return GsCreatePasteResult{.id = id};
    }

    GsPasteView execute(const GsGetPaste& action) {
        const auto found = _pastes.find(action.id);
        if (found == _pastes.end()) {
            throw std::runtime_error{"no such paste: " + action.id};
        }
        return found->second;
    }

private:
    std::map<std::string, GsPasteView> _pastes;
};
```

**Registration names the types.**

```cpp
BRIDGE_REGISTER_MODEL(GsPasteModel, "GettingStarted_PasteModel")
BRIDGE_REGISTER_ACTION(GsPasteModel, GsCreatePaste, "GettingStarted_CreatePaste")
BRIDGE_REGISTER_ACTION(GsPasteModel, GsGetPaste, "GettingStarted_GetPaste")
```

**And the call site never says which backend it is running against.** The test
body below is generated over both deployments:

```cpp
morph::exec::MainThreadExecutor gui;      // where .then / .onError land
morph::exec::ThreadPoolExecutor pool{2};  // where models run

auto server = std::make_shared<morph::backend::RemoteServer>(pool);
morph::bridge::Bridge bridge{makeBackend(deployment, pool, server)};
morph::bridge::BridgeHandler<GsPasteModel> pastes{bridge, &gui};

std::string createdId;
bool created = false;
pastes.execute(GsCreatePaste{.content = "hello, morph", .syntax = "text"}).then([&](GsCreatePasteResult result) {
    createdId = result.id;
    created = true;
});

// The callback has *not* run yet, even in-process.
CHECK_FALSE(created);

pumpUntil(gui, created);  // ← your UI event loop turning
REQUIRE(created);
CHECK(createdId == "paste-1");
```

`makeBackend` is the only thing that differs between the two runs: a
`LocalBackend` over the worker pool, or a `SimulatedRemoteBackend` over a
`RemoteServer` — which walks the same serialise/dispatch path a real socket
does, minus the socket.

That `CHECK_FALSE(created)` is not decoration. **A `Completion` always resolves
through the executor it was issued with, never inline** — even when the model
ran in this very process. Section 8 is about what follows from that.

Run it:

```console
$ ./build/examples/concepts/morph_concepts_tests "[getting-started]"
Filters: [getting-started]
...
===============================================================================
All tests passed (21 assertions in 3 test cases)
```

## 3. Building and running bookmarks

bookmarks is [rung 1 of the application ladder](../examples/LADDER.md): a
multi-user bookmark manager with a few related entities, SQLite behind
Lightweight, signed-token sign-in, screens the server serves, and a real socket
server. It is the same four parts from section 2 at real scale.

It needs Qt 6.5+, an ODBC SQLite3 driver, and a C++23 compiler. Configure:

```sh
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON \
    -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=bookmarks \
    -DMORPH_BUILD_TESTS=ON -DMORPH_BUILD_EXAMPLES=ON
cmake --build build --target ladder_bookmarks_tests ladder_bookmarks_server morph_examples_client
```

Run the suite first — it is the fastest confirmation that your toolchain, your
ODBC driver and your Qt install all agree:

```sh
ctest --test-dir build -L ladder-bookmarks
```

Then the app. The server owns the database, the signing secret, the action
journal, the metadata-fetch worker and the outbox relay. The secret has no
default — it signs every token the server mints and verifies every token it is
shown, so a built-in fallback would be a published signing key:

```console
$ BOOKMARKS_TOKEN_SECRET="pick-something-real" \
  BOOKMARKS_DB="DRIVER=SQLite3;Database=bookmarks.db;Timeout=5000" \
  BOOKMARKS_PORT=8766 ./build/examples/bookmarks/ladder_bookmarks_server
bookmarks-server: listening on ws://127.0.0.1:8766
```

The client is the generic one every example shares: it draws whatever screens
the application serves. It runs in either deployment mode. Against the server:

```sh
./build/examples/client/morph_examples_client --server ws://127.0.0.1:8766
```

or entirely in-process, hosting the models itself:

```sh
./build/examples/client/morph_examples_client --app bookmarks --db local.db
```

Sign in with any username — the login is a development login with no password
([`dto/auth_dto.hpp`](../examples/bookmarks/include/bookmarks/dto/auth_dto.hpp)
says exactly what that does and does not mean). Two clients with two usernames
against one server show two isolated collections and a shared feed.

Once signed in, **Explorer** under the navigator's Tools (or `--screen
explorer` at start) lists the application's screens and its models with every
action: choose one to see its facts, its fields, the screens that use it, and
a live form that runs it. `--ui=tui` shows the same client in the terminal.

Section 12 comes back to what is — and is not — different between those two
command lines.

> **Note on the two modes.** Local mode is deliberately the *smaller*
> deployment, not an equivalent one: `bookmarks::app::App` — the action
> journal, the metadata-fetch worker and the outbox relay — lives only in the
> server binary, so a local-mode client journals nothing and never fills in a
> fetched title. And because `LocalBackend` runs no authorizer, local mode is
> single-user by construction: the two-user isolation is only meaningful
> against the server.

## 4. The model is the application

This is `examples/IMPLEMENTATION.md` rule 1, and the single most load-bearing
idea in the framework: **all business logic, all invariants and all persistence
access live in plain, single-threaded model classes with typed actions.**
Nothing domain-shaped may live in a screen, in a client, or in `main()`.

bookmarks' main entity is one class,
[`include/bookmarks/models/bookmark_model.hpp:53`](../examples/bookmarks/include/bookmarks/models/bookmark_model.hpp):

```cpp
class BookmarkModel {
public:
    CreateBookmarkResult execute(const CreateBookmark& action);
    BookmarkView execute(const EditBookmark& action);
    Ack execute(const ArchiveBookmark& action);
    Ack execute(const UnarchiveBookmark& action);
    Ack execute(const DeleteBookmark& action);
    BookmarkView execute(const GetBookmark& action);
    ListBookmarksResult execute(const ListBookmarks& action);
    GetChangesSinceResult execute(const GetChangesSince& action);
    BulkEditResult execute(const BulkEdit& action);
    Ack execute(const RecordMetadata& action);
    ImportBookmarksResult execute(const ImportBookmarks& action);
    ExportBookmarksResult execute(const ExportBookmarks& action);
};
```

That is the entire public surface. One overload per action, results returned by
value, failures thrown as the app's own typed error set
([`core/errors.hpp`](../examples/bookmarks/include/bookmarks/core/errors.hpp):
`NotFound`, `ValidationError`, `Conflict`, `Forbidden`, `TooLarge`). Never
encode a failure as a magic value in a result DTO — morph captures the thrown
exception as a `std::exception_ptr` and delivers it to the caller's
`.onError(...)`, and over a socket the `what()` string travels back in the
error envelope. (`TagModel`, `SharedFeedModel` and `AuthModel` sit beside it,
in the same shape.)

**A model is single-threaded per instance, and must not know the framework
exists.** morph runs each model instance on its own strand, so two `execute()`
calls on one instance never overlap — which is why `BookmarkModel` has no mutex
anywhere. It also means a model must not reach for an executor, a `Bridge` or a
background thread of its own: there is no framework seam for that,
deliberately. Background work is orchestration; it belongs at the app layer,
re-entering the model as an ordinary client call — which is exactly what
bookmarks' metadata-fetch worker does, dispatching `RecordMetadata` through the
same server a socket client reaches.

**Who is calling is read, not passed.** No action carries an owner. Each
`execute()` reads the caller's principal from `morph::session::current()`,
which the backend installs around every dispatched call, and refuses with
`Forbidden` when there is none
([`src/models/bookmark_model.cpp`](../examples/bookmarks/src/models/bookmark_model.cpp)):

```cpp
[[nodiscard]] const std::string& requireOwner() {
    const auto* ctx = ::morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw Forbidden{"no authenticated principal"};
    }
    return ctx->principal;
}
```

Over a socket, `RemoteServer` overwrites that principal with the one its
authorizer verified before the model runs. In-process nothing verifies it, so
the model's own check is the only one — which is why rule 1 makes models
re-check their own authorization whichever backend they run behind.

Write this layer first, and test it (section 11) before there is any bridge or
any screen. Every ladder rung does.

## 5. Strong ids, DTOs, and what `validate()` is for

### Why `BookmarkId` rather than `std::int64_t`

`examples/IMPLEMENTATION.md` rule 3 requires entity identity in a DTO to be a
per-entity strong id type exposing `hasValue()`.
[`core/types.hpp:30`](../examples/bookmarks/include/bookmarks/core/types.hpp)
(comments and one body trimmed):

```cpp
struct BookmarkId {
    std::optional<std::int64_t> value;

    constexpr BookmarkId() noexcept = default;
    explicit BookmarkId(std::int64_t id) noexcept : value{id} {}

    [[nodiscard]] static BookmarkId fromOptional(std::optional<std::int64_t> payload) noexcept;

    [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }
    [[nodiscard]] std::int64_t operator*() const noexcept { return *value; }
    [[nodiscard]] auto operator<=>(const BookmarkId&) const noexcept = default;
};
```

Two things it buys. First, ordinary type safety: a `BookmarkId`, a `TagId` and
a pagination `Cursor` are all an integer on the wire, and the compiler will not
let you pass one where another belongs. Second, the empty state is *in the
type* — `hasValue()` distinguishes "not entered yet" from "entered", which is
what lets a schema-driven form and a `validate()` body agree about what a
required field means.

On the wire it is just a nullable integer; the strong typing lives in the C++
type only, declared by a `glz::meta` specialisation at the bottom of that file.

### DTOs are wire types, kept apart from storage types

[`dto/bookmark_dto.hpp:44`](../examples/bookmarks/include/bookmarks/dto/bookmark_dto.hpp)
(comments trimmed):

```cpp
struct CreateBookmark {
    std::string url;
    std::string title;  // empty = not yet known; the metadata worker fills it in
    std::string description;
    std::string notes;
    std::vector<std::string> tags;  // tag names; auto-created on first use
    Visibility visibility = Visibility::Private;

    static constexpr std::array<std::string_view, 5> optionalFields{"title", "description", "notes", "tags",
                                                                    "visibility"};

    static constexpr bool explicitSubmit = true;

    [[nodiscard]] bool validate() const noexcept {
        return !url.empty() && url.size() <= kMaxUrlBytes && title.size() <= kMaxTitleBytes;
    }
};
```

Note what is *not* a raw scalar: `visibility` is an `enum class`, not a `bool`,
and the result's `id` is a `BookmarkId`. The remaining fields are loose strings
because they genuinely are free-form text. Section 10 shows what that discipline
pays for, and what `optionalFields` and `explicitSubmit` do to the form.

The DTO layer is deliberately separate from the storage layer (section 6); the
model maps between them.

### What `validate()` is, and what it is not

Declaring `bool validate() const` on an action is the entire opt-in. The
framework calls it before `execute()` and rejects the action if it returns
false — on **both** execution paths, local and remote, so a remote client
cannot skip it by skipping a client-side gate. That is asserted, not assumed:

```console
$ ./build/examples/concepts/morph_concepts_tests \
      "getting started: an action's validate() gates execution in both deployments"
...
All tests passed (2 assertions in 1 test case)
```

The rejection surfaces as
`action failed validation: <ModelType>/<ActionType>` on `.onError`. That is
also why `BookmarkModel::execute(const CreateBookmark&)` still opens with its
own `if (!action.validate()) throw ValidationError{…}`: a test that calls the
model directly goes through no dispatcher.

**But `validate()` runs after decode**, and that is the important limitation.
By the time it sees the action, the JSON has already been turned into typed
fields — so it cannot see anything the decode itself normalised. The case in
point is `morph::math::Rational`: its wire decode *clamps* what it cannot
represent rather than failing, so `{"num":5,"den":0,"dp":2}` would arrive as a
perfectly plausible `5/1` that no `validate()` body could recognise as altered.
That is why the codec boundary — not the model — rejects unrepresentable
values, by wrapping the read in a `WireClampScope` and throwing if anything was
clamped ([`docs/spec/core/registry.md`](spec/core/registry.md), "the codec
boundary"). A local caller constructing the same value in code is unaffected,
because nothing decoded it.

Two consequences for a model author:

- A schema's `required` array and a client-side form gate are **UX, not
  security**. The server dispatcher runs whatever payload arrives; a model
  re-checks its own preconditions.
- If a value's decode cannot fail, do not expect `validate()` to catch a bad
  one. Ask whether the type's codec rejects it.

## 6. Persistence

`IMPLEMENTATION.md` rule 4 routes all persistence through the
[Lightweight](https://github.com/LASTRADA-Software/Lightweight) ORM, in three
pieces.

**The entity**, kept strictly separate from the DTOs
([`db/bookmark_entity.hpp:39`](../examples/bookmarks/include/bookmarks/db/bookmark_entity.hpp)):

```cpp
struct BookmarkRecord {
    static constexpr std::string_view TableName = "bookmarks";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"owner_principal"}> ownerPrincipal;
    Light::Field<Light::SqlAnsiString<2048>, Light::SqlRealName{"url"}> url;
    Light::Field<Light::SqlAnsiString<512>, Light::SqlRealName{"title"}> title;
    // ...
    Light::Field<bool, Light::SqlRealName{"is_shared"}> isShared{false};
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs{0};
    // ...
};
```

The storage type is where `bool` and raw integers are fine; the DTO above maps
`isShared` to `Visibility` and the integer id to a `BookmarkId`. The column
widths are pinned to the DTO's `kMaxUrlBytes`/`kMaxTitleBytes` by
`static_assert`s in the model, so a value `validate()` accepted can never be
silently truncated on the way in.

**The migration**, which is the only DDL mechanism — no `PRAGMA user_version`
scheme, no hand-run scripts
([`src/db/schema.cpp:24`](../examples/bookmarks/src/db/schema.cpp)):

```cpp
LIGHTWEIGHT_SQL_MIGRATION(20260807000001, "Create bookmarks tables") {
    plan.CreateTableIfNotExists("bookmarks")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("owner_principal", Varchar(64))
        .RequiredColumn("url", Varchar(2048))
        // ...
    plan.CreateIndex("idx_bookmarks_owner", "bookmarks", {"owner_principal"});
    // ... the tags, bookmark_tags and imported_ops tables
}
```

`LIGHTWEIGHT_SQL_MIGRATION` registers with a process-wide `MigrationManager` at
static-init time, so simply linking this translation unit makes the schema
known — the same mechanism the registration macros use, and with the same
force-link caveat (section 7).

**The access pattern.** A model holds no connection of its own. Each
`execute()` acquires one from the pool for its own duration and returns it
before returning. Writes that touch more than one row run in one transaction
([`src/models/bookmark_model.cpp:353`](../examples/bookmarks/src/models/bookmark_model.cpp)):

```cpp
auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
::Lightweight::SqlTransaction transaction{mapper->Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
mapper->Create(rec);
applyTagSet(mapper.Get(), rec.id.Value(), owner, action.tags);
transaction.Commit();
```

A bookmark and its tag associations land together or not at all: any throw
before `Commit()` rolls the transaction back. This is safe without locks
precisely because of the strand: no two `execute()` calls on one instance
overlap, and each acquisition is self-contained within one call. It is also why
`BookmarkModel` can be registered *plain* — no shared instance, no primary key —
and still serve every device a user signs in from: all real state is the
database, partitioned by `owner_principal`, so every instance a user registers
reads and writes the same rows.

## 7. Registration, and what it buys

```cpp
BRIDGE_REGISTER_MODEL(bookmarks::BookmarkModel, "BookmarkModel")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::CreateBookmark, "CreateBookmark")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::EditBookmark, "EditBookmark")
// ...
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::ListBookmarks, "ListBookmarks",
                       ::morph::model::Loggable::No)
```

([`models/bookmark_model.hpp:71`](../examples/bookmarks/include/bookmarks/models/bookmark_model.hpp).)

Each macro emits two things:

1. an **explicit specialisation** of a traits template — `ModelTraits<M>` or
   `ActionTraits<A>` — carrying the string type id and, for an action, the JSON
   codec functions (`toJson`, `fromJson`, `resultToJson`, `resultFromJson`) and
   the deduced `Result` type; and
2. a **file-scope initialiser** that registers the type with the process-level
   `ModelRegistryFactory` / `ActionDispatcher` before `main()` runs.

**The type ids are strings because a remote peer can only name a type by
string.** A wire envelope carries `"BookmarkModel"` and `"CreateBookmark"`; the
server looks them up in the dispatcher and reconstructs typed C++ from the
body. That is the whole reason registration exists — and the reason an unknown
id fails at runtime, not at compile time.

The `Result` type is *deduced* from `decltype(model.execute(action))`, which is
why the macro needs the model to be a complete type with `execute(A)` declared.
Nothing else about the model's shape matters.

The optional fourth argument is the journal policy. It defaults to
`Loggable::Yes`, so every action is recorded to an attached action log unless
you opt out — `ListBookmarks` does, being a pure query, and so does `BulkEdit`,
which writes its own journal entry in the same transaction as its rows (a
transactional outbox, drained by `App::relayOutboxOnce()`).

Three things that bite:

- **Every TU invoking `BRIDGE_REGISTER_ACTION` must also include
  `<morph/core/bridge.hpp>`**, directly or transitively. The macro calls
  `registerActionExecutorOnce<M, A>`, which is only *declared* in
  `registry.hpp`. Omit it and you get an unresolved external symbol at link
  time, not a compile error. (Headers live under `include/morph/<subsystem>/`,
  hence the `core/`.)
- **If your models live in a static library, force-link it**
  (`--whole-archive` / `-force_load` / `WHOLE_ARCHIVE`). The initialiser object
  is never referenced by name, so a linker is entitled to drop it — and the
  model then silently never registers.
- **A model used remotely must be default-constructible**, because the server
  re-creates instances from the string type id through the registry and cannot
  use an ad-hoc factory closure the way `LocalBackend` can.

> **Where to put the macros.** Either place works. Put the invocation in the
> model header when several translation units need the `ActionTraits`
> specialisation — which is what `examples/IMPLEMENTATION.md` rule 1 prescribes
> and what the ladder's model headers do — or in a single `.cpp` when only one
> TU needs it. A header included by many `.cpp` files registers the model once
> per including TU, which the registry absorbs by reassigning an equivalent
> factory over the same key; that is legal, not an ODR violation — see
> ["Header placement is legal"](spec/core/registry.md#header-placement-is-legal).

## 8. The bridge: `Completion<T>` and the executor

Three objects sit between a call site and a model.

- **`Bridge`** — the single, process-wide hub. It owns the active backend and
  tracks every live handler so it can re-point them when the backend is swapped
  (going offline → online, say). You construct **one** and share it.
- **`BridgeHandler<M>`** — your typed handle to one model type. It registers
  `M` on the bridge on construction and deregisters on destruction. Create one
  per model type, wherever in the UI you talk to that model.
- **`IExecutor*`** — where `.then` / `.onError` run. Models execute on a worker
  pool; callbacks are always marshalled back onto *this* executor, so they land
  on your UI thread and never block it.

**The executor is independent of everything else.** Where the model ran has no
bearing on where your callback runs. That decoupling is what makes local and
remote interchangeable at the call site.

> **Lifetime rule:** a `Bridge` must outlive every `BridgeHandler` registered
> on it. A handler deregisters itself on destruction, so destroying the bridge
> first is undefined behaviour.

### This is where a new reader's first real bug lives

A `Completion` **always** resolves through the executor — even a local
backend's immediate result is posted, not delivered inline (section 2 asserts
this). So the receiver can be destroyed before the handler runs, and the
natural spelling is silently wrong:

```cpp
completion.then([this](BookmarkView v) { /* `this` may be long gone */ });
```

This is not hypothetical: a receiver destroyed while a completion was still in
flight, whose callback then wrote to freed memory, has been found by
AddressSanitizer in this repository.

`Completion` has no cancellation of its own; a receiver opts in to a lifetime
gate instead. `morph::async::CallbackScope`
([`docs/spec/core/callback_scope.md`](spec/core/callback_scope.md)) is a data
member that hands out weak tokens, and `then(scope, handler)` /
`onError(scope, handler)` deliver only while the scope is alive and has not
been stopped. A sketch (not code from the tree):

```cpp
class BookmarkViewer {
public:
    void open(bookmarks::BookmarkId id) {
        _bookmarks.execute(bookmarks::GetBookmark{.id = id})
            .then(_scope, [this](bookmarks::BookmarkView const& view) { show(view); })
            .onError(_scope, [this](std::exception_ptr const& error) { report(error); });
    }

private:
    morph::bridge::BridgeHandler<bookmarks::BookmarkModel> _bookmarks;
    morph::async::CallbackScope _scope;  // declared last: destroyed first
};
```

Declare the scope **last**, so it dies before everything its callbacks touch.

The same hazard has an app-layer shape: an object that owns both a worker pool
and the executor that pool's completions post to must destroy the pool first.
`bookmarks::app::App` declares its executor before its pool for exactly that
reason, and its header explains the ordering.

Whichever spelling you pick, the rule is the same: **never capture a bare
`this` in a completion callback.**

## 9. Screens on top

bookmarks has no client code of its own. Its server serves **screens** —
documents that say what to show and which actions to run — and the generic
client draws them, in Qt Quick or in a terminal. A screen is written in C++
against the action types
([`screens/bookmarks_screens.cpp`](../examples/bookmarks/screens/bookmarks_screens.cpp),
abridged):

```cpp
auto const marks = screen.model<BookmarkModel>("marks", {.instance = ui::Instance::Private});
auto const list = ui::element("list");
auto const bookmarks = screen.query<ListBookmarks>("bookmarks", marks, {});
auto const archive = screen.mutation<BulkEdit>("archive", marks);
auto const rows = bookmarks.value().at(&ListBookmarksResult::bookmarks);
return ui::column({
    ui::form<CreateBookmark>(marks).id(ui::element("createForm")).testId("createForm"),
    ui::table(rows, {.key = ui::row("id"), /* title and URL columns */, .selection = ui::Selection::Multiple})
        .id(list).testId("bookmarks"),
    ui::button({.label = "Archive selected",
                .onClick = ui::run(archive, {{&BulkEdit::ids, list.selection()},
                                             {&BulkEdit::archive, ui::AnyHandle{std::string{"Archive"}}}})}),
    /* the edit, import, rename and merge forms, the tag table, and the error line */
});
```

The screen decides nothing about the domain. It names the model, the query
that lists bookmarks and the mutation; every rule about what a bookmark may be
stays in `BookmarkModel`, and nothing on the screen says when the list
refetches. Archiving a selection is
one `BulkEdit` built from the table's own selection, so it is one transaction
in the model however many rows are selected.

Sign-in is a screen too: `bookmarks.login` runs `Login` and installs the
token its reply carries with `ui::signIn`, and the catalog hides
`bookmarks.main` until a principal is signed in, because its queries need one.

The screen is data on the wire, so the same document runs in either deployment
mode, and on any frontend. Its tests run headless: they mount it on a recording
backend and drive it by test id, with no window system at all
([`screens/tests/test_bookmarks_screens.cpp`](../examples/bookmarks/screens/tests/test_bookmarks_screens.cpp),
its helpers inlined):

```cpp
harness.edit("username", "alice");
harness.click("signIn");
REQUIRE(harness.waitForText("count", "0 bookmarks"));
harness.editField("createForm", "url", "https://example.org/a");
REQUIRE(harness.submitForm("createForm"));
REQUIRE(harness.waitForText("count", "1 bookmarks"));
```

### What refreshes after an action

The action says what it changes, beside its registration, and every screen
showing that data refreshes: on this client, on every other connected client,
and when an AI agent runs the action over MCP. Two declarations do it
([`models/bookmark_model.hpp`](../examples/bookmarks/include/bookmarks/models/bookmark_model.hpp),
[`dto/bookmark_dto.hpp`](../examples/bookmarks/include/bookmarks/dto/bookmark_dto.hpp)):

```cpp
// A result type that is shared data, keyed by its `id` member
BRIDGE_REGISTER_RECORD(bookmarks::BookmarkSummary, "Bookmarks_Bookmark", ::morph::record::privateKeys)

// What the action changes: archiving moves bookmarks between lists, and changes the tag counts
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::BulkEdit, "BulkEdit", ::morph::model::Loggable::No)
BRIDGE_REGISTER_EFFECTS(bookmarks::BulkEdit, ::morph::effects::changes<bookmarks::BookmarkSummary>,
                        ::morph::effects::changes<bookmarks::BookmarkView>,
                        ::morph::effects::changes<bookmarks::TagSummary>)
```

`changes<R>` refetches every query holding an `R`; `updates<R>` is for an
action whose reply carries the changed rows, which the caller's screens show
at once, before any fetch; `none` is for an action that changes nothing. A query whose
action declares effects is refused, because a query must only read. How to
choose, the checklist for a new action and every error message with its fix
are in [`docs/KEEPING-SCREENS-CURRENT.md`](KEEPING-SCREENS-CURRENT.md).

## 10. Forms from the compiled action type

`morph::forms::schemaJson<A>()` derives a JSON Schema from the action type
itself. Nothing declares it separately, so it cannot describe a field the
action does not have — which is the entire point. `ui::form<CreateBookmark>`
above is drawn from exactly this schema.

Here is `schemaJson<bookmarks::CreateBookmark>()` as it is emitted, pretty-printed:

```json
{
  "type": "object",
  "properties": {
    "description": { "type": "string", "x-order": 2, "title": "Description" },
    "notes":       { "type": "string", "x-order": 3, "title": "Notes" },
    "tags":        { "type": "array", "items": { "type": "string" }, "x-order": 4, "title": "Tags" },
    "title":       { "type": "string", "x-order": 1, "title": "Title" },
    "url":         { "type": "string", "x-order": 0, "title": "Url" },
    "visibility": {
      "type": "string",
      "oneOf": [ { "title": "Private", "const": "Private" },
                 { "title": "Shared", "const": "Shared" } ],
      "x-order": 5, "title": "Visibility"
    }
  },
  "additionalProperties": false,
  "title": "bookmarks::CreateBookmark",
  "required": ["url"],
  "x-submitMode": "explicit"
}
```

Read that against the DTO in section 5 and every line of it is earned:

- `visibility` is a string enumeration, not a bare ordinal or a checkbox,
  because it is an `enum class` and the DTO header specialises `glz::meta` for
  it. Without that specialisation `schemaJson<A>()` refuses to compile.
- `x-order` preserves declaration order. The properties themselves come out sorted by name, so without it a
  generated form would show `description` first.
- `required` is exactly `["url"]` — every reflected member is required unless
  it is a `std::optional` or is named in the action's own `optionalFields`
  opt-out. That opt-out is what lets a bookmark be created without a title, so
  the background metadata fetch has something to complete; a test pins that
  the generated schema does not mark `title` required
  ([`tests/test_bookmark_dto.cpp`](../examples/bookmarks/tests/test_bookmark_dto.cpp)).
- `x-submitMode: "explicit"` comes from `explicitSubmit = true`. A form
  otherwise submits as soon as it is valid, which for an action that stores a
  row would store one bookmark per typed character; with it, the form renders
  its own Submit button.

This is the payoff for the type discipline in section 5. A form built from raw
`bool` and `int` members could not have produced any of it.

And, per section 5: `required` here is a **client-side** gate. The server runs
whatever payload arrives.

## 11. Testing

Every rung tests the model layer with no screen at all — that is the point of
keeping the domain in a plain class. The pattern is a fixture, a real schema, a
principal, and a direct call
([`tests/test_bookmark_model.cpp:68`](../examples/bookmarks/tests/test_bookmark_model.cpp)):

```cpp
TEST_CASE("CreateBookmark stores a bookmark owned by the authenticated principal", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal principal{"alice"};

    bookmarks::CreateBookmark action;
    action.url = "https://example.com";
    action.title = "Example";
    action.tags = {"work", "reading"};
    const auto id = model.execute(action).id;
    REQUIRE(id.hasValue());

    const auto view = model.execute(bookmarks::GetBookmark{.id = id});
    CHECK(view.url == "https://example.com");
    CHECK(view.title == "Example");
    CHECK(view.readState == bookmarks::ReadState::Unread);
    CHECK(view.archiveState == bookmarks::ArchiveState::Active);
    CHECK(view.tags.size() == 2);
}
```

No bridge, no executor, no pumping — the model is just a C++ object.
`DbFixture` drops every table in a real on-disk SQLite database and re-applies
the registered migrations, so each case starts from a clean, real schema.
`ScopedPrincipal` is a small helper in the same file that installs a
`session::Context` for the scope, standing in for what a backend installs
around a dispatched call; without it, the same `CreateBookmark` throws
`Forbidden`, and the next test case asserts exactly that.

When a test *does* need the framework, `BackendRig` supplies it in whichever
deployment shape is under test — `Mode::Local`, `Mode::LocalSingleThread` (the
WASM-parity, single-threaded shape) or `Mode::Socket` (a real loopback
WebSocket) — and `pumpUntil` / `awaitQt` turn the event loop until a completion
settles. Section 12 shows what that combination is for.

Note the shape of the async helpers: because callbacks arrive through an
executor, a test must *pump* rather than wait. Blocking a test thread on a
completion that resolves on the same thread's executor deadlocks.

## 12. The payoff: making it remote

Here is the headline claim, as something you can run rather than something you
have to believe.

bookmarks' backend-matrix test body is generated over all three deployments
([`tests/test_bookmark_model.cpp:641`](../examples/bookmarks/tests/test_bookmark_model.cpp),
abridged):

```cpp
const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
DbFixture fixture;

constexpr std::string_view kSecret = "matrix-test-secret";
const auto authorizer =
    std::make_shared<bookmarks::auth::BookmarksAuthorizer>(std::string{kSecret}, morph::session::hmacSha256);
BackendRig rig{mode, 1, authorizer};

const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
morph::session::Context ctx;
ctx.principal = "alice";
ctx.token =
    issuer.issue(morph::session::SessionToken{.principal = "alice", .expiresAtMs = 4102444800000, .roles = {}});
rig.bridge(0).setDefaultSession(ctx);

auto handler = rig.client<bookmarks::BookmarkModel>(0);
bookmarks::CreateBookmark create;
create.url = "https://matrix.example";
create.title = "Matrix";
const auto createResult = awaitQt(handler.execute(create));
const auto listResult = awaitQt(handler.execute(bookmarks::ListBookmarks{}));
const auto view = awaitQt(handler.execute(bookmarks::GetBookmark{.id = createResult.id}));
CHECK(view.url == "https://matrix.example");
```

`Mode::Local` runs `BookmarkModel` on a worker pool in this process.
`Mode::Socket` stands up a `RemoteServer` behind a real `QtWebSocketServer` on
an ephemeral port, with the authorizer installed, and gives the client its own
`QtWebSocketBackend` over a loopback socket. **Nothing between the handler and
the `CHECK` changes.** Not the model, not the DTOs, not the registration, not
the call site — only which backend the `Bridge` was constructed with. The
session the bridge carries is the same in every mode; only `Socket` has an
authorizer to verify its token.

The application-level equivalent is the two command lines from section 3:
with `--server`, every action the screen runs crosses the socket as a
`register` or `execute` envelope naming `BookmarkModel`; without it, the same
screen runs the same actions in-process and no envelope exists at all.

**What actually differs**, and is worth knowing before you rely on it:

- **Registration timing.** Remote mode's registration is a round trip; a first
  fetch made before a handler's bind settles is held until it does.
- **Authorization exists only remotely.** `RemoteServer` runs every request
  through an `IAuthorizer` — bookmarks' verifies the signed token on every
  `execute` and the instance's owner — while `LocalBackend` does not authorize
  at all. The model's own principal check (section 4) is what holds in both.
- **Message size is bounded remotely.** A payload too large for the server's
  envelope limit is refused by the transport, as a typed error the client
  renders. In-process there is no such bound, which is why `ImportBookmarks`
  carries its own, smaller chunk limit and throws `TooLarge` past it.
- **Who owns the store.** In remote mode the server owns the database. A
  client that also opened the same SQLite file behind the server's back would
  be a second writer, so the client opens bookmarks' database **only** in local
  mode.

`SimulatedRemoteBackend` (section 2) is the third shape: the same
serialise/dispatch path as the socket, with no socket. A bug that only appears
remotely is usually reproducible in a plain unit test through it — and it is
also how bookmarks' metadata-fetch worker reaches the server as an ordinary
client.

## 13. What morph does not do

A guide that only sells is worth less than one that tells you when to stop.
Read [`docs/spec/`](spec/) before relying on any of these; the short version:

- **It is not a general-purpose RPC framework.** Request/response, plus one
  kind of server-initiated message: the change notifications that keep
  screens current ([`docs/KEEPING-SCREENS-CURRENT.md`](KEEPING-SCREENS-CURRENT.md)).
  No streaming and no general bidirectional channel.
- **`Completion<T>` is a leaf callback primitive, not a future.** No `T→U`
  chaining, no `co_await`, and no cancellation; lifetime binding is the opt-in
  `CallbackScope` of section 8.
- **Security is app-supplied.** There is no built-in authentication: a
  `Context`'s identity is whatever the client claims until an `IAuthorizer`
  verifies it, and the default authorizer allows everything. Signed tokens,
  version negotiation, size and timeout bounds, opaque model ids and
  register/instance authorization hooks all ship, but they are **opt-in**.
  bookmarks turns on signed tokens and a live-model cap and leaves the rest at
  the default; a server that configures none of them assumes a trusted,
  authenticated transport and is not a hardened public-internet server.
- **Instance subscriptions are best-effort and in-process.** `subscribe<R>`
  fans out to handlers on the same `Bridge`, with no replay and no durability.
  Across clients, what keeps screens current is declared effects, which tell
  every connection what changed but not the new values.
- **Exact numbers are fixed-width.** `Rational` is an `int64` pair; `+`/`-`/`*`
  can overflow rather than returning an error, and high decimal precision
  shrinks the representable magnitude.
- **The journal is an audit trail, not event sourcing.** `replay()` /
  `undoLast()` re-execute the recorded action against a fresh model *object* —
  which does not isolate the database that object immediately reopens. For any
  DB-backed model that is a live mutation, not a reconstruction, which is why
  bookmarks offers no generic undo and its README says so.
- **Registration is global, macro-driven and runtime-resolved.** No runtime
  deregistration; an unknown type id fails at runtime, not compile time.
- **Conflict resolution is not a framework concern.** On a backend switch morph
  fires `onBackendChanged()` on the fresh model instance and steps back.
- **No 1.0 compatibility promise yet.** morph is `0.1.0`; anything may change
  in any release. The policy that starts at 1.0 is already published in
  [`docs/spec/VERSIONING.md`](spec/VERSIONING.md).

## 14. Where to go next

You now have a question rather than a topic, which is what the map is for.

| If you are asking… | Read |
|---|---|
| how does an action actually reach a model? | [`docs/spec/README.md`](spec/README.md) — it traces one end to end |
| the big picture, in prose | [`docs/ARCHITECTURE.md`](ARCHITECTURE.md) |
| what may I attach to a result, and when does it run? | `docs/spec/core/completion.md`, `docs/spec/core/executor.md` |
| what exactly do the macros generate? | `docs/spec/core/registry.md` |
| what differs between local and remote? | `docs/spec/core/backend.md` |
| what is on the wire? | `docs/spec/core/wire.md` |
| who is allowed to do what? | `docs/spec/security.md`, `docs/spec/session/session.md` |
| what serialises access to my model? | `docs/spec/concurrency_and_lifetimes.md` |
| how do I keep several screens on one model instance? | `docs/spec/core/shared_instances.md` |
| how do screens refresh after an action, and what do I declare? | [`docs/KEEPING-SCREENS-CURRENT.md`](KEEPING-SCREENS-CURRENT.md) |
| how does an AI agent drive my app? | `docs/spec/ui/agent.md`, and bank's `--mcp-stdio` ([`examples/bank/README.md`](../examples/bank/README.md)) |
| how do I generate a form? | `docs/spec/forms/forms.md`, and [`examples/forms`](../examples/forms) |
| how do I test all this? | [`examples/TESTING.md`](../examples/TESTING.md) |
| what are the rules for an app built on morph? | [`examples/IMPLEMENTATION.md`](../examples/IMPLEMENTATION.md) |
| what does a bigger app look like? | [`examples/LADDER.md`](../examples/LADDER.md) — bank (medium) and lims (large), and what each is for |

For the production-hardening features this guide skipped — the action log and
transactional outbox, offline queue durability, register authorization,
transport limits, protocol versioning, observability and graceful shutdown —
[`examples/concepts`](../examples/concepts) has one small, runnable file each.
