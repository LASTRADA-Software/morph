# Dictation in the chat

**Status:** design, approved in conversation on 2026-11-19. The spec section lands in `docs/spec/ui/chat.md` with the
code.

## Goal

A person can speak a message to the chat instead of typing it. The words land in the chat's input, where the person
reads them, corrects them and sends them as typed text. Speech becomes text **on the server**, through a configured
speech-to-text **provider**. morph embeds no recognition engine and no model.

Out of scope: spoken replies, wake words, streaming partial transcripts, and voice commands that act on a screen without
the assistant. Each can be built on top of this later without changing it.

## Why this shape

- **Server-side, through a provider.** Qt has no speech recognition. Running an engine in the client (whisper.cpp) adds a
  native dependency and 75 MB to 1.5 GB of model to every client, cannot run in the browser build, and is slow on modest
  machines. Operating-system dictation differs per platform, and Linux has none. A provider behind the server fits how the
  chat already works: the model provider is server-side, its key never reaches a client, and every client gets the feature,
  the browser build included.
- **One wire format.** `POST {baseUrl}/v1/audio/transcriptions` (multipart: `file`, `model`, optional `language`)
  is spoken by hosted Whisper, by self-hosted whisper.cpp and faster-whisper servers, and by most gateways. A deployment
  that must keep audio on its own network points `baseUrl` at a server of its own.
- **Insert, never send.** A misheard word sent to an assistant whose tools change data costs more than one more press of
  Send. The transcript is text the person has not yet said; it becomes a message only when they send it.

## The person's view

- A microphone button beside **Send**. It shows only when the server offers dictation and this client can record.
- Pressing it starts recording; the button becomes a stop button showing the seconds recorded. Pressing it again stops.
  Recording also stops by itself at `maxDictationSeconds` (60).
- Then the button shows that it is waiting, and the transcript is **appended** to the input, after a space when the
  input is not empty. The panel document has no cursor position to insert at, and appending matches what a person
  dictating a whole message expects.
- A failure (no microphone, permission denied, the provider refused or timed out) shows as a line in the panel, in the
  role a tool failure uses, and leaves the input as it was. Nothing fails silently.

## Components

### Server: `ITranscriber` (`include/morph/ui/chat/transcriber.hpp`)

```cpp
struct TranscribeRequest { std::string audio; std::string mediaType; std::optional<std::string> language; };
struct TranscriberSink { std::function<void(std::expected<std::string, std::string>)> done; };  // once, any thread
class ITranscriberCall { public: virtual void cancel() = 0; };
class ITranscriber {
public:
    [[nodiscard]] virtual std::shared_ptr<ITranscriberCall> transcribe(TranscribeRequest, TranscriberSink) = 0;
};
```

Shaped like `IProvider`/`IProviderCall`: callable from any thread, `done` exactly once unless cancelled. Nothing outside
an implementation names a provider. `testing::FakeTranscriber` scripts answers for tests.

### Server: `makeQtTranscriber(config, owner)` (`include/morph/qt/qt_transcriber.hpp`)

The one place that names a transcription provider. Its one name is `openai-transcriptions`: a multipart `POST` to
`{baseUrl}/v1/audio/transcriptions` over `QNetworkAccessManager` on `owner`'s thread, `Authorization: Bearer <key>`,
`response_format=json`, reading `text`. A non-2xx answer becomes the failure, with the provider's `error.message` when it
has one and never the request. The key appears in no log and no message.

### Server: `ChatService`

- `ChatOptions::transcriber` (a `shared_ptr<ITranscriber>`, empty by default). With none, the service does not serve
  `ui-chat-transcribe`, and `TurnState::dictation` is false.
- **`ui-chat-transcribe`**, body `TranscribeBody{audio: base64, mediaType: "audio/wav", language?}`, reply
  `TranscribeReply{text}`. It passes `authorize(session, "ui", "chat")` like every chat kind. `handle` checks the bounds
  and replies to a refusal at once; the call itself is posted to the worker, and the reply is sent when the transcriber
  answers, so the server strand never waits on it.
- **Bounds, checked before decoding:** the base64 text is at most `ceil(maxDictationBytes / 3) * 4`; the decoded audio is
  at most `maxDictationBytes`; `mediaType` is `audio/wav`. A refusal says which bound.
- **Deadline:** `toolCallTimeout`. A late answer is ignored.
- **One at a time per connection.** A second `ui-chat-transcribe` while one runs on the same connection is refused
  (`busy`); a person has one microphone.
- **Cancelling:** the connection closing, `ui-chat-end` and the service's destruction cancel a running call. There is no
  separate cancel kind: the client drops a reply it no longer wants.
- **Nothing kept.** Neither audio nor text is stored, journaled or added to a conversation. The transcript reaches the
  assistant only as part of a later `ui-chat-turn`, which is text like any other.
- `StateRequest` with no turn (the probe) answers `TurnState{dictation: bool}` beside what it answers today, so a client
  learns in one round trip whether to offer the button. The field is optional on the wire: an older peer ignores it.

### Configuration (`config.hpp`, `loadChatConfig`)

```json
"transcription": { "name": "openai-transcriptions", "baseUrl": "https://…", "model": "whisper-1",
                   "apiKeyEnv": "STT_KEY", "language": "en" }
```

`TranscriptionConfig{name, baseUrl, model, apiKey | apiKeyEnv, language?}`, optional as a whole. The rules are the
provider block's: an unknown key is refused, `apiKey` and `apiKeyEnv` together are refused, a refusal never quotes the
text. `language` is the default when the client sends none. `ChatLimits` gains `maxDictationSeconds` (60) and
`maxDictationBytes` (4 MiB), each readable from `limits`, and 0 is refused for both.

### Client: `IDictationRecorder` (`include/morph/ui/chat/dictation.hpp`) and `morph::qt::QtDictationRecorder`

```cpp
class IDictationRecorder {
public:
    [[nodiscard]] virtual std::optional<std::string> unavailable() const = 0;  // why it cannot record, or nothing
    virtual std::expected<void, std::string> start(std::chrono::seconds limit) = 0;
    virtual void stop() = 0;                                // ends the clip; `onClip` follows
    std::function<void(std::expected<std::string, std::string>)> onClip;  // WAV bytes, or why there are none
    std::function<void(std::chrono::seconds)> onProgress;
};
```

The interface sits in the UI layer so the panel binding is testable without Qt. `QtDictationRecorder` (in `morph_qt`,
built only when `Qt6::Multimedia` is found; `MORPH_ENABLE_DICTATION` defaults to ON when it is) uses `QAudioSource` at
16 kHz, mono, 16-bit, and wraps the samples as WAV in memory. WAV avoids codecs that differ by platform, and every
provider of this format accepts it. Sixty seconds is about 1.9 MB, 2.6 MB in base64, inside the 8 MiB envelope bound.
Audio is held in memory only, never written to disk.

### Client: `ChatClient::transcribe(wav, done)`

Sends `ui-chat-transcribe` under the current session and calls back once with the text or the failure, including when
the exchange fails. `ChatClient::dictationOffered()` reports what the probe answered.

### Client: `ChatPanelBinding` and the panel document

The binding takes an optional `IDictationRecorder*`. It adds:

| Host value / command | Type | Meaning |
|---|---|---|
| `chat.canDictate` | bool | The server offers dictation, the recorder is available, and the person is on a screen |
| `chat.dictation` | `"idle"` \| `"recording"` \| `"waiting"` | What the button shows |
| `chat.dictationSeconds` | int | Seconds recorded so far |
| `chat.dictated` | record `{seq, text}` | The latest transcript; `seq` grows by one per transcript |
| `chat.dictateStart`, `chat.dictateStop` | command, `safeForUntrusted` | Start and stop recording; they do only what the person could do by pressing the button |

The panel document gains a microphone button bound to these, and a `watch` on `chat.dictated` that appends its `text`
to `draft`. The insertion stays in the document, so every frontend that has a recorder gets the same behaviour, and
the binding never reaches into the document's state.

A failure appends a client-side entry to the conversation's display (role `muted`, as a tool line is drawn); it is not
sent to the server.

## Failure modes

| What happens | What the person sees |
|---|---|
| No microphone, or permission denied | No button where `unavailable()` answers at startup; otherwise the failure line when `start` fails |
| Server offers no dictation | No button |
| Clip over a bound | The client stops at `maxDictationSeconds`; the server refuses anything over `maxDictationBytes` and the failure line names it |
| Provider error or timeout | The failure line with the provider's message or "the transcription did not answer in time"; the input is unchanged |
| Connection lost while waiting | The failure line; the state returns to idle |
| Sign-out while recording | Recording stops and the clip is dropped |

## Testing

- **Service** (`FakeTranscriber`): a round trip; the gate refuses an unauthorised session; each bound refuses before
  decoding; `busy` on a second call; the deadline; a close cancels the call; the probe reports `dictation` only with a
  transcriber; nothing reaches the conversation.
- **Config:** the block is read; the unknown-key, both-keys and zero-limit refusals.
- **Qt transcriber:** against a local `QTcpServer` speaking just enough HTTP: the multipart layout, the auth header,
  `language`, a 4xx with an error body, a malformed body. A live test against a real endpoint runs only when
  `MORPH_LIVE_STT_KEY` is set.
- **Recorder:** WAV header and sample format, from a fake audio device fed known samples; the limit stops the clip.
- **Binding and panel** (recording backend, a fake recorder and a fake client): button states through a whole dictation;
  the transcript appended to `draft` with a space; a failure line; the button hidden without an offer.
- Mutation checks: the bound check, the append watch, and the `dictation` probe field are each mutated out and their
  tests must fail.

## Where it lands

One PR from `feat/chat-dictation`, after the current issue batch merges: this design, the `chat.md` section, the code and
the tests.
