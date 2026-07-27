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
desktop GUI** — which also builds to **WebAssembly** and runs entirely in the browser.

> **▶ Try the live demo:** **https://lastrada-software.github.io/morph/demo/**
> It opens signed in as a seeded demo user (`demo` / `demo1234`) with two accounts.
> Everything runs client-side — no server. First load fetches a ~31 MB `.wasm`, so
> give it a few seconds. (See [WebAssembly demo](#webassembly-demo-self-contained-github-pages).)

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
- **`include/bank/db/`** — Lightweight entity records (`*_entity.hpp`, aggregated by
  `entities.hpp`), the shared `WithMapper` mixin (one lazily-opened `DataMapper` per
  model), `user_ops.hpp` (principal→`user_id` resolution), and reusable `ledger_ops.hpp`
  (relation-aware debit/credit/post-entry + the `loadOwned` ownership guard).
- **`include/bank/models/` + `src/models/`** — the models. The `BRIDGE_REGISTER_*`
  macros live in the **model header** so every `.execute()` call site sees the
  `ActionTraits` specialisation. `AccountModel` and `CustomerModel` are **stateful
  and keyed** (see below); the remaining models are still per-domain and stateless.
- **`src/db/schema.cpp`** — all `LIGHTWEIGHT_SQL_MIGRATION` table definitions.
- **`include/bank/app/` + `src/app/`** — `App`: shared worker pool, GUI executor,
  `Bridge`, database setup, and login (which sets the bridge's default session).

### Stateful, keyed models

`AccountModel` holds **one account, in memory**, for the lifetime of the instance. It
declares `using PrimaryKey = std::int64_t`, so morph keys instances by account id, and
`GetAccount`/`CloseAccount` declare that they carry that key (`BRIDGE_KEY_FROM`). Two
`BridgeHandler<AccountModel, AllowShared>` handlers naming the same account — in one
GUI, or in two clients over one `RemoteServer` — reach a single instance and a single
balance.

This is the shape morph is built around, and it is what makes the per-model strand
load-bearing: the instance owns mutable state, so its unsynchronised read-modify-write
is correct precisely because no two actions on one instance ever overlap.

`CustomerModel` is the per-*owner* half that `AccountModel` used to also be doing:
`ListAccounts` and `OpenAccount` were never account-scoped, which is why both DTOs
carry an `owner` while `GetAccount`/`CloseAccount` carry an account id. It is keyed by
owner username.

SQLite stays authoritative. The instance is a cache with identity: hydrated on first
use, written through on every mutation, dropped when the instance goes away.

**The honest edge.** `Transfer`, bill payment and loan disbursement move money across
two accounts inside a single `SqlTransaction` owned by a *different* model, because
morph has no cross-instance transaction and this example must not imply it does. Those
writes land behind a cached row's back, so every balance write bumps a counter in
[`bank/db/row_versions.hpp`](include/bank/db/row_versions.hpp) and a cached reader
re-hydrates when the version it captured is stale. A real deployment would use the
store's own row version instead.

### Why per-model `DataMapper`?

morph runs each model on its own strand (single-threaded), so a model can own its own
connection with no locking. The database is an **on-disk SQLite file** (not `:memory:`,
which is private per connection) so every model's connection sees the same data.
Cross-row atomic operations (transfer, bill payment, loan disbursement/repayment) run
inside a `SqlTransaction`.

### Relations: `BelongsTo` / `HasMany`

The schema is modelled with Lightweight's relation types rather than bare foreign-key
columns:

- **`BelongsTo<&UserRecord::id>`** — every owned record (`accounts`, `payees`, `cards`,
  `loans`, `payments`, `budgets`, `notifications`) references its owner by `user_id`.
  Authorization is expressed *through the relation*: `db::loadOwned` navigates
  `rec->user->username` (lazily loaded) and compares it to the session principal.
- **`BelongsTo` for the id FKs** — `transactions.account_id`, the nullable
  `transactions.counterparty_id` (NULL for deposits/withdrawals, set for transfers),
  `payments.from_account_id` / `payee_id`, `cards.account_id`, `loans.account_id`.
- **`HasMany`** — `UserRecord::accounts` (used by `ListAccounts` and `StatementModel`)
  and `PayeeRecord::payments`. The migrations also declare the matching SQL
  `RequiredForeignKey`/`ForeignKey` constraints.

Two quirks of the current Lightweight version (non-reflection build) shaped the entity
layout, and are documented inline in the `*_entity.hpp` headers:

1. **`HasMany<Child>` resolves the child's foreign key by *ordinal member index***, not
   by type — so each `HasMany` field is placed at the same member index as the
   back-pointing `BelongsTo` on the child (e.g. `UserRecord::accounts` and
   `AccountRecord::user` are both at index 5). The relations test locks this in.
2. **A record that has a `HasMany` member can't be used with `DataMapper::Update` or the
   fluent `Query<T>()`** (both enumerate every member without a storage guard). So
   `UserRecord`/`PayeeRecord` are read-only *aggregates* (used via `Create`/`Delete`/
   `QuerySingle` + navigation), and a relation-free *projection* over the same table
   (`UserRow`/`PayeeRow`) backs the fluent list queries and credential updates.
   `AccountRecord` carries no `HasMany` at all because it is updated on every balance
   change.

The wire DTOs are unchanged by this (they still expose `owner` as a username and ids as
integers), so the GUI and CLI are unaffected; models map the relation values to the DTO.

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

### WebAssembly demo (self-contained, GitHub Pages)

**Open it in a browser — nothing to install:** **https://lastrada-software.github.io/morph/demo/**

You land signed in as the seeded demo user (`demo` / `demo1234`) with two accounts;
open accounts, deposit/withdraw/transfer, issue cards, add payees & pay bills, and take
a loan. It's a static page — the first load fetches a ~31 MB `.wasm` (streaming-compiled
by the browser), then it's instant. Published from `master` by
[`.github/workflows/wasm-demo.yml`](../../.github/workflows/wasm-demo.yml).

`gui_wasm/` is a **single-threaded WebAssembly** build of the same GUI that runs
**entirely in the browser** — the morph model layer is the "server in the background,"
with no external process. Since Lightweight (ODBC/SQLite) can't run in a browser, the
WASM build swaps persistence for an **in-memory store** (`gui_wasm/include/bank/wasm/`)
behind **shadow model headers** that shine ahead of the native ones on the include path,
so the QML, controllers and DTOs are reused unchanged. `BankClient` is dual-moded
(`#ifdef __EMSCRIPTEN__`): models run on the Qt event loop via `QtExecutor` (no thread
pool, no database). Single-threaded ⇒ no SharedArrayBuffer ⇒ **no COOP/COEP headers**, so
plain GitHub Pages hosts it. A demo user (`demo` / `demo1234`) and two accounts are
seeded and auto-signed-in.

Build locally (needs Qt-for-WASM + a matching emsdk):

```
source /path/to/qt6-wasm/emsdk/emsdk_env.sh
export EM_CACHE="$PWD/.emcache"
/path/to/qt6-wasm/bin/qt-cmake -S . -B build-wasm -G Ninja \
  -DMORPH_BUILD_EXAMPLES=ON -DMORPH_BUILD_BANK_EXAMPLE=ON \
  -DMORPH_BUILD_BANK_GUI=ON -DMORPH_BUILD_TESTS=OFF
cmake --build build-wasm --target bank_gui_wasm
python3 -m http.server -d build-wasm/examples/bank/gui_wasm 8000   # open bank_gui_wasm.html
```

CI (`.github/workflows/wasm-demo.yml`) builds the bundle with a matched host+wasm Qt pair
and publishes it to `…github.io/<repo>/demo/`, coexisting with the Doxygen docs at the
site root. The native (non-Emscripten) build is unaffected — the whole native stack is
gated `if(NOT EMSCRIPTEN)` in `CMakeLists.txt`.

## Tests

Each model has a `tests/test_*.cpp` (Catch2). `tests/bank_test_support.hpp` provides
`await(completion, gui)` — pumps the GUI executor until a `Completion` resolves — and a
shared on-disk test database. Notable cross-cutting tests:

- `test_remote.cpp` — runs `AccountModel` over `SimulatedRemoteBackend` and shows a
  custom `IAuthorizer` rejecting an action.
- `test_offline.cpp` — parks deposits in an `InMemoryOfflineQueue` while "offline" and
  replays them via `SyncWorker` on "reconnect".

## Status

Models, tests, CLI, the Qt 6 GUI, and a self-contained WebAssembly build (hosted
on GitHub Pages) are complete. Possible extensions: durable in-browser
persistence (IDBFS/OPFS) for the WASM build, switching its in-browser backend to
`RemoteServer` + `SimulatedRemoteBackend` to surface the JSON wire protocol, and
wiring the desktop GUI over `morph::qt::QtWebSocketBackend` for a true networked
client/server split.
