# Getting started: building an app on morph

This is the document that comes before the others. It builds one small thing
end to end and explains why each piece exists, using
[`examples/pastebin`](../examples/pastebin) — the smallest complete morph
application in the tree — as the worked example.

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
journal, the offline queue, sessions and authorization are named here only
where a first app trips over them.

## Contents

- [1. What morph is for](#1-what-morph-is-for)
- [2. The whole seam in one file](#2-the-whole-seam-in-one-file)
- [3. Building and running pastebin](#3-building-and-running-pastebin)
- [4. The model is the application](#4-the-model-is-the-application)
- [5. Strong ids, DTOs, and what `validate()` is for](#5-strong-ids-dtos-and-what-validate-is-for)
- [6. Persistence](#6-persistence)
- [7. Registration, and what it buys](#7-registration-and-what-it-buys)
- [8. The bridge: `Completion<T>` and the executor](#8-the-bridge-completiont-and-the-executor)
- [9. A GUI on top](#9-a-gui-on-top)
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

## 3. Building and running pastebin

pastebin is [rung 1 of the application ladder](../examples/LADDER.md): one
entity, one model, SQLite behind Lightweight, a Qt/QML client, and a real
socket server. It is the same four parts from section 2 at real scale.

It needs Qt 6.5+, an ODBC SQLite3 driver, and a C++23 compiler. Configure:

```sh
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DMORPH_BUILD_QT=ON -DMORPH_BUILD_FORMS_QML=ON \
    -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=pastebin \
    -DMORPH_BUILD_TESTS=ON -DMORPH_BUILD_EXAMPLES=ON
cmake --build build --target ladder_pastebin_tests ladder_pastebin_server ladder_pastebin_gui
```

Run the suite first — it is the fastest confirmation that your toolchain, your
ODBC driver and your Qt install all agree:

```console
$ ctest --test-dir build -L ladder-pastebin
...
100% tests passed out of 55
```

Then the app. The server owns the database, the action journal and the expiry
sweep:

```console
$ PASTEBIN_DB="DRIVER=SQLite3;Database=pastebin.db;Timeout=5000" \
  PASTEBIN_PORT=8765 ./build/examples/pastebin/ladder_pastebin_server --seed
pastebin-server: seeded a plain public paste as blue-yak-734
pastebin-server: seeded an editable C++ snippet as quiet-hen-290
pastebin-server: seeded a SQL snippet as quiet-jay-694
pastebin-server: seeded a private paste as wild-ant-277
pastebin-server: seeded a burn-after-1 paste as dark-ant-399
pastebin-server: seeded a paste expiring in two minutes as sharp-bee-657
pastebin-server: listening on port 8765
```

(The animal-name ids are allocated randomly, so yours will differ.)

The desktop client runs in either deployment mode. Against the server:

```sh
./build/examples/pastebin/ladder_pastebin_gui --server ws://127.0.0.1:8765
```

or entirely in-process, hosting `PasteModel` itself:

```sh
PASTEBIN_DB="DRIVER=SQLite3;Database=local.db;Timeout=5000" \
  ./build/examples/pastebin/ladder_pastebin_gui
```

Section 12 comes back to what is — and is not — different between those two
command lines.

> **Note on the two modes.** Local mode is deliberately the *smaller*
> deployment, not an equivalent one:
> [`gui/main.cpp:53-73`](../examples/pastebin/gui/main.cpp) explains that
> `pastebin::app::App` — the durable action log and the periodic expiry sweep —
> lives only in the server binary, so a local-mode client journals nothing and
> never reclaims expired rows. Opening an expired paste still fails correctly,
> because `GetPaste`'s own guard never depends on the sweep having run.

## 4. The model is the application

This is `examples/IMPLEMENTATION.md` rule 1, and the single most load-bearing
idea in the framework: **all business logic, all invariants and all persistence
access live in plain, single-threaded model classes with typed actions.**
Nothing domain-shaped may live in a presenter, in QML, or in `main()`.

pastebin's whole domain is one class,
[`include/pastebin/models/paste_model.hpp:30`](../examples/pastebin/include/pastebin/models/paste_model.hpp):

```cpp
class PasteModel {
public:
    CreatePasteResult execute(const CreatePaste& action);
    PasteView        execute(const GetPaste& action);
    PasteView        execute(const EditPaste& action);
    Ack              execute(const DeletePaste& action);
    ListPastesResult execute(const ListPastes& action);
    Ack              execute(const ExpirePaste& action);
};
```

That is the entire public surface. One overload per action, results returned by
value, failures thrown as the app's own typed error set
([`core/errors.hpp`](../examples/pastebin/include/pastebin/core/errors.hpp):
`NotFound`, `Expired`, `Burned`, `ValidationError`, `Conflict`, `TooLarge`).
Never encode a failure as a magic value in a result DTO — morph captures the
thrown exception as a `std::exception_ptr` and delivers it to the caller's
`.onError(...)`, and over a socket the `what()` string travels back in the
error envelope.

**A model is single-threaded per instance, and must not know the framework
exists.** morph runs each model instance on its own strand, so two `execute()`
calls on one instance never overlap — which is why `PasteModel` has no mutex
and no `std::atomic` anywhere. It also means a model must not reach for an
executor, a `Bridge` or a background thread of its own: there is no framework
seam for that, deliberately
([#129](https://github.com/LASTRADA-Software/morph/issues/129) was closed as
wrong-layer, and
[`docs/findings/r5-003-no-model-level-background-job-seam.md`](findings/r5-003-no-model-level-background-job-seam.md)
records it). Background work is orchestration; it belongs at the app layer,
re-entering the model as an ordinary client call — which is exactly what
pastebin's expiry sweep does.

Write this layer first, and test it (section 11) before there is any bridge or
any GUI. Every ladder rung does.

## 5. Strong ids, DTOs, and what `validate()` is for

### Why `PasteId` rather than `std::string`

`examples/IMPLEMENTATION.md` rule 3 requires entity identity in a DTO to be a
per-entity strong id type exposing `hasValue()`.
[`core/types.hpp:28`](../examples/pastebin/include/pastebin/core/types.hpp):

```cpp
struct PasteId {
    std::optional<std::string> value;

    constexpr PasteId() noexcept = default;
    explicit PasteId(std::string id) noexcept : value{std::move(id)} {}

    [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }
    [[nodiscard]] const std::string& operator*() const noexcept { return *value; }
    [[nodiscard]] auto operator<=>(const PasteId&) const noexcept = default;
};
```

Two things it buys. First, ordinary type safety: a `PasteId` and a
`PasteCursor` are both opaque strings, and the compiler will not let you pass
one where the other belongs. Second, the empty state is *in the type* —
`hasValue()` distinguishes "not entered yet" from "entered as the empty
string", which is what lets a schema-driven form and a `validate()` body agree
about what a required field means.

On the wire it is just a nullable string; the strong typing lives in the C++
type only, declared by a `glz::meta` specialisation at the bottom of that file.

### DTOs are wire types, kept apart from storage types

[`dto/paste_dto.hpp:64`](../examples/pastebin/include/pastebin/dto/paste_dto.hpp):

```cpp
struct CreatePaste {
    std::string content;
    std::string syntax;
    ::morph::time::Timestamp expiresAt;  // empty = never expires
    Reads burnAfterReads;                // empty = no burn limit
    Visibility visibility = Visibility::Public;
    Editability editability = Editability::Immutable;

    static constexpr std::array<std::string_view, 4> optionalFields{
        "expiresAt", "burnAfterReads", "visibility", "editability"};

    [[nodiscard]] bool validate() const noexcept { /* ... */ }
};
```

Note what is *not* a raw scalar: instants are `morph::time::Timestamp`, counts
are a unit-tagged `Reads` quantity, and the two flags are `enum class`. Only
`content` and `syntax` are loose strings, because they genuinely are free-form
text. Section 10 shows what that discipline pays for.

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
`action failed validation: <ModelType>/<ActionType>` on `.onError`.

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

pastebin stores one table. `IMPLEMENTATION.md` rule 4 routes all persistence
through the [Lightweight](https://github.com/LASTRADA-Software/Lightweight)
ORM, in three pieces.

**The entity**, kept strictly separate from the DTOs
([`db/paste_entity.hpp:49`](../examples/pastebin/include/pastebin/db/paste_entity.hpp)):

```cpp
struct PasteRecord {
    static constexpr std::string_view TableName = "pastes";

    Light::Field<Light::SqlAnsiString<32>, Light::PrimaryKey::AutoAssign, Light::SqlRealName{"id"}> id;
    Light::Field<Light::SqlMaxDynamicWideString, Light::SqlRealName{"content"}> content;
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"syntax"}> syntax;
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs{0};
    // ...
};
```

**The migration**, which is the only DDL mechanism — no `PRAGMA user_version`
scheme, no hand-run scripts
([`src/db/schema.cpp:30`](../examples/pastebin/src/db/schema.cpp)):

```cpp
LIGHTWEIGHT_SQL_MIGRATION(20260806000001, "Create pastes table") {
    plan.CreateTableIfNotExists("pastes")
        .PrimaryKey("id", Varchar(32))
        .RequiredColumn("content", NVarchar(0))
        .RequiredColumn("syntax", Varchar(32))
        // ...
}
```

`LIGHTWEIGHT_SQL_MIGRATION` registers with a process-wide `MigrationManager` at
static-init time, so simply linking this translation unit makes the schema
known — the same mechanism the registration macros use, and with the same
force-link caveat (section 7).

**The access pattern.** A model holds no connection of its own. Each
`execute()` acquires one from the pool for its own duration and returns it
before returning:

```cpp
auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
```

This is safe without locks precisely because of the strand: no two `execute()`
calls on one instance overlap, and each acquisition is self-contained within
one call. It is also why `PasteModel` can be registered *plain* — no shared
instance, no primary key — while still getting burn-after-read atomicity: the
guarantee comes from SQL, not from a C++ instance. The single conditional
statement it rests on is at
[`src/models/paste_model.cpp:166`](../examples/pastebin/src/models/paste_model.cpp):

```cpp
constexpr std::string_view kConsumeReadSql = R"(UPDATE pastes
       SET read_count = read_count + 1
     WHERE id = ?
       AND (expires_at_ms IS NULL OR expires_at_ms > ?)
       AND (burn_after_reads IS NULL OR read_count < burn_after_reads))";
```

Every guard a read must respect lives in that one `WHERE`, so the guard and the
increment are applied indivisibly: of N clients racing for the last allowed
read, exactly one gets a non-zero affected-row count. Raw SQL from inside a
model is a *sanctioned escape*, not the norm — rule 4 pre-enumerates the cases
and requires a written finding for each.

## 7. Registration, and what it buys

```cpp
BRIDGE_REGISTER_MODEL(pastebin::PasteModel, "PasteModel")
BRIDGE_REGISTER_ACTION(pastebin::PasteModel, pastebin::CreatePaste, "CreatePaste")
BRIDGE_REGISTER_ACTION(pastebin::PasteModel, pastebin::GetPaste, "GetPaste")
// ...
BRIDGE_REGISTER_ACTION(pastebin::PasteModel, pastebin::ListPastes, "ListPastes",
                       ::morph::model::Loggable::No)
```

([`models/paste_model.hpp:81`](../examples/pastebin/include/pastebin/models/paste_model.hpp).)

Each macro emits two things:

1. an **explicit specialisation** of a traits template — `ModelTraits<M>` or
   `ActionTraits<A>` — carrying the string type id and, for an action, the JSON
   codec functions (`toJson`, `fromJson`, `resultToJson`, `resultFromJson`) and
   the deduced `Result` type; and
2. a **file-scope initialiser** that registers the type with the process-level
   `ModelRegistryFactory` / `ActionDispatcher` before `main()` runs.

**The type ids are strings because a remote peer can only name a type by
string.** A wire envelope carries `"PasteModel"` and `"CreatePaste"`; the
server looks them up in the dispatcher and reconstructs typed C++ from the
body. That is the whole reason registration exists — and the reason an unknown
id fails at runtime, not at compile time.

The `Result` type is *deduced* from `decltype(model.execute(action))`, which is
why the macro needs the model to be a complete type with `execute(A)` declared.
Nothing else about the model's shape matters.

The optional fourth argument is the journal policy. It defaults to
`Loggable::Yes`, so every action is recorded to an attached action log unless
you opt out — `ListPastes` does, being a pure query.

Three things that bite:

- **Every TU invoking `BRIDGE_REGISTER_ACTION` must also include
  `<morph/core/bridge.hpp>`**, directly or transitively. The macro calls
  `registerActionExecutorOnce<M, A>`, which is only *declared* in
  `registry.hpp`. Omit it and you get an unresolved external symbol at link
  time, not a compile error. (Note the `core/` — headers live under
  `include/morph/<subsystem>/`, and several documents still show a flat
  `<morph/bridge.hpp>` that does not exist:
  [#235](https://github.com/LASTRADA-Software/morph/issues/235).)
- **If your models live in a static library, force-link it**
  (`--whole-archive` / `-force_load` / `WHOLE_ARCHIVE`). The initialiser object
  is never referenced by name, so a linker is entitled to drop it — and the
  model then silently never registers.
- **A model used remotely must be default-constructible**, because the server
  re-creates instances from the string type id through the registry and cannot
  use an ad-hoc factory closure the way `LocalBackend` can.

> **Where to put the macros — currently contested.** `README.md` and
> [`docs/spec/core/registry.md`](spec/core/registry.md) say to put each
> invocation in exactly one `.cpp` and never in a header. `examples/`
> does the opposite: all 33 ladder model headers invoke the macros in the
> model header, as `examples/IMPLEMENTATION.md` rule 1 prescribes, so every
> call site sees the `ActionTraits` specialisation. pastebin's own
> `paste_model.hpp` is included by seven translation units, several of which
> link into one binary, and it builds and passes. The contradiction is tracked
> in [#231](https://github.com/LASTRADA-Software/morph/issues/231); until it is
> resolved, follow the examples, since that is what the tree is actually built
> and tested as.

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
completion.then([this](PasteView v) { /* `this` may be long gone */ });
```

This is not hypothetical. [#137](https://github.com/LASTRADA-Software/morph/issues/137)
was a real use-after-free found by AddressSanitizer: a presenter destroyed
while a completion was still in flight, whose callback then wrote to freed
stack memory during test teardown.

`Completion` has no cancellation and no way to bind a handler to a receiver's
lifetime — [`docs/spec/core/completion.md`](spec/core/completion.md) lists
cancellation under **Out of scope** in so many words. A framework-level
lifetime-and-stop token is tracked as
[#138](https://github.com/LASTRADA-Software/morph/issues/138) and has not
landed. **Until it does, write the guard yourself.** The ladder's shared
presenter base does it with a `QPointer`
([`examples/common/gui/presenter.hpp:133`](../examples/common/gui/presenter.hpp)):

```cpp
QPointer<Presenter> self{this};
completion
    .then([self, onOk = std::move(onOk)](T value) {
        if (!self) { return; }
        // ... and re-check `self` after onOk returns: the callback itself
        // may destroy the presenter.
    })
    .onError([self, onErr = std::move(onErr)](const std::exception_ptr& err) {
        if (!self) { return; }
        // ...
    });
```

Outside Qt the same shape is spelled with a `shared_ptr<const void> _liveness`
member (declared **last**, so it dies first) captured as a
`weak_ptr<const void>` and re-checked before touching `this`. morph uses that
idiom internally; `Bridge` itself is one example.

Whichever spelling you pick, the rule is the same: **never capture a bare
`this` in a completion callback.**

## 9. A GUI on top

pastebin's UI layer is three thin layers and no domain logic.

**The presenter** translates and routes; it decides nothing
([`gui_lib/paste_presenter.cpp:16`](../examples/pastebin/gui_lib/paste_presenter.cpp)):

```cpp
void PastePresenter::create(CreatePaste action) {
    track<CreatePasteResult>(
        _handler.execute(std::move(action)),
        [this](CreatePasteResult result) { emit created(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}
```

`track()` is the shared base from section 8 — it owns the lifetime guard and a
busy counter, so a presenter subclass never writes either. Note what the
presenter does *not* do: it does not construct an executor or a backend, and it
links `Qt6::Core` only, so it is instantiable under a plain `QCoreApplication`
and testable with no window system at all.

**The QML adapter** exists because of a constraint worth knowing before you hit
it. `moc` is not a C++ front end, and it mis-parses morph's template-heavy
`bridge.hpp` (and any ORM headers a model header pulls in), emitting the rest
of the file inside a namespace it wrongly believes is still open. So a
`Q_OBJECT` class that needs those headers guards them:

```cpp
#ifndef Q_MOC_RUN
#include "paste_presenter.hpp"
#include <morph/core/bridge.hpp>
#endif
```

`moc` needs nothing from them — only the macros, signals and `Q_INVOKABLE`
signatures. This is why every rung hand-writes one `QObject` per model rather
than generating it: the adapter translates C++ DTOs into the `QString` /
`QVariantMap` shapes QML binds against, and does nothing else.

**The shell** picks a deployment mode and builds everything else from inside a
readiness callback ([`gui/main.cpp:79`](../examples/pastebin/gui/main.cpp)):

```cpp
AppContext ctx{serverUrl ? AppContext::Mode{Remote{.url = *serverUrl}}
                         : AppContext::Mode{Local{.workers = 4}}};

ctx.onReady([&] {
    formsBridge = std::make_unique<FormsBridge>(ctx.bridge(), ctx.executor());
    pasteBridge = std::make_unique<PasteBridge>(ctx.bridge(), ctx.executor());
    engine.loadFromModule(MORPH_LADDER_QML_URI, "Main");
});
```

`onReady` matters in remote mode: the socket connects asynchronously, so a
remote context is not usable the line after its constructor returns. In local
mode the callback fires synchronously. Related, and worth knowing: a handler's
registration round trip settles asynchronously too, and
`Bridge::whenBound()` is the seam that tells you when — pastebin's `Main.qml`
gates its first `refresh()` on it rather than retrying on a timer.

## 10. Forms from the compiled action type

`morph::forms::schemaJson<A>()` derives a JSON Schema from the action type
itself. Nothing declares it separately, so it cannot describe a field the
action does not have — which is the entire point.

pastebin's create form renders from one line
([`gui_lib/paste_schemas.hpp:31`](../examples/pastebin/gui_lib/paste_schemas.hpp)):

```cpp
[[nodiscard]] inline std::string pasteSchemasJson() {
    return std::string{"{\"CreatePaste\":"} + ::morph::forms::schemaJson<pastebin::CreatePaste>() + "}";
}
```

Here is what `schemaJson<pastebin::CreatePaste>()` actually produces
(abridged, and pretty-printed):

```json
{
  "type": "object",
  "properties": {
    "content":  { "type": "string", "x-order": 0, "title": "Content" },
    "syntax":   { "type": "string", "x-order": 1, "title": "Syntax" },
    "expiresAt": {
      "type": ["string", "null"], "format": "date-time",
      "x-order": 2, "title": "Expires At"
    },
    "burnAfterReads": {
      "type": ["object", "null"],
      "properties": {
        "num": { "$ref": "#/$defs/int64_t" },
        "den": { "$ref": "#/$defs/int64_t" },
        "dp":  { "$ref": "#/$defs/uint32_t" }
      },
      "ExtUnits": { "unitAscii": "count", "unitUnicode": "" },
      "x-order": 3, "title": "Burn After Reads", "x-decimalPlaces": 1
    },
    "visibility": {
      "type": "string",
      "oneOf": [ { "title": "Public", "const": "Public" },
                 { "title": "Private", "const": "Private" } ],
      "x-order": 4, "title": "Visibility"
    },
    "editability": { "…": "same shape: Immutable | Editable" }
  },
  "$defs": { "int64_t": { "…": "…" }, "uint32_t": { "…": "…" } },
  "title": "pastebin::CreatePaste",
  "required": ["content", "syntax"]
}
```

Read that against the DTO in section 5 and every line of it is earned:

- `expiresAt` renders as a date-time input because it is a
  `morph::time::Timestamp`, and is nullable because a `Timestamp` can be empty.
- `burnAfterReads` carries its unit (`ExtUnits`) and its input step
  (`x-decimalPlaces`) because it is a `Quantity<Unit::count, 1>`. Units never
  travel on the wire — they live in the C++ type and in the schema.
- `visibility` and `editability` are string enumerations, not bare ordinals,
  because the DTO header specialises `glz::meta` for them.
- `x-order` preserves declaration order, so a generated form does not shuffle.
- `required` is exactly `["content", "syntax"]` — every reflected member is
  required unless it is a `std::optional` or is named in the action's own
  `optionalFields` opt-out. That opt-out is what lets "empty = never expires"
  and "empty = no burn limit" mean what the DTO says they mean.

This is the payoff for the type discipline in section 5. A form built from raw
`std::string` and `double` members could not have produced any of it.

And, per section 5: `required` here is a **client-side** gate. The server runs
whatever payload arrives.

## 11. Testing

Every rung tests the model layer with no GUI at all — that is the point of
keeping the domain in a plain class. The pattern is a fixture, a real schema,
and a direct call:

```cpp
TEST_CASE("CreatePaste stores a paste under a freshly allocated animal-name id", "[pastebin][model]") {
    DbFixture fixture;
    pastebin::PasteModel model;

    const auto id = model.execute(makeCreate("hello", "cpp")).id;
    REQUIRE(id.hasValue());

    const auto view = model.execute(pastebin::GetPaste{.id = id});
    CHECK(view.content == "hello");
    CHECK(view.syntax == "cpp");
}
```

No bridge, no executor, no pumping — the model is just a C++ object.
`DbFixture` drops every table in a real on-disk SQLite database and re-applies
the registered migrations, so each case starts from a clean, real schema.

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

pastebin's fuzz-corpus test body is generated over two deployments
([`tests/test_paste_model.cpp:1441`](../examples/pastebin/tests/test_paste_model.cpp)):

```cpp
const auto mode = GENERATE(Mode::Local, Mode::Socket);
DbFixture fixture;
BackendRig rig{mode, 1};
auto handler = rig.client<pastebin::PasteModel>(0);

const auto id = awaitQt(handler.execute(makeCreate(content))).id;
const auto fetched = awaitQt(handler.execute(pastebin::GetPaste{.id = id}));
CHECK(fetched.content == content);
```

`Mode::Local` runs `PasteModel` on a worker pool in this process.
`Mode::Socket` stands up a `RemoteServer` behind a real `QtWebSocketServer` on
an ephemeral port and gives the client its own `QtWebSocketBackend` over a
loopback socket. **Nothing between those two lines changes.** Not the model,
not the DTOs, not the registration, not the call site — only which backend the
`Bridge` was constructed with.

The application-level equivalent is the two command lines from section 3, and
you can watch the difference from the outside. Running the client against the
server produces this in the server's log:

```
[INFO ] [QtWebSocketServer] connection 1 accepted (1 live)
[DEBUG] [dispatchMessage] connection 1: kind=register callId=1 typeId=PasteModel …
[DEBUG] [dispatchMessage] connection 1: kind=register callId=2 typeId=PasteModel …
[DEBUG] [dispatchMessage] connection 1: kind=execute callId=3 modelId=… modelType=PasteModel actionType=ListPastes bodyBytes=2
[INFO ] [QtWebSocketServer] connection 1 disconnected (0 live), closeCode=1000 reason=
```

(Abridged: the real lines carry a few more empty fields, and `modelId` is a
freshly minted opaque number that differs per run.)

Running it without `--server` produces no such traffic at all — the same
`PasteBridge`, the same `PastePresenter`, the same `BridgeHandler<PasteModel>`,
the same `ListPastes`, executed in-process.

**What actually differs**, and is worth knowing before you rely on it:

- **Registration timing.** Remote mode's registration is a round trip. Build
  your handlers inside a readiness callback (section 9) and gate first fetches
  on `Bridge::whenBound()`.
- **Authorization exists only remotely.** `RemoteServer` runs every request
  through an `IAuthorizer`; `LocalBackend` does not authorize at all. The
  default authorizer allows everything (section 13).
- **Message size is bounded remotely.** A `CreatePaste` too large for the
  server's envelope limit is refused by the transport, as a typed error the
  client renders. In-process there is no such bound.
- **Who owns the store.** In remote mode the server owns the database. A
  client that also opens the same SQLite file behind the server's back is a
  second writer, which is exactly the contention pastebin's `SQLITE_BUSY` work
  exists to avoid — so pastebin's client calls `db::setup()` **only** in local
  mode.

`SimulatedRemoteBackend` (section 2) is the third shape: the same
serialise/dispatch path as the socket, with no socket. A bug that only appears
remotely is usually reproducible in a plain unit test through it.

## 13. What morph does not do

A guide that only sells is worth less than one that tells you when to stop.
Read [`docs/spec/`](spec/) before relying on any of these; the short version:

- **It is not a general-purpose RPC framework.** Request/response only. No
  streaming, no bidirectional channels, no server-initiated push.
- **`Completion<T>` is a leaf callback primitive, not a future.** No `T→U`
  chaining, no `co_await`, no cancellation, and no lifetime binding (section
  8 — write the guard yourself,
  [#138](https://github.com/LASTRADA-Software/morph/issues/138)).
- **Security is app-supplied.** There is no built-in authentication: a
  `Context`'s identity is whatever the client claims until an `IAuthorizer`
  verifies it, and the default authorizer allows everything. Version
  negotiation, size and timeout bounds, opaque model ids and register/instance
  authorization hooks all ship, but they are **opt-in**. A server that
  configures none of them assumes a trusted, authenticated transport and is not
  a hardened public-internet server. pastebin deliberately runs that fail-open
  default and has a test asserting the delta, as executable documentation.
- **Instance subscriptions are best-effort and in-process.** `subscribe<R>`
  fans out to handlers on the same `Bridge`; two separate clients sharing an
  instance do not see each other's results until they ask again. No replay, no
  durability, no coalescing.
- **Exact numbers are fixed-width.** `Rational` is an `int64` pair; `+`/`-`/`*`
  can overflow rather than returning an error, and high decimal precision
  shrinks the representable magnitude.
- **The journal is an audit trail, not event sourcing.** `replay()` /
  `undoLast()` re-execute the recorded action against a fresh model *object* —
  which does not isolate the database that object immediately reopens. For any
  DB-backed model that is a live mutation, not a reconstruction. pastebin's
  README works this through in detail and concludes that its UI must never
  expose a raw undo over the journal.
- **Registration is global, macro-driven and runtime-resolved.** No runtime
  deregistration; an unknown type id fails at runtime, not compile time.
- **Conflict resolution is not a framework concern.** On a backend switch morph
  fires `onBackendChanged()` on the fresh model instance and steps back.
- **There is no install/export target.** `find_package(morph)` does not exist
  today; consuming morph from outside this tree means `add_subdirectory` /
  `FetchContent`, or adding `include/` to your include path and wiring Glaze
  yourself ([#232](https://github.com/LASTRADA-Software/morph/issues/232)).
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
| how do I generate a form? | `docs/spec/forms/forms.md`, and [`examples/forms`](../examples/forms) |
| how do I test all this? | [`examples/TESTING.md`](../examples/TESTING.md) |
| what are the rules for an app built on morph? | [`examples/IMPLEMENTATION.md`](../examples/IMPLEMENTATION.md) |
| what does a bigger app look like? | [`examples/LADDER.md`](../examples/LADDER.md) — the rungs above this one, and what each is for |

For the production-hardening features this guide skipped — the action log and
transactional outbox, offline queue durability, register authorization,
transport limits, protocol versioning, observability and graceful shutdown —
[`examples/concepts`](../examples/concepts) has one small, runnable file each.
