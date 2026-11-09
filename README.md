# morph

A typed, asynchronous bridge between a UI and its business models, in C++23.

You write a model as plain, single-threaded C++. You call it from the UI and get
the result back on the UI thread. morph handles the threading, serialisation and
transport, and the call site is the same whether the model runs in this process
or on a server across a WebSocket.

> **New here?** [`docs/GETTING-STARTED.md`](docs/GETTING-STARTED.md) builds a
> small app end to end, first in-process and then over a socket.

## The idea in one example

A model and its actions are ordinary types:

```cpp
struct Double { int x = 0; };

struct Calculator {
    int execute(const Double& a) { return a.x * 2; }
};
```

Register them once, in the `.cpp` that owns the model:

```cpp
#include <morph/core/registry.hpp>

BRIDGE_REGISTER_MODEL (Calculator,         "Calculator")
BRIDGE_REGISTER_ACTION(Calculator, Double, "Double")
```

Call the model from the UI:

```cpp
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>

morph::exec::ThreadPoolExecutor pool{4};     // runs the models
morph::exec::MainThreadExecutor ui;          // where results are delivered; your event loop

morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), ui};
morph::bridge::BridgeHandler<Calculator> calc{bridge, &ui};

calc.execute(Double{21})
    .then   ([](int result)           { /* 42, on the UI thread */ })
    .onError([](std::exception_ptr e) { /* also on the UI thread */ });
```

To run the model on a server, construct the `Bridge` with a remote backend
instead of `LocalBackend`. The model, the registration and the call site do not
change. The few places where local and remote behave differently, such as
authorization and exception types, are listed in
[`docs/spec/core/locality.md`](docs/spec/core/locality.md).

### What each piece does

- **`Bridge`**: one per process. It owns the active backend and can swap it at
  run time, for example from offline to online. It belongs to one executor, its
  owner (usually the UI loop), and is constructed and used there.
- **`BridgeHandler<M>`**: the UI's typed handle to model `M`. Create one wherever
  a screen needs that model, and destroy handlers before their bridge.
- **The callback executor** (`&ui` above): every `.then` and `.onError` runs
  there, never on a worker thread. Use `MainThreadExecutor` for a plain loop or
  `morph::qt::QtExecutor` under Qt.
- **Strands**: each model instance runs its actions one at a time, while
  different models run in parallel. Models need no locks.

Two rules that bite:

- **Registration must survive the link.** If your models live in a static
  library, link it whole (`--whole-archive`, `-force_load`, or CMake's
  `WHOLE_ARCHIVE`). Otherwise the linker drops the registration and the model
  never registers.
- **A remote model must be registered and default-constructible**, because the
  server creates instances from the registered type name.

## What else is in the box

Everything beyond the core is opt-in: include a header only when you need it.

- **Server-driven screens.** A server serves its screens as declarative JSON
  documents (`ui/1`). One generic client renders them with Qt Quick, in a
  terminal, or in the browser through WebAssembly. Actions declare which data
  they change (`BRIDGE_REGISTER_EFFECTS`), so every open screen on every client
  refreshes without the screen saying what to reload.
  See [`docs/KEEPING-SCREENS-CURRENT.md`](docs/KEEPING-SCREENS-CURRENT.md) and
  [`docs/spec/ui/`](docs/spec/ui).
- **Forms from action types.** `morph::forms` generates a JSON Schema for every
  action, with units, precision, bounds, choices and required fields, so a
  client can build the form at run time.
- **Exact values.** `morph::math::Rational` is an exact decimal fraction,
  `morph::units::Quantity<Unit>` adds compile-time unit checking (kg / m³ gives
  kg/m³; kg + m³ does not compile), and `morph::time` has strict ISO-8601 UTC
  timestamps.
- **Shared instances.** Handlers that opt in with `AllowShared` and name the same
  primary key reach one model instance, across clients too. See
  [`docs/spec/core/shared_instances.md`](docs/spec/core/shared_instances.md).
- **Coroutines.** A client can `co_await` a `Completion<T>`, and a model's
  action can itself be a coroutine.
- **Action journal.** `morph::journal` keeps an ordered, replayable record of
  executed actions for auditing, and supports undo by replay.
- **Offline support.** `morph::offline` provides connectivity monitoring,
  durable write queues (file or SQLite), replay on reconnect and an ordered
  reconnect sequence.
- **Sessions and authorization.** A `session::Context` travels with every call,
  and `RemoteServer` checks each request with an `IAuthorizer` before it runs.
- **Transports.** Qt 6 WebSockets, or a raw POSIX socket transport.

## Building

On Linux:

```sh
cmake --preset clang-release
cmake --build build/clang-release --target morph_tests
./build/clang-release/tests/morph_tests
```

To use morph in your own project, install it and use
`find_package(morph CONFIG REQUIRED)` with `morph::morph`.
[`docs/BUILDING.md`](docs/BUILDING.md) covers dependencies (Glaze, core-cpp,
optional Qt), every CMake option, optional components, and vendoring.

## Examples

| Example | What it shows |
|---|---|
| [`examples/bookmarks`](examples/bookmarks) | The small, complete path: models, SQLite, sign-in, server-served screens, local and remote. Used by the tutorial. |
| [`examples/bank`](examples/bank) | A real domain: many models, money, shared instances, the offline queue, and a WebAssembly client |
| [`examples/lims`](examples/lims) | A large app: units, versioned forms, role-gated workflows, offline capture, the journal as an audit trail |
| [`examples/forms`](examples/forms) | Schema-driven forms over exact, unit-tagged lab values |
| [`examples/concepts`](examples/concepts) | One small test per production feature: outbox, durable queue, authorization, limits, versioning, shutdown |
| [`examples/client`](examples/client) | The generic client that renders every example's screens |

Bookmarks, bank and lims form the [application ladder](examples/LADDER.md),
apps of increasing size that exist to test morph rather than to ship.

## Limitations

- **Security is up to the application.** There is no built-in authentication,
  and the default authorizer allows everything. Size limits, timeouts, protocol
  versioning and authorization hooks exist but are opt-in. See
  [`docs/spec/security.md`](docs/spec/security.md).
- **`Rational` is fixed-width** (`int64` numerator and denominator). Arithmetic
  that overflows saturates and logs an error; use `checkedAdd` and similar
  functions to detect overflow.
- **Journal replay re-executes actions**, so undo is exact only for
  deterministic, in-memory models. Models with their own database can use the
  transactional outbox instead.
- **Registration is global and resolved at run time.** An unknown type name
  fails when it is called, not when it is compiled.
- **No stability promise before 1.0.** morph is `0.1.0`. The policy that will
  apply from 1.0 is in [`docs/spec/VERSIONING.md`](docs/spec/VERSIONING.md).

## Documentation

- [`docs/GETTING-STARTED.md`](docs/GETTING-STARTED.md): a tutorial that builds
  one app end to end.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md): layers, wire protocol,
  deployment topologies and the reasons behind them.
- [`docs/KEEPING-SCREENS-CURRENT.md`](docs/KEEPING-SCREENS-CURRENT.md): how
  screens refresh after an action, with a checklist for new actions.
- [`docs/BUILDING.md`](docs/BUILDING.md): dependencies, options, installation.
- [`docs/spec/`](docs/spec): the authoritative design spec for each type and
  subsystem.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) and [`AGENTS.md`](AGENTS.md): how to
  change morph.

## License

Apache-2.0.
