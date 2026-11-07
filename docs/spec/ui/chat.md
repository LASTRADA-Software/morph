# Server-hosted chat (`morph::ui::chat`)

`include/morph/ui/chat/`. A chat built into a client, whose assistant runs on the server and calls the actions of the
screen the person is looking at. It is one more frontend: the same actions, the same checks, and the GUI reacts through
the push path every other change uses. The design is in
`docs/superpowers/specs/2026-11-07-server-hosted-chat-design.md`.

## Contents

- [Protocol](#protocol)
- [Tools](#tools)
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
