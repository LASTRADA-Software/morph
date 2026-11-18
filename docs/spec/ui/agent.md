# `morph::ui::McpAdapter` — agents through the server

`include/morph/ui/agent.hpp`. An agent such as Claude Code acts on an application through the same dispatch as every
GUI: its tool calls are ordinary envelopes on a server connection of their own. Every check a GUI's calls get applies
to them, and every GUI watching what they change is told. The design is spec 8 §11
(`docs/superpowers/specs/2026-10-16-server-push-design.md`).

## Contents

- [Shape](#shape)
- [Tools](#tools)
- [Identity and checks](#identity-and-checks)
- [Threads](#threads)
- [Over a transport](#over-a-transport)
- [stdio](#stdio)
- [From the generic client](#from-the-generic-client)
- [Agents and the built-in chat](#agents-and-the-built-in-chat)
- [Design decisions](#design-decisions)

## Shape

| Type | Role |
|---|---|
| `McpOptions` | `session`, the identity every envelope carries; `serverName` and `serverVersion`, answered by `initialize`; `callbacks`, the serial owner `handle` completes on (required); `callTimeout`, the bound on each envelope sent over a transport (30 s by default) |
| `McpAdapter(server, options)` | Opens a connection on `server` (`openConnection`). Its destructor closes it, which releases the adapter's private instances |
| `McpAdapter(transport, options)` | Sends its envelopes over a borrowed `backend::IEnvelopeTransport`, a connection to a server in another process. It opens and closes nothing |
| `resetter()` | A `std::function<void()>` that forgets what the adapter cached about its server; the transport's disconnect handler |
| `handle(message)` | One JSON-RPC 2.0 message in. Completes with the reply's text, or with nothing for a notification. It never fails: every fault is answered as a JSON-RPC error |
| `serveStdio(adapter, owner, in, out)` | MCP's stdio transport over newline-delimited streams |
| `serveStdioOnOwner(adapter, owner, in, out)` | The same, for an owner another thread runs |

`handle` answers `initialize`, `ping`, `tools/list` and `tools/call`. Any other request is refused with error
`-32601` (`kMcpMethodNotFound`). A message that is not JSON is `-32700`; one with no `method` is `-32600`; a
`tools/call` the adapter cannot use (no tool name, arguments that are not an object, a tool the catalog does not
know, a keyed tool without its instance) is `-32602`. A notification, which carries no `id`, gets no reply,
whatever its method.

## Tools

The adapter reads the session's `ui-catalog` once, at the first call that needs it, and keeps it for the
adapter's life.

- **One tool per permitted action**, named by the action id: the catalog's `permitted`, which covers the actions a
  served document names and the principal may run. Its `inputSchema` is the action's schema from the model's
  `schemas`, made an object.
- **A keyed model's tool** also takes `instance`, the instance key, which is required. The call runs on the shared
  instance with that key, registered (`register` with `shared`) at its first use and kept by model type and key.
- **Any other model's tool** runs on one private instance per model type, registered on the adapter's connection at
  its first call.
- **`evaluate`** takes `{action, body}` and is a dry run through `ui-evaluate`: nothing runs and nothing changes.
- **A call's result** is `{content: [{type: "text", text}], isError}`. The text is the action's result JSON, or the
  server's `err` message with `isError: true`. A refused call is such a result, not a JSON-RPC error, so the agent
  sees why.

## Identity and checks

The deployer builds the adapter with the `session::Context` its envelopes carry, for example from a token in the
environment. The server authenticates it on every envelope, as it does a GUI's, so authentication and the
principal, `authorize` and `authorizeInstance`, limits, the journal, cancellation and the in-flight count all
apply. `authorize` reads the session as sent, before the server writes the verified principal into it, so the
session carries the principal its token proves.

Fan-out treats the adapter's connection like any other. A successful call's changes reach every watching
connection, and a refused call changes nothing and posts nothing.

## Threads

A `Completion` is delivered on a serial owner, so the adapter takes one, `McpOptions::callbacks`. It must outlive
the adapter and every call still in flight. The server answers on its own pool threads, and the adapter's caches
are guarded by a mutex that is never held while a continuation runs: a continuation may need the lock again.

## Over a transport

The adapter touches its server in three places: `openConnection` when it is built, `closeConnection` when it is
destroyed, and `RemoteServer::handle` for every envelope it sends (the catalog, `register`, `execute`,
`evaluate`). Its shared state holds a link in place of a server pointer: one function that sends an envelope and
calls back with the reply, and an optional close. The in-process constructor's link opens a connection and closes
it in the destructor. The transport constructor's link is `transport.request(envelope, *options.callbacks)`.

- **The reply shape is one.** A reply resolves the callback whether it is `ok` or `err`. A rejection
  (`DisconnectedError`, or any other failure to deliver) becomes `wire::makeErr(what)`, so every caller of `send`
  sees the same thing, and the agent sees a tool error.
- **The transport is borrowed** and must outlive the adapter. Its connection is the agent's connection, and the
  server releases the adapter's private instances when that connection drops. The session stays the adapter's:
  `send` stamps `options.session` on every envelope, and the transport stamps nothing.
- **`callTimeout` bounds each envelope.** A call that gets no reply in time answers
  `wire::makeErr("no reply from the server in <n> s")`, and a reply that arrives later is dropped. `serveStdio`
  handles one message at a time, so without the bound a request the server never answers would block every later
  message, `ping` included. Zero means no bound; the in-process constructor ignores it, since `RemoteServer`
  always replies.
- **A generation guard on the caches.** A reset bumps a generation counter under the mutex and clears the cached
  catalog and every cached model id. `withCatalog` and `withInstance` record the generation when they send and
  cache the reply only if it is unchanged when the reply arrives. Without the guard, a `register` reply already
  queued on the owner when the connection drops would cache an id the server has released, and every later call
  on that model would fail until the next drop. A call that read a cached id just before a reset still sends it
  once; the server refuses it, the agent sees a tool error, and its retry registers afresh.
- **`resetter()` holds a `weak_ptr`** to the shared state, so calling it after the adapter is gone does nothing,
  and it may be called from any thread. A transport's disconnect handler outlives the adapter that borrows the
  transport, which is why the reset is a weak callable and not a member function.
- **What a transport does with a call during an outage** is the transport's. With `retryFirstConnect`, a call
  made before the first connect is held across every failed attempt. A call in flight when an established
  connection drops is rejected, and so is a call made during a reconnect, at the next failed attempt.
  `callTimeout` bounds anything the server never answers. The agent sees each rejection as a tool error.

## stdio

`serveStdio` reads one message per line of `in`, skipping empty lines, and writes each reply on a line of `out`,
flushed, in order, until `in` ends. Its calling thread is the adapter's owner: it pumps the `MainThreadExecutor`
it is given while it waits for each reply. `serveStdioOnOwner` serves the same stream when another thread turns the
owner, such as a Qt event loop: each `handle` is posted to the owner and the calling thread waits for its reply. An agent launches the server process with an option that turns it on
(bank's server: `--mcp-stdio`), and the process serves its WebSocket clients as before.

## From the generic client

The generic client serves an adapter with `--mcp-stdio`, remote over a transport or local over an in-process server.
See [`../client/client.md`](../client/client.md#mcp-on-stdio).

## Agents and the built-in chat

`docs/spec/ui/chat.md` describes a chat built into the client, whose assistant also runs on the server. It differs from
an agent over this adapter in what it acts on:

| | An agent over `McpAdapter` | The built-in chat |
|---|---|---|
| Where the model runs | In the agent, which brings its own | On the server, behind `chat::IProvider` |
| Tools | Every action `permitted` lists | What the open screen names |
| Identity | The session the adapter was built with | The signed-in user's session |
| Instances | Its own, on a connection of its own | The screen's own |
| Journal | Like any other client's entry | An entry marked with the origin `assistant` |

## Design decisions

- **No transport in the adapter.** `handle` is message in, message out, so the same adapter serves stdio today
  and an HTTP transport later, and a test drives it directly.
- **A refused call is a tool result, not a protocol error.** An agent reads `isError` and the server's reason and
  can act on it; a JSON-RPC error would tell it only that the call was malformed.
- **`ping` is answered** because MCP's base protocol requires it of every server.
- **Reset on disconnect, guarded by a generation.** The server releases an adapter's instances when its connection
  drops, so resetting at the disconnect makes the next call register fresh ones, and the generation covers replies
  already queued when the connection dropped.
- **A bound on each call, not a concurrent `serveStdio`.** MCP allows replies out of order, so a concurrent reader
  would also stop one unanswered call from blocking the rest. That is a larger change, and a bounded wait keeps
  the server responsive.
