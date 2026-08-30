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
# Start a server; it prints the port it bound (PASTEBIN_PORT=0 → the OS picks).
PASTEBIN_DB="DRIVER=SQLite3;Database=$PWD/pastebin.db;Timeout=5000" \
PASTEBIN_PORT=0 QT_QPA_PLATFORM=offscreen \
    ./build/examples/pastebin/ladder_pastebin_server
# pastebin-server: listening on port 60101

python3 scripts/scenario/morph_scenario.py \
    --server ws://127.0.0.1:60101 \
    scripts/scenario/scenarios/pastebin.scenario
```

`scenarios/` holds the ones that ship:

| File | Server | What it covers |
|---|---|---|
| `pastebin.scenario` | `ladder_pastebin_server` | create/read/list, two clients, a private paste, both refusal messages, hand-built hostile envelopes |
| `bookmarks_login.scenario` | `ladder_bookmarks_server` | a failed sign-in, a retry, a forged token, another principal's token |
| `pastebin_broken.scenario` | `ladder_pastebin_server` | *meant to fail* — see below |

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
model  PasteModel              # default typeId every client registers

# Open a named connection: connect, "hello", "register".
client alice
client bob url=ws://127.0.0.1:9000 model=OtherModel principal=bob token=abc123
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
| `client <name> [opt=value ...]` | Opens a connection, sends `hello`, sends `register`. Options: `url`, `model`, `principal`, `token`, `contextKey`, `protocol` (a version number, or `none` to skip the handshake entirely). The new client becomes current. |
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
`scenarios/bookmarks_login.scenario`, which also shows a forged token and
another principal's token being refused. `examples/TESTING.md` notes that a
failed-then-retried sign-in appears in no in-process rig, because rigs arrive
already authenticated.

## Proving a scenario's assertions are real

A scenario that passes proves nothing until you know it *can* fail.
`mutate_scenario.py` flips one assertion at a time — `ok`↔`err`, `==`↔`!=`,
`~`↔`!~` — reruns the scenario, and reports any mutant that still passed:

```bash
python3 scripts/scenario/mutate_scenario.py \
    --server ws://127.0.0.1:60101 \
    scripts/scenario/scenarios/pastebin.scenario
```

A surviving mutant is an assertion that is not load-bearing. It exits non-zero
if any survives.

`scenarios/pastebin_broken.scenario` is a scenario that is *meant* to fail: it
asserts a paste reads back with content it was never created with. Run it to see
what a failure report looks like.

## Measuring what the corpus covers

A corpus grows by whoever last added a file, and "have we covered everything?"
has no answer unless the universe is countable. `scenario_coverage.py` makes it
countable: it enumerates every envelope kind `RemoteServer::dispatchMessage`
handles and every refusal string it can put on the wire, straight out of
`include/morph/core/remote.hpp`, then diffs that against what the scenario
files actually send and assert.

```bash
python3 scripts/scenario/scenario_coverage.py
```

Exit `0` if every kind and refusal is covered or exempt, `1` if something is
uncovered, `2` if the tool itself is broken — a header it cannot read, an
extraction too small to be believable, or a stale allowlist.

Anything deliberately left uncovered goes in `coverage_allowlist.json` with a
**written reason**, and is checked in both directions: an entry for something
now covered, or for something `remote.hpp` no longer has, fails the run. The
list can only shrink deliberately.

The report is itself a control, so it is built not to pass while measuring
nothing: it refuses to run if it extracts an implausibly small surface (rename
`makeErr` and it fails loudly rather than reporting full coverage over an empty
universe), and `test_morph_scenario.py` drives it against fixtures whose right
answers are known, including one where the correct exit code is non-zero.

## Self-test

`test_morph_scenario.py` covers the parser, the value syntax, the path reader
and the comparison rules. It needs no server:

```bash
python3 scripts/scenario/test_morph_scenario.py
```

## What this deliberately does not do

- **No `sleep`, and no wall-clock waits.** Every assertion is on a reply to a
  request this file sent. A scenario cannot become a source of flaky timing the
  way [morph#147](https://github.com/LASTRADA-Software/morph/issues/147) once
  did, because there is nothing to wait *on*.
- **No server lifecycle.** It drives a server someone else started. Starting,
  waiting for the port and tearing down belong to whatever runs it.
- **No schema validation of inputs.** [morph#171](https://github.com/LASTRADA-Software/morph/issues/171)
  proposed checking a scenario's inputs against the server's served JSON Schema
  before sending. morph does not serve schemas over the wire: `schemaJson<A>()`
  and `Bridge::schemasJson()` are compile-time, in-process APIs, and no envelope
  `kind` exposes them to a remote client. See
  [morph#234](https://github.com/LASTRADA-Software/morph/issues/234). Until one
  does, this runner sends what the file says, which is also what makes
  deliberately malformed payloads expressible.
- **It does not replace the C++ tests.** Model behaviour is tested in-process
  and stays there. This covers the seam those tests assume away.

## Spec drift found while writing this

`docs/spec/core/wire.md`'s envelope tables omit the `primary` and `shared`
fields and the `attach`, `assign` and `instances` kinds that
`include/morph/core/wire.hpp` and `RemoteServer::dispatchMessage` actually carry
— see [morph#233](https://github.com/LASTRADA-Software/morph/issues/233). A
client written from the spec alone gets an incomplete envelope; this one sends
the full field set taken from the header.
