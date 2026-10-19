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
- [stdio](#stdio)
- [Design decisions](#design-decisions)

## Shape

| Type | Role |
|---|---|
| `McpOptions` | `session`, the identity every envelope carries; `serverName` and `serverVersion`, answered by `initialize`; `callbacks`, the serial owner `handle` completes on (required) |
| `McpAdapter(server, options)` | Opens a connection on `server` (`openConnection`). Its destructor closes it, which releases the adapter's private instances |
| `handle(message)` | One JSON-RPC 2.0 message in. Completes with the reply's text, or with nothing for a notification. It never fails: every fault is answered as a JSON-RPC error |
| `serveStdio(adapter, owner, in, out)` | MCP's stdio transport over newline-delimited streams |

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

## stdio

`serveStdio` reads one message per line of `in`, skipping empty lines, and writes each reply on a line of `out`,
flushed, in order, until `in` ends. Its calling thread is the adapter's owner: it pumps the `MainThreadExecutor`
it is given while it waits for each reply. An agent launches the server process with an option that turns it on
(bank's server: `--mcp-stdio`), and the process serves its WebSocket clients as before.

## Design decisions

- **No transport in the adapter.** `handle` is message in, message out, so the same adapter serves stdio today
  and an HTTP transport later, and a test drives it directly.
- **A refused call is a tool result, not a protocol error.** An agent reads `isError` and the server's reason and
  can act on it; a JSON-RPC error would tell it only that the call was malformed.
- **`ping` is answered** because MCP's base protocol requires it of every server.
