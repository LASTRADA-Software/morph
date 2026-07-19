# morph

A typed, asynchronous bridge between your UI and business-object models.

You call a model from the UI and get the result back on the UI — without
writing any concurrency, serialisation, or transport code, and without the call
site caring whether the model runs in this process or across a socket.

Models may live **in-process** (local mode) or in a **remote server process**
(remote mode). The GUI code is identical in both cases — only the backend
implementation changes. Results come back as a `Completion<T>` whose callbacks
always fire on the executor you chose (e.g. your UI event loop), so the UI never
blocks and never has to deal with a background worker directly.

- Header-only, C++23, namespace `morph`
- JSON reflection via [Glaze](https://github.com/stephenberry/glaze) — no
  hand-written serialisation per action
- Exact, unit-tagged action values (`morph::math::Rational`,
  `morph::units::Quantity<Unit>`) and JSON-Schema generation per action
  (`morph::forms`) so clients can build their forms at runtime
- Ordered, replayable action log (`morph::journal`) and offline/reconnect
  primitives (`morph::offline`)
- Optional Qt 6 WebSocket transport (built when `MORPH_BUILD_QT=ON`)

## What problem does it solve?

Desktop and cross-process apps repeatedly re-solve the same plumbing: move a
request off the UI thread, run it, marshal the result back, and — sooner or
later — do the same thing again over a socket when the logic moves to a server.
morph collapses that into one seam. You write the model as **plain,
single-threaded C++**; the framework owns:

- **Concurrency** — one *strand* per model instance serialises that model's
  calls while different models run in parallel, so model authors never touch a
  mutex.
- **Marshalling** — `.then(...)` / `.onError(...)` always run on the executor
  you pass, so results land on the UI thread and the UI thread never blocks.
- **Transport** — the same registered action travels a JSON wire protocol to a
  remote `RemoteServer` with no change to the model or the call site.

### Who it's for

A good fit if you are building a C++ GUI (Qt or otherwise) over a
request/response domain model and want the option to relocate that model to a
server later without rewriting the UI, or you want schema-driven forms and exact
decimal values for financial/lab data. Less of a fit if you need a
general-purpose RPC framework, streaming/bidirectional channels, or a hardened
public-internet server — see [Status & limitations](#status--limitations).

## The main idea

You write a model as plain, single-threaded C++. The framework handles
concurrency (one strand per model), result marshalling, and transport:

```cpp
struct MyAction { int x = 0; };

struct MyModel {
    int execute(const MyAction& a) { return a.x * 2; }
};
```

Register the model and its actions once, in the `.cpp` that owns the model:

```cpp
#include <morph/registry.hpp>

BRIDGE_REGISTER_MODEL (MyModel,            "MyModel")
BRIDGE_REGISTER_ACTION(MyModel, MyAction,  "MyAction")
```

> **Registration is per–translation-unit.** Put each `BRIDGE_REGISTER_*` call in
> exactly one `.cpp` (the one that owns the model), never in a header — the
> macros emit an explicit template specialisation plus a file-scope initialiser,
> so a header invocation is an ODR violation. If your models live in a **static
> library**, force-link it (`--whole-archive` / `-force_load` /
> `WHOLE_ARCHIVE`), or the linker will drop the unreferenced registration object
> and the model silently never registers.

Then drive it from the GUI — **the same code works local or remote**:

```cpp
#include <morph/bridge.hpp>
#include <morph/backend.hpp>
#include <morph/executor.hpp>

morph::exec::ThreadPoolExecutor pool{4};        // worker pool that runs models
morph::exec::MainThreadExecutor guiExecutor;    // stands in for your GUI event loop

// Local: model lives in this process, executed on the worker pool.
morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
morph::bridge::BridgeHandler<MyModel> handler{bridge, &guiExecutor};

handler.execute(MyAction{21})
    .then   ([](int result)            { /* result == 42, runs on the GUI side */ })
    .onError([](std::exception_ptr e)  { /* also runs on the GUI side */ });
```

To go remote, construct the `Bridge` with a different backend (e.g.
`SimulatedRemoteBackend` or `QtWebSocketBackend`) — the model, the registration,
and the call site above are unchanged.

> **Lifetime rule:** a `Bridge` must outlive every `BridgeHandler` registered on
> it. A handler deregisters itself from its bridge on destruction, so destroying
> the bridge first is undefined behaviour. Construct the bridge before, and
> destroy it after, all handlers that use it.

### The three pieces

- **`guiExecutor`** — a `morph::exec::IExecutor*` that decides *where* result
  callbacks run. Models execute on a worker pool, but `.then(...)` /
  `.onError(...)` are always marshalled back onto this executor, so they land
  wherever you want (typically the GUI event loop) and never block it. Use
  `MainThreadExecutor` (pump it with `runFor(...)` from your loop) for a plain
  app, or `morph::qt::QtExecutor` for Qt. The worker pool you pass to
  `LocalBackend` is a *separate* executor — it runs the models themselves.
- **`Bridge`** — the single, process-wide hub. It owns the active backend (local
  or remote) and tracks every live handler so it can re-point them when the
  backend is swapped (e.g. going offline → online). You construct **one**
  `Bridge` and share it.
- **`BridgeHandler<M>`** — your typed, GUI-facing handle to one model type `M`.
  It registers `M` on the bridge on construction and deregisters on destruction
  (RAII). This is the object you call `.execute(...)` / `.subscribe<A>(...)` on.
  Create one per model type, wherever in the UI you need to talk to that model.

## Multiple models across multiple files

A real app has several models and constructs handlers in different translation
units (one per screen/widget). The pattern is: **declare each model and its
actions in a header, register it once in a matching `.cpp`, share a single
`Bridge`, and construct a `BridgeHandler<M>` wherever you need it.**

The model and its action/result types go in a header. Any code that constructs
`BridgeHandler<OrdersModel>` or calls `.execute(PlaceOrder{...})` needs the
complete `OrdersModel` definition, so put it here rather than forward-declaring
it:

```cpp
// orders_model.hpp
#pragma once

struct PlaceOrder { int sku = 0; int qty = 0; };
struct OrderId    { long id = 0; };

struct OrdersModel {
    OrderId execute(const PlaceOrder& a);   // defined in orders_model.cpp
};
```

The matching `.cpp` includes that header and registers the model. The
registration macros run at static-init time, so simply linking this `.cpp` into
your binary makes the model known to the framework — there is no central list to
maintain:

```cpp
// orders_model.cpp
#include "orders_model.hpp"
#include <morph/registry.hpp>

OrderId OrdersModel::execute(const PlaceOrder& a) { /* ... */ return OrderId{/* ... */}; }

BRIDGE_REGISTER_MODEL (OrdersModel,             "OrdersModel")
BRIDGE_REGISTER_ACTION(OrdersModel, PlaceOrder, "PlaceOrder")
```

In the GUI layer, share one `Bridge` and one `guiExecutor`; each screen owns the
handler for the model it drives:

```cpp
// app.cpp  — wiring shared across the whole UI.
morph::exec::ThreadPoolExecutor pool{4};
morph::exec::MainThreadExecutor  guiExecutor;
morph::bridge::Bridge            bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
```

```cpp
// orders_screen.cpp  — uses the Orders model only.
#include "orders_model.hpp"

morph::bridge::BridgeHandler<OrdersModel> orders{bridge, &guiExecutor};

orders.execute(PlaceOrder{.sku = 42, .qty = 3})
    .then([](OrderId id) { showConfirmation(id); });
```

The screen files reach the shared `bridge` and `guiExecutor` however your app
exposes them (an `extern` declaration, a context object, dependency injection).
Each `BridgeHandler` is independent and tied to its own model type; they all
share the same `Bridge` and the same callback executor, and when a handler goes
out of scope it cleanly deregisters itself.

> A model that is used **remotely** must be registered via
> `BRIDGE_REGISTER_MODEL` and be **default-constructible** — the server
> re-creates instances from the string type id through the registry, and cannot
> use an ad-hoc factory closure the way `LocalBackend` can. A model that works
> locally via a capturing factory will fail at `register` time in remote mode if
> it isn't macro-registered.

## Subsystems

morph is layered: the async/bridge core is always present; everything else is an
opt-in header you include only if you need it.

| Namespace | Header(s) | What it gives you |
|---|---|---|
| `morph::exec` | `executor.hpp`, `strand.hpp` | `IExecutor`, `ThreadPoolExecutor`, `MainThreadExecutor`, per-model `StrandExecutor` |
| `morph::async` | `completion.hpp` | `Completion<T>` — move-only result handle with `.then` / `.onError` |
| `morph::model` | `registry.hpp`, `model.hpp` | Registration traits, validators, `ActionDispatcher`, type-erased holders |
| `morph::backend` | `backend.hpp`, `remote.hpp` | `LocalBackend`, `RemoteServer`, `SimulatedRemoteBackend` |
| `morph::bridge` | `bridge.hpp` | `Bridge`, `BridgeHandler<M>` — the user-facing API |
| `morph::wire` | `wire.hpp` | JSON `Envelope` protocol between client and server |
| `morph::session` | `session.hpp` | `Context` (principal/locale/metadata), `IAuthorizer` |
| `morph::journal` | `journal.hpp`, `action_log.hpp`, `file_action_log.hpp` | Ordered, coalescing, replayable action log; undo via replay |
| `morph::offline` | `network_monitor.hpp`, `offline_queue.hpp`, `sync_worker.hpp`, `reconnect_coordinator.hpp` | Connectivity detection, write queue, replay-on-reconnect |
| `morph::math` | `rational.hpp` | Exact `int64` rational values with a decimal-precision tag |
| `morph::units` | `quantity.hpp` | Unit-tagged, optionally-empty values with a compile-time unit algebra |
| `morph::time` | `datetime.hpp` | UTC `DateTime` / optionally-empty `Timestamp` with strict ISO-8601 codec |
| `morph::forms` | `forms.hpp`, `choice.hpp` | JSON-Schema generation and form-field types for runtime-built GUIs |
| `morph::qt` | `qt/*.hpp` | `QtExecutor`, `QtWebSocketBackend`, `QtWebSocketServer` (with `MORPH_BUILD_QT=ON`) |

The full design rationale for each is in [`docs/spec/`](docs/spec) (one file per
type/subsystem) and [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Exact values, units, and auto-generated forms

Actions can carry exact, unit-tagged values instead of raw doubles, and every
action type can describe itself as a JSON Schema — enough for a client to render
its form at runtime:

```cpp
#include <morph/forms.hpp>   // pulls in quantity.hpp + rational.hpp

// The application defines its unit system: an enum, its metadata, and a
// consteval algebra (see examples/forms/lab_units.hpp for the full pattern).
enum class Unit : std::uint16_t { scalar, kg, m3, kg_per_m3 };

using Mass    = morph::units::Quantity<Unit::kg>;
using Volume  = morph::units::Quantity<Unit::m3>;
using Density = morph::units::Quantity<Unit::kg_per_m3>;

struct ComputeDryDensity {
    Mass massDry{};      // starts empty ("not entered"), unit known from the type
    Volume volume{};
    bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

struct LabModel {
    // kg / m³ -> kg/m³, deduced and checked at compile time; kg + m³ won't compile.
    Density execute(const ComputeDryDensity& a) { return a.massDry / a.volume; }
};
```

- Values are exact `int64` fractions with a decimal-precision tag — no
  floating-point rounding, canonical `{"num":617,"den":50,"dp":2}` on the wire.
  Units never travel; they live in the C++ types and the schemas. (The `int64`
  numerator/denominator is a fixed-width rational, not a bignum — see
  [Status & limitations](#status--limitations).)
- Each field *declares* its precision in the type — the unit's default unless
  overridden (`Quantity<Unit::m3, 4>`) — which becomes the form's input step
  (`x-decimalPlaces`); the value's actual precision stays runtime data that
  propagates through arithmetic and can be retagged (`withDecimalPlaces`).
- `morph::time::Timestamp` fields travel as strict ISO-8601 UTC strings and
  render as date-time inputs (`"format": "date-time"`); malformed timestamps are
  wire *errors*, not clamped values.
- Unit systems may declare convertible entry units with exact ratios
  (`UnitTraits<E>::relations`): fields grow a unit selector, values recalculate
  exactly on switch, and payloads always stay canonical. The schema's
  `x-unitAlternatives` list is derived from the same `relations` that drive
  conversion, so there is nothing to keep in sync.
- `morph::forms::Choice<std::int64_t, "ListSamples">` declares a combo box: the
  options are the rows returned by executing the named action, referenced from
  the schema via `x-optionsAction` — the option list stays living data served by
  the model, never a copy baked into the form. (The action name and row fields
  are string references, resolved at runtime — a typo fails on the client, not at
  compile time.)
- A `Quantity` can be *empty* ("not measured yet"). A field is required unless it
  is `std::optional` or listed in the action's `optionalFields` opt-out — the
  same declaration drives the schema's `required` array, the form's submit gate,
  and `validate()`. Note the schema's `required` is a **client-side** gate; the
  server dispatcher runs whatever payload arrives, so a model must re-check its
  own preconditions.
- `morph::forms::schemaJson<ComputeDryDensity>()` returns a JSON Schema with
  units (`ExtUnits`), decimal steps (`x-decimalPlaces`), field order (`x-order`),
  bounds, and descriptions (via `glz::json_schema<A>`).

[`examples/forms`](examples/forms) shows the whole loop with two renderers
driven purely by the generated schemas — a self-contained HTML page and a
runtime-built Qt Quick GUI:

```sh
cmake -B build -G Ninja -DMORPH_BUILD_FORMS_QML=ON    # QML renderer optional
ninja -C build
./build/examples/forms/morph_forms_demo --emit-html   # write forms_demo.html, open it
./build/examples/forms/morph_forms_demo               # REPL: paste lines from the page
./build/examples/forms/gui_qml/morph_forms_qml        # same forms, rendered in QML
```

## Action log (audit & undo)

`morph::journal` records executed actions as an ordered, replayable log —
distinct from the offline queue: the queue holds *pending* writes and deletes
them once delivered, while the journal is a permanent trail the framework never
prunes. Install one sink in `main()` and every model records automatically:

```cpp
morph::journal::setActionLog(std::make_shared<morph::journal::FileActionLog>("audit.ndjson"));
```

Each successful, loggable action becomes a `LogEntry` (model type, entity key,
payload/result JSON, principal, timestamp). `SessionLog` adds in-memory history
with `checkpoint()` coalescing and `undoLast()`, which reconstructs state by
*replaying* the remaining actions. This model is exact for pure, deterministic,
in-memory models; a model that owns an external store (SQL, network) needs care,
because replay re-executes actions rather than re-applying stored results — see
the design notes in [`docs/spec/journal/journal.md`](docs/spec/journal/journal.md).

## Offline support

`morph::offline` adds connectivity primitives for apps that must keep working
while disconnected:

- **`NetworkMonitor`** — background probe thread; fires `onOffline` / `onOnline`
  on state change. Callbacks run on the probe thread and must not block — set an
  atomic or post to an executor, don't run reconnect logic inline.
- **`IOfflineQueue`** — durable store of opaque payloads enqueued while offline.
  Only an in-memory implementation ships; a durable (SQL/file) queue is the
  caller's to provide against the interface.
- **`SyncWorker`** — drains the queue and replays each item on reconnect, with
  retry and dead-lettering. Replay must be idempotent (items are retried).
- **`ReconnectCoordinator`** — sequences *reconnect → activate → bind → replay*
  in a strict, unit-tested order. Pure policy: all side effects are injected, so
  it owns no thread and touches no socket.

Conflict resolution is deliberately **not** a framework concern: on a backend
switch morph fires `onBackendChanged()` on the fresh model instance and steps
back; how the model reconciles is its own business.

## Sessions & authorization

Each `execute` can carry a `morph::session::Context` (principal, locale,
metadata) across the wire, and `RemoteServer` runs every request through an
`IAuthorizer` before dispatch. This is a **policy** hook, not authentication:
the `Context` is client-supplied data with no built-in integrity, the default
authorizer allows everything, and the local backend does not authorize at all.
Treat authentication as the transport's job (e.g. TLS + a token your authorizer
validates) and enforce security-critical checks inside the model. See
[`docs/spec/session/session.md`](docs/spec/session/session.md).

## Building & dependencies

- **Compiler:** C++23 (developed against recent Clang; libstdc++/libc++). The
  default logger uses `std::println`, so a C++23 standard library is required.
- **Dependencies:** [Glaze](https://github.com/stephenberry/glaze) (JSON
  reflection), fetched via [vcpkg](https://vcpkg.io) (`vcpkg.json` manifest).
  Optional: Qt 6 for the WebSocket transport and QML example.
- **Build system:** CMake (presets in `CMakePresets.json`) + Ninja.

```sh
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset clang-release
cmake --build build/clang-release --target morph_tests
./build/clang-release/tests/morph_tests
```

Relevant CMake options: `MORPH_BUILD_TESTS`, `MORPH_BUILD_EXAMPLES`,
`MORPH_BUILD_QT`, `MORPH_BUILD_FORMS_QML`, `MORPH_BUILD_DOCUMENTATION`.
Because morph is header-only, using it in your own project is just adding
`include/` to your include path and linking Glaze.

## Examples

- [`examples/forms`](examples/forms) — the schema-driven forms loop (HTML +
  QML renderers) over an exact-value lab model.
- [`examples/bank`](examples/bank) — a larger app: SQLite-backed models behind
  DTO/entity layers, a Qt desktop GUI, and a self-contained single-threaded Qt
  **WASM** build. Demonstrates the action log end-to-end and the local/remote
  split at app scale.

## Status & limitations

morph is a young, actively developed library with thorough test coverage of its
core. It is honest about the following boundaries — read the per-subsystem specs
in [`docs/spec/`](docs/spec) before relying on any of these in production:

- **Security is app-supplied.** The wire protocol has no version negotiation,
  no message-size or timeout bounds, and no built-in authentication; `Context`
  identity is unauthenticated and `RemoteServer` model ids are guessable
  sequential integers with control messages unauthorized. `RemoteServer`
  assumes a trusted, authenticated transport — it is not a hardened
  public-internet server as shipped.
- **`Completion<T>` is a leaf callback primitive**, not a composable future: one
  handler per outcome, no `T→U` chaining, no `co_await`, no cancellation.
- **Exact numbers are fixed-width.** `Rational` is an `int64` pair; `+`/`-`/`*`
  can overflow (undefined behaviour) rather than returning an error, and high
  decimal precision shrinks the representable magnitude. Wire input is *clamped*,
  not rejected, on malformed values.
- **Journal replay re-executes actions**, so undo/reconstruction is only exact
  for pure, in-memory models; there is no transactional outbox tying the log to a
  model's own store yet.
- **Offline durability is bring-your-own.** Only an in-memory queue ships; the
  crash-safety story depends on a durable queue you implement.
- **Registration is global and macro-driven** (per-TU, static-init, string type
  ids); there is no runtime deregistration and unknown ids fail at runtime, not
  compile time.

## Learn more

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — namespace map, layer diagram,
  wire protocol, deployment topologies, and design rationale.
- [`docs/spec/`](docs/spec) — the authoritative per-type/subsystem design specs.

## License

Apache-2.0.
