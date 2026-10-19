# Scenario runner

Drive a **running** morph server from a plain-text file describing what a user
does, as a real out-of-process client.

`morph_scenario.py` opens a TCP connection to a `ladder_<rung>_server`, performs
the RFC 6455 WebSocket handshake, and speaks `morph::wire`'s JSON envelope
protocol ([`docs/spec/core/wire.md`](../../docs/spec/core/wire.md)) directly. It
does not link against morph, does not share a process with it, and implements
the protocol from the spec rather than from morph's own C++ — so a protocol bug
that is symmetric on both sides of morph's own client/server pair is visible
here and invisible to morph's own tests.

Standard library only: no build step, no dependencies, no `pip install`.
Verified on CPython 3.9.6, 3.12 and 3.14.

## Running one

```bash
# Start a server; it prints the port it bound (BOOKMARKS_PORT=0 → the OS picks).
BOOKMARKS_TOKEN_SECRET="pick-something-real" \
BOOKMARKS_DB="DRIVER=SQLite3;Database=$PWD/bookmarks.db;Timeout=5000" \
BOOKMARKS_PORT=0 QT_QPA_PLATFORM=offscreen \
    ./build/examples/bookmarks/ladder_bookmarks_server
# bookmarks-server: listening on ws://127.0.0.1:60101

python3 scripts/scenario/morph_scenario.py \
    --server ws://127.0.0.1:60101 \
    scripts/scenario/scenarios/bookmarks/archive-unarchive-round-trip.scenario
```

## Running a whole rung

`run_scenarios.py` starts the server, runs every file in that rung's
directory against it, and tears it down — the lifecycle `morph_scenario.py`
deliberately does not own. It needs the rung servers built:

```bash
cmake --preset clang-release -B build/ladder-srv -DMORPH_BUILD_LADDER=ON \
    -DMORPH_LADDER_RUNGS=all -DMORPH_BUILD_NET=ON -DMORPH_BUILD_QT=ON \
    -DMORPH_BUILD_TESTS=ON
cmake --build build/ladder-srv --target ladder_bookmarks_server ladder_bank_server

python3 scripts/scenario/run_scenarios.py                     # every rung
python3 scripts/scenario/run_scenarios.py --rung bookmarks
python3 scripts/scenario/run_scenarios.py --rung bank --twice --mutate
```

With the ladder on, `-DMORPH_LADDER_RUNGS=all` builds bank too: it is rung 2,
with its own `CMakeLists.txt` rather than `morph_add_rung()`.

Note that `--build-dir` applies to *every* rung in the run. With no
`--build-dir` the runner globs `build/*/examples/<name>/`, so servers built in
different build directories still work as long as each binary exists under
exactly one of them.

| Flag | Meaning |
|---|---|
| `--rung NAME` | Restrict to one rung; repeatable. Default: every rung with a directory. |
| `--build-dir DIR` | Where the `ladder_<rung>_server` binaries live. Searched under `build/*/` if omitted, and ambiguity is an error rather than an arbitrary pick. |
| `--mutate` | After a file passes, run `mutate_scenario.py` on it; fail on any survivor. |
| `--twice` | Run the directory a second time against the same database. |
| `-v` | Pass `--verbose` through to the runner. |

Each rung gets **one** server for its whole directory, on a fresh SQLite
database in a temp directory, with its port bound to `0` so runs cannot
collide. That is why every scenario must be **re-runnable against a database
it has already run on**: no assertion may depend on the database being empty,
and listing assertions pin to captured ids rather than to counts. `--twice`
is what proves it — it reruns the directory against the database the first
pass left behind and demands the same result.

`broken-on-purpose.scenario` has its verdict inverted: it is meant to fail, so
a run in which it passes is a failure.

No rung's database is seeded: every rung creates its own root entity over the
wire. (`RungSpec.seed_sql` exists for a rung that needs fixture rows before its
first step; none does.)

`scenarios/` holds one directory per rung with a server, and one file per
workflow. lims has no scenarios yet.

| Directory | Server | What it covers |
|---|---|---|
| `bookmarks/` | `ladder_bookmarks_server` | sign-in and forged tokens, CRUD, archive, atomic bulk edit, tag rename/merge, filters, the changes-since poll, import/export, two users and a shared feed; the live-model cap; an envelope the server cannot decode |
| `bank/` | `ladder_bank_server` | registration and password-only sign-in, accounts and the ledger behind them, overdraft boundaries, transfers, bills and scheduled/standing instructions, cards, loans and amortisation, budgets, notifications, statements; two customers, the anonymous session, and the stateful account cache |

Some of the bank files assert behaviour that is **wrong**, deliberately, and
say so in their own header comments: `GenerateStatement` reporting a live
balance in a field named `closingBalanceMinor` is one, and a DTO's `validate()`
shadowing the model's own error message is another. An `expect ok` over a defect
records what the server does today so the day it changes is not a silent one; it
is not an endorsement, and the fix is meant to turn those assertions red. That
is the opposite arrangement from `broken-on-purpose.scenario`, which fails today
by design.

`bank/an-owner-named-outright-is-checked-against-the-session.scenario` is what
that arrangement looks like when the ownership gate holds. Were
`bank::resolveOwner()` to prefer a caller-supplied owner name over the session
principal, ten actions would serve a signed-in customer another customer's
data, and every assertion in that file would have to read `expect ok`. It reads
`expect err` instead, and pins the
enforcement in the same shape it once pinned the defect.

The rung a scenario belongs to is its parent directory name — that is how
per-rung action coverage is attributed, so a file loose in `scenarios/` is
counted against no rung and is refused by the self-test.

`bookmarks/broken-on-purpose.scenario` is *meant to fail* — see below.

Exit code `0` if every assertion held, `1` if one did not (the run stops at the
first failure and prints the state at that point), `2` if the file is malformed
or unreadable — a malformed file is rejected before anything connects.

| Flag | Meaning |
|---|---|
| `--server ws://host:port` | Overrides the file's `server` line. |
| `--timeout SECONDS` | Per-read timeout (default 10). |
| `-v` / `--verbose` | Print every reply and every assertion that held. |

## The format

One directive per line. `#` starts a comment; blank lines are ignored. Steps run
top to bottom, on the connection of whichever client is current.

```
# Settings — both optional, both must precede the first step.
server ws://127.0.0.1:8765     # default url for every client
model  BookmarkModel           # default typeId every client registers

# Open a named connection: connect, "hello", "register".
client alice
client bob url=ws://127.0.0.1:9000 model=OtherModel principal=bob token=abc123
client carol principal=$who token=$token   # captures expand here, as in `session`
use alice                      # switch the current client

# Dispatch an action: an "execute" envelope with these named inputs as its body.
do CreatePaste content="hello world" syntax=plaintext
expect ok capture id=$.id

do GetPaste id=$id
expect ok field content == "hello world"
expect ok field readCount.num == 1

do GetPaste id=no-such-paste
expect err message == "GetPaste: no such paste"

# Hand-built envelope — anything a typed C++ client cannot express.
send hello protocolVersion=99
expect err message == "protocol version unsupported"

deregister                     # destroy the current client's model instance
expect ok
close bob                      # drop a connection (no expect)
```

### Directives

| Directive | Effect |
|---|---|
| `server <ws-url>` | Default url for clients declared later. `--server` overrides it. |
| `model <TypeId>` | Default `typeId` clients register. |
| `client <name> [opt=value ...]` | Opens a connection, sends `hello`, sends `register`. Options: `url`, `model`, `principal`, `token`, `contextKey`, `protocol` (a version number, or `none` to skip the handshake entirely). `principal`, `token` and `contextKey` expand `$capture` references, exactly as `session` does; `url`, `model` and `protocol` do not, and a capture written in one of them is refused by name rather than sent literally. The new client becomes current. |
| `use <name>` | Makes an existing client current. |
| `session [principal=<v>] [token=<v>]` | Replaces the current client's session — the step that turns a `Login` result into the credentials every later `execute` carries. |
| `do <ActionType> [field=value ...]` | Sends `execute` on the current client, with the named fields as the action body. |
| `send <kind> [field=value ...]` | Sends a hand-built envelope. Fields are envelope fields (`modelId`, `typeId`, `body`, `protocolVersion`, …), not action inputs. `callId` is assigned by the runner. |
| `deregister` | Deregisters the current client's model instance. |
| `close [name]` | Closes a connection. Defaults to the current client. |
| `expect ok\|err [clause ...]` | Asserts against the reply to the step above it. |

**Every `do`, `send` and `deregister` must be followed by at least one `expect`
line.** A scenario that omits one is rejected as malformed rather than run — a
scenario whose steps assert nothing would pass no matter what the server did,
which is precisely the failure mode this runner exists to avoid. Several
`expect` lines may follow one step; each asserts against the same reply.

### `expect` clauses

| Clause | Meaning |
|---|---|
| `capture <name>=<path>` | Binds `$name` to the value at `<path>` for later steps. Fails if the path is absent. |
| `field <path> <op> <value>` | Compares the value at `<path>` in the reply body. |
| `message <op> <value>` | Shorthand for `field @message <op> <value>` — the `err` envelope's message. |

Operators: `==` and `!=` compare JSON values; `~` and `!~` test a Python regular
expression against the value's text (its JSON form, for non-strings).

An **absent path fails every operator, `!=` and `!~` included**: "the field is
not there" must never read as "the field differs from X", or a renamed or
dropped result field would quietly satisfy the assertion watching it.

### Paths

A path reads the reply's **body**, parsed as JSON: `content`, `readCount.num`,
`pastes[0].id`, optionally written `$.content`. A path starting with `@` reads
an **envelope** field instead: `@modelId`, `@message`, `@kind`, `@callId`.

### Values

| Written | Sent as |
|---|---|
| `content="hello world"` | JSON string (`\"`, `\\`, `\n` … honoured) |
| `syntax=plaintext` | JSON string — a bare word is a string |
| `limit=12`, `rate=1.5` | JSON number |
| `flag=true`, `x=null` | JSON literal |
| `page={"cursor":"abc"}` | raw JSON, object or array |
| `id=$id` | the captured value, with its own type |
| `note="paste $id"` | `$id` expands inside strings, stringified |

## Multiple clients

Each `client` is its own socket, its own session, and its own registered model
instance. That is what makes a scenario worth more than a `curl` script: two
clients reading each other's writes, or a private paste that must not appear in
the other's listing, are exactly the cases an in-process test assumes away.

## Sessions

`execute` carries a `session::Context`; the server's authorizer may verify its
`token` and overwrite `principal` before dispatch. `session` sets both from
values an earlier step captured, so a sign-in that fails, is retried, and only
then authorizes work is one file — see
`scenarios/bookmarks/login-retry-and-forged-tokens.scenario`, which also shows a forged token and
another principal's token being refused. `examples/TESTING.md` notes that a
failed-then-retried sign-in appears in no in-process rig, because rigs arrive
already authenticated.

The credentials may equally be written on the `client` line that opens the
connection — `client store model=BookmarkModel principal=$who token=$token` — and
mean the same thing as the two-line `client` then `session` form. That equality
is load-bearing: were `client` to read its options raw, `$token` would go to
the server as six literal characters and the run would fail several steps later
with a bare `unauthorized` naming neither the step nor the cause.

## Load benchmark

`bench_bookmarks.py` measures how many concurrent users one bookmarks server
carries. Each simulated user repeats a session: connect, `Login`, open the
collection (`ListBookmarks`), a random 5-10 actions from a weighted mix with
100-1000 ms of think time before each, then disconnect. It reports
sessions/s, actions/s, latency per step, failures, the server's CPU (in total
and per thread) and RSS read from `/proc`, and the clients' own CPU. Linux
only.

```bash
python3 scripts/scenario/bench_bookmarks.py --users 1000
python3 scripts/scenario/bench_bookmarks.py --store memory --transport net --sweep 2500,10000,20000 --json out.json
python3 scripts/scenario/bench_bookmarks.py --workload framework --users 10000   # no model work at all
python3 scripts/scenario/bench_bookmarks.py --users 64 --think-ms 0              # closed loop: the throughput ceiling
```

A sweep runs one fresh server per user count and ends with the largest count
whose action p99 and failure rate meet `--slo-p99-ms` and
`--slo-failure-rate`.

What the server is made of, each passed to it through its environment:

| Flag | Server variable | Choices |
|---|---|---|
| `--store` | `BOOKMARKS_STORE` | `sql` (default): the database. `memory`: each model instance keeps its own bookmarks and nothing is persisted or journalled, so a run measures the framework rather than the database. |
| `--transport` | `BOOKMARKS_TRANSPORT` | `qt` (default): `QtWebSocketServer` on the main thread. `net`: `morph::net::SocketServer` on its own I/O loop. |
| `--workers` | `BOOKMARKS_WORKERS` | The model worker pool size (default 4). |
| `--max-live-models` | `BOOKMARKS_MAX_LIVE_MODELS` | The cap on live model instances; the benchmark's default `0` is unbounded. |

With `--store memory` every session starts from an empty collection, so it
first creates `--seed-bookmarks` (default 5-10) bookmarks before opening it.
`--workload framework` replaces the session's actions with `Login` round
trips, which never reach a model's storage. `--sqlite-wal` creates the SQL
database in WAL mode; without it SQLite's rollback journal makes every writer
block every reader. Users start over `--ramp` seconds (default users/500) so a
large count does not connect in one burst.

`--tracy DIR` records the server with `tracy-capture` from start-up to exit,
writes `DIR/bookmarks-<users>.tracy` and its zone statistics
`DIR/bookmarks-<users>-zones.csv`, and prints the hottest zones. It needs a
server built with `-DMORPH_ENABLE_TRACY=ON -DMORPH_TRACY_ON_DEMAND=OFF` (pass
that build's `--build-dir`), plus `tracy-capture` and `tracy-csvexport` from
the same Tracy tag, on `PATH` or given with `--tracy-tools`. Lightweight's own
SQL zones are recorded too. Instrumentation costs time, so take throughput
from an uninstrumented build and the profile from this one.

The users are asyncio tasks, one event loop in each of `--procs` processes,
so a reply wakes the user waiting for it at once even with thousands per
process. A run warns when clients and server together saturated the host: its
figures are then a floor, not the server's ceiling.

## Proving a scenario's assertions are real

A scenario that passes proves nothing until you know it *can* fail.
`mutate_scenario.py` flips one assertion at a time — `ok`↔`err`, `==`↔`!=`,
`~`↔`!~` — reruns the scenario, and reports any mutant that still passed:

```bash
python3 scripts/scenario/mutate_scenario.py \
    --server ws://127.0.0.1:60101 \
    scripts/scenario/scenarios/bookmarks/archive-unarchive-round-trip.scenario
```

A surviving mutant is an assertion that is not load-bearing. It exits non-zero
if any survives.

`scenarios/bookmarks/broken-on-purpose.scenario` is a scenario that is *meant* to fail: it
asserts a bookmark reads back with a title it was never created with. Run it to see
what a failure report looks like.

## In CI

`.github/workflows/ci.yml`'s `ladder-tests` job which runs the whole corpus against real servers. It builds them anyway
for its own ladder tests, so the build the corpus needs is already paid for:
`-DMORPH_LADDER_RUNGS=all` gives it every rung's `ladder_<rung>_server`. The final
step then passes `--rung` once per directory under `scenarios/`, so the set
that runs is the set that exists, and `--build-dir` so no stale binary from
another build tree can be picked up. A missing binary is not softened into a
skip: `run_scenarios.py` exits `2` and the job fails.

Ahead of that build, on a bare checkout, the same job refuses any corpus it
could not have run: every `scenarios/<name>/` directory must have a `RungSpec`
in `run_scenarios.py`, and must be either a rung listed in `examples/rungs.txt`
with an `examples/<name>/src/server/` for `morph_add_rung()` to build, or a
name whose `examples/<name>/CMakeLists.txt` declares
`add_executable(ladder_<name>_server)` itself. A corpus that is neither fails
the job in seconds, naming itself, rather than after a full Qt build — or,
worse, by being quietly left out of a run that then reports green.

`scripts/scenario/` is one of the paths in the job's change filter
(`scripts/ladder_rungs.sh ci-path-regex`), so a pull request touching only the
corpus still reaches the job that runs it.

## What this deliberately does not do

- **No `sleep`, and no wall-clock waits.** Every assertion is on a reply to a
  request this file sent. A scenario cannot become a source of flaky timing the
  way a polling loop does, because there is nothing to wait *on*.
- **No server lifecycle.** It drives a server someone else started. Starting,
  waiting for the port and tearing down belong to whatever runs it.
- **No schema validation of inputs.** The runner does not check a scenario's
  inputs against the server's served JSON Schema before sending, and the reason
  is *not* that it could not: the `schemas` kind serves exactly that document,
  `required` array, declared bounds and all, out of a live server. The narrower statement is the true one: this runner sends what the
  file says without consulting it, which is also what makes deliberately
  malformed payloads expressible. Validating against the served schema is
  therefore *possible* and merely not done.
- **It does not replace the C++ tests.** Model behaviour is tested in-process
  and stays there. This covers the seam those tests assume away.

## Spec drift found while writing this

`docs/spec/core/wire.md`'s envelope tables omit the `primary` and `shared`
fields and the `attach`, `assign` and `instances` kinds that
`include/morph/core/wire.hpp` and `RemoteServer::dispatchMessage` actually
carry. A client written from the spec alone gets an incomplete envelope; this
one sends the full field set taken from the header.
