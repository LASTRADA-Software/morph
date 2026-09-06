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

## Bank and the application ladder

Bank predates [the application ladder](../LADDER.md) and is **unnumbered prior
art**: `LADDER.md` cites it in its intro, measures rung effort in
"bank-equivalents", and gives it no rung number. That is deliberate — the
numbers in `LADDER.md`'s table are load-bearing for the rungs that consume each
other's answers, and bank has nothing to take but a renumbering. Concretely,
bank is absent from [`examples/rungs.txt`](../rungs.txt) and never calls
`morph_add_rung()`, which is what keeps it out of `ci.yml`'s `ladder-tests`
and `ladder-sanitizers`, `wasm-ladder.yml`'s build loop, `coverage.sh` and
`codecov.yml`'s per-rung components — the only workflow that builds bank at all
is `wasm-demo.yml`, and only its WebAssembly GUI. `ladder_bank_server` below is a local `add_executable` for the same
reason; the `ladder_` prefix is the scenario tooling's naming convention, not a
rung claim.

Conventions bank **shares** with the rungs: persistence exclusively through the
Lightweight ORM, with the schema owned by `LIGHTWEIGHT_SQL_MIGRATION`
definitions — [`LADDER.md`](../LADDER.md) calls that "bank's pattern" and binds
every rung to it — models as the application, plain aggregates on the wire, and
the shared testkit's `QmlSurfaceAudit`. Conventions it does **not** follow: no
schema-driven forms (its GUI hand-rolls one QObject controller per domain
rather than using `morph::qt::forms::FormsControllerCore`), no
`examples/common/gui` presenter architecture, and no
[`TESTING.md`](../TESTING.md) dual-deployment-mode rig — its remote coverage is
`SimulatedRemoteBackend` in `test_remote.cpp` plus the scenario corpus below.
Bringing those conventions into line is
[morph#87](https://github.com/LASTRADA-Software/morph/issues/87), which remains
open; the numbering question that issue also raises is the part that is settled.

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

`AccountModel` holds **one account, in memory**, for the lifetime of the instance. The
class itself says nothing about keys: a single `BRIDGE_KEY_FROM(AccountModel, GetAccount,
&GetAccount::id)` next to the other registrations both deduces the key type and records
which action carries it, so morph keys instances by account id. Two
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
| **Payee** | add (with IBAN validation), remove, list |
| **Payment** | one-off bill pay, scheduled payments, standing orders, cancel |
| **Card** | issue debit/credit, freeze/unfreeze/cancel, set limit, change PIN |
| **Loan** | apply (disburse), amortization schedule, repay, payoff |
| **Budget** | per-category limits, spending-by-kind analytics |
| **Notification** | post, list (unread filter), mark read / mark all read |
| **Statement** | date-ranged credit/debit summary across all accounts |

morph features exercised: `Completion` then/onError, **sessions** (principal scoping +
authorization), **validation** (`validate()` + the dispatch-path validator
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

### Standalone server (`ladder_bank_server`)

`src/server/main.cpp` builds a headless WebSocket server that hosts every bank
model over `morph::wire`, so a real out-of-process client can drive them. It
needs `-DMORPH_BUILD_QT=ON` (the transport is `morph::qt`'s `QtWebSocketServer`;
no GUI is involved):

```sh
cmake -G Ninja -B build -S . -DMORPH_BUILD_BANK_EXAMPLE=ON -DMORPH_BUILD_QT=ON
cmake --build build --target ladder_bank_server

BANK_DB="DRIVER=SQLite3;Database=$PWD/bank.db;Timeout=5000" BANK_PORT=0 \
    ./build/examples/bank/ladder_bank_server
# bank-server: listening on ws://127.0.0.1:54321
```

`BANK_PORT=0` lets the OS pick a free port, which the server prints. It writes
an audit trail to `bank_actions.jsonl` in its working directory, the same
`morph::journal::FileActionLog` the CLI installs — the models' read-only actions
carry `Loggable::No`, so what lands there is the mutating half of the surface.

#### Scenarios

`scripts/scenario/scenarios/bank/` holds 22 scenario files that drive this
binary as a real out-of-process WebSocket client, between them dispatching all
41 of bank's registered actions. `run_scenarios.py` starts the server, runs the
directory against it on a throwaway SQLite database, and tears it down:

```sh
python3 scripts/scenario/run_scenarios.py --rung bank --build-dir build
```

`--build-dir` names the directory `ladder_bank_server` was built into; add
`--twice` to rerun the directory against the database the first pass left
behind. The format, the flags and what the corpus is measured against are in
[`scripts/scenario/README.md`](../../scripts/scenario/README.md). Note that
`--rung bank` is the runner's spelling for "the `bank` directory" — bank is not
a rung, per the section above.

#### This server authenticates nobody — do not copy it as an authentication example

**It trusts the principal the client asserts.** Bank's `AuthModel` mints no
bearer token: `LoginRequest` verifies a password and returns the *principal* for
the client to install (which is what `App::login()` does with it). There is
therefore no signed artefact for a server to verify, and morph's default
`allowAllAuthorizer()` does not authenticate at all — it leaves the principal
unvouched-for, `RemoteServer` *clears* it, and every bank action then fails with
"no session principal". So `main.cpp` installs an authorizer that vouches for
whatever non-empty principal arrives, and anyone who can open a socket can claim
to be any customer.

What that does and does not leave standing: per-row ownership *is* enforced by
the models (`db::loadOwned` navigates a row to its owner and compares that with
the session principal), so cross-customer isolation is real and testable — it is
what `another-customer-cannot-touch-your-account.scenario` pins. What is absent
is credential *proof*. A server that needs it swaps in
`morph::session::SigningAuthorizer` and issues a token to verify against; bank
has none to issue.

#### Defects the scenario corpus pins on purpose

Three things the corpus asserts as current behaviour because they are true, not
because they are right. A scenario passing over any of them is a record, not an
endorsement; when one is fixed, the file that pins it is meant to fail.

- **[morph#471](https://github.com/LASTRADA-Software/morph/issues/471) — a
  caller-supplied owner beats the session principal.**
  `bank::resolveOwner()` ([`include/bank/core/principal.hpp:24`](include/bank/core/principal.hpp))
  returns `action.owner` whenever it is non-empty and only falls back to the
  session principal when it is not; nothing compares the two. Ten actions across
  eight models resolve their scope through it — `ListAccounts`, `ListCards`,
  `ListPayees`, `ListPayments`, `ListLoans`, `ListBudgets`,
  `ListNotifications`, `GenerateStatement`, `OpenAccount` and `MarkAllRead` —
  so a signed-in customer who types another customer's username is served that
  customer's data, and `MarkAllRead` *writes* to it. `SpendingByKind` consults
  no owner at all and answers a caller with no session. Actions addressed by
  row id are unaffected: they load the row and check its owner, which is the
  pattern the ten should follow.
  `an-owner-named-outright-is-not-checked-against-the-session.scenario` is the
  inventory: every cross-owner read and the `MarkAllRead` write are `expect
  ok` there, beside the id-addressed calls that are correctly refused.
- **A DTO's `validate()` shadows the model's own `ValidationError`.** Fifteen
  bank actions carry a `validate()` predicate on the wire DTO *and* open their
  `execute()` with `if (!action.validate()) throw ValidationError{"…"}`. Over
  the wire morph's dispatch path runs `validate()` first and refuses with
  `"action failed validation: <Model>/<Action>"`, so those fifteen hand-written
  messages are unreachable from a remote client and the scenarios assert the
  generic string instead.
- **`GenerateStatement`'s `closingBalanceMinor` is not a closing balance.** It
  reports each account's *current* balance
  (`src/models/statement_model.cpp:48`), not the balance as at `toMs`, so a
  statement over a past window still moves when the account does. The debit and
  credit totals beside it are windowed correctly.

### Qt 6 QML GUI

A **QML (Qt Quick)** desktop GUI (`gui/`) is built when `-DMORPH_BUILD_BANK_GUI=ON`
is also passed (requires Qt 6 Quick + Quick Controls 2):

```sh
cmake -G Ninja -B build -S . -DMORPH_BUILD_BANK_EXAMPLE=ON -DMORPH_BUILD_BANK_GUI=ON
cmake --build build --target bank_gui
./build/examples/bank/gui/bank_gui
```

Structure:

- **`gui/bank_gui_lib`** — a static library holding `BankClient` and every
  controller, linking `Qt6::Core` only (no Quick, no Qml). `bank_gui` is
  `main.cpp` plus the QML module on top of it; `bank_gui_tests` and
  `bank_gui_qml_tests` (see [Tests](#tests)) are the other consumers.
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
- **`gui/qml/`** — the front-end: `Main` (login ⇄ shell + a toast that carries
  both the controllers' `error` signals and the `posted`/`paid` confirmations),
  `AppShell` (sidebar + stacked pages), the five pages, and reusable components
  (`Panel`, `AppButton`, `Field`, `Pill`, `Picker`). Bundled via
  `qt_add_qml_module` (URI `BankGui`). The warm, "Claude-inspired" palette comes
  from `gui/Theme.hpp` and is installed as the `theme` context property — in
  `bank_gui_lib` rather than `main.cpp` so the QML tests instantiate the shipped
  `.qml` with the shipped palette.

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

`tests/gui/` is a second binary, `bank_gui_tests`, built only when
`-DMORPH_BUILD_BANK_GUI=ON` is also set — `bank_tests` links no Qt at all and
is built in configures that have none. It holds `test_bank_qml_surface.cpp`,
which points the ladder testkit's `QmlSurfaceAudit`
(`examples/common/testkit/qml_surface.hpp`, `examples/TESTING.md`) at the six
controllers and the thirteen `.qml` files, and fails on any name that exists on
one side only: a renamed `Q_INVOKABLE`, a `Connections` handler for a signal
that is gone, a property read that would resolve to `undefined`. None of those
is a compile error or a QML warning; the pane just stays empty.

```sh
cmake --build build --target bank_gui_tests
./build/examples/bank/bank_gui_tests
```

A third binary, `bank_gui_qml_tests`, holds the cases that need a live QML
engine — behaviour that lives in the `.qml` and is invisible from C++. It loads
the shipped files out of the source tree by URL (the `BankGui` module is inside
the `bank_gui` executable and cannot be linked; QML's implicit directory import
resolves `Panel`/`Picker`/… with no `qmldir`), wires the real controllers as
context properties, and reads values back off the items the engine created.
`MoveMoneyPage`'s account picker is the worked example: `TransactionController`
is self-consistent under any C++ drive, and the bug was a `ComboBox` whose
`currentIndex` nothing restored after `refresh()` replaced its model. It is
kept out of `bank_gui_tests` so that binary stays `Qt6::Core`-only and needs no
platform plugin.

```sh
cmake --build build --target bank_gui_qml_tests
QT_QPA_PLATFORM=offscreen ./build/examples/bank/bank_gui_qml_tests
```

## Status

Models, tests, CLI, the Qt 6 GUI, and a self-contained WebAssembly build (hosted
on GitHub Pages) are complete. Possible extensions: durable in-browser
persistence (IDBFS/OPFS) for the WASM build, switching its in-browser backend to
`RemoteServer` + `SimulatedRemoteBackend` to surface the JSON wire protocol, and
wiring the desktop GUI over `morph::qt::QtWebSocketBackend` for a true networked
client/server split.
