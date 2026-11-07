# Server-hosted chat (`morph::ui::chat`)

`include/morph/ui/chat/`. A chat built into a client, whose assistant runs on the server and calls the actions of the
screen the person is looking at. It is one more frontend: the same actions, the same checks, and the GUI reacts through
the push path every other change uses. The design is in
`docs/superpowers/specs/2026-11-07-server-hosted-chat-design.md`.

## Contents

- [Protocol](#protocol)
- [Tools](#tools)
- [Providers](#providers)
- [The turn loop](#the-turn-loop)
- [Conversations](#conversations)
- [The client](#the-client)

## Protocol

`protocol.hpp`. Four envelope kinds on the GUI's connection, under its session:

| Kind | Body | Reply |
|---|---|---|
| `ui-chat-turn` | `TurnRequest`: screen, identity, params, the aliases it holds, and the text | `TurnAccepted{turnId}`, at once |
| `ui-chat-state` | `StateRequest{turnId?}`; with no id, it only asks whether a chat is available | `TurnState` |
| `ui-chat-cancel` | none | `ok` |
| `ui-chat-end` | none | `ok` |

A `TurnRequest` describes where the person is, and is a hint about what they mean. It grants nothing, because every
action it leads to is checked under the envelope's session.

A turn's progress goes out as `TurnEvent`s on the `chat` event topic, to that connection only. **Events are hints.** The
server writes nothing to a connection that is behind, so a turn's last event can be lost with no error. `TurnState` is
the record, and a client reads it when a turn starts, on `resync` and on reconnect. A client ignores event topics it does
not know, so one with no chat is unaffected.

## Tools

`screen_tools.hpp`. `screenTools` offers the actions the open screen names, at any scope: its queries, its mutations and
its model-bound nodes such as forms. Each is bound to the instance the client says that alias holds. So the assistant
has a short list of what this screen can do, and not every action in the application.

- **An alias counts only if its scope, name and type match the document's declaration.** A client's context cannot point
  a tool at a model the screen does not declare.
- **A tool's name is `<alias>__<action>`.** A name a provider's tool format would refuse (`^[a-zA-Z0-9_-]{1,64}$`), or one
  already taken, gets a shortened stem and a hash of its scope, alias and action. Two instances of one model type on one
  screen are told apart this way, which a list keyed by model type could not do.
- **A credential action gets no tool.** A model should not be handed a password to pass along.

## Providers

`provider.hpp`. `IProvider::stream` takes a `Request` (system text, conversation, tools) and a `ProviderSink`, and returns
an `IProviderCall` to cancel it. Text pieces arrive in order on any thread, and `done` runs exactly once unless the call
was cancelled, possibly before `stream` returns.

A model's output is untrusted input: a `ToolCall` may name no tool, and its arguments may not be an object. Nothing
outside an implementation names a provider. `testing::FakeChatProvider` is a scripted one for tests.

## The turn loop

`service.hpp`. `ChatService` is an `IEnvelopeService`, built with a provider, a worker executor, the served bundle and
`ChatLimits`. With no provider it serves no kind, so a client's probe finds no chat.

`handle` runs on the server strand and must not block, and a model call takes seconds. So `ui-chat-turn` checks the gate
and the limits, replies at once, and posts the work to the worker. Each turn then does one thing at a time:

1. Read `ui-catalog` and each bound model type's `schemas`, under the turn's session. Permissions are read fresh each
   turn, and a screen the principal may not open fails the turn.
2. Build the tools, and call the provider with the system prompt and the screen's context.
3. Run each tool call, and feed the results back, until a response has no calls, a limit is reached, or the turn stops.

**A tool call is an unscoped execute.** It goes through `RemoteServer::executeWithOrigin` with the client's session and
the instance id from the context.

- `dispatchExecute` checks the session and the instance's owner rather than the connection, so every check applies.
- Fan-out reaches the GUI through the instance's holding scopes and the matching principal.
- The connection's principal baseline is left alone, so a GUI signing in again during a turn does not lose its watches.
- The assistant's call ids cannot collide with the GUI's.
- An unscoped execute is not cancellable, so cancelling a turn stops it between actions.

**A gate of its own.** Every chat kind passes `authorize(session, "ui", "chat")`.

**A model's output is untrusted.** A call naming no tool on this screen, or whose arguments are not a JSON object, comes
back to the model as a tool error and counts against `maxToolCallsPerTurn`. Nothing a provider returns can fault the
worker. Destructive actions are allowed, and a record's contents reach the model, so that cap is also what bounds the
harm one steered turn can do.

**Stopping.** `ui-chat-cancel`, `ui-chat-end`, the connection closing and the service's destruction each stop a turn and
cancel its provider call. A callback arriving later finds the turn stopped and does nothing.

**Journaling.** A tool call's entry carries the origin `assistant` and the turn's text. An action that changes data but
registered `Loggable::No` is warned about at the first service dispatch, and its tool result says it was not journaled.
It is not refused, because working-copy edits legitimately opt out and the later save is the journaled change.

Finished turns are kept, a few per principal, so a client that lost the last event can read how one ended. A principal
reading a turn that is not theirs gets the same empty answer as for one that does not exist.

## Conversations

`conversations.hpp`. One conversation per principal, screen and identity. A screen's identity value is already what "this
record" means to a document, so a different record starts a fresh conversation and returning to one continues it. A
screen with no record has one conversation per principal.

`ChatLimits` bounds them. `idleRetention` ends a conversation left untouched, and reading one counts as touching it. The
least recently used goes past `maxConversationsPerPrincipal`. Past `maxMessagesPerConversation` the oldest messages go,
along with whatever would leave the history starting with anything but something a person said, since a provider refuses
a history that opens with a reply or with a result whose call is gone. Nothing survives a restart, and the journal keeps
what actually changed.

A turn adds whole rounds only, a call together with its results, so one stopped part way leaves nothing a provider would
refuse.

**Ending a chat.** The server cannot see a client's sign-out, which happens only in the client, and it learns of a new
principal only from the connection's next envelope. So a client signing out sends `ui-chat-end`, which stops the
principal's turn on that connection and ends their conversations. A turn on another connection of theirs keeps running.
The idle limit covers a client that never sent it. A token that stays valid after sign-out is not something a chat can
revoke.

## The client

`client.hpp`. `ChatClient` sends what a person says and keeps what to show per conversation, for display only and for the
session. It runs on the owner, and takes the source's exchange, the current session and a change callback.

- **The probe.** `probe()` sends `ui-chat-state` with no turn. An `ok` means this server offers this principal a chat; any
  other answer means it does not, which is not a fault to report.
- **State beats events.** It reads `TurnState` when a turn starts, when an event says one ended, and on `resync`. What it
  reads replaces what events built, and `TurnState::seq` says which events it already covers.
- **A turn belongs to its conversation.** Switching tabs selects another conversation and leaves a running turn where it
  started.
- **A reconnect ends a running turn.** A new connection has new instance ids, and the old connection's close stopped the
  turn on the server. What was shown stays, and the server's conversation continues from the next turn.
- **`end` calls back whatever happens**, including when the exchange fails, so a sign-out never waits on a dead
  connection.
