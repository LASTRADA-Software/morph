# Dictation in the chat

**Status:** design, revised after one review against the code. The spec section lands in `docs/spec/ui/chat.md` with
the code.

## Goal

A person can speak a message to the chat instead of typing it. The words land in the chat's input, where the person
reads them, corrects them and sends them as typed text. Speech becomes text **on the server**, through a configured
speech-to-text **provider**. morph embeds no recognition engine and no model.

Out of scope: spoken replies, wake words, streaming partial transcripts, and voice commands that act on a screen without
the assistant. Each can be built on top of this later without changing it.

## Decisions

| Decision | Why |
|---|---|
| Speech to text on the server, through a provider | Qt has no recognition; an engine in the client (whisper.cpp) adds a native dependency and 75 MB–1.5 GB of model to every client and cannot run in the browser build; OS dictation differs per platform and Linux has none. The chat's model provider is already server-side and its key never reaches a client. |
| One wire format: `POST {baseUrl}/v1/audio/transcriptions` | Hosted Whisper, self-hosted whisper.cpp and faster-whisper servers, and most gateways speak it. A deployment that keeps audio on its own network points `baseUrl` at its own server. |
| The transcript is appended to the input, never sent | A misheard word sent to an assistant whose tools change data costs more than one more press of Send. The panel document has no cursor position, and a person dictating a message expects it at the end. |
| Accept now, result later, as a turn does | `IEnvelopeService::handle` returns its reply synchronously and `RemoteServer::post` may drop an event for a connection that is behind. The chat already solves this for turns: an immediate accept, a hint event, and a state record the client reads. Dictation reuses that shape instead of adding an asynchronous reply path to core. |
| The dictation host values and commands reach the panel document only | `HostVocabulary::publish` installs a vocabulary session-wide, and every screen, including a custom screen or an assistant's draft, mounts in that session. A screen must not be able to start the microphone or read a transcript. This needs a framework seam, specified below. |
| The client records 16 kHz mono 16-bit, resampling when the device offers another format | It keeps the clip's size a function of its length alone, so the bounds hold on every device. |
| Recording is refused while the panel is hidden | A microphone must never run without its indicator in view. The panel is hidden in the frame's compact form. |

## The person's view

- A **Dictate** button beside **Send**, with the microphone icon when the theme has one and the label alone when it does
  not. It shows only when the server offers dictation, the client can record, and the person is on a screen.
- Pressing it starts recording. The button becomes **Stop (12 s)**, counting the seconds. Pressing it again stops.
  Recording stops by itself at the server's `maxDictationSeconds`, or when the clip reaches the server's
  `maxDictationBytes`, whichever comes first.
- Then the button reads **Transcribing…** and is disabled. The transcript is appended to the input: trimmed, after one
  space when the input is not empty and does not already end in whitespace.
- Dictating is allowed while a turn runs. It only changes the input, which a person can also type into during a turn.
- A failure (no microphone, permission denied, a refusal, a provider error, a timeout, a lost connection) adds a **note**
  to the conversation's display, drawn in the `error` role, and leaves the input as it was. Nothing fails silently.

## Wire

Three additions to `protocol.hpp`, under the GUI's session and the chat's gate (`authorize(session, "ui", "chat")`).

| Kind | Body | Reply |
|---|---|---|
| `ui-chat-transcribe` | `TranscribeRequest{audio: base64, mediaType: "audio/wav", language?}` | `TranscribeAccepted{id}`, at once |
| `ui-chat-transcription` | `TranscriptionQuery{id}` | `TranscriptionState{id, status: "running" \| "done" \| "failed", text?, error?}` |
| event `transcription` on the `chat` topic | `TranscriptionState` | none: a hint |

- `StateRequest` with no turn (the probe) answers, beside what it answers today, `TurnState::dictation`: an optional
  `{maxSeconds, maxBytes}`. Absent means the server offers no dictation. It is optional on the wire; an older peer
  ignores it. `ChatClient::probe` reads the body (today it reads only whether the answer is `ok`).
- **Events are hints, the state is the record**, as for turns. The client reads `ui-chat-transcription` when the event
  arrives, on `resync` and on reconnect. A finished transcription stays readable for `transcriptionRetention`
  (60 seconds) and is then forgotten; a query for an unknown id or another principal's id gets the same `failed`
  answer with "no such transcription".

## Server

### `ITranscriber` (`include/morph/ui/chat/transcriber.hpp`)

```cpp
struct TranscriptionInput { std::string audio; std::string mediaType; std::optional<std::string> language; };
struct TranscriberSink { std::function<void(std::expected<std::string, std::string>)> done; };  // once, any thread
class ITranscriberCall { public: virtual ~ITranscriberCall() = default; virtual void cancel() = 0; };
class ITranscriber {
public:
    virtual ~ITranscriber() = default;
    [[nodiscard]] virtual std::shared_ptr<ITranscriberCall> transcribe(TranscriptionInput, TranscriberSink) = 0;
};
```

Shaped like `IProvider`/`IProviderCall`: callable from any thread, `done` exactly once unless cancelled, possibly before
`transcribe` returns. Nothing outside an implementation names a provider. `testing::FakeTranscriber` scripts answers and
records whether each call was cancelled.

### `makeQtTranscriber(config, owner)` (`include/morph/qt/qt_transcriber.hpp`)

Returns `std::expected<std::shared_ptr<ITranscriber>, std::string>`, as `makeQtChatProvider` does. The one place that
names a transcription provider; its one name is `openai-transcriptions`. A multipart `POST` to
`{baseUrl}/v1/audio/transcriptions` over `QNetworkAccessManager` on `owner`'s thread: parts `file` (the WAV,
`audio.wav`), `model`, `response_format=json`, and `language` when the input carries one; header
`Authorization: Bearer <key>`. It reads `text`. A non-2xx answer fails with the provider's `error.message` when it has
one, never with the request. `cancel` aborts the reply. The key appears in no log and no message.

### `ChatService`

- `ChatOptions::transcriber` (`shared_ptr<ITranscriber>`, empty by default). With none the service serves neither new
  kind and the probe's `dictation` is absent.
- **Accepting.** `handle` refuses at once, with a reason naming the bound, a request that: arrives with no connection
  (`cid == 0`, as `startTurn` refuses); has base64 text over `ceil(maxDictationBytes / 3) * 4`, checked before decoding;
  decodes to more than `maxDictationBytes`; is not valid base64; has a `mediaType` other than `audio/wav`; or arrives
  while the same connection has a transcription running (`busy`), or while the principal has
  `maxTranscriptionsPerPrincipal` (2) running across connections. Otherwise it records the transcription, replies
  `TranscribeAccepted{id}` and posts the call to the worker.
- **Language.** The request's `language`, else the configured `transcription.language`, else none. The service applies
  the default, so a transcriber sees one value.
- **Where it lives.** `Shared::transcriptions`, a map from id to `{principal, connection, status, text or error,
  call, deadline, finishedAt}`, guarded like the turns' table.
- **Ending, exactly once.** Each accepted transcription ends in exactly one of `done` or `failed`, recorded in the table
  and sent as the `transcription` event to its connection: the transcriber's answer; the deadline (`toolCallTimeout`),
  which cancels the call and fails with "the transcription did not answer in time"; `ui-chat-end`, which cancels and
  fails with "cancelled". A connection closing cancels the call and drops the entry, since no one can read it. The
  service's destruction cancels every call and sends nothing. An answer arriving after the entry ended is ignored. The
  `busy` slot and the principal's count free when the entry ends.
- **Nothing kept beyond the retention.** Neither audio nor text is journaled, logged or added to a conversation. The audio
  buffer is released when the call is started. The text reaches the assistant only inside a later `ui-chat-turn`, as
  text like any other.

### Configuration (`config.hpp`, `loadChatConfig`)

```json
"transcription": { "name": "openai-transcriptions", "baseUrl": "https://…", "model": "whisper-1",
                   "apiKeyEnv": "STT_KEY", "language": "en" }
```

`TranscriptionConfig{name, baseUrl, model, apiKey | apiKeyEnv, language?}`, optional as a whole, with the provider
block's rules: an unknown key is refused, `apiKey` and `apiKeyEnv` together are refused, a refusal never quotes the text.
`ChatLimits` gains `maxDictationSeconds` (60), `maxDictationBytes` (4 MiB), `maxTranscriptionsPerPrincipal` (2) and
`transcriptionRetention` (60 s, `transcriptionRetentionSeconds` in the file). 0 is refused for each.
`maxDictationBytes` above `(wire::kMaxEnvelopeBytes - 4096) * 3 / 4` (about 6 MiB) is refused: past that the transport,
not the service, would refuse the request, and the person would not learn which bound they crossed.

## Framework seam: host values and commands for one document

`HostValue` and `HostCommand` gain `documents`, a set of document ids; empty means every document, as today. A
`{"host": name}` expression or command in a document whose id is not in the set resolves as an unknown host name: a
loaded document that names it is refused as it is today for an unknown name, and nothing runs. The check belongs to the
session, which knows which document an expression belongs to; `HostCommand::run` still is not told the caller.

The panel's existing `chat.*` names keep `documents` empty, so nothing changes for them. The dictation names are
registered for `kChatPanelId` only. A custom screen's id carries the custom prefix and an application screen's id is the
application's, so neither can be the panel; the bundle refuses an application screen with the id `morph.chat`.

## Client

### `IDictationRecorder` (`include/morph/ui/chat/dictation.hpp`)

```cpp
struct DictationLimits { std::chrono::seconds maxSeconds; std::size_t maxBytes; };
class IDictationRecorder {
public:
    virtual ~IDictationRecorder() = default;
    [[nodiscard]] virtual std::optional<std::string> unavailable() const = 0;  // known now: why it cannot record
    virtual void start(DictationLimits limits) = 0;  // asks for permission when the platform does; async
    virtual void stop() = 0;                          // ends the clip; onClip follows
    std::function<void(std::expected<void, std::string>)> onStarted;  // recording began, or why not
    std::function<void(std::chrono::seconds)> onProgress;
    std::function<void(std::expected<std::string, std::string>)> onClip;  // WAV bytes, or why there are none
};
```

Permission is asynchronous on several platforms and in the browser, so `start` answers through `onStarted`.
`unavailable()` reports what is known without asking, such as no input device; it is not a promise that `start` will
succeed. The interface sits in the UI layer, so the binding is tested without Qt.

### `morph::qt::QtDictationRecorder`

In `morph_qt`, built when `Qt6::Multimedia` is found (`MORPH_ENABLE_DICTATION`, ON when it is). `QAudioSource` on the
owner thread at the device's preferred format; samples are converted to 16 kHz mono 16-bit (`resampleToSpeech`, a pure
function), and the clip stops at `maxSeconds` or when the WAV would pass `maxBytes`. `wavFromPcm16` (pure) writes the
header. Audio stays in memory and is never written to disk. At 16 kHz mono 16-bit, 60 s is 1,920,044 bytes, 2,560,060 in
base64.

### `ChatClient`

- `dictation()` answers the probe's limits, or nothing.
- `transcribe(wav, language, done)` sends `ui-chat-transcribe`, keeps the id, and calls `done` exactly once: with the
  text when the state reads `done`, with the error when it reads `failed`, when the exchange fails, on a reconnect (the
  old connection's close dropped the entry), and when `end` runs. A `transcription` event or a `resync` makes it read
  the state.
- `note(text)` adds a client-side entry of kind `note` to the active conversation's display. Notes sit outside a turn's
  range, so `show` keeps them when it redraws a turn; they are not sent to the server.

### `ChatPanelBinding` and the panel document

The binding takes an optional `IDictationRecorder*` and a `std::function<bool()> visible`. It registers, for the panel
document only:

| Host value / command | Type | Meaning |
|---|---|---|
| `chat.canDictate` | bool | `dictation()` is set, `unavailable()` is empty, the person is on a screen, and the panel is visible |
| `chat.dictation` | string: `idle`, `recording`, `waiting` | What the button shows |
| `chat.dictationSeconds` | int | Seconds recorded so far |
| `chat.dictated` | record `{seq: int, text: string}` | The latest transcript; `seq` grows by one per transcript |
| `chat.dictateStart`, `chat.dictateStop` | command | Start and stop. `dictateStart` does nothing unless `chat.canDictate` holds |

`refresh` stops a recording and drops its clip when the panel stops being visible or the person signs out.

The document adds the button, with its label from `if` on `chat.dictation` and the seconds through `format`, enabled
unless `waiting`, and a top-level `watch` on `{"host": "chat.dictated"}` that sets `draft` to the trimmed concatenation
described above. A watch on a host value does not fire on its first evaluation and fires on each change of the record,
so each new `seq` appends once; the seconds tick changes a different value and does not fire it.

## Failure modes

| What happens | What the person sees |
|---|---|
| No input device | No button |
| Permission denied | A note, from `onStarted` |
| Server offers no dictation | No button |
| Panel hidden while recording | Recording stops, the clip is dropped |
| Clip at a bound | The client stops recording at the bound and sends what it has |
| Refused by the server | A note naming the bound or `busy` |
| Provider error, timeout | A note with the provider's message or the timeout; the input is unchanged |
| Connection lost or sign-out while waiting | A note; the state returns to `idle` |

## Testing

Each test that shows something does not happen sits beside a case showing it does, so a feature that did nothing fails.

- **Service** (`FakeTranscriber`): a round trip through accept, event and state read; the gate refuses an unauthorised
  session; invalid base64 over the bound gets the bound's refusal, not a decode error; each other refusal; `busy` on a
  second call from one connection while a call from another connection of the same principal is accepted, and the third
  is refused at the per-principal cap; the deadline fails the entry and `FakeTranscriber` saw `cancel()`; `ui-chat-end`
  likewise; a close cancels and drops the entry; an answer after the entry ended changes nothing; an answer after the
  service's destruction does not crash; another principal reading the id gets "no such transcription"; the probe carries
  `dictation` with a transcriber and not without; after a transcription, the next turn's conversation holds only what
  the turn sent.
- **Config:** the block read and applied; unknown key, both keys, zero limits, `maxDictationBytes` over the envelope cap.
- **Qt transcriber:** against a local `QTcpServer` speaking enough HTTP: the multipart parts, the auth header,
  `language` present and absent, a 4xx with an error body, a malformed body, `cancel` aborting. A live test against a
  real endpoint runs only when `MORPH_LIVE_STT_KEY` is set.
- **Audio:** `wavFromPcm16` header bytes; `resampleToSpeech` from 48 kHz stereo to 16 kHz mono on a known signal; the
  limits stop a clip, with an injected clock.
- **Host seam:** a document that is not the panel naming `chat.dictateStart` is refused at load and cannot run it, while
  the panel can.
- **Binding and panel** (recording backend, fake recorder, fake client): button states through a dictation; the
  transcript appended with the spacing rules (empty input, input ending in text, input ending in a space); two
  transcripts appended in order; the seconds tick does not append; a failure note survives a turn redraw; no button
  without an offer, and the button with one; hiding the panel stops a recording.
- **Mutation checks**, each of which must fail its tests: the bound check, `busy`, the per-principal cap, cancel on the
  deadline and on `ui-chat-end`, the append watch, the probe's `dictation` field, the document scoping of host names.

## Where it lands

One PR from `feat/chat-dictation`, after the current issue batch merges: this design, the `chat.md` section, the host
vocabulary seam in its spec, the code and the tests.
