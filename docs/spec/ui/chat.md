# Server-hosted chat (`morph::ui::chat`)

`include/morph/ui/chat/`. A chat built into a client, whose assistant runs on the server and calls the actions of the
screen the person is looking at. It is one more frontend: the same actions, the same checks, and the GUI reacts through
the push path every other change uses. The design is in
`docs/superpowers/specs/2026-11-07-server-hosted-chat-design.md`.

## Contents

- [Protocol](#protocol)
- [Providers](#providers)

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

## Providers

`provider.hpp`. `IProvider::stream` takes a `Request` (system text, conversation, tools) and a `ProviderSink`, and returns
an `IProviderCall` to cancel it. Text pieces arrive in order on any thread, and `done` runs exactly once unless the call
was cancelled, possibly before `stream` returns.

A model's output is untrusted input: a `ToolCall` may name no tool, and its arguments may not be an object. Nothing
outside an implementation names a provider. `testing::FakeChatProvider` is a scripted one for tests.
