# Server-hosted chat (`morph::ui::chat`)

`include/morph/ui/chat/`. A chat built into a client, whose assistant runs on the server and calls the actions of the
screen the person is looking at. It is one more frontend: the same actions, the same checks, and the GUI reacts through
the push path every other change uses. The design is in
`docs/superpowers/specs/2026-11-07-server-hosted-chat-design.md`.

## Contents

- [Protocol](#protocol)
- [Tools](#tools)
- [Providers](#providers)
- [Configuration](#configuration)
- [The turn loop](#the-turn-loop)
- [Conversations](#conversations)
- [The client](#the-client)
- [The panel](#the-panel)

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

`morph::qt::makeQtChatProvider(config, owner)` in `qt/qt_chat_provider.hpp` is the one place that names a provider. Its
one name is `anthropic`, over `QNetworkAccessManager` on `owner`'s thread, and `stream` may be called from any thread.

- `model` and `apiKey` have no default. Which model a deployment pays for is the deployer's choice, and the key appears in
  no log and no chat message.
- Requests stream. At the provider's public address they ask for `fallbacks: "default"`, so a request a safety classifier
  declines is rerun on a model suited to that category. At any other `baseUrl` they do not, since a service speaking the
  same format, such as OpenRouter at `https://openrouter.ai/api`, can refuse the whole request for that field. A refusal
  that remains is reported as the turn's failure.
- **What must be replayed goes back unchanged.** `Response::opaque` holds the assistant message as the provider gave it,
  including the model's signed reasoning, and comes back through `Message::opaque`. Only the provider reads it, so nothing
  else in the chat knows a provider's content format. It is valid only in the conversation that produced it, so
  `ConversationStore` clears it when trimming edits the history, and the message then goes back as its text and calls.
- A tool call cut off by the token limit is reported as a failure and not run, since it usually still parses as a partial
  object.

## Configuration

`config.hpp`. `ChatConfig` is a plain struct of the provider, the limits and the system prompt. An application fills it
from wherever it keeps settings, and hands it to `makeQtChatProvider` and `ChatOptions::configure`. `loadChatConfig` is an
optional reader that fills it from a JSON file:

```json
{
  "provider": { "name": "anthropic", "model": "...", "apiKeyEnv": "THE_KEY_VARIABLE" },
  "limits": { "maxToolCallsPerTurn": 16, "idleRetentionMinutes": 480 },
  "systemPrompt": "..."
}
```

- `provider` takes `name`, `model`, `baseUrl`, `maxTokens`, and one of `apiKeyEnv` and `apiKey`. `apiKeyEnv` names an
  environment variable, which keeps the secret out of the file. Naming both is refused.
- `limits` takes each field of `ChatLimits`, with `idleRetentionMinutes` for its time limit. What is left out keeps its
  default.
- A key the format does not have is refused, so a misspelled limit cannot silently leave the default in force. A limit of
  0 is refused, since it would refuse every turn.
- A refusal says what is wrong without quoting the text, which may hold the key.

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

## The panel

`panel.hpp`. `ChatPanel` is built from backend widgets, so every shell frontend has it: a conversation's lines
(`chat_entry_<n>` inside `chat_entries`), an input (`chat_input`), a send button (`chat_send`) and, while a turn runs, a
stop button (`chat_cancel`). It reads where the person is at each send, so a message goes to the screen they are looking
at when they send it. A screen with no document behind it, such as the explorer, disables the input.

The client shell mounts it in `ShellFrame::chatSlot()`. It shows the slot only after a probe finds a chat, selects the
active tab's conversation as tabs change, and forwards the source's events and reconnects to the `ChatClient`. A sign-out
sends `ui-chat-end` first and continues when that is answered or fails. The panel is hidden in the frame's compact form,
which has no width to spare beside the page.

The panel shows on every screen with a document. A screen built around a working copy, whose edits are not stored until a
separate save and whose flags are set by its own buttons, does not yet work with an assistant, whose calls change the
working copy without those flags following.
