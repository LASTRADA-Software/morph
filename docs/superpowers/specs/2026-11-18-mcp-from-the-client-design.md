# An MCP server from the generic client

An agent such as Claude Code can already act on a morph application through `morph::ui::McpAdapter`
(`docs/spec/ui/agent.md`), but only one way: embedded in an application's own server process (bank's
`--mcp-stdio`), which the agent then has to launch. This design adds the two ways that make an installable MCP
server:

| Mode | Command | Reaches |
|---|---|---|
| **Remote** | `morph_remote_client --mcp-stdio --server ws://host:port` | A running server, over WebSocket |
| **Local** | `morph_examples_client --mcp-stdio --app lims --db lims.db --user alice` | The application's database, in-process |

Both modes are a flag on the generic client (`docs/spec/client/client.md`) because the client already has exactly
these two sources: `--server` and `--app`. With `--mcp-stdio`, the client mounts no frontend. It serves MCP on
stdin and stdout until stdin ends.

```sh
claude mcp add lims -- morph_examples_client --mcp-stdio --app lims --db "$PWD/lims.db" --user alice
claude mcp add lims-remote -e MORPH_MCP_TOKEN=... -- morph_remote_client --mcp-stdio --server wss://lab:9000
```

`morph_remote_client` links no application, so one binary serves every morph server. Its tools come from the
server's `ui-catalog` at run time.

## Contents

- [Units](#units)
- [The adapter over a transport](#the-adapter-over-a-transport)
- [Remote mode](#remote-mode)
- [Local mode](#local-mode)
- [Identity](#identity)
- [Stdout](#stdout)
- [Errors](#errors)
- [Testing](#testing)
- [Decisions](#decisions)
- [Out of scope](#out-of-scope)

## Units

| Unit | Where | Change |
|---|---|---|
| `McpAdapter` | `include/morph/ui/agent.hpp` | A constructor over a borrowed `backend::IEnvelopeTransport`; `McpOptions::callTimeout`; `resetter()`; a generation guard on its caches |
| `SocketBackendConfig`, `QtWebSocketBackendConfig` | `include/morph/net/socket_backend.hpp`, `include/morph/qt/qt_websocket_backend.hpp` | `retryFirstConnect`, default off |
| `client::makeLocalServer` | `include/morph/client/source.hpp`, `src/client/source.cpp` | Builds the in-process `RemoteServer` for a `LocalApp`. Used by local MCP and by the test kit's `SimulatedAppSource`, which hand-builds the same server today |
| `client::Args` | `include/morph/client/args.hpp`, `src/client/args.cpp` | `--mcp-stdio`, a flag; `user`'s doc covers both sources |
| `client::serveMcp` | new `include/morph/client/mcp.hpp`, `src/client/mcp.cpp` | Builds the adapter for either source and serves stdio |
| Transport selection for MCP | `src/client/transport_net.cpp`, `src/client/transport_qt.cpp` | Overloads that need no frontend context |
| The client's `main` | `examples/client/main.cpp` | Calls `serveMcp` before selecting a frontend when `--mcp-stdio` is given |
| Specs and docs | `docs/spec/ui/agent.md`, `docs/spec/client/client.md`, `docs/spec/core/backend.md`, `examples/bank/README.md` | The new constructor, the flag, `retryFirstConnect`, both install lines |

Bank's `--mcp-stdio` stays as it is: it is the embedded mode, sharing the server with the GUIs that process
serves.

## The adapter over a transport

The adapter touches its server in three places: `openConnection` in the constructor, `closeConnection` in the
destructor, and `RemoteServer::handle` in `Shared::send`. Every request it makes (the catalog, `register`,
`execute`, `evaluate`) goes through `send`.

`Shared` gets a link in place of the server pointer: one function that sends an envelope and calls back with the
reply, plus an optional close.

- **`McpAdapter(std::shared_ptr<backend::RemoteServer>, McpOptions)`** is unchanged in behaviour. Its link opens a
  connection, sends through `RemoteServer::handle` on that connection, and closes it in the destructor.
- **`McpAdapter(backend::IEnvelopeTransport& transport, McpOptions)`** is new. Its link is
  `transport.request(envelope, *options.callbacks)`. A reply resolves the callback whether it is `ok` or `err`. A
  rejection (`DisconnectedError`, or any other failure to deliver) becomes `wire::makeErr(what)`, so every caller
  of `send` sees one shape. The transport is borrowed and must outlive the adapter. The adapter opens and closes
  nothing: the transport's connection is the agent's connection, and the server releases the adapter's private
  instances when that connection drops.
- **`McpOptions::callTimeout`** bounds each envelope sent over a transport, 30 seconds by default. A call that
  gets no reply in time answers `wire::makeErr("no reply from the server in <n> s")`, and a reply that arrives
  later is dropped. `serveStdio` handles one message at a time, so without this bound a request the server never
  answers would block every later message, `ping` included. The in-process constructor ignores it:
  `RemoteServer` always replies.
- **A generation guard.** `Shared` keeps a generation counter. A reset bumps it under the mutex, clearing the
  cached catalog and every cached model id. `withCatalog` and `withInstance` record the generation when they send,
  and they cache the reply only if the generation is unchanged when it arrives. Without the guard, a `register`
  reply already queued on the owner when the connection drops would cache an id the server has released, and every
  later call on that model would fail until the next drop. A call that read a cached id just before a reset still
  sends it once. The server refuses it, the agent sees that as a tool error, and the agent's retry registers
  afresh.
- **`resetter()`** returns a `std::function<void()>` that performs the reset. It holds a `weak_ptr` to `Shared`, so
  calling it after the adapter is gone does nothing. It may be called from any thread. A transport's disconnect
  handler outlives the adapter that borrows the transport, which is why this is a weak callable and not a member
  function.

The session stays the adapter's: `send` stamps `options.session` on every envelope, and `IEnvelopeTransport`
stamps nothing.

## Remote mode

`serveMcp` with `Args::server` set picks the transport:

| URL | `morph::net` built | Qt built | Transport |
|---|---|---|---|
| `ws://` | yes | either | `net::SocketBackend`, on a loop of its own |
| `ws://` | no | yes | `QtWebSocketBackend` |
| `wss://` | either | yes | `QtWebSocketBackend` |
| any | no transport fits | | Refused: exit 2, naming the missing transport |

`morph::net` is preferred for `ws://` because it needs no event loop on the main thread.

The arrangement is bank's: stdio runs on a thread of its own, which owns a `MainThreadExecutor` it pumps while
`serveStdio` waits. With the Qt transport, the main thread runs a `QCoreApplication` event loop that the backend
lives on, and stdio's end quits it. With `SocketBackend`, the main thread simply joins the stdio thread.

The transport is built with `retryFirstConnect = true`. The backends otherwise stop for good when the first
connect never reaches the server. A Claude Code session started before the application server would then have a
dead MCP server, and retrying fixes that. A call made before the first connect is held across every failed
attempt and sent once the server is reached. A call in flight when an established connection drops is rejected,
and so is a call made during a reconnect, at the next failed attempt. `callTimeout` bounds anything the server
never answers. The agent sees each rejection as a tool error.

The disconnect handler is the adapter's `resetter()`, so the next call registers fresh instances.

`initialize`, `ping` and `tools/list` before the catalog arrives behave as they do today: `initialize` and `ping`
need no server, and `tools/list` waits for the catalog, bounded by `callTimeout`.

## Local mode

Local MCP has the same semantics as the client's local GUI mode (`--app` without `--mcp-stdio`). It is not the
application's production server process. An application's own `main` may wire more than `LocalApp` describes:
lims's server, for example, installs a token secret, a ledger port, an event relay and a billing schedule.
Behaviour that only such a `main` installs is absent from both local modes. An agent that needs it uses remote
mode against that server.

`serveMcp` with `Args::app` set:

1. Looks the application up in the `LocalApp` list the binary links. If the name is unknown, `serveMcp` lists the
   names it knows on stderr and exits with code 2.
2. Runs its `setUp` with `LocalSetup{db, user}`, which opens the database. lims's, for example, appoints `--user`
   as the first supervisor of a fresh lab.
3. Builds the server with `client::makeLocalServer(app, workers, config)`: a `RemoteServer` whose services hold a
   `ui::UiService` over the application's bundle, checked by the application's `serverAuthorizer()` when it has
   one and by allow-all otherwise. The test kit's `SimulatedAppSource` builds this same server by hand today and
   moves onto the helper, passing its clock settings in `config`.
4. Builds the adapter on that server with the in-process constructor, and serves stdio on the main thread with a
   `MainThreadExecutor` owner.
5. When stdin ends, destroys the adapter, then the server, then the worker pool, in the order
   `docs/spec/concurrency_and_lifetimes.md` requires.

The action's own validation, effects and model-level checks run as they do for any dispatch. Server-level
authorization is whatever `serverAuthorizer` gives; no linked application sets one today, so it is allow-all. No
action journal is installed, as in local GUI mode.

## Identity

| Source | Principal | Token |
|---|---|---|
| Remote | `--user`, else `MORPH_MCP_PRINCIPAL` | `MORPH_MCP_TOKEN`, if set |
| Local | `--user`, the development principal `LocalSetup` already carries | none |

The token is read from the environment, never from the command line, because process listings show arguments.
`claude mcp add -e` sets it. Remote mode's principal falls back to `MORPH_MCP_PRINCIPAL` so that one environment
works with bank's embedded mode too. A remote server authenticates the session on every envelope, as it does a
GUI's. Whether a principal without a token is accepted is that server's authorizer's decision. A local server
with no `serverAuthorizer` checks nothing, so local mode ignores `MORPH_MCP_TOKEN`, and setting it prints a warning
on stderr.

## Stdout

MCP's stdio transport requires stdout to carry nothing but JSON-RPC. Nothing in `morph::client` or the adapter
writes to stdout, and morph's logger writes to stderr. The application code that local mode links is free to
print, however, and Lightweight's SQL logger writes to stdout.

So at start, before anything else runs, `serveMcp` duplicates file descriptor 1 for the JSON-RPC channel, then
points descriptor 1 at stderr (`dup`/`dup2`, `_dup`/`_dup2` on Windows). A stray print lands in Claude Code's server
log instead of corrupting the protocol.

## Errors

- **A refused start** writes one line to stderr and exits with code 2 before reading stdin. A start is refused for
  bad arguments, an unknown application, a URL no built transport can reach, or a bundle that does not build.
  Claude Code shows the server as failed, with that line in its log.
- **Frontend options are refused, not ignored.** With `--mcp-stdio`, any argument meant for a frontend is refused:
  `--screen`, `--exit-after-mount`, `--dev`, `--grab`, `--ui`, or anything else left after the client's own
  options. `MORPH_UI_DEV` and `MORPH_UI` are environment-wide settings, not arguments to this run, so they are
  ignored.
- **A failed call** is the adapter's existing behaviour. A refused or failed action is a tool result with
  `isError`. A transport failure or a timeout is a tool result carrying that message.

## Testing

- **The adapter over a transport.**
  - The adapter cases in `tests/test_push_agent.cpp` run again against `SimulatedRemoteBackend(server, cid)` as the
    transport: tools listed, an execute that reaches a watching connection, a refused call reported as `isError`,
    and `evaluate` changing nothing.
  - A scripted fake transport, whose replies the test releases by hand, covers what the simulated backend cannot:
    - A rejection shows up as a tool error.
    - A request that never replies times out after `callTimeout`.
    - A `register` reply released after `resetter()` ran is not cached, so the next call registers again. This
      case fails if the generation guard is removed.
    - `resetter()` called after the adapter is destroyed does nothing.
- **Remote mode end to end**, where `morph::net` is built.
  - A `morph::net::SocketServer` over a test `RemoteServer`, with `serveMcp` driven through pipes: `initialize`,
    `tools/list`, and one `tools/call` whose effect a second connection observes.
  - A server closed and reopened mid-run does not break the next call.
  - A server that starts after `serveMcp` serves calls once it is up.
- **`retryFirstConnect`** in each backend's own tests: with the flag set, a server that listens after the first
  failed attempt is reached; without it, the backend stops as it does today.
- **Local mode end to end.**
  - `serveMcp` over a test `LocalApp` with an in-memory model: `tools/list` names its actions, and a call's result
    comes back.
  - In the ladder build, one case runs `--app lims` on a scratch database through a shell pipe, as bank's
    `test_mcp_stdio.cpp` does, and checks that every line on stdout parses as a JSON-RPC message.
- **Arguments.** `--mcp-stdio` parsing, and each refusal under [Errors](#errors).

## Decisions

- **A flag on the client, not a new binary.** The client already resolves both sources, links the applications
  for local mode, and builds the transports. A new binary would copy all three.
- **The transport constructor borrows.** `IEnvelopeTransport` has a protected, non-virtual destructor and lives
  inside a `RemoteTransport` the caller owns. Ownership stays where the client's GUI path keeps it.
- **Reset on disconnect, guarded by a generation.** The server releases an adapter's instances when its connection
  drops, so resetting at the disconnect makes the next call register fresh ones. The generation guard covers
  replies already queued when the connection dropped.
- **A bound on each call, not a concurrent `serveStdio`.** MCP allows replies out of order, so a concurrent reader
  would also stop one held call from blocking the rest. That is a larger change, though, and a bounded wait is
  enough to keep the server responsive.
- **Local mode matches local GUI mode, through `RemoteServer`, never through SQL.** The tools are the application's
  own actions. Making local mode match an application's production `main` needs a `LocalApp` hook that `main`
  shares. That is a separate change, worth making once an application needs it.
- **A `makeLocalServer` helper.** The test kit already hand-builds a `RemoteServer` from a `LocalApp`. Local MCP
  would be the second place to do so, which is the signal that the framework is missing the seam.

## Out of scope

- MCP `resources`, `prompts` and progress notifications. The adapter serves tools only.
- MCP's Streamable HTTP transport. `handle` is transport-free, so it can be added later without touching the
  adapter.
- Telling GUIs connected to a different server process about writes local mode makes to the database. Fan-out
  reaches only connections on the same `RemoteServer`. Remote mode is the answer when people have the application
  open.
- Concurrent message handling in `serveStdio`.
