# Bank — a worked example of `morph` + Lightweight

A feature-rich demo banking application built on two libraries:

- **[morph](../../README.md)** — the typed, asynchronous UI ↔ model bridge. Every
  banking domain is a plain single-threaded C++ model; morph handles concurrency
  (one strand per model), result marshalling, and transport. The *same* model code
  and call sites run **in-process (local)** or **across a server (remote)**.
- **[Lightweight](https://github.com/LASTRADA-Software/Lightweight)** — the ORM /
  ODBC layer. Each model persists to **SQLite** through a typed `DataMapper`; the
  schema is owned by Lightweight migrations.

It ships the models, a full test suite, a scripted CLI driver, a server, and
**server-defined screens** the generic client draws — which also builds to
**WebAssembly** and runs entirely in the browser.

> **▶ Try the live demo:** **https://lastrada-software.github.io/morph/demo/**
> Sign in as the seeded demo user (`demo` / `demo1234`), who has two accounts.
> Everything runs client-side — no server. First load fetches a ~46 MB `.wasm`, so
> give it a few seconds. (See [WebAssembly demo](#webassembly-demo-self-contained-github-pages).)

## Bank and the application ladder

Bank predates [the application ladder](../LADDER.md) and is **unnumbered prior
art**: `LADDER.md` cites it in its intro, measures rung effort in
"bank-equivalents", and gives it no rung number. That is deliberate — the
numbers in `LADDER.md`'s table are load-bearing for the rungs that consume each
other's answers, and bank has nothing to take but a renumbering. Concretely,
bank is absent from [`examples/rungs.txt`](../rungs.txt) and never calls
`morph_add_rung()`, which is what keeps it out of `ci.yml`'s `ladder-tests`
and `ladder-sanitizers`, `coverage.sh` and `codecov.yml`'s per-rung
components; `ci.yml`'s `bank-sanitizers` and all-features jobs build it with
`-DMORPH_BUILD_BANK_EXAMPLE=ON`, and `wasm-demo.yml` builds the browser demo.
`ladder_bank_server` below is a local `add_executable` for the same reason; the `ladder_` prefix is the scenario tooling's naming convention, not a
rung claim.

Conventions bank **shares** with the rungs: persistence exclusively through the
Lightweight ORM, with the schema owned by `LIGHTWEIGHT_SQL_MIGRATION`
definitions — [`LADDER.md`](../LADDER.md) calls that "bank's pattern" and binds
every rung to it — models as the application, plain aggregates on the wire, and
[server-defined screens](#server-defined-screens). The convention it does
**not** follow is the [`TESTING.md`](../TESTING.md) dual-deployment-mode rig — its remote coverage is
`SimulatedRemoteBackend` in `test_remote.cpp` plus the scenario corpus below.
Bringing that into line is open work; the numbering question is
the part that is settled.

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

What that does and does not leave standing: ownership *is* enforced by the
models, in both of the two spellings an action can be addressed by. A row id
goes through `db::loadOwned`, which navigates the row to its owner and compares
that with the session principal; an owner name goes through
`bank::resolveOwner`, which compares the caller-supplied `owner` with the
session principal and refuses on a mismatch. Cross-customer isolation is
therefore real and testable —
`another-customer-cannot-touch-your-account.scenario` pins the first spelling
and `an-owner-named-outright-is-checked-against-the-session.scenario` the
second. What is absent is credential *proof*. A server that needs it swaps in
`morph::session::SigningAuthorizer` and issues a token to verify against; bank
has none to issue.

#### Defects the scenario corpus pins on purpose

Two things the corpus asserts as current behaviour because they are true, not
because they are right. A scenario passing over either of them is a record, not
an endorsement; when one is fixed, the file that pins it is meant to fail.

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

### WebAssembly demo (self-contained, GitHub Pages)

**Open it in a browser — nothing to install:** **https://lastrada-software.github.io/morph/demo/**

Sign in as the seeded demo user (`demo` / `demo1234`), who has two accounts; open
accounts, deposit/withdraw/transfer, issue cards, add payees and take a loan. It's a
static page — the first load fetches a ~46 MB `.wasm` (streaming-compiled by the
browser), then it's instant. Published from `master` by
[`.github/workflows/wasm-demo.yml`](../../.github/workflows/wasm-demo.yml).

The page is the generic client ([`examples/client`](../client)) built for
**single-threaded WebAssembly**, running bank's [server-defined screens](#server-defined-screens)
in process on the Qt Quick renderer — the same screens the native client and bank's server
serve. Since Lightweight (ODBC/SQLite) can't run in a browser, bank's models link against
an **in-memory store** (`bank_memory_lib`: `memory/src/models/*_wasm.cpp` behind the
shadow model headers in `memory/include/bank/`, which come ahead of the native ones
on the include path), seeded by `bank::wasm::seedDemo()`. The models run on the Qt event
loop. Single-threaded ⇒ no SharedArrayBuffer ⇒ **no COOP/COEP headers**, so plain GitHub
Pages hosts it. The client links with Asyncify, because the Qt Quick frontend waits for
events in its own event loop.

Build locally (needs Qt 6.10 for WASM and its emsdk, 4.0.7):

```
source /path/to/qt6-wasm/emsdk/emsdk_env.sh
export EM_CACHE="$PWD/.emcache"
/path/to/qt6-wasm/bin/qt-cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DMORPH_BUILD_EXAMPLES=ON -DMORPH_BUILD_BANK_EXAMPLE=ON \
  -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_TESTS=OFF
cmake --build build-wasm --target morph_examples_client
python3 -m http.server -d build-wasm/examples/client 8000   # open morph_examples_client.html
```

CI (`.github/workflows/wasm-demo.yml`) builds the bundle with a matched host+wasm Qt pair
and publishes it to `…github.io/<repo>/demo/`, coexisting with the Doxygen docs at the
site root.

## Server-defined screens

[`screens/bank_screens.cpp`](screens/bank_screens.cpp) declares bank's screens as UI documents (spec 4) with the
C++ builders:
- **`bank.login`** signs in, or registers and signs in. A successful reply's `principal` is installed with
  `signIn`, and the accounts open.
- **`bank.accounts`**: the customer's accounts, as the `AccountCards` component and as a table with Close; an
  open-account form; and the history of the account chosen.
- **`bank.move`**: deposit, withdraw and transfer forms over the account chosen in a picker. The choice survives the
  refetch each of them causes, so the next deposit lands in the account the picker names. `TransferFlow` shows the
  transfer's two accounts.
- **`bank.cards`**, **`bank.payees`** and **`bank.loans`**: lists with their actions and forms. A loan's schedule is
  the model's `LoanScheduleRequest`, shown through the `LoanSchedule` component.

Each signed-in screen has the sidebar, which also signs out. `AccountCards`, `TransferFlow` and `LoanSchedule` are
host components; their built-in fallbacks are what a client without them shows. Amounts are entered in cents: bank's
actions take minor units.

`morph_examples_client --app bank` runs them in process over a SQLite file (`--db`, default `bank.db`). The screen
tests (`screens/tests/`) and the render smoke run on the screen harness and on Qt Quick offscreen.

## Tests

Each model has a `tests/test_*.cpp` (Catch2). `tests/bank_test_support.hpp` provides
`await(completion, gui)` — pumps the GUI executor until a `Completion` resolves — and a
shared on-disk test database. Notable cross-cutting tests:

- `test_remote.cpp` — runs `AccountModel` over `SimulatedRemoteBackend` and shows a
  custom `IAuthorizer` rejecting an action.
- `test_offline.cpp` — parks deposits in an `InMemoryOfflineQueue` while "offline" and
  replays them via `SyncWorker` on "reconnect".

## Status

Models, tests, CLI, the server, its screens, and a self-contained WebAssembly
build (hosted on GitHub Pages) are complete. Possible extensions: durable
in-browser persistence (IDBFS/OPFS) for the WASM build, and switching its
in-browser backend to `RemoteServer` + `SimulatedRemoteBackend` to surface the
JSON wire protocol.
