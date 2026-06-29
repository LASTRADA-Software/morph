# Bank — a worked example of `morph` + Lightweight

A feature-rich demo banking application built on two libraries:

- **[morph](../../README.md)** — the typed, asynchronous UI ↔ model bridge. Every
  banking domain is a plain single-threaded C++ model; morph handles concurrency
  (one strand per model), result marshalling, and transport. The *same* model code
  and call sites run **in-process (local)** or **across a server (remote)**.
- **[Lightweight](https://github.com/LASTRADA-Software/Lightweight)** — the ORM /
  ODBC layer. Each model persists to **SQLite** through a typed `DataMapper`; the
  schema is owned by Lightweight migrations.

It ships the models, a full test suite, a scripted CLI driver, and a **Qt 6
desktop GUI**.

## Architecture: two type layers

morph actions/results must be plain aggregates (Glaze serialises them onto the wire).
Lightweight entities are `Field<>`-wrapped structs. The example keeps them separate
and maps between them in the model:

```
GUI / CLI ──actions/results (plain DTOs)──▶ morph Bridge ──▶ Model
                                                              │  maps DTO ⇄ entity
                                                              ▼
                                                    Lightweight DataMapper ──▶ SQLite
```

- **`include/bank/dto/`** — wire DTOs (the morph action/result types). Amounts are
  integer **minor units** (cents); enums travel as their integer values.
- **`include/bank/db/`** — Lightweight entity records (`*_entity.hpp`), the shared
  `WithMapper` mixin (one lazily-opened `DataMapper` per model), and reusable
  `ledger_ops.hpp` (debit/credit/post-entry helpers used by every money-moving model).
- **`include/bank/models/` + `src/models/`** — one model per banking domain. The
  `BRIDGE_REGISTER_*` macros live in the **model header** so every `.execute()` call
  site sees the `ActionTraits` specialisation.
- **`src/db/schema.cpp`** — all `LIGHTWEIGHT_SQL_MIGRATION` table definitions.
- **`include/bank/app/` + `src/app/`** — `App`: shared worker pool, GUI executor,
  `Bridge`, database setup, and login (which sets the bridge's default session).

### Why per-model `DataMapper`?

morph runs each model on its own strand (single-threaded), so a model can own its own
connection with no locking. The database is an **on-disk SQLite file** (not `:memory:`,
which is private per connection) so every model's connection sees the same data.
Cross-row atomic operations (transfer, bill payment, loan disbursement/repayment) run
inside a `SqlTransaction`.

## Models & features

| Model | Actions |
|---|---|
| **Auth** | register, login, change password, `WhoAmI` (session introspection) |
| **Account** | open (checking/savings/credit), list, get, close; overdraft, interest |
| **Transaction** | deposit, withdraw, **atomic transfer**, paginated history |
| **Payee** | add (with IBAN validation + `set<>` streaming), remove, list |
| **Payment** | one-off bill pay, scheduled payments, standing orders, cancel |
| **Card** | issue debit/credit, freeze/unfreeze/cancel, set limit, change PIN |
| **Loan** | apply (disburse), amortization schedule, repay, payoff |
| **Budget** | per-category limits, spending-by-kind analytics |
| **Notification** | post, list (unread filter), mark read / mark all read |
| **Statement** | date-ranged credit/debit summary across all accounts |

morph features exercised: `Completion` then/onError, **sessions** (principal scoping +
authorization), **validation** (`validate()` + the `set<>`/`subscribe<>` streaming
form flow), **local ↔ remote parity**, a custom **`IAuthorizer`**, and the **offline
queue + `SyncWorker`** replay path.

## Build & run

The example is **off by default** (it pulls a heavy dependency tree — reflection-cpp,
stdexec, yaml-cpp, libzip — via Lightweight). Enable it from the repo root:

```sh
cmake -G Ninja -B build -S . -DMORPH_BUILD_BANK_EXAMPLE=ON
cmake --build build --target bank_tests bank_cli
```

Requirements: a C++23 compiler, **unixODBC**, and the **SQLite3 ODBC driver**
registered with unixODBC (the connection string is `DRIVER=SQLite3;Database=…`).

```sh
# Run the test suite
./build/examples/bank/bank_tests

# Run the scripted tour (same scenario on local, then remote, backend)
./build/examples/bank/bank_cli
```

### Qt 6 QML GUI

A **QML (Qt Quick)** desktop GUI (`gui/`) is built when `-DMORPH_BUILD_BANK_GUI=ON`
is also passed (requires Qt 6 Quick + Quick Controls 2):

```sh
cmake -G Ninja -B build -S . -DMORPH_BUILD_BANK_EXAMPLE=ON -DMORPH_BUILD_BANK_GUI=ON
cmake --build build --target bank_gui
./build/examples/bank/gui/bank_gui
```

Structure:

- **`gui/BankClient`** — owns the worker pool, a `morph::qt::QtExecutor`, the
  `Bridge` (local backend), DB setup, and the session. UI-toolkit-agnostic.
- **`gui/controllers/`** — one QObject controller per domain (`AppController`,
  `AccountController`, …), exposed to QML as context properties (`app`,
  `accounts`, `txns`, `cards`, `payees`, `loans`). Each calls
  `BridgeHandler<Model>.execute(...).then(...)` — callbacks land on the Qt GUI
  thread via `QtExecutor` — and publishes display-ready data as `Q_PROPERTY`
  `QVariantList`s (money pre-formatted), plus an `error(QString)` signal. The
  heavy morph/Lightweight includes are hidden from `moc` behind `#ifndef
  Q_MOC_RUN` (moc follows includes and its parser trips on them).
- **`gui/qml/`** — the front-end: `Main` (login ⇄ shell + error toast),
  `AppShell` (sidebar + stacked pages), the five pages, and reusable components
  (`Panel`, `AppButton`, `Field`, `Pill`, `Picker`). Bundled via
  `qt_add_qml_module` (URI `BankGui`). The warm, "Claude-inspired" palette is
  passed in from `main.cpp` as the `theme` object.

A headless screenshot smoke test runs when `BANK_GUI_SMOKE=<dir>` is set (with
`QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software`): it seeds data, signs in,
and grabs a PNG of each page.

## Tests

Each model has a `tests/test_*.cpp` (Catch2). `tests/bank_test_support.hpp` provides
`await(completion, gui)` — pumps the GUI executor until a `Completion` resolves — and a
shared on-disk test database. Notable cross-cutting tests:

- `test_remote.cpp` — runs `AccountModel` over `SimulatedRemoteBackend` and shows a
  custom `IAuthorizer` rejecting an action.
- `test_offline.cpp` — parks deposits in an `InMemoryOfflineQueue` while "offline" and
  replays them via `SyncWorker` on "reconnect".

## Status

Models, tests, CLI, and the Qt 6 GUI are complete. Possible extensions: wiring
the GUI over the Qt WebSocket backend (`morph::qt::QtWebSocketBackend`) for a
true client/server split, and surfacing the offline queue in the UI.
