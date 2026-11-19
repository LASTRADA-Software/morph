# Server-hosted chat (`morph::ui::chat`)

`include/morph/ui/chat/`. A chat built into a client, whose assistant runs on the server and calls the actions of the
screen the person is looking at. It is one more frontend: the same actions, the same checks, and the GUI reacts through
the push path every other change uses. The design is in
`docs/superpowers/specs/2026-11-07-server-hosted-chat-design.md`.

## Contents

- [Protocol](#protocol)
- [Tools](#tools)
- [Extra tools](#extra-tools)
- [Providers](#providers)
- [Configuration](#configuration)
- [The turn loop](#the-turn-loop)
- [Conversations](#conversations)
- [Dictation](#dictation)
- [The client](#the-client)
- [The panel](#the-panel)

## Protocol

`protocol.hpp`. Six envelope kinds on the GUI's connection, under its session:

| Kind | Body | Reply |
|---|---|---|
| `ui-chat-turn` | `TurnRequest`: screen, title, identity, params, the aliases it holds, and the text | `TurnAccepted{turnId}`, at once |
| `ui-chat-state` | `StateRequest{turnId?}`; with no id, it only asks whether a chat is available | `TurnState` |
| `ui-chat-cancel` | none | `ok` |
| `ui-chat-end` | none | `ok` |
| `ui-chat-transcribe` | `TranscribeRequest{audio, mediaType, language?}`: a dictated clip, base64, `audio/wav` | `TranscribeAccepted{id}`, at once |
| `ui-chat-transcription` | `TranscriptionQuery{id}` | `TranscriptionState{id, status: running \| done \| failed, text?, error?}` |

`ui-chat-state` with no turn (the probe) also answers `TurnState::dictation`, `{maxSeconds, maxBytes}`, when the server
offers dictation, and leaves it out when it does not. A transcription ends with a `TranscriptionEvent` on the `chat`
topic, `{kind: "transcription", state}`; its `kind` tells it from a `TurnEvent`, and an older client reads it as a turn
event with turn id 0 and drops it. Like a turn's events it is a hint: the client reads `TranscriptionState`.

A `TurnRequest` describes where the person is, and is a hint about what they mean. It grants nothing, because every
action it leads to is checked under the envelope's session. Its `title` is the screen's title as the person sees it
(`DocumentScreenHost::chatContext` reads it from the screen's head), and the system text tells the model that the person
knows the record by that name, so it speaks of "Sample 16/1" and not of a key or an internal number.

**What a turn reports.** A tool call carries the call's arguments (JSON text, cut like a result's short form) and
whether it ran, in `ToolStep` and in its `tool` event. A round's text is not glued to the last one's: `TurnState::text`
puts a blank line between rounds, and a `text` event whose `newRound` is set is the first piece of a round that follows
tool calls. `TurnState::parts` lists the rounds' texts and the tool calls in the order the turn made them, so a client can
draw what was said before a call before it, not after. A state without `parts`, from a server that does not send them, is
read as its `steps` and then its `text`.

**Drafts.** `TurnRequest::draft` is the document of the draft the person has open on the screen, as JSON text, and
`TurnState::draft` is the last document the server accepted this turn. A `draft` event on the `chat` topic carries that
document in its `text`, so a client can mount it at once; like every event it is a hint, and a client that missed it reads
the document from `TurnState`. All three are optional on the wire: a peer that does not know them ignores them, and a
message with none says nothing. The server refuses a turn whose draft is over `ChatLimits::maxDraftBytes` (256 KiB) or is
not a JSON object, so a client's text is bounded and parsed before anything reads it. The server reaches a draft only
through an extra tool, which is also what checks it.

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

## Extra tools

`extra_tool.hpp`. `ChatOptions::tools` adds tools beside the open screen's own, for what an application's chat can do
that is not one of the screen's actions. A `ChatTool` is a `ToolSpec`, an `offer`, a `handler` and a `record`.

- **Offered per turn.** `offer` runs once when the turn builds its tools, on the worker, with the turn's session and the
  `TurnRequest` (screen, params, identity, models, draft) and answers whether this turn offers the tool. It can add text to
  the system prompt whether or not it offers the tool, which is where a tool that is not offered says why. What it works out
  can travel to the handler in `ChatToolOffer::carried`. With no `offer` the tool is always offered.
- **Run by a handler.** A call whose arguments parse to a JSON object runs `handler` on the worker with the context by
  value. It answers through a callback, from any thread: a handler backed by a coroutine starts it with
  `morph::async::spawn` and keeps whatever it captures alive until it answers. The first answer counts and later ones are
  ignored, and one that arrives after the turn stopped changes nothing.
- **A deadline for every call.** An offer, a handler and the document source that has not answered within
  `ChatLimits::toolCallTimeout` (30 seconds by default; `toolCallTimeoutSeconds` in the configuration file) are answered by
  the chat: an offer counts as declined, a handler's call as failed ("the tool did not answer in time"), and the document
  source fails the turn. The turn goes on, and the late answer is ignored. Stopping the turn, such as by its connection
  closing, drops a pending deadline.
- **The same rules as a screen's tools.** An unknown name, or arguments that are not an object, are a tool error counted
  against `maxToolCallsPerTurn`, and the handler is not called. A handler that throws is a failed call, and the model is told
  only that the tool failed. Nothing a provider returns faults the worker. Stopping the turn cancels it.
- **What an outcome can do.** `isError` marks the call failed, and `ChatToolContext::failedCalls` tells the next call of the
  tool how many already failed, so a tool can set its own limit. `stop` ends the turn as failed with that text, after the
  call's result is recorded. `draft` becomes `TurnState::draft` and goes out as a `draft` event; a failed call's draft is
  dropped, and so is a draft longer than `ChatLimits::maxDraftBytes`, which makes the call fail. The `stop` text is cut like
  a step's message. A turn that stopped before the outcome arrived records neither.
- **What the conversation keeps.** `record` rewrites a call's arguments before the conversation stores them, so a large
  argument is not carried through every later turn. The message's provider replay (`Message::opaque`) is dropped with it,
  since it holds the call as the model wrote it. The turn itself sees the call as the model wrote it.
- **Names.** 1 to 64 characters of letters, digits, `_` and `-`, with no `__`, which every screen tool's name has, and distinct
  from the other extra tools. `ChatService` refuses a tool that breaks the rules, or has no handler, when it is built.

- **The turn's current document.** `ChatToolContext::document` is the latest draft a tool accepted in this turn, else the
  draft the request carried, else absent (the tool then reads where the turn started, from the bundle or `external`). The
  service sets it from `TurnState::draft` each time it builds a context, so a call sees what the calls before it accepted;
  the server is the only writer of the document within a turn.

`propose_screen` and the read tools `screen_outline`, `screen_find` and `screen_node` (`propose_screen.hpp`) are the first
extra tools: they let the assistant read and change the open screen's document, and
[custom_screens.md](custom_screens.md#the-assistant) describes them. A chat reads a screen the bundle does not hold, such as a
person's own custom screen, through `ChatOptions::documentSource`, which is asked only for an id the bundle has no screen of,
once per turn, and answers for the turn's person alone. What it read reaches each tool as `ChatToolContext::external`.

A tool is data-in, checks-out: it treats the request's fields, the draft above all, as text someone may have written, and
does what it does under the session it is given.

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

`morph::qt::makeQtTranscriber(config, owner)` in `qt/qt_transcriber.hpp` is the one place that names a speech-to-text
provider. Its one name is `openai-transcriptions`: a multipart `POST` to `{baseUrl}/v1/audio/transcriptions` over
`QNetworkAccessManager` on `owner`'s thread, with the parts `file` (the WAV, as `audio.wav`), `model`,
`response_format=json`, and `language` when the input carries one, and the header `Authorization: Bearer <key>`. It reads
`text`. A non-2xx answer fails with the status and the provider's `error.message` when it has one, never with the
request; an answer without a `text` fails as unreadable. `cancel` aborts the reply. A deployment that keeps audio on its
own network points `baseUrl` at its own server; an empty `baseUrl` is `https://api.openai.com`. `model` and the key have
no default, and the key appears in no log and no message.

## Configuration

`config.hpp`. `ChatConfig` is a plain struct of the provider, the limits and the system prompt. An application fills it
from wherever it keeps settings, and hands it to `makeQtChatProvider` and `ChatOptions::configure`. `loadChatConfig` is an
optional reader that fills it from a JSON file:

```json
{
  "provider": { "name": "anthropic", "model": "...", "apiKeyEnv": "THE_KEY_VARIABLE" },
  "transcription": { "name": "openai-transcriptions", "model": "whisper-1", "apiKeyEnv": "STT_KEY", "language": "en" },
  "limits": { "maxToolCallsPerTurn": 16, "idleRetentionMinutes": 480 },
  "systemPrompt": "..."
}
```

- `provider` takes `name`, `model`, `baseUrl`, `maxTokens`, and one of `apiKeyEnv` and `apiKey`. `apiKeyEnv` names an
  environment variable, which keeps the secret out of the file. Naming both is refused.
- `transcription`, optional, names the dictation's provider with `name`, `baseUrl`, `model`, `language`, and one of
  `apiKeyEnv` and `apiKey`, with the provider block's rules: an unknown key is refused, both keys are refused, and a
  refusal never quotes the text. Without it the chat offers no dictation.
- `limits` also takes `maxDictationSeconds` (60), `maxDictationBytes` (4 MiB), `maxTranscriptionsPerPrincipal` (2) and
  `transcriptionRetentionSeconds` (60). 0 is refused for each, and so is a `maxDictationBytes` above
  `kMaxDictationBytes` (about 6 MiB, what one envelope carries once base64 grows it by 4/3).
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
2. Build the tools: the screen's own, then each extra tool whose `offer` accepts the turn. Call the provider with the
   system prompt, what the extra tools added to it, and the screen's context.
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
least recently used goes past `maxConversationsPerPrincipal`. Past `maxMessagesPerConversation` or `maxConversationBytes`
the oldest messages go,
along with whatever would leave the history starting with anything but something a person said, since a provider refuses
a history that opens with a reply or with a result whose call is gone. Nothing survives a restart, and the journal keeps
what actually changed.

A turn adds whole rounds only, a call together with its results, so one stopped part way leaves nothing a provider would
refuse. What the person said goes in with the first round, so a turn stopped before the model answered leaves no trace: the
next turn must not read a stopped request as still wanted, since a change applies at once. A turn stopped later keeps what
it did, because those changes happened.

A tool's result is cut at `maxToolResultBytes`, with a note saying so, so one large list cannot make every later request
on that record too large. A turn's state shows a short form of each result, so it always fits one envelope.

**Ending a chat.** The server cannot see a client's sign-out, which happens only in the client, and it learns of a new
principal only from the connection's next envelope. So a client signing out sends `ui-chat-end`, which stops the
principal's turn on that connection and ends their conversations. A turn on another connection of theirs keeps running.
The idle limit covers a client that never sent it. A token that stays valid after sign-out is not something a chat can
revoke.

## Dictation

A person can speak a message instead of typing it. The words land in the chat's input, where the person reads,
corrects and sends them as typed text. Speech becomes text on the server, through a configured speech-to-text
provider; morph embeds no recognition engine and no model.

**The seam.** `transcriber.hpp`. `ITranscriber::transcribe` takes a `TranscriptionInput` (the clip, its media type and
one language or none) and a `TranscriberSink`, and returns an `ITranscriberCall` to cancel it. It is callable from any
thread, and `done` runs exactly once unless the call was cancelled, possibly before `transcribe` returns. Nothing
outside an implementation names a provider. `testing::FakeTranscriber` scripts answers and counts cancels.

**Accepting.** `ChatOptions::transcriber` turns dictation on; with none the service serves neither transcription kind
and its probe offers none, and a transcriber without a provider is refused when the service is built.
`ui-chat-transcribe` passes the chat's gate and is answered at once, as a turn is: refused with a reason naming the
bound when it arrives with no connection, when its base64 text is longer than `ceil(maxDictationBytes / 3) * 4`
(checked before decoding), when it decodes to more than `maxDictationBytes`, when it is not base64, when its media type
is not `audio/wav`, when the connection already has a transcription running (`busy`), or when the principal has
`maxTranscriptionsPerPrincipal` running across connections. Otherwise it is recorded, answered with
`TranscribeAccepted{id}`, and transcribed on the worker. The request's `language` wins over the configured
`transcription.language`; the service applies the default, so a transcriber sees one value.

**Reading.** `ui-chat-transcription` answers the state. A query for another principal's id, for one forgotten and for
one that never was gets the same `failed` answer, "no such transcription", so ids reveal nothing.

**Ending, exactly once.** Each accepted transcription ends in exactly one of `done` and `failed`, recorded in the
service's table and sent as a `transcription` event to its connection: the transcriber's answer; the deadline,
`ChatLimits::toolCallTimeout`, which cancels the call and fails with "the transcription did not answer in time"; or
`ui-chat-end`, which cancels the principal's transcription on that connection and fails it with "cancelled". A
connection closing cancels its calls and drops their entries, since no one can read them, and the service's destruction
cancels every call and sends nothing. An answer arriving after the entry ended is ignored. The connection's `busy` slot
and the principal's count free when the entry ends.

**Nothing kept beyond the retention.** A finished transcription stays readable for `transcriptionRetention` (60
seconds), swept whenever the table is touched, and then reads as "no such transcription". Neither audio nor text is
journaled, logged or added to a conversation; the clip moves into the transcriber's call when it starts. The text
reaches the assistant only inside a later `ui-chat-turn`, as text like any other.

## The client

`client.hpp`. `ChatClient` sends what a person says and keeps what to show per conversation, for display only and for the
session. It runs on the owner, and takes the source's exchange, the current session and a change callback.

- **The probe.** `probe()` sends `ui-chat-state` with no turn. An `ok` means this server offers this principal a chat; any
  other answer means it does not, which is not a fault to report.
- **State beats events.** It reads `TurnState` when a turn starts, when an event says one ended, and on `resync`. What it
  reads replaces what events built, and `TurnState::seq` says which events it already covers.
- **A turn is kept as its parts.** An `Entry` of a tool call carries the call's `arguments` and `ok`, and each round's text
  is its own `assistant` entry, in the order the turn made them. Events build the same parts: a `text` piece joins the
  text before it unless a tool call came between or the event says `newRound`.
- **A turn belongs to its conversation.** Switching tabs selects another conversation and leaves a running turn where it
  started.
- **A reconnect ends a running turn.** A new connection has new instance ids, and the old connection's close stopped the
  turn on the server. What was shown stays, and the server's conversation continues from the next turn.
- **`end` calls back whatever happens**, including when the exchange fails, so a sign-out never waits on a dead
  connection.
- **A draft goes back to the tab that asked.** `send` takes an opaque `origin`, which the client hands back with each
  draft the turn proposes (`onDraft`, once per distinct document, whether the `draft` event or the turn's state brought
  it first). The shell uses it to name the tab, so a draft the assistant proposes belongs to the tab the turn started
  in even when the person has since moved to another.

## The panel

`panel.hpp`. The panel is a document like any screen, `kChatPanelDocument`, so every frontend draws it and it uses the same
vocabulary an application's screens use: a `forEach` of the conversation's lines (`chat_entry`, inside `chat_entries`), an
input (`chat_input`), a send button (`chat_send`) and, while a turn runs, a stop button (`chat_cancel`).

`ChatPanelBinding` connects it to a `ChatClient` through a `HostVocabulary`, the seam a document has to anything the client
owns: the values `chat.entries`, `chat.running`, `chat.onScreen` and `chat.canSend`, and the commands `chat.send` and
`chat.cancel`. The commands are safe for an untrusted document: they do only what the person could do by typing, under
their own session, and every action they lead to is checked on the server. The binding reads where the person is at each
send, so a message goes to the screen they are looking at when they send it. A screen with no document behind it, such as
the explorer, disables the input. So does a running turn: the input takes text only while a message can be sent
(`chat.canSend`, like the send button). Enter runs the input's submit, which clears it, so an enabled input during a turn
would throw away what the person typed, without sending it. A turn ends for the client when it reads the turn's state
after the `done` event, one round trip after the turn's last text is shown.

A screen's own draft is part of where the person is: the shell reads the draft the active tab holds at each send and
carries it as `TurnRequest::draft` (`CustomScreenChrome::draftOf`), or nothing for a tab with no draft. The client bounds
it with the default `ChatLimits` (`maxDraftBytes`) and sends none when the draft is over the bound, which the server
would refuse. A draft the assistant proposes is mounted through the custom-screen chrome's draft path
([`custom_screens.md`](custom_screens.md#the-client): `CustomScreenChrome::proposeDraft`) for the turn's own tab only,
and only while that tab is open and still shows the screen the turn was about; the next turn then carries that draft.

The client shell mounts the document in `ShellFrame::chatSlot()`. It shows the slot only after a probe finds a chat, selects the
active tab's conversation as tabs change, and forwards the source's events and reconnects to the `ChatClient`. A sign-out
sends `ui-chat-end` first and continues when that is answered or fails. The panel is hidden in the frame's compact form,
which has no width to spare beside the page.

The panel shows on every screen with a document. A screen built around a working copy, whose edits are not stored until a
separate save and whose flags are set by its own buttons, does not yet work with an assistant, whose calls change the
working copy without those flags following.
