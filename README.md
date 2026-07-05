# morph

A typed, asynchronous bridge between your UI and business-object models.

You call a model from the UI and get the result back on the UI — without
writing any concurrency, serialisation, or transport code, and without the call site
caring whether the model runs in this process or across a socket.

Models may live **in-process** (local mode) or in a **remote server process** (remote
mode). The GUI code is identical in both cases — only the backend implementation
changes. Results come back as a `Completion<T>` whose callbacks always fire on the
executor you chose (e.g. your UI event loop), so the UI never blocks and never has to
deal with a background worker directly.

- Header-only, C++23, namespace `morph`
- JSON reflection via [Glaze](https://github.com/stephenberry/glaze) — no hand-written
  serialisation per action
- Exact, unit-tagged action values (`morph::math::Rational`,
  `morph::units::Quantity<Unit>`) and JSON-Schema generation per action
  (`morph::forms`) so clients can build their forms at runtime
- Optional Qt 6 WebSocket transport (built when `MORPH_BUILD_QT=ON`)

## The main idea

You write a model as plain, single-threaded C++. The framework handles concurrency
(one strand per model), result marshalling, and transport:

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
`SimulatedRemoteBackend` or `QtWebSocketBackend`) — the model, the registration, and
the call site above are unchanged.

### The three pieces

- **`guiExecutor`** — a `morph::exec::IExecutor*` that decides *where* result
  callbacks run. Models execute on a worker pool, but `.then(...)` / `.onError(...)`
  are always marshalled back onto this executor, so they land wherever you want
  (typically the GUI event loop) and never block it. Use `MainThreadExecutor` (pump it
  with `runFor(...)` from your loop) for a plain app, or `morph::qt::QtExecutor` for
  Qt. The worker pool you pass to `LocalBackend` is a *separate* executor — it runs
  the models themselves.
- **`Bridge`** — the single, process-wide hub. It owns the active backend (local or
  remote) and tracks every live handler so it can re-point them when the backend is
  swapped (e.g. going offline → online). You construct **one** `Bridge` and share it.
- **`BridgeHandler<M>`** — your typed, GUI-facing handle to one model type `M`. It
  registers `M` on the bridge on construction and deregisters on destruction (RAII).
  This is the object you call `.execute(...)` / `.subscribe<A>(...)` on. Create one
  per model type, wherever in the UI you need to talk to that model.

## Multiple models across multiple files

A real app has several models and constructs handlers in different translation units
(one per screen/widget). The pattern is: **declare each model and its actions in a
header, register it once in a matching `.cpp`, share a single `Bridge`, and construct
a `BridgeHandler<M>` wherever you need it.**

The model and its action/result types go in a header. Any code that constructs
`BridgeHandler<OrdersModel>` or calls `.execute(PlaceOrder{...})` needs the complete
`OrdersModel` definition, so put it here rather than forward-declaring it:

```cpp
// orders_model.hpp
#pragma once

struct PlaceOrder { int sku = 0; int qty = 0; };
struct OrderId    { long id = 0; };

struct OrdersModel {
    OrderId execute(const PlaceOrder& a);   // defined in orders_model.cpp
};
```

The matching `.cpp` includes that header and registers the model. The registration
macros run at static-init time, so simply linking this `.cpp` into your binary makes
the model known to the framework — there is no central list to maintain:

```cpp
// orders_model.cpp
#include "orders_model.hpp"
#include <morph/registry.hpp>

OrderId OrdersModel::execute(const PlaceOrder& a) { /* ... */ return OrderId{/* ... */}; }

BRIDGE_REGISTER_MODEL (OrdersModel,             "OrdersModel")
BRIDGE_REGISTER_ACTION(OrdersModel, PlaceOrder, "PlaceOrder")
```

A second model lives in its own header + `.cpp` pair, following the same shape:

```cpp
// inventory_model.hpp
#pragma once

struct CheckStock { int sku = 0; };
struct StockLevel { int onHand = 0; };

struct InventoryModel {
    StockLevel execute(const CheckStock& a);
};
```

```cpp
// inventory_model.cpp
#include "inventory_model.hpp"
#include <morph/registry.hpp>

StockLevel InventoryModel::execute(const CheckStock& a) { /* ... */ return StockLevel{/* ... */}; }

BRIDGE_REGISTER_MODEL (InventoryModel,             "InventoryModel")
BRIDGE_REGISTER_ACTION(InventoryModel, CheckStock, "CheckStock")
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

```cpp
// inventory_widget.cpp  — uses the Inventory model only.
#include "inventory_model.hpp"

morph::bridge::BridgeHandler<InventoryModel> stock{bridge, &guiExecutor};

stock.execute(CheckStock{.sku = 42})
    .then([](StockLevel s) { updateBadge(s.onHand); });
```

The screen files reach the shared `bridge` and `guiExecutor` however your app exposes
them (an `extern` declaration, a context object, dependency injection). Each
`BridgeHandler` is independent and tied to its own model type; they all share the same
`Bridge` and the same callback executor, and when a handler goes out of scope it
cleanly deregisters itself.

## Offline support

`morph::offline` adds connectivity primitives for apps that must keep working while
disconnected:

- **`NetworkMonitor`** — background probe thread; fires `onOffline` / `onOnline` on
  state change.
- **`IOfflineQueue`** — durable store of opaque payloads enqueued while offline.
- **`SyncWorker`** — drains the queue and replays each item on reconnect, with
  built-in retry and dead-lettering.
- **`ReconnectCoordinator`** — sequences *reconnect → activate → bind → replay* in a
  strict, unit-tested order when connectivity returns. Pure policy: all side effects
  are injected, so it owns no thread and touches no socket.

## Exact values, units, and auto-generated forms

Actions can carry exact, unit-tagged values instead of raw doubles, and every
action type can describe itself as a JSON Schema — enough for a client to
render its form at runtime:

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
  Units never travel; they live in the C++ types and the schemas.
- A `Quantity` can be *empty* ("not measured yet"). A field is required unless
  it is `std::optional` or listed in the action's `optionalFields` opt-out —
  the same declaration drives the schema's `required` array, the form's
  submit gate, and `validate()`.
- `morph::forms::schemaJson<ComputeDryDensity>()` returns a JSON Schema with
  units (`ExtUnits`), decimal steps (`x-decimalPlaces`), field order
  (`x-order`), bounds, and descriptions (via `glz::json_schema<A>`).

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

## Learn more

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the namespace map, layer
diagram, wire protocol, deployment topologies, and design rationale.

## License

Apache-2.0.
