# Chat Dictation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A person can speak a message to the chat instead of typing it. The words land in the chat's input, where the person reads them, corrects them and sends them as typed text.

**Architecture:** Speech becomes text **on the server**, through a configured speech-to-text **provider**. morph embeds no recognition engine and no model.

**Tech Stack:** C++23; a multipart `POST` to `{baseUrl}/v1/audio/transcriptions` over `QNetworkAccessManager`; `QAudioSource` in `morph_qt`, built when `Qt6::Multimedia` is found (`MORPH_ENABLE_DICTATION`); Catch2.

**Global Constraints:**
- Speech to text on the server, through a provider.
- One wire format: `POST {baseUrl}/v1/audio/transcriptions`.
- The transcript is appended to the input, never sent.
- Accept now, result later, as a turn does.
- The dictation host values and commands reach the panel document only.
- The client records 16 kHz mono 16-bit, resampling when the device offers another format.
- Recording is refused while the panel is hidden.
- Nothing fails silently.
- Neither audio nor text is journaled, logged or added to a conversation.
- The key appears in no log and no message.

---

## Design corrections

1. **A host name outside its documents is absent, not refused at load.** The design says a document naming an
   out-of-scope name "is refused as it is today for an unknown name". Nothing refuses an unknown host name at load
   today: a value nobody registered reads as `null` (`Session::client`, `src/ui/interpreter.cpp:836-845`), a command
   nobody registered is reported as missing when it runs and does not run (`ScreenCore::runHostCommand`,
   `src/ui/interpreter.cpp:4110-4125`), and both specs say so ("Absent is safe",
   `docs/superpowers/specs/2026-10-08-app-hosting-design.md` §14; "Running a command" step 1,
   `docs/spec/ui/host_vocabulary.md`). The load path never sees the client's vocabulary (`loadDocumentText` takes only a
   catalog). **Planned instead:** an out-of-scope name resolves exactly as an unknown one does today: it reads `null`,
   `{"can": "host:name"}` is false, and running it reports `host command `name` is missing` and runs nothing. The
   design's security goal holds (a screen cannot start the microphone or read a transcript). The host-seam test checks
   those three outcomes instead of a load refusal (Task 1, Task 9).
2. **The `transcription` event names its kind.** The design gives the event's body as a bare `TranscriptionState`. The
   `chat` topic's payload is read as a `TurnEvent` by every client (`ChatClient::deliverEvent`,
   `include/morph/ui/chat/client.hpp:146-178`), and `readMessage` ignores unknown keys
   (`include/morph/ui/bundle.hpp:435-458`), so a bare state cannot be told from a turn event except by fields being
   absent. **Planned instead:** the payload is `TranscriptionEvent{kind: "transcription", state: TranscriptionState}`.
   An older client reads it as a `TurnEvent` with `turnId` 0 and drops it, since its running turn's id is never 0 and
   `seq` 0 is never newer than what it holds (Task 2 tests this).

## Decisions the design leaves open, taken here

- **A transcriber needs a provider.** `ChatService` with a transcriber and no provider throws `std::invalid_argument`,
  as a provider without a worker does today (`src/ui/chat_service.cpp:1046-1049`): dictation fills the chat's input,
  and with no provider there is no chat. A dictation limit of 0, or `maxDictationBytes` above `kMaxDictationBytes`, is
  refused the same way, so a programmatic `ChatLimits` gets the checks the file reader applies.
- **Retention is swept when the table is touched.** Finished entries older than `transcriptionRetention` are dropped at
  each accept and each query, with no timer. A query after the retention therefore always reads "no such
  transcription".
- **`ChatClient::end` forgets the dictation offer.** The binding has no other sign-out signal: the shell's sign-out calls
  `end` first (`src/client/shell.cpp:489-500`). With the offer gone `refresh` stops a recording and drops its clip, as
  the design asks for a sign-out. The next probe brings the offer back.
- **An empty transcript is a note.** A transcript that is only whitespace adds the note "Nothing was heard in the
  recording" and leaves the input as it was, so nothing fails silently.
- **`makeQtTranscriber`** defaults an empty `baseUrl` to `https://api.openai.com`, and refuses a missing key or model,
  as `makeQtChatProvider` does.
- **`maxDictationSeconds`** is a `std::size_t`, like the other counts in `ChatLimits`; `transcriptionRetention` is a
  `std::chrono::seconds`, read from `transcriptionRetentionSeconds`.
- **The application screen id `morph.chat`** is refused by `UiRegistry::checkNewEntry`, which covers `addScreen` and
  `addEntry`. `src/ui/service.cpp` names the id in a local constant rather than including the chat headers; the test
  compares the refusal against `chat::kChatPanelId`, so the two cannot drift apart unnoticed.
- **The Dictate button's icon** is the theme icon name `chat.dictate`; a theme without it draws the label alone, which is
  what a button does for a name its set lacks (`docs/spec/ui/theme.md`, "Icons").

## Conventions for every task

- Work in `/home/yaraslau/repo/morph-dictation` on `feat/chat-dictation`. Every command below runs from that directory.
- Configure once, before Task 1:
  ```bash
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ \
        -DMORPH_BUILD_TESTS=ON -DMORPH_BUILD_EXAMPLES=ON -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON
  ```
- Builds take the shared slot:
  `flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target <target>`.
  Run builds in the foreground. A build that times out is rerun, never backgrounded.
- New Catch2 tests run through `ctest --test-dir build -R '<regex>' --output-on-failure`. Qt tests run with
  `QT_QPA_PLATFORM=offscreen`. Test names are ASCII and never start with `--`.
- A mutation check copies the file to
  `/tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/<name>.bak`, applies the
  mutation, rebuilds, expects the named test to FAIL, then restores with `cp` from the backup **and `touch`es the file**,
  since a restored copy keeps its old mtime and ninja would keep the mutant binary.
- Public headers keep complete `@param`/`@tparam`/`@return`. Comments and docs say what the code does now and why: no
  history, no issue numbers. Never use `slots`, `signals` or `emit` as identifiers in `include/morph`.
- Every commit message ends with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01No1NznwgG54dkVGoxXz2Z4
  ```

## File structure

| File | Status | Responsibility |
|---|---|---|
| `include/morph/ui/host_vocabulary.hpp` | modify | `HostCommand::documents`, `HostValue::documents`; `publish` hands the session each limited name's documents |
| `include/morph/ui/interpreter.hpp` | modify | `HostScopes`; `Session::setHostScopes`, `Session::hostNameIn`; `base64Encode` declared beside `base64Decode` |
| `src/ui/interpreter.cpp` | modify | `ScopeEnv::client`, `ScopeEnv::control` and `ScreenCore::runHostCommand` treat an out-of-scope name as absent; `base64Encode` leaves the anonymous namespace |
| `src/ui/service.cpp` | modify | `UiRegistry::checkNewEntry` refuses the screen id `morph.chat` |
| `include/morph/ui/chat/protocol.hpp` | modify | `ui-chat-transcribe`, `ui-chat-transcription`, the `transcription` event, `TurnState::dictation`, their readers |
| `include/morph/ui/chat/conversations.hpp` | modify | `ChatLimits` dictation bounds; `kMaxDictationBytes` |
| `include/morph/ui/chat/config.hpp` | modify | `TranscriptionConfig`, `ChatConfig::transcription`, the file's `transcription` block and dictation limits |
| `include/morph/ui/chat/transcriber.hpp` | create | `TranscriptionInput`, `TranscriberSink`, `ITranscriberCall`, `ITranscriber` |
| `include/morph/ui/testing/fake_transcriber.hpp` | create | `testing::FakeTranscriber`, a scripted `ITranscriber` that records inputs and cancels |
| `include/morph/ui/chat/service.hpp` | modify | `ChatOptions::transcriber`, `ChatOptions::transcriptionLanguage`; `ChatService::Transcription` |
| `src/ui/chat_service.cpp` | modify | The transcription table: accept, refusals, worker call, deadline, endings, retention, query, probe offer |
| `include/morph/ui/chat/dictation.hpp` | create | `DictationLimits`, `IDictationRecorder`, `wavFromPcm16`, `SpeechResampler`, `resampleToSpeech`, `DictationClip` |
| `include/morph/qt/qt_transcriber.hpp` | create | `makeQtTranscriber` |
| `src/qt/qt_transcriber.cpp` | create | The `openai-transcriptions` multipart call over `QNetworkAccessManager` |
| `include/morph/qt/qt_dictation_recorder.hpp` | create | `QtDictationRecorder` |
| `src/qt/qt_dictation_recorder.cpp` | create | `QAudioSource` capture, permission, resampling, clip bounds |
| `include/morph/ui/chat/client.hpp` | modify | `ChatClient::dictation`, `transcribe`, `note`; transcription events; notes kept through a turn redraw |
| `include/morph/ui/testing/fake_dictation_recorder.hpp` | create | `testing::FakeDictationRecorder`, driven by hand |
| `include/morph/ui/chat/panel.hpp` | modify | The Dictate button and the append `watch` in `kChatPanelDocument`; `ChatPanelBinding` dictation names, state and recorder |
| `include/morph/ui/shell_frame.hpp`, `src/ui/shell_frame.cpp` | modify | `ShellFrame::chatVisible` |
| `include/morph/client/shell.hpp` | modify | `ShellOptions::dictation` |
| `src/client/shell.cpp` | modify | Makes the recorder, hands it and the panel's visibility to the binding, refreshes on compact, sign-in state and frame rebuild |
| `examples/client/main.cpp`, `examples/client/CMakeLists.txt` | modify | The Qt Quick client hands the shell a `QtDictationRecorder` |
| `examples/lims/src/server/main.cpp` | modify | The lims server reads `transcription` and builds the transcriber |
| `CMakeLists.txt` | modify | New headers in `morph`/`morph_qt` file sets; `qt_transcriber.cpp`; `MORPH_ENABLE_DICTATION` and `Qt6::Multimedia` |
| `tests/CMakeLists.txt` | modify | `test_chat_transcription.cpp`, `test_chat_dictation_audio.cpp` |
| `tests/qt/CMakeLists.txt` | modify | `test_qt_transcriber.cpp`; `test_qt_dictation_recorder.cpp` under `MORPH_ENABLE_DICTATION` |
| `tests/qt/fake_http_server.hpp` | create | The local `QTcpServer` speaking enough HTTP, shared by the Qt provider and transcriber tests |
| `tests/qt/test_qt_chat_provider.cpp` | modify | Uses `fake_http_server.hpp` instead of its own copy |
| `tests/test_ui_host_vocabulary.cpp`, `tests/test_ui_service.cpp` | modify | Document scope; the reserved panel id |
| `tests/test_chat_protocol.cpp`, `tests/test_chat_config.cpp` | modify | Wire and configuration |
| `tests/test_chat_transcription.cpp` | create | The service's transcription table |
| `tests/test_chat_dictation_audio.cpp` | create | WAV, resampling, clip bounds |
| `tests/qt/test_qt_transcriber.cpp`, `tests/qt/test_qt_dictation_recorder.cpp` | create | The Qt transcriber and recorder |
| `tests/test_chat_client.cpp`, `tests/test_chat_panel.cpp` | modify | Client and binding |
| `tests/qt/test_qt_keywords.cpp` | modify | Includes the new chat headers after Qt's |
| `tests/client/test_custom_screens.cpp` | modify | The shell end to end |
| `docs/spec/ui/host_vocabulary.md` | modify | "Document scope" |
| `docs/spec/ui/chat.md` | modify | Protocol, configuration, dictation server, Qt transcriber, client, panel |
| `docs/spec/client/client.md` | modify | The shell's recorder and the panel's visibility |

---
## Task 1: Host names limited to the documents they are registered for

**Files:**
- Modify: `include/morph/ui/host_vocabulary.hpp:36-60` (the two structs), `:98-111` (`add(HostValue)`), `:237-261` (`publish`, members)
- Modify: `include/morph/ui/interpreter.hpp:575-581` (after `HostCommands`), `:596-603` (`Session` constructor), `:726-738` (host setters), `:1013-1016` (members)
- Modify: `src/ui/interpreter.cpp:828-831` (after `Session::setHostCommands`), `:2114-2125` (`ScopeEnv::control`), `:2167` (`ScopeEnv::client`), `:4110-4112` (`ScreenCore::runHostCommand`)
- Modify: `src/ui/service.cpp:34-36` (anonymous namespace), `:449-457` (`UiRegistry::checkNewEntry`)
- Test: `tests/test_ui_host_vocabulary.cpp` (new case at the end), `tests/test_ui_service.cpp:171-200` (new section)
- Docs: `docs/spec/ui/host_vocabulary.md`

**Interfaces:**
- Consumes: `Session::setHostValues`, `Session::setHostCommands`, `ScreenCore::doc()`, `Document::id`.
- Produces:
  - `std::set<std::string, std::less<>> HostCommand::documents;` and `std::set<std::string, std::less<>> HostValue::documents;` (empty: every document)
  - `using HostScopes = std::map<std::string, std::set<std::string, std::less<>>, std::less<>>;` in `morph::ui`
  - `void Session::setHostScopes(HostScopes scopes);`
  - `[[nodiscard]] bool Session::hostNameIn(std::string_view name, std::string_view document) const;`
  - `UiRegistry::addScreen`/`addEntry` refuse the id `morph.chat` with "the id `morph.chat` is the chat panel's".

- [ ] **Step 1: Write the failing tests.**

Append to `tests/test_ui_host_vocabulary.cpp`:

```cpp
namespace {

/// A screen with id @p id that reads `panel.secret`, asks whether it can run `panel.start`, and runs it.
std::string scopedScreen(std::string const& id) {
    return R"({"vocab":"ui/1","id":")" + id + R"(","view":{"kind":"column","children":[)" +
           R"({"kind":"text","testId":"secret_)" + id +
           R"(","text":{"fn":"coalesce","args":[{"host":"panel.secret"},"null"]}},)" +
           R"({"kind":"text","testId":"can_)" + id +
           R"(","text":{"fn":"format","args":["{0}",{"can":"host:panel.start"}]}},)" +
           R"({"kind":"button","testId":"start_)" + id + R"(","label":"start","onClick":{"host":"panel.start"}}]}})";
}

}  // namespace

// Mutation: drop the document check in ScopeEnv::client, ScopeEnv::control or ScreenCore::runHostCommand.
TEST_CASE("host vocabulary: a name limited to one document is absent from every other", "[ui][host]") {
    Harness h;
    HostVocabulary vocabulary;
    std::vector<std::string> ran;
    vocabulary.add(HostCommand{.name = "panel.start",
                               .version = 1,
                               .args = {},
                               .safeForUntrusted = true,
                               .run = [&ran](ValueRecord const& /*args*/) { ran.emplace_back("start"); },
                               .documents = {"panel"}});
    vocabulary.add(HostValue{
        .name = "panel.secret", .version = 1, .type = "string", .initial = Value{"heard"}, .documents = {"panel"}});
    vocabulary.connect(h.session);
    auto const panel = h.mount(scopedScreen("panel"));
    auto const other = h.mount(scopedScreen("other"));

    CHECK(h.text("secret_panel") == "heard");
    CHECK(h.text("secret_other") == "null");
    CHECK(h.text("can_panel") == "true");
    CHECK(h.text("can_other") == "false");

    h.backend.click(h.byTestId("Button", "start_other"));
    h.settle();
    CHECK(ran.empty());
    CHECK(std::ranges::any_of(h.diagnostics, [](auto const& refusal) {
        return refusal.message().contains("host command `panel.start` is missing");
    }));

    h.backend.click(h.byTestId("Button", "start_panel"));
    h.settle();
    CHECK(ran == std::vector<std::string>{"start"});
}
```

In `tests/test_ui_service.cpp`, add `#include <morph/ui/chat/panel.hpp>` to the includes, and add this section inside
`TEST_CASE("UiRegistry::addScreen refuses what it cannot serve", "[ui-service][registry]")`, after the section
`"an id registered twice"`:

```cpp
    SECTION("the chat panel's id") {
        std::string const panel = R"({"vocab":"ui/1","id":"morph.chat","view":{"kind":"text","text":"x"}})";
        REQUIRE(refused(registry.addScreen(entry(std::string{morph::ui::chat::kChatPanelId}), panel)) ==
                "the id `morph.chat` is the chat panel's");
        CatalogEntry host = entry(std::string{morph::ui::chat::kChatPanelId});
        host.host = "Chat";
        REQUIRE(refused(registry.addEntry(host)) == "the id `morph.chat` is the chat panel's");
    }
```

- [ ] **Step 2: Run them and expect FAIL.**

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_tests
```

Expected: the build fails with `error: no member named 'documents'` (designated initializer of `morph::ui::HostCommand`).

- [ ] **Step 3: Implement the seam.**

In `include/morph/ui/host_vocabulary.hpp`, add to `HostCommand` after `run`:

```cpp
    /// @brief The ids of the documents that may name it; empty for every document. In any other document
    ///        `{"host": name}` finds no such command: it is reported missing and nothing runs, and
    ///        `{"can": "host:name"}` is false.
    std::set<std::string, std::less<>> documents;
```

and to `HostValue` after `initial`:

```cpp
    /// @brief The ids of the documents that may read it; empty for every document. In any other document it reads as
    ///        `null`, as a value nobody registered does.
    std::set<std::string, std::less<>> documents;
```

In `add(HostValue value)`, insert before `_values.insert_or_assign(...)`:

```cpp
        _valueDocuments.insert_or_assign(value.name, value.documents);
```

Replace `publish()` with:

```cpp
    /// Hands the session the values, the documents each limited name belongs to, and the commands runnable now.
    void publish() {
        if (_session == nullptr) {
            return;
        }
        std::set<std::string, std::less<>> available;
        HostScopes scopes;
        for (auto const& [name, registered] : _commands) {
            if (registered.command.safeForUntrusted || _verified) {
                available.insert(name);
            }
            if (!registered.command.documents.empty()) {
                scopes[name].insert(registered.command.documents.begin(), registered.command.documents.end());
            }
        }
        for (auto const& [name, documents] : _valueDocuments) {
            if (!documents.empty()) {
                scopes[name].insert(documents.begin(), documents.end());
            }
        }
        _session->runtime().batch([&] {
            _session->setHostValues(_values);
            _session->setHostScopes(std::move(scopes));
            _session->setHostCommands(
                HostCommands{.available = std::move(available),
                             .run = [this](std::string const& name, Value const& args) { return run(name, args); }});
        });
    }
```

and add the member after `_valueVersions`:

```cpp
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> _valueDocuments;
```

In `include/morph/ui/interpreter.hpp`, after `struct HostCommands { ... };`:

```cpp
/// @brief The host names limited to some documents, each with the ids of the documents that may name it. A name not
///        listed is in scope for every document.
using HostScopes = std::map<std::string, std::set<std::string, std::less<>>, std::less<>>;
```

In the `Session` constructor's initializer list, after `_hostCommandNames{runtime},` add `_hostScopes{runtime},`. After
`hostCommandRunner()` add:

```cpp
    /// @brief Limits host names to documents: a name listed is in scope only for the document ids listed with it, and
    ///        a name not listed is in scope everywhere. Out of scope, a value reads as `null`, `{"can": "host:name"}`
    ///        is false and a command is missing, exactly as for a name nobody registered.
    /// @param scopes The names and their documents.
    void setHostScopes(HostScopes scopes) { _hostScopes.set(std::move(scopes)); }

    /// @brief Whether a document may name a host value or command. A tracked read.
    /// @param name     The host name.
    /// @param document The document's id.
    /// @return False when the name is limited to other documents.
    [[nodiscard]] bool hostNameIn(std::string_view name, std::string_view document) const;
```

and the member after `_hostCommandNames`:

```cpp
    reactive::Signal<HostScopes> _hostScopes;
```

In `src/ui/interpreter.cpp`, after `Session::setHostCommands`:

```cpp
bool Session::hostNameIn(std::string_view name, std::string_view document) const {
    HostScopes const& scopes = _hostScopes.get();
    auto const found = scopes.find(name);
    return found == scopes.end() || found->second.contains(document);
}
```

Replace `ScopeEnv::client`:

```cpp
Value ScopeEnv::client(ExprKind form, std::string_view name) const {
    Session const& session = _scope->core.session;
    // A host value limited to other documents reads as one nobody registered.
    if (form == ExprKind::Host && !session.hostNameIn(name, _scope->core.doc().id)) {
        return Value{};
    }
    return session.client(form, name);
}
```

In `ScopeEnv::control`, make the `Can` branch start with:

```cpp
    if (form == ExprKind::Can) {
        static constexpr std::string_view kHostPrefix = "host:";
        if (name.starts_with(kHostPrefix) &&
            !session.hostNameIn(name.substr(kHostPrefix.size()), _scope->core.doc().id)) {
            return Value{false};
        }
```

(the existing query, mutation and `session.can(name)` lines follow unchanged). In `ScreenCore::runHostCommand`, make the
first statements:

```cpp
void ScreenCore::runHostCommand(Command const& command, CommandCtx const& ctx, Done const& done) {
    // A command limited to other documents is one this document cannot see: missing, and nothing runs.
    if (!session.hostNameIn(command.target, doc().id)) {
        report(command.path, "host command `" + command.target + "` is missing");
        done(false);
        return;
    }
    Value const args = evalIn(*ctx.scope, ctx.bindings.get(), command.value);
```

In `src/ui/service.cpp`, inside the anonymous namespace after `using detail::DocumentActions;`:

```cpp
/// @brief The id the client mounts its chat panel under. The panel reads host values no other document may, so no
///        screen of an application may take the id.
constexpr std::string_view kChatPanelScreen = "morph.chat";
```

and in `UiRegistry::checkNewEntry`, after the `checkEntry` refusal:

```cpp
    if (entry.id == kChatPanelScreen) {
        return Refusal{.document = entry.id, .path = {}, .reason = "the id `morph.chat` is the chat panel's"};
    }
```

- [ ] **Step 4: Run them and expect PASS.**

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_tests
ctest --test-dir build -R 'host vocabulary|UiRegistry::addScreen refuses' --output-on-failure
```

Expected: all pass, including the existing host vocabulary cases.

- [ ] **Step 5: Mutation check (document scoping).**

```bash
cp src/ui/interpreter.cpp /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/interpreter.cpp.bak
```

Delete the `if (!session.hostNameIn(command.target, doc().id)) { ... }` block in `ScreenCore::runHostCommand`, rebuild
`morph_tests`, run `ctest --test-dir build -R 'name limited to one document' --output-on-failure`. Expected FAIL at
`CHECK( ran.empty() )`. Restore:

```bash
cp /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/interpreter.cpp.bak src/ui/interpreter.cpp && touch src/ui/interpreter.cpp
```

Repeat with the `form == ExprKind::Host` check in `ScopeEnv::client` deleted: expected FAIL at
`CHECK( h.text("secret_other") == "null" )`. Restore the same way and rebuild.

- [ ] **Step 6: Update the spec.**

In `docs/spec/ui/host_vocabulary.md`, add `- [Document scope](#document-scope)` to the contents after `[`can`](#can)`,
and this section after `## \`can\``:

```markdown
## Document scope

`HostCommand::documents` and `HostValue::documents` list the ids of the documents that may name the command or the
value; empty, the default, means every document. In any other document the name is absent, exactly as a name nobody
registered: a value reads as `null`, `{"can": "host:name"}` is false, and a command is reported as
`host command `name` is missing` and does not run. The session decides, since it knows which document an expression
belongs to (`Session::setHostScopes`, `Session::hostNameIn`); a command's `run` is still not told who called it.

A document id is not a credential the server can forge into a client: the chat panel's id, `morph.chat`, is refused for an
application's screen by `UiRegistry`, a custom screen's id carries the `custom:` prefix, and an assistant's draft takes
the id of the screen it edits. So a name limited to the panel reaches the client's own panel document and nothing else.
```

and this row to the decisions table:

```markdown
| A value or command for one document | `documents` on the registration, checked by the session per expression | Every screen mounts in one session; without it, a vocabulary is session-wide and any screen could run a command meant for one document. |
```

- [ ] **Step 7: Commit.**

```bash
git add include/morph/ui/host_vocabulary.hpp include/morph/ui/interpreter.hpp src/ui/interpreter.cpp src/ui/service.cpp \
        tests/test_ui_host_vocabulary.cpp tests/test_ui_service.cpp docs/spec/ui/host_vocabulary.md
git commit -m "$(cat <<'MSG'
Limit a host value or command to the documents it is registered for

A name outside its documents is absent there, as an unregistered one is, and no
application screen may take the chat panel's id.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznwgG54dkVGoxXz2Z4
MSG
)"
```

---

## Task 2: The dictation wire, its bounds and its configuration

**Files:**
- Modify: `include/morph/ui/chat/protocol.hpp:19-28` (kinds), `:104-126` (`TurnState`), `:190-206` (readers, enum names)
- Modify: `include/morph/ui/chat/conversations.hpp:15` (include), `:22-55` (`ChatLimits`)
- Modify: `include/morph/ui/chat/config.hpp:27-49` (config structs), `:56-86` (file structs), `:108-179` (`parseChatConfig`)
- Test: `tests/test_chat_protocol.cpp` (two cases at the end), `tests/test_chat_config.cpp` (four cases at the end)
- Docs: `docs/spec/ui/chat.md` ("Protocol", "Configuration")

**Interfaces:**
- Consumes: `ui::detail::readMessage`, `ui::detail::writeMessage`, `wire::kMaxEnvelopeBytes`.
- Produces (namespace `morph::ui::chat`):
  - `inline constexpr std::string_view kTranscribeKind = "ui-chat-transcribe";`
  - `inline constexpr std::string_view kTranscriptionKind = "ui-chat-transcription";`
  - `inline constexpr std::string_view kTranscriptionEventKind = "transcription";`
  - `inline constexpr std::string_view kDictationMediaType = "audio/wav";`
  - `struct DictationOffer { std::uint64_t maxSeconds = 0; std::uint64_t maxBytes = 0; };`
  - `std::optional<DictationOffer> TurnState::dictation;`
  - `struct TranscribeRequest { std::string audio; std::string mediaType; std::optional<std::string> language; };`
  - `struct TranscribeAccepted { std::uint64_t id = 0; };`
  - `struct TranscriptionQuery { std::uint64_t id = 0; };`
  - `enum class TranscriptionStatus : std::uint8_t { Running, Done, Failed };`
  - `struct TranscriptionState { std::uint64_t id = 0; TranscriptionStatus status = TranscriptionStatus::Running; std::optional<std::string> text; std::optional<std::string> error; };`
  - `struct TranscriptionEvent { std::string kind = std::string{kTranscriptionEventKind}; TranscriptionState state; };`
  - `readTranscribeRequest`, `readTranscribeAccepted`, `readTranscriptionQuery`, `readTranscriptionState`, `readTranscriptionEvent`, each `(std::string_view) -> std::expected<T, std::string>`
  - `inline constexpr std::size_t kMaxDictationBytes = (wire::kMaxEnvelopeBytes - 4096) * 3 / 4;`
  - `ChatLimits::maxDictationSeconds` (60), `maxDictationBytes` (4 MiB), `maxTranscriptionsPerPrincipal` (2), `std::chrono::seconds transcriptionRetention{60}`
  - `struct TranscriptionConfig { std::string name; std::string baseUrl; std::string model; std::string apiKey; std::optional<std::string> language; };`
  - `std::optional<TranscriptionConfig> ChatConfig::transcription;`
  - `detail::keyOf(std::optional<std::string> given, std::optional<std::string> const& variable, EnvironmentLookup const& environment, std::string_view where) -> std::expected<std::string, std::string>`

- [ ] **Step 1: Write the failing tests.**

Append to `tests/test_chat_protocol.cpp`:

```cpp
// Mutation: drop `dictation` from TurnState, or write a null text into a finished state.
TEST_CASE("chat_wire_dictation_bodies_round_trip", "[chat][protocol][dictation]") {
    chat::TranscribeRequest const request{.audio = "UklGRg==", .mediaType = "audio/wav", .language = "de"};
    auto const readRequest = chat::readTranscribeRequest(chat::writeMessage(request));
    REQUIRE(readRequest.has_value());
    CHECK(readRequest->audio == "UklGRg==");
    CHECK(readRequest->mediaType == "audio/wav");
    CHECK(readRequest->language == "de");
    CHECK(chat::readTranscribeAccepted(chat::writeMessage(chat::TranscribeAccepted{.id = 9}))->id == 9);
    CHECK(chat::readTranscriptionQuery(chat::writeMessage(chat::TranscriptionQuery{.id = 9}))->id == 9);

    chat::TranscriptionState const done{
        .id = 9, .status = chat::TranscriptionStatus::Done, .text = "hello", .error = std::nullopt};
    std::string const doneText = chat::writeMessage(done);
    CHECK(doneText.contains(R"("status":"done")"));
    CHECK_FALSE(doneText.contains("error"));
    auto const readDone = chat::readTranscriptionState(doneText);
    REQUIRE(readDone.has_value());
    CHECK(readDone->status == chat::TranscriptionStatus::Done);
    CHECK(readDone->text == "hello");

    chat::TranscriptionEvent const event{.kind = std::string{chat::kTranscriptionEventKind}, .state = done};
    auto const readEvent = chat::readTranscriptionEvent(chat::writeMessage(event));
    REQUIRE(readEvent.has_value());
    CHECK(readEvent->kind == "transcription");
    CHECK(readEvent->state.id == 9);

    chat::TurnState offer;
    offer.dictation = chat::DictationOffer{.maxSeconds = 60, .maxBytes = 4194304};
    auto const readOffer = chat::readTurnState(chat::writeMessage(offer));
    REQUIRE(readOffer.has_value());
    REQUIRE(readOffer->dictation.has_value());
    CHECK(readOffer->dictation->maxSeconds == 60);
    CHECK(readOffer->dictation->maxBytes == 4194304);
    CHECK_FALSE(chat::writeMessage(chat::TurnState{}).contains("dictation"));
}

// Mutation: drop the event's `kind`, so an older client could not tell it from a turn's.
TEST_CASE("chat_wire_an_older_client_drops_a_transcription_event", "[chat][protocol][dictation]") {
    chat::TranscriptionEvent const event{
        .kind = std::string{chat::kTranscriptionEventKind},
        .state = {.id = 3, .status = chat::TranscriptionStatus::Failed, .text = std::nullopt, .error = "cancelled"}};
    auto const asTurn = chat::readTurnEvent(chat::writeMessage(event));
    REQUIRE(asTurn.has_value());
    CHECK(asTurn->turnId == 0);
    CHECK(asTurn->seq == 0);
    CHECK(asTurn->kind == "transcription");
}
```

Append to `tests/test_chat_config.cpp`:

```cpp
// Mutation: leave the transcription block's key variable unread.
TEST_CASE("chat_config_dictation_block_and_limits_are_read", "[chat][config][dictation]") {
    auto const config = chat::parseChatConfig(R"({
      "provider": { "name": "anthropic", "model": "m", "apiKey": "k" },
      "transcription": { "name": "openai-transcriptions", "baseUrl": "https://stt.example", "model": "whisper-1",
                         "apiKeyEnv": "THE_KEY", "language": "en" },
      "limits": { "maxDictationSeconds": 30, "maxDictationBytes": 1000000, "maxTranscriptionsPerPrincipal": 3,
                  "transcriptionRetentionSeconds": 90 } })",
                                              environment);
    REQUIRE(config.has_value());
    REQUIRE(config->transcription.has_value());
    CHECK(config->transcription->name == "openai-transcriptions");
    CHECK(config->transcription->baseUrl == "https://stt.example");
    CHECK(config->transcription->model == "whisper-1");
    CHECK(config->transcription->apiKey == kSecret);
    CHECK(config->transcription->language == "en");
    CHECK(config->limits.maxDictationSeconds == 30);
    CHECK(config->limits.maxDictationBytes == 1000000);
    CHECK(config->limits.maxTranscriptionsPerPrincipal == 3);
    CHECK(config->limits.transcriptionRetention == std::chrono::seconds{90});
}

// Mutation: make the transcription block required.
TEST_CASE("chat_config_dictation_defaults_without_a_block", "[chat][config][dictation]") {
    auto const config =
        chat::parseChatConfig(R"({ "provider": { "name": "anthropic", "model": "m", "apiKey": "k" } })", environment);
    REQUIRE(config.has_value());
    CHECK_FALSE(config->transcription.has_value());
    CHECK(config->limits.maxDictationSeconds == 60);
    CHECK(config->limits.maxDictationBytes == std::size_t{4} * 1024 * 1024);
    CHECK(config->limits.maxTranscriptionsPerPrincipal == 2);
    CHECK(config->limits.transcriptionRetention == std::chrono::seconds{60});
}

// Mutation: let the transcription block ignore unknown keys, or let its inline key win over its variable.
TEST_CASE("chat_config_dictation_block_refuses_an_unknown_key_or_both_keys", "[chat][config][dictation]") {
    std::string const unknown = R"({ "provider": { "name": "a", "model": "m", "apiKey": "k" },
      "transcription": { "name": "openai-transcriptions", "model": "w", "apiKey": ")" +
                                std::string{kSecret} + R"(", "voice": "x" } })";
    auto const refusedUnknown = chat::parseChatConfig(unknown, environment);
    REQUIRE_FALSE(refusedUnknown.has_value());
    CHECK_FALSE(refusedUnknown.error().contains(kSecret));
    auto const both = chat::parseChatConfig(R"({ "provider": { "name": "a", "model": "m", "apiKey": "k" },
      "transcription": { "name": "openai-transcriptions", "model": "w", "apiKey": "k", "apiKeyEnv": "THE_KEY" } })",
                                            environment);
    REQUIRE_FALSE(both.has_value());
    CHECK(both.error() == "the chat configuration's `transcription` names both `apiKey` and `apiKeyEnv`");
}

// Mutation: accept a dictation limit of zero, or a byte bound the envelope could not carry.
TEST_CASE("chat_config_dictation_limit_of_zero_or_past_the_envelope_is_refused", "[chat][config][dictation]") {
    auto const withLimits = [](std::string const& limits) {
        return chat::parseChatConfig(
            R"({ "provider": { "name": "a", "model": "m", "apiKey": "k" }, "limits": {)" + limits + "} }", environment);
    };
    for (char const* const name : {"maxDictationSeconds", "maxDictationBytes", "maxTranscriptionsPerPrincipal",
                                   "transcriptionRetentionSeconds"}) {
        auto const refused = withLimits("\"" + std::string{name} + "\": 0");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().contains(name));
    }
    auto const past = withLimits("\"maxDictationBytes\": " + std::to_string(chat::kMaxDictationBytes + 1));
    REQUIRE_FALSE(past.has_value());
    CHECK(past.error() == "the chat configuration's `maxDictationBytes` must be at most " +
                              std::to_string(chat::kMaxDictationBytes));
    CHECK(withLimits("\"maxDictationBytes\": " + std::to_string(chat::kMaxDictationBytes)).has_value());
}
```

- [ ] **Step 2: Run them and expect FAIL.**

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_tests
```

Expected: the build fails with `error: no type named 'TranscribeRequest' in namespace 'morph::ui::chat'`.

- [ ] **Step 3: Implement the wire.**

In `include/morph/ui/chat/protocol.hpp`, after `kChatTopic`:

```cpp
/// @brief The envelope kind that hands a dictated clip to the server. Its body is a `TranscribeRequest`, its reply a
///        `TranscribeAccepted`.
inline constexpr std::string_view kTranscribeKind = "ui-chat-transcribe";
/// @brief The envelope kind that reads a transcription's state. Its body is a `TranscriptionQuery`, its reply a
///        `TranscriptionState`.
inline constexpr std::string_view kTranscriptionKind = "ui-chat-transcription";
/// @brief The `kind` of a `TranscriptionEvent` on the `chat` topic, which tells it from a `TurnEvent`.
inline constexpr std::string_view kTranscriptionEventKind = "transcription";
/// @brief The one media type a dictated clip may have: 16 kHz mono 16-bit PCM in a WAV container.
inline constexpr std::string_view kDictationMediaType = "audio/wav";
```

Before `struct TurnState`:

```cpp
/// @brief The dictation a server offers, as its probe answers it.
struct DictationOffer {
    /// @brief The longest clip, in seconds.
    std::uint64_t maxSeconds = 0;
    /// @brief The largest clip, in bytes, before base64.
    std::uint64_t maxBytes = 0;
};
```

At the end of `TurnState`:

```cpp
    /// @brief The dictation this server offers; set only in the answer to a request with no turn (the probe), and
    ///        absent when the server offers none. An older peer ignores it.
    std::optional<DictationOffer> dictation;
```

After `struct TurnEvent`:

```cpp
/// @brief A dictated clip handed to the server for transcription.
struct TranscribeRequest {
    /// @brief The clip, base64 (RFC 4648).
    std::string audio;
    /// @brief Its media type, which must be `kDictationMediaType`.
    std::string mediaType;
    /// @brief The language spoken, such as `en`; absent leaves it to the server's configured default.
    std::optional<std::string> language;
};

/// @brief The reply to a `TranscribeRequest`: the clip was taken and is being transcribed.
struct TranscribeAccepted {
    /// @brief The transcription's id, which its event and state carry.
    std::uint64_t id = 0;
};

/// @brief Asks for a transcription's state.
struct TranscriptionQuery {
    /// @brief The transcription.
    std::uint64_t id = 0;
};

/// @brief How far a transcription has got.
enum class TranscriptionStatus : std::uint8_t {
    Running,  ///< Still being transcribed.
    Done,     ///< Finished; `TranscriptionState::text` holds the text.
    Failed,   ///< Stopped by a fault, a limit or a cancel; `TranscriptionState::error` says which.
};

/// @brief A transcription's state as the server holds it, which outranks any event.
struct TranscriptionState {
    /// @brief The transcription's id.
    std::uint64_t id = 0;
    /// @brief How far it has got.
    TranscriptionStatus status = TranscriptionStatus::Running;
    /// @brief The text, once done.
    std::optional<std::string> text;
    /// @brief Why it failed, once failed.
    std::optional<std::string> error;
};

/// @brief A hint that a transcription ended, on the `chat` topic. Events can be lost, so `TranscriptionState` is the
///        record.
struct TranscriptionEvent {
    /// @brief Always `kTranscriptionEventKind`, which an older client reads as a turn event's kind and ignores.
    std::string kind = std::string{kTranscriptionEventKind};
    /// @brief The state it ended in.
    TranscriptionState state;
};
```

After `readTurnEvent`:

```cpp
/// @brief Reads a `TranscribeRequest`.
/// @param text The body.
/// @return The request, or why it was refused.
[[nodiscard]] inline std::expected<TranscribeRequest, std::string> readTranscribeRequest(std::string_view text) {
    return ui::detail::readMessage<TranscribeRequest>(text, "the dictation", {}, {});
}

/// @brief Reads a `TranscribeAccepted`.
/// @param text The body.
/// @return The reply, or why it was refused.
[[nodiscard]] inline std::expected<TranscribeAccepted, std::string> readTranscribeAccepted(std::string_view text) {
    return ui::detail::readMessage<TranscribeAccepted>(text, "the dictation's reply", {}, {});
}

/// @brief Reads a `TranscriptionQuery`.
/// @param text The body.
/// @return The query, or why it was refused.
[[nodiscard]] inline std::expected<TranscriptionQuery, std::string> readTranscriptionQuery(std::string_view text) {
    return ui::detail::readMessage<TranscriptionQuery>(text, "the transcription query", {}, {});
}

/// @brief Reads a `TranscriptionState`.
/// @param text The body.
/// @return The state, or why it was refused.
[[nodiscard]] inline std::expected<TranscriptionState, std::string> readTranscriptionState(std::string_view text) {
    return ui::detail::readMessage<TranscriptionState>(text, "the transcription's state", {}, {});
}

/// @brief Reads a `TranscriptionEvent`.
/// @param text The event's payload.
/// @return The event, or why it was refused.
[[nodiscard]] inline std::expected<TranscriptionEvent, std::string> readTranscriptionEvent(std::string_view text) {
    return ui::detail::readMessage<TranscriptionEvent>(text, "the transcription event", {}, {});
}
```

At the end of the file:

```cpp
/// @brief Names a transcription's status on the wire.
template <>
struct glz::meta<morph::ui::chat::TranscriptionStatus> {
    using enum morph::ui::chat::TranscriptionStatus;
    /// @brief The names.
    static constexpr auto value = glz::enumerate("running", Running, "done", Done, "failed", Failed);
};
```

In `include/morph/ui/chat/conversations.hpp`, add `#include "../../core/wire.hpp"` after `#include "provider.hpp"`,
and before `struct ChatLimits`:

```cpp
/// @brief The largest dictated clip a server may accept, in bytes before base64: what fits one envelope once grown by
///        4/3 and wrapped. Past it the transport, not the chat, would refuse the request, and the person would not
///        learn which bound they crossed.
inline constexpr std::size_t kMaxDictationBytes = (wire::kMaxEnvelopeBytes - 4096) * 3 / 4;
```

At the end of `ChatLimits`:

```cpp
    /// @brief The longest clip a person may dictate, in seconds; the client stops recording there.
    std::size_t maxDictationSeconds = 60;
    /// @brief The largest clip the server takes, in bytes after decoding; the client stops recording before a clip
    ///        would pass it. At most `kMaxDictationBytes`.
    std::size_t maxDictationBytes = std::size_t{4} * 1024 * 1024;
    /// @brief Transcriptions one principal may have running across connections. One connection may have one.
    std::size_t maxTranscriptionsPerPrincipal = 2;
    /// @brief How long a finished transcription stays readable before it is forgotten.
    std::chrono::seconds transcriptionRetention{60};
```

In `include/morph/ui/chat/config.hpp`, after `ProviderConfig`:

```cpp
/// @brief Which speech-to-text provider a chat's dictation uses, and how to reach it.
struct TranscriptionConfig {
    /// @brief The provider's name, as whatever makes transcribers knows it.
    std::string name;
    /// @brief Where it is reached; empty for its public address.
    std::string baseUrl;
    /// @brief The model.
    std::string model;
    /// @brief The secret it is called with. It appears in no log and no message.
    std::string apiKey;
    /// @brief The language a clip is transcribed in when its request names none; absent leaves it to the provider.
    std::optional<std::string> language;
};
```

At the end of `ChatConfig`:

```cpp
    /// @brief The dictation's provider; absent when the chat offers no dictation.
    std::optional<TranscriptionConfig> transcription;
```

In `namespace detail`, after `ProviderFile`:

```cpp
struct TranscriptionFile {
    std::string name;
    std::string baseUrl;
    std::optional<std::string> apiKey;
    std::optional<std::string> apiKeyEnv;
    std::string model;
    std::optional<std::string> language;
};
```

At the end of `LimitsFile`:

```cpp
    std::optional<std::size_t> maxDictationSeconds;
    std::optional<std::size_t> maxDictationBytes;
    std::optional<std::size_t> maxTranscriptionsPerPrincipal;
    std::optional<std::int64_t> transcriptionRetentionSeconds;
```

In `ConfigFile`, after `ProviderFile provider;`: `std::optional<TranscriptionFile> transcription;`. After
`processEnvironment`:

```cpp
/// @brief The key a provider block names: inline, from the environment, or none.
/// @param given       The inline `apiKey`.
/// @param variable    The `apiKeyEnv`.
/// @param environment How the variable is looked up.
/// @param where       What the block is, for the refusal.
/// @return The key, empty for none, or why not: both named, or a variable that is not set.
[[nodiscard]] inline std::expected<std::string, std::string> keyOf(std::optional<std::string> given,
                                                                   std::optional<std::string> const& variable,
                                                                   EnvironmentLookup const& environment,
                                                                   std::string_view where) {
    if (given && variable) {
        return std::unexpected(std::string{where} + " names both `apiKey` and `apiKeyEnv`");
    }
    if (given) {
        return std::move(*given);
    }
    if (!variable) {
        return std::string{};
    }
    auto key = environment ? environment(*variable) : std::nullopt;
    if (!key) {
        return std::unexpected("the environment variable `" + *variable + "` named by `apiKeyEnv` is not set");
    }
    return std::move(*key);
}
```

In `parseChatConfig`, replace the provider key block (from `if (file.provider.apiKey && file.provider.apiKeyEnv)` to
the end of the `else if (file.provider.apiKeyEnv)` branch) with:

```cpp
    auto providerKey = detail::keyOf(std::move(file.provider.apiKey), file.provider.apiKeyEnv, environment,
                                     "the chat configuration");
    if (!providerKey) {
        return std::unexpected(std::move(providerKey.error()));
    }
    config.provider.apiKey = std::move(*providerKey);
    if (file.transcription) {
        detail::TranscriptionFile& block = *file.transcription;
        auto key = detail::keyOf(std::move(block.apiKey), block.apiKeyEnv, environment,
                                 "the chat configuration's `transcription`");
        if (!key) {
            return std::unexpected(std::move(key.error()));
        }
        config.transcription = TranscriptionConfig{.name = std::move(block.name),
                                                   .baseUrl = std::move(block.baseUrl),
                                                   .model = std::move(block.model),
                                                   .apiKey = std::move(*key),
                                                   .language = std::move(block.language)};
    }
```

After the `apply(limits.maxProposalsPerTurn, ...)` line:

```cpp
    apply(limits.maxDictationSeconds, config.limits.maxDictationSeconds, "maxDictationSeconds", problem);
    apply(limits.maxDictationBytes, config.limits.maxDictationBytes, "maxDictationBytes", problem);
    apply(limits.maxTranscriptionsPerPrincipal, config.limits.maxTranscriptionsPerPrincipal,
          "maxTranscriptionsPerPrincipal", problem);
```

After the `if (!problem.empty())` return:

```cpp
    if (config.limits.maxDictationBytes > kMaxDictationBytes) {
        return std::unexpected("the chat configuration's `maxDictationBytes` must be at most " +
                               std::to_string(kMaxDictationBytes));
    }
    if (limits.transcriptionRetentionSeconds) {
        if (*limits.transcriptionRetentionSeconds <= 0) {
            return std::unexpected(
                std::string{"the chat configuration's `transcriptionRetentionSeconds` must be above 0"});
        }
        config.limits.transcriptionRetention = std::chrono::seconds{*limits.transcriptionRetentionSeconds};
    }
```

`kMaxDictationBytes` is visible in `config.hpp` through its existing `#include "conversations.hpp"`.

- [ ] **Step 4: Run them and expect PASS.**

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_tests
ctest --test-dir build -R 'chat_wire_|chat_config_|chat_bodies|older_peer|whole_file|leaves_out|misspelled|naming_both|key_variable|refusal_never|limit_of_zero|first_bad_limit|name_no_key|read_from_disk' --output-on-failure
```

Expected: the six new cases and every existing config case pass (the provider's own refusals keep their messages).

- [ ] **Step 5: Update the spec.**

In `docs/spec/ui/chat.md`, "Protocol": change "Four envelope kinds" to "Six envelope kinds" and add two rows to the
table:

```markdown
| `ui-chat-transcribe` | `TranscribeRequest{audio, mediaType, language?}`: a dictated clip, base64, `audio/wav` | `TranscribeAccepted{id}`, at once |
| `ui-chat-transcription` | `TranscriptionQuery{id}` | `TranscriptionState{id, status: running \| done \| failed, text?, error?}` |
```

and after the table:

```markdown
`ui-chat-state` with no turn (the probe) also answers `TurnState::dictation`, `{maxSeconds, maxBytes}`, when the server
offers dictation, and leaves it out when it does not. A transcription ends with a `TranscriptionEvent` on the `chat`
topic, `{kind: "transcription", state}`; its `kind` tells it from a `TurnEvent`, and an older client reads it as a turn
event with turn id 0 and drops it. Like a turn's events it is a hint: the client reads `TranscriptionState`.
```

In "Configuration", add `"transcription": { "name": "openai-transcriptions", "model": "whisper-1", "apiKeyEnv": "STT_KEY", "language": "en" }`
to the example and these bullets:

```markdown
- `transcription`, optional, names the dictation's provider with `name`, `baseUrl`, `model`, `language`, and one of
  `apiKeyEnv` and `apiKey`, with the provider block's rules: an unknown key is refused, both keys are refused, and a
  refusal never quotes the text. Without it the chat offers no dictation.
- `limits` also takes `maxDictationSeconds` (60), `maxDictationBytes` (4 MiB), `maxTranscriptionsPerPrincipal` (2) and
  `transcriptionRetentionSeconds` (60). 0 is refused for each, and so is a `maxDictationBytes` above
  `kMaxDictationBytes` (about 6 MiB, what one envelope carries once base64 grows it by 4/3).
```

- [ ] **Step 6: Commit.**

```bash
git add include/morph/ui/chat/protocol.hpp include/morph/ui/chat/conversations.hpp include/morph/ui/chat/config.hpp \
        tests/test_chat_protocol.cpp tests/test_chat_config.cpp docs/spec/ui/chat.md
git commit -m "$(cat <<'MSG'
Add the dictation wire, its bounds and its configuration

Two chat kinds and a transcription event, the probe's dictation offer, the
dictation limits, and an optional transcription block in the chat's file.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznwgG54dkVGoxXz2Z4
MSG
)"
```

---

## Task 3: `ITranscriber`, `FakeTranscriber` and the service accepting a clip

**Files:**
- Create: `include/morph/ui/chat/transcriber.hpp`, `include/morph/ui/testing/fake_transcriber.hpp`, `tests/test_chat_transcription.cpp`
- Modify: `include/morph/ui/chat/service.hpp:4-21` (includes), `:52-78` (`ChatOptions`), `:130-138` (private)
- Modify: `src/ui/chat_service.cpp:10-46` (includes), `:50-55` (`configure`), `:92-112` (`Shared` members), `:141-154` (`readState`), after `:430` (new `Shared` functions), `:1032-1049` (constructor), `:1065-1094` (`serves`, `handle`)
- Modify: `CMakeLists.txt:678-696` (header file set), `tests/CMakeLists.txt:39` (source list), `tests/qt/test_qt_keywords.cpp:11-14` (includes)
- Docs: `docs/spec/ui/chat.md` (new "Dictation" section, first half)

**Interfaces:**
- Consumes: Task 2's protocol types and `ChatLimits` fields; `base64Decode` (`include/morph/ui/interpreter.hpp:2785`); `Shared::serverOf`, `Shared::now`, `ChatService::err`.
- Produces (namespace `morph::ui::chat`):
  - `struct TranscriptionInput { std::string audio; std::string mediaType; std::optional<std::string> language; };`
  - `struct TranscriberSink { std::function<void(std::expected<std::string, std::string>)> done; };`
  - `class ITranscriberCall { public: virtual ~ITranscriberCall() = default; virtual void cancel() = 0; };`
  - `class ITranscriber { public: virtual ~ITranscriber() = default; [[nodiscard]] virtual std::shared_ptr<ITranscriberCall> transcribe(TranscriptionInput input, TranscriberSink sink) = 0; };`
  - `class testing::FakeTranscriber final : public ITranscriber` with `script(std::expected<std::string, std::string>)`, `hold(bool)`, `release()`, `calls()`, `cancelled()`, `inputs()`
  - `std::shared_ptr<ITranscriber> ChatOptions::transcriber;` and `std::optional<std::string> ChatOptions::transcriptionLanguage;`
  - In `chat_service.cpp`: `struct ChatService::Transcription`, `Shared::transcriptions`, `Shared::startTranscription`, `Shared::callTranscriber`, `Shared::endTranscription(self, entry, outcome, CancelCall, Tell)`, `Shared::readTranscription`, `Shared::forgetExpired`, `enum class Shared::CancelCall { No, Yes }`, `enum class Shared::Tell { No, Yes }`

- [ ] **Step 1: Write the failing tests.**

Create `tests/test_chat_transcription.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Dictation on the server: a clip accepted at once, transcribed on the worker, and its result read from the state the
// `transcription` event points at. Each refusal names its bound, and each transcription ends exactly once.

// clang-format off
#include <morph/ui/value.hpp>
// clang-format on

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/chat/config.hpp>
#include <morph/ui/chat/protocol.hpp>
#include <morph/ui/chat/service.hpp>
#include <morph/ui/chat/transcriber.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/testing/fake_chat_provider.hpp>
#include <morph/ui/testing/fake_transcriber.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
namespace ui = morph::ui;
namespace chat = morph::ui::chat;
namespace wire = morph::wire;
using morph::backend::ConnectionId;

constexpr std::string_view kScreen = R"({"vocab":"ui/1","id":"s","view":{"kind":"text","text":"S"}})";

// "RIFF" and four bytes, base64: what the service takes is opaque to it.
constexpr std::string_view kClip = "UklGRgABAgM=";

// The token is the principal; `muted` may not chat.
struct Authorizer final : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(morph::session::Context const& ctx, std::string_view modelType,
                                 std::string_view actionType) const override {
        return !(ctx.principal == "muted" && modelType == "ui" && actionType == "chat");
    }
    [[nodiscard]] std::optional<std::string> authenticate(morph::session::Context const& ctx) const override {
        return ctx.token.empty() ? std::nullopt : std::optional{ctx.token};
    }
    [[nodiscard]] bool authorizeInstance(morph::session::Context const& /*ctx*/, std::string_view /*modelType*/,
                                         std::string_view /*actionType*/, std::uint64_t /*modelId*/,
                                         std::string_view /*owner*/) const override {
        return true;
    }
};

struct EventWire {
    std::mutex mutex;
    std::vector<std::string> sent;

    morph::backend::ConnectionWriter writer() {
        return {.write =
                    [this](std::string frame) {
                        std::scoped_lock const lock{mutex};
                        sent.push_back(std::move(frame));
                    },
                .backlog = [] { return std::size_t{0}; }};
    }

    // The transcription events sent so far, oldest first.
    [[nodiscard]] std::vector<chat::TranscriptionState> transcriptions() {
        std::scoped_lock const lock{mutex};
        std::vector<chat::TranscriptionState> out;
        for (std::string const& frame : sent) {
            auto const env = wire::decode(frame);
            wire::EventBody body;
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(body, env.body) || body.topic != chat::kChatTopic) {
                continue;
            }
            auto const event = chat::readTranscriptionEvent(body.payload.str);
            if (event && event->kind == chat::kTranscriptionEventKind) {
                out.push_back(event->state);
            }
        }
        return out;
    }
};

bool waitFor(auto done, std::chrono::milliseconds limit = 5s) {
    auto const deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        morph::testing::storageOwner().runFor(2ms);
    }
    return true;
}

std::shared_ptr<ui::ServedBundle const> bundleOf() {
    ui::UiRegistry registry{ui::ApplicationInfo{.id = "dictation", .version = "1"}};
    ui::CatalogEntry entry;
    entry.id = "s";
    REQUIRE(registry.addScreen(entry, std::string{kScreen}).has_value());
    auto built = registry.build();
    REQUIRE(built.has_value());
    return *built;
}

struct Rig {
    // Declared ahead of the pools, so it outlives the threads that write frames into it.
    EventWire eventWire;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::ThreadPoolExecutor worker{2};
    std::shared_ptr<chat::testing::FakeChatProvider> provider = std::make_shared<chat::testing::FakeChatProvider>();
    std::shared_ptr<chat::testing::FakeTranscriber> fake = std::make_shared<chat::testing::FakeTranscriber>();
    // The service's clock, in whole seconds, which a test moves by hand.
    std::shared_ptr<std::atomic<std::int64_t>> clock = std::make_shared<std::atomic<std::int64_t>>(0);
    std::shared_ptr<morph::backend::RemoteServer> server;
    std::shared_ptr<chat::ChatService> service;
    std::atomic<std::uint64_t> nextCall{1};

    explicit Rig(chat::ChatLimits limits = {}, std::shared_ptr<chat::ITranscriber> const& other = nullptr,
                 bool withTranscriber = true) {
        auto const bundle = bundleOf();
        chat::ChatOptions options;
        options.provider = provider;
        options.worker = &worker;
        options.bundle = bundle;
        options.limits = limits;
        if (withTranscriber) {
            options.transcriber = other != nullptr ? other : std::shared_ptr<chat::ITranscriber>{fake};
        }
        options.transcriptionLanguage = "en";
        options.now = [clock = clock] { return std::chrono::steady_clock::time_point{std::chrono::seconds{clock->load()}}; };
        service = std::make_shared<chat::ChatService>(std::move(options));
        morph::backend::ServerConfig config;
        config.services.push_back(std::make_shared<ui::UiService>(bundle));
        config.services.push_back(service);
        server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<Authorizer>(), std::move(config));
    }

    wire::Envelope call(wire::Envelope env, ConnectionId cid, std::string const& token = "alice") {
        env.callId = nextCall.fetch_add(1);
        env.session.token = token;
        env.session.principal = token;
        auto replied = std::make_shared<std::promise<std::string>>();
        auto answer = replied->get_future();
        server->handle(wire::encode(env), [replied](std::string const& text) { replied->set_value(text); }, cid);
        REQUIRE(waitFor([&] { return answer.wait_for(0s) == std::future_status::ready; }));
        return wire::decode(answer.get());
    }

    ConnectionId gui(std::string const& token = "alice") {
        auto const cid = server->openConnection();
        server->setConnectionWriter(cid, eventWire.writer());
        wire::Envelope hello;
        hello.kind = std::string{ui::kHelloKind};
        hello.body = R"({"vocabularies":["ui/1","expr/1"],"events":true})";
        REQUIRE(call(std::move(hello), cid, token).kind == "ok");
        return cid;
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a test helper; the trailing arguments are rarely given.
    wire::Envelope transcribeRaw(std::string_view audio, ConnectionId cid, std::string const& token = "alice",
                                 std::string mediaType = "audio/wav", std::optional<std::string> language = {}) {
        wire::Envelope env;
        env.kind = std::string{chat::kTranscribeKind};
        env.body = chat::writeMessage(chat::TranscribeRequest{
            .audio = std::string{audio}, .mediaType = std::move(mediaType), .language = std::move(language)});
        return call(std::move(env), cid, token);
    }

    std::uint64_t transcribe(std::string_view audio, ConnectionId cid, std::string const& token = "alice",
                             std::optional<std::string> language = {}) {
        auto const reply = transcribeRaw(audio, cid, token, "audio/wav", std::move(language));
        INFO(reply.message);
        REQUIRE(reply.kind == "ok");
        auto const accepted = chat::readTranscribeAccepted(reply.body);
        REQUIRE(accepted.has_value());
        return accepted->id;
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a test helper; the token is almost always left out.
    chat::TranscriptionState state(std::uint64_t id, ConnectionId cid, std::string const& token = "alice") {
        wire::Envelope env;
        env.kind = std::string{chat::kTranscriptionKind};
        env.body = chat::writeMessage(chat::TranscriptionQuery{.id = id});
        auto const reply = call(std::move(env), cid, token);
        REQUIRE(reply.kind == "ok");
        auto const read = chat::readTranscriptionState(reply.body);
        REQUIRE(read.has_value());
        return *read;
    }

    chat::TranscriptionState settled(std::uint64_t id, ConnectionId cid, std::string const& token = "alice") {
        chat::TranscriptionState last;
        REQUIRE(waitFor([&] {
            last = state(id, cid, token);
            return last.status != chat::TranscriptionStatus::Running;
        }));
        return last;
    }

    chat::TurnState probe(ConnectionId cid) {
        wire::Envelope env;
        env.kind = std::string{chat::kStateKind};
        env.body = "{}";
        auto const reply = call(std::move(env), cid);
        REQUIRE(reply.kind == "ok");
        auto const read = chat::readTurnState(reply.body);
        REQUIRE(read.has_value());
        return *read;
    }
};

}  // namespace

// Mutation: apply no default language, or send the event to no one.
TEST_CASE("chat_dictation_is_accepted_at_once_and_read_from_the_state", "[chat][dictation]") {
    Rig rig;
    auto const cid = rig.gui();
    rig.fake->script(std::string{"hello there"});
    auto const id = rig.transcribe(kClip, cid);
    auto const state = rig.settled(id, cid);
    CHECK(state.status == chat::TranscriptionStatus::Done);
    CHECK(state.text == "hello there");
    CHECK_FALSE(state.error.has_value());
    REQUIRE(waitFor([&] { return !rig.eventWire.transcriptions().empty(); }));
    auto const told = rig.eventWire.transcriptions().front();
    CHECK(told.id == id);
    CHECK(told.status == chat::TranscriptionStatus::Done);

    auto const inputs = rig.fake->inputs();
    REQUIRE(inputs.size() == 1);
    CHECK(inputs.front().audio == std::string{"RIFF\x00\x01\x02\x03", 8});
    CHECK(inputs.front().mediaType == "audio/wav");
    CHECK(inputs.front().language == "en");

    rig.fake->script(std::string{"hallo"});
    static_cast<void>(rig.settled(rig.transcribe(kClip, cid, "alice", "de"), cid));
    CHECK(rig.fake->inputs().back().language == "de");
}

// Mutation: answer the transcription kinds before the chat's gate.
TEST_CASE("chat_dictation_is_behind_the_chat_gate", "[chat][dictation]") {
    Rig rig;
    auto const cid = rig.gui("muted");
    auto const reply = rig.transcribeRaw(kClip, cid, "muted");
    CHECK(reply.kind == "err");
    CHECK(reply.message == wire::kUnauthorizedMessage);
    CHECK(rig.fake->calls() == 0);
}

// Mutation: decode before checking the text's size (the oversized garbage then reads "not base64"), or drop the
// decoded size check.
TEST_CASE("chat_dictation_refusals_name_their_bound", "[chat][dictation]") {
    chat::ChatLimits limits;
    limits.maxDictationBytes = 31;
    Rig rig{limits};
    auto const cid = rig.gui();
    std::string const bound = "the recording is larger than 31 bytes";
    std::string tenGroups;
    for (int group = 0; group < 10; ++group) {
        tenGroups += "YWFh";
    }

    auto const garbage = rig.transcribeRaw(std::string(45, '!'), cid);
    CHECK(garbage.kind == "err");
    CHECK(garbage.message == bound);
    auto const decodedOver = rig.transcribeRaw(tenGroups + "YWE=", cid);
    CHECK(decodedOver.message == bound);
    CHECK(rig.transcribeRaw("@@@@", cid).message == "the recording is not base64");
    CHECK(rig.transcribeRaw(kClip, cid, "alice", "audio/ogg").message == "the recording is not audio/wav");
    CHECK(rig.transcribeRaw(kClip, 0).message == "dictation needs a connection");
    CHECK(rig.fake->calls() == 0);

    rig.fake->script(std::string{"ok"});
    auto const atTheBound = rig.transcribe(tenGroups + "YQ==", cid);
    CHECK(rig.settled(atTheBound, cid).status == chat::TranscriptionStatus::Done);
    CHECK(rig.fake->inputs().front().audio.size() == 31);
}

// Mutations: drop the connection's `busy` check (the second call on a1 is accepted); drop the principal's cap (the
// call on a3 is accepted).
TEST_CASE("chat_dictation_busy_and_the_principal_cap", "[chat][dictation]") {
    chat::ChatLimits limits;
    limits.maxTranscriptionsPerPrincipal = 2;
    Rig rig{limits};
    rig.fake->hold(true);
    auto const a1 = rig.gui();
    auto const a2 = rig.gui();
    auto const a3 = rig.gui();
    auto const b1 = rig.gui("bob");

    auto const first = rig.transcribe(kClip, a1);
    auto const busy = rig.transcribeRaw(kClip, a1);
    CHECK(busy.kind == "err");
    CHECK(busy.message.starts_with("busy"));
    auto const second = rig.transcribe(kClip, a2);
    auto const capped = rig.transcribeRaw(kClip, a3);
    CHECK(capped.kind == "err");
    CHECK(capped.message == "too many dictations are being transcribed");
    static_cast<void>(rig.transcribe(kClip, b1, "bob"));

    rig.fake->release();
    static_cast<void>(rig.settled(first, a1));
    static_cast<void>(rig.settled(second, a2));
    static_cast<void>(rig.transcribe(kClip, a3));
}

// Mutation: leave `dictation` out of the probe's answer, or put it there without a transcriber.
TEST_CASE("chat_dictation_is_offered_by_the_probe_only_with_a_transcriber", "[chat][dictation]") {
    Rig offered;
    auto const offer = offered.probe(offered.gui()).dictation;
    REQUIRE(offer.has_value());
    CHECK(offer->maxSeconds == 60);
    CHECK(offer->maxBytes == std::uint64_t{4} * 1024 * 1024);
    CHECK(offered.service->serves(chat::kTranscribeKind));

    Rig plain{chat::ChatLimits{}, nullptr, false};
    CHECK_FALSE(plain.probe(plain.gui()).dictation.has_value());
    CHECK_FALSE(plain.service->serves(chat::kTranscribeKind));
    CHECK_FALSE(plain.service->serves(chat::kTranscriptionKind));
}

// Mutation: answer a query without comparing the principal.
TEST_CASE("chat_dictation_of_another_principal_reads_as_none", "[chat][dictation]") {
    Rig rig;
    auto const cid = rig.gui();
    rig.fake->script(std::string{"mine"});
    auto const id = rig.transcribe(kClip, cid);
    CHECK(rig.settled(id, cid).text == "mine");
    auto const bob = rig.state(id, rig.gui("bob"), "bob");
    CHECK(bob.status == chat::TranscriptionStatus::Failed);
    CHECK(bob.error == "no such transcription");
    CHECK_FALSE(bob.text.has_value());
    auto const unknown = rig.state(id + 1000, cid);
    CHECK(unknown.error == "no such transcription");
}

// Mutation: accept a transcriber with no provider, or a byte bound past the envelope.
TEST_CASE("chat_dictation_misconfiguration_is_refused_at_construction", "[chat][dictation]") {
    morph::exec::ThreadPoolExecutor worker{1};
    auto const bundle = bundleOf();
    auto const optionsWith = [&](chat::ChatLimits limits, bool withProvider) {
        chat::ChatOptions options;
        if (withProvider) {
            options.provider = std::make_shared<chat::testing::FakeChatProvider>();
        }
        options.worker = &worker;
        options.bundle = bundle;
        options.limits = limits;
        options.transcriber = std::make_shared<chat::testing::FakeTranscriber>();
        return options;
    };
    CHECK_THROWS_AS(static_cast<void>(chat::ChatService{optionsWith(chat::ChatLimits{}, false)}), std::invalid_argument);
    chat::ChatLimits past;
    past.maxDictationBytes = chat::kMaxDictationBytes + 1;
    CHECK_THROWS_AS(static_cast<void>(chat::ChatService{optionsWith(past, true)}), std::invalid_argument);
    chat::ChatLimits zero;
    zero.maxTranscriptionsPerPrincipal = 0;
    CHECK_THROWS_AS(static_cast<void>(chat::ChatService{optionsWith(zero, true)}), std::invalid_argument);
    chat::ChatLimits atTheCap;
    atTheCap.maxDictationBytes = chat::kMaxDictationBytes;
    CHECK_NOTHROW(static_cast<void>(chat::ChatService{optionsWith(atTheCap, true)}));
}

// Mutation: leave the configured language out of `configure`.
TEST_CASE("chat_dictation_language_comes_from_the_configuration", "[chat][dictation]") {
    chat::ChatConfig config;
    config.transcription = chat::TranscriptionConfig{
        .name = "openai-transcriptions", .baseUrl = {}, .model = "whisper-1", .apiKey = "k", .language = "fr"};
    chat::ChatOptions options;
    options.configure(config);
    CHECK(options.transcriptionLanguage == "fr");
    config.transcription.reset();
    options.configure(config);
    CHECK_FALSE(options.transcriptionLanguage.has_value());
}
```

In `tests/CMakeLists.txt`, add `test_chat_transcription.cpp` after `test_chat_service.cpp` in `morph_tests`. In
`tests/qt/test_qt_keywords.cpp`, add `#include <morph/ui/chat/transcriber.hpp>` after
`#include <morph/ui/chat/service.hpp>`.

- [ ] **Step 2: Run them and expect FAIL.**

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_tests
```

Expected: the build fails with `fatal error: 'morph/ui/chat/transcriber.hpp' file not found`.

- [ ] **Step 3: Write the seam and the fake.**

Create `include/morph/ui/chat/transcriber.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

/// @file
/// @brief The seam between a chat's dictation and the speech-to-text provider behind it. Nothing outside an
///        implementation of `ITranscriber` names a provider. Specified in `docs/spec/ui/chat.md`.

namespace morph::ui::chat {

/// @brief One clip to transcribe.
struct TranscriptionInput {
    /// @brief The clip's bytes.
    std::string audio;
    /// @brief Its media type, `audio/wav`.
    std::string mediaType;
    /// @brief The language spoken; the service has already applied its configured default, so this is the one value
    ///        to use. Nothing leaves it to the provider.
    std::optional<std::string> language;
};

/// @brief Where a transcription delivers.
struct TranscriberSink {
    /// @brief The text, or why there is none, from any thread, exactly once unless cancelled. It may run before
    ///        `ITranscriber::transcribe` returns.
    std::function<void(std::expected<std::string, std::string>)> done;
};

/// @brief An outstanding transcription.
class ITranscriberCall {
public:
    ITranscriberCall() = default;
    ITranscriberCall(ITranscriberCall const&) = delete;
    ITranscriberCall& operator=(ITranscriberCall const&) = delete;
    ITranscriberCall(ITranscriberCall&&) = delete;
    ITranscriberCall& operator=(ITranscriberCall&&) = delete;
    virtual ~ITranscriberCall() = default;

    /// @brief Stops the call. Its sink hears nothing after this returns, unless a delivery had already begun.
    ///        Callable from any thread, more than once, and after the call answered, when it does nothing.
    virtual void cancel() = 0;
};

/// @brief A speech-to-text provider behind a chat's dictation.
class ITranscriber {
public:
    ITranscriber() = default;
    ITranscriber(ITranscriber const&) = delete;
    ITranscriber& operator=(ITranscriber const&) = delete;
    ITranscriber(ITranscriber&&) = delete;
    ITranscriber& operator=(ITranscriber&&) = delete;
    virtual ~ITranscriber() = default;

    /// @brief Starts a transcription. Callable from any thread.
    /// @param input The clip.
    /// @param sink  Where it delivers.
    /// @return The call, for cancelling it.
    [[nodiscard]] virtual std::shared_ptr<ITranscriberCall> transcribe(TranscriptionInput input,
                                                                       TranscriberSink sink) = 0;
};

}  // namespace morph::ui::chat
```

Create `include/morph/ui/testing/fake_transcriber.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../chat/transcriber.hpp"

/// @file
/// @brief A scripted `ITranscriber` for tests.

namespace morph::ui::chat::testing {

/// @brief Answers each call with the next scripted outcome, and fails once the script runs out. Thread-safe.
///
/// Delivers before `transcribe` returns, unless `hold(true)` leaves each call outstanding until `release`. Counts the
/// calls cancelled before they delivered.
class FakeTranscriber final : public ITranscriber {
public:
    /// @brief Adds an outcome to the script.
    /// @param outcome What the next unscripted call answers: the text, or why it failed.
    void script(std::expected<std::string, std::string> outcome) {
        std::scoped_lock const lock{_state->mutex};
        _state->script.push_back(std::move(outcome));
    }

    /// @brief Sets whether calls wait for `release`.
    /// @param held Whether they do.
    void hold(bool held) {
        std::scoped_lock const lock{_state->mutex};
        _state->held = held;
    }

    /// @brief Delivers every held call that was not cancelled, and stops holding.
    void release() {
        std::vector<std::shared_ptr<Pending>> pending;
        {
            std::scoped_lock const lock{_state->mutex};
            _state->held = false;
            pending.swap(_state->pending);
        }
        for (auto const& call : pending) {
            call->deliver();
        }
    }

    /// @brief The calls made so far.
    /// @return Their number.
    [[nodiscard]] std::size_t calls() const {
        std::scoped_lock const lock{_state->mutex};
        return _state->inputs.size();
    }

    /// @brief The calls cancelled before they delivered.
    /// @return Their number.
    [[nodiscard]] std::size_t cancelled() const {
        std::scoped_lock const lock{_state->mutex};
        return _state->cancelled;
    }

    /// @brief What each call was given.
    /// @return A copy of the inputs, oldest first.
    [[nodiscard]] std::vector<TranscriptionInput> inputs() const {
        std::scoped_lock const lock{_state->mutex};
        return _state->inputs;
    }

    /// @brief Starts a call.
    /// @param input The clip.
    /// @param sink  Where it delivers.
    /// @return The call.
    [[nodiscard]] std::shared_ptr<ITranscriberCall> transcribe(TranscriptionInput input, TranscriberSink sink) override {
        auto call = std::make_shared<Pending>(_state, std::move(sink));
        bool held = false;
        {
            std::scoped_lock const lock{_state->mutex};
            _state->inputs.push_back(std::move(input));
            if (!_state->script.empty()) {
                call->outcome = std::move(_state->script.front());
                _state->script.pop_front();
            } else {
                call->outcome = std::unexpected(std::string{"the fake transcriber's script ran out"});
            }
            held = _state->held;
            if (held) {
                _state->pending.push_back(call);
            }
        }
        if (!held) {
            call->deliver();
        }
        return call;
    }

private:
    struct State;

    // Holds the state, so a call outlives the transcriber and a late cancel touches nothing freed. A finished call
    // lets go of its sink, which usually owns whoever owns this transcriber; it is destroyed outside the lock.
    struct Pending final : ITranscriberCall {
        Pending(std::shared_ptr<State> shared, TranscriberSink given)
            : state{std::move(shared)}, sink{std::move(given)} {}

        void cancel() override {
            TranscriberSink dropped;
            std::shared_ptr<Pending> unheld;
            {
                std::scoped_lock const lock{state->mutex};
                if (finished) {
                    return;
                }
                finished = true;
                ++state->cancelled;
                dropped = std::move(sink);
                auto const position = std::ranges::find_if(
                    state->pending, [this](std::shared_ptr<Pending> const& held) { return held.get() == this; });
                if (position != state->pending.end()) {
                    unheld = std::move(*position);
                    state->pending.erase(position);
                }
            }
        }

        void deliver() {
            TranscriberSink taken;
            {
                std::scoped_lock const lock{state->mutex};
                if (finished) {
                    return;
                }
                finished = true;
                taken = std::move(sink);
            }
            if (taken.done) {
                taken.done(std::move(outcome));
            }
        }

        std::shared_ptr<State> state;
        TranscriberSink sink;
        std::expected<std::string, std::string> outcome = std::unexpected(std::string{});
        bool finished = false;
    };

    struct State {
        mutable std::mutex mutex;
        std::deque<std::expected<std::string, std::string>> script;
        std::vector<TranscriptionInput> inputs;
        std::vector<std::shared_ptr<Pending>> pending;
        std::size_t cancelled = 0;
        bool held = false;
    };

    std::shared_ptr<State> _state = std::make_shared<State>();
};

}  // namespace morph::ui::chat::testing
```

In `CMakeLists.txt`, add `include/morph/ui/chat/transcriber.hpp` after `include/morph/ui/chat/service.hpp` and
`include/morph/ui/testing/fake_transcriber.hpp` after `include/morph/ui/testing/fake_chat_provider.hpp`.

- [ ] **Step 4: Implement accepting, answering and reading.**

In `include/morph/ui/chat/service.hpp`, add `#include <optional>` and `#include "transcriber.hpp"`. In `ChatOptions`,
after `provider`:

```cpp
    /// @brief Turns a dictated clip into text. Null: the service serves neither transcription kind and its probe offers
    ///        no dictation. Needs a provider, since the text goes into the chat's input.
    std::shared_ptr<ITranscriber> transcriber;
    /// @brief The language a clip is transcribed in when its request names none; nothing leaves it to the transcriber.
    std::optional<std::string> transcriptionLanguage;
```

Change the `ChatService(ChatOptions)` `@throws` to: "with a provider but no worker or bundle, with a transcriber but no
provider, a dictation limit of 0 or a `maxDictationBytes` above `kMaxDictationBytes`, or with an extra tool whose name
is invalid or taken by another, or which has no handler." In the private section, after `struct Shared;` add
`struct Transcription;`.

In `src/ui/chat_service.cpp`, add `#include <morph/ui/chat/transcriber.hpp>` and `#include <morph/ui/interpreter.hpp>`.
Make `configure`:

```cpp
void ChatOptions::configure(ChatConfig const& config) {
    limits = config.limits;
    if (!config.systemPrompt.empty()) {
        systemPrompt = config.systemPrompt;
    }
    transcriptionLanguage = config.transcription ? config.transcription->language : std::nullopt;
}
```

After `struct ChatService::Turn { ... };`:

```cpp
// A dictated clip being transcribed, or one that ended and is kept for `transcriptionRetention`. `id`, `principal` and
// `cid` never change; the rest is guarded by Shared::mutex.
struct ChatService::Transcription {
    std::uint64_t id = 0;
    std::string principal;
    backend::ConnectionId cid = 0;
    TranscriptionStatus status = TranscriptionStatus::Running;
    std::string text;
    std::string error;
    std::shared_ptr<ITranscriberCall> call;
    ::morph::async::detail::TimeoutScheduler::Handle deadline = 0;
    std::chrono::steady_clock::time_point finishedAt{};
};
```

In `Shared`, after `std::atomic<std::uint64_t> nextCall{1};`:

```cpp
    // Guarded by `mutex`.
    std::map<std::uint64_t, std::shared_ptr<Transcription>> transcriptions;
    std::atomic<std::uint64_t> nextTranscription{1};

    using TranscriptionPtr = std::shared_ptr<Transcription>;
    // Whether ending a transcription cancels its call, and whether its connection hears of it.
    enum class CancelCall : std::uint8_t { No, Yes };
    enum class Tell : std::uint8_t { No, Yes };
```

In `readState`, after the `if (asked->turnId) { ... }` block:

```cpp
        else if (options.transcriber != nullptr) {
            state.dictation = DictationOffer{.maxSeconds = static_cast<std::uint64_t>(options.limits.maxDictationSeconds),
                                             .maxBytes = static_cast<std::uint64_t>(options.limits.maxDictationBytes)};
        }
```

After `serverOf`, add:

```cpp
    // Drops finished transcriptions past their retention. The caller holds `mutex`.
    void forgetExpired() {
        auto const cutoff = now() - options.limits.transcriptionRetention;
        std::erase_if(transcriptions, [cutoff](auto const& held) {
            return held.second->status != TranscriptionStatus::Running && held.second->finishedAt <= cutoff;
        });
    }

    [[nodiscard]] static TranscriptionState stateOf(Transcription const& entry) {
        TranscriptionState state{.id = entry.id, .status = entry.status, .text = std::nullopt, .error = std::nullopt};
        if (entry.status == TranscriptionStatus::Done) {
            state.text = entry.text;
        } else if (entry.status == TranscriptionStatus::Failed) {
            state.error = entry.error;
        }
        return state;
    }

    // A hint to the transcription's connection. Lost events are expected: the client reads the state.
    static void tellTranscription(Shared& self, backend::ConnectionId cid, TranscriptionState state) {
        auto const server = serverOf(self);
        if (server == nullptr) {
            return;
        }
        server->post(cid, wire::encode(wire::makeEvent(
                              kChatTopic, writeMessage(TranscriptionEvent{.kind = std::string{kTranscriptionEventKind},
                                                                          .state = std::move(state)}))));
    }

    // Ends a transcription, once: with its text, or with why it failed. Whatever ends it second finds it ended and
    // does nothing, which is what makes a late answer harmless.
    static void endTranscription(std::shared_ptr<Shared> const& self, TranscriptionPtr const& entry,
                                 std::expected<std::string, std::string> outcome, CancelCall cancel, Tell tell) {
        std::shared_ptr<ITranscriberCall> call;
        std::optional<::morph::async::detail::TimeoutScheduler::Handle> handle;
        TranscriptionState state;
        {
            std::scoped_lock const lock{self->mutex};
            if (entry->status != TranscriptionStatus::Running) {
                return;
            }
            if (outcome) {
                entry->status = TranscriptionStatus::Done;
                entry->text = std::move(*outcome);
            } else {
                entry->status = TranscriptionStatus::Failed;
                entry->error = std::move(outcome.error());
            }
            entry->finishedAt = self->now();
            call = std::move(entry->call);
            entry->call = nullptr;
            if (entry->deadline != 0) {
                handle = entry->deadline;
            }
            entry->deadline = 0;
            state = stateOf(*entry);
        }
        cancelTimer(*self, handle);
        if (call != nullptr && cancel == CancelCall::Yes) {
            call->cancel();
        }
        if (tell == Tell::Yes) {
            tellTranscription(*self, entry->cid, std::move(state));
        }
    }

    [[nodiscard]] static bool transcribing(Shared& self, Transcription const& entry) {
        std::scoped_lock const lock{self.mutex};
        return entry.status == TranscriptionStatus::Running;
    }

    // Starts the transcriber's call on the worker. The clip moves into the call, so the service holds no audio once
    // it started. An entry that ended before its call existed has the call cancelled at once; cancelling one that
    // already answered does nothing.
    static void callTranscriber(std::shared_ptr<Shared> const& self, TranscriptionPtr const& entry,
                                TranscriptionInput input) {
        if (!transcribing(*self, *entry)) {
            return;
        }
        TranscriberSink sink{.done = [weakSelf = std::weak_ptr<Shared>{self}, weakEntry = std::weak_ptr<Transcription>{entry}](
                                         std::expected<std::string, std::string> outcome) {
            auto const lockedSelf = weakSelf.lock();
            auto const lockedEntry = weakEntry.lock();
            if (lockedSelf != nullptr && lockedEntry != nullptr) {
                endTranscription(lockedSelf, lockedEntry, std::move(outcome), CancelCall::No, Tell::Yes);
            }
        }};
        std::shared_ptr<ITranscriberCall> call;
        try {
            call = self->options.transcriber->transcribe(std::move(input), std::move(sink));
        } catch (std::exception const& error) {
            endTranscription(self, entry, std::unexpected("the transcription failed: " + std::string{error.what()}),
                             CancelCall::No, Tell::Yes);
            return;
        }
        bool ended = false;
        {
            std::scoped_lock const lock{self->mutex};
            if (entry->status == TranscriptionStatus::Running) {
                entry->call = call;
            } else {
                ended = true;
            }
        }
        if (ended && call != nullptr) {
            call->cancel();
        }
    }

    // `handle` runs on the server strand and must not block, so a clip is checked, recorded and accepted here, and
    // transcribed on the worker. The base64 text's size is checked before decoding, so an oversized body is refused
    // for its size, not for what it holds.
    [[nodiscard]] static std::string startTranscription(std::shared_ptr<Shared> const& self,
                                                        wire::Envelope const& request, backend::ConnectionId cid) {
        auto parsed = readTranscribeRequest(request.body);
        if (!parsed) {
            return err(request, parsed.error());
        }
        if (cid == 0) {
            return err(request, "dictation needs a connection");
        }
        ChatLimits const& limits = self->options.limits;
        std::string const bound = "the recording is larger than " + std::to_string(limits.maxDictationBytes) + " bytes";
        if (parsed->audio.size() > ((limits.maxDictationBytes + 2) / 3) * 4) {
            return err(request, bound);
        }
        std::optional<std::string> audio = base64Decode(parsed->audio);
        if (!audio) {
            return err(request, "the recording is not base64");
        }
        if (audio->size() > limits.maxDictationBytes) {
            return err(request, bound);
        }
        if (parsed->mediaType != kDictationMediaType) {
            return err(request, "the recording is not " + std::string{kDictationMediaType});
        }
        parsed->audio = std::string{};
        auto entry = std::make_shared<Transcription>();
        entry->id = self->nextTranscription.fetch_add(1);
        entry->principal = request.session.principal;
        entry->cid = cid;
        {
            std::scoped_lock const lock{self->mutex};
            self->forgetExpired();
            std::size_t onConnection = 0;
            std::size_t ofPrincipal = 0;
            for (auto const& [id, held] : self->transcriptions) {
                static_cast<void>(id);
                if (held->status == TranscriptionStatus::Running) {
                    onConnection += held->cid == cid ? 1U : 0U;
                    ofPrincipal += held->principal == entry->principal ? 1U : 0U;
                }
            }
            if (onConnection > 0) {
                return err(request, "busy: a dictation is already being transcribed on this connection");
            }
            if (ofPrincipal >= limits.maxTranscriptionsPerPrincipal) {
                return err(request, "too many dictations are being transcribed");
            }
            self->transcriptions.insert_or_assign(entry->id, entry);
        }
        TranscriptionInput input{.audio = std::move(*audio),
                                 .mediaType = std::move(parsed->mediaType),
                                 .language = parsed->language ? std::move(parsed->language)
                                                              : self->options.transcriptionLanguage};
        try {
            self->options.worker->post([self, entry, input = std::move(input)]() mutable {
                callTranscriber(self, entry, std::move(input));
            });
        } catch (std::exception const& error) {
            std::scoped_lock const lock{self->mutex};
            self->transcriptions.erase(entry->id);
            return err(request, error.what());
        }
        return wire::encode(wire::makeOk(request.callId, writeMessage(TranscribeAccepted{.id = entry->id})));
    }

    // The same answer for another principal's transcription, one forgotten and one that never was, so ids reveal
    // nothing.
    [[nodiscard]] std::string readTranscription(wire::Envelope const& request) {
        auto const asked = readTranscriptionQuery(request.body);
        if (!asked) {
            return err(request, asked.error());
        }
        TranscriptionState state{.id = asked->id,
                                 .status = TranscriptionStatus::Failed,
                                 .text = std::nullopt,
                                 .error = std::string{"no such transcription"}};
        {
            std::scoped_lock const lock{mutex};
            forgetExpired();
            auto const found = transcriptions.find(asked->id);
            if (found != transcriptions.end() && found->second->principal == request.session.principal) {
                state = stateOf(*found->second);
            }
        }
        return wire::encode(wire::makeOk(request.callId, writeMessage(state)));
    }
```

In the constructor, after the provider check:

```cpp
    if (_shared->options.transcriber != nullptr) {
        ChatLimits const& limits = _shared->options.limits;
        if (_shared->options.provider == nullptr) {
            throw std::invalid_argument{"ChatService: a transcriber needs a provider"};
        }
        if (limits.maxDictationSeconds == 0 || limits.maxDictationBytes == 0 ||
            limits.maxTranscriptionsPerPrincipal == 0 || limits.transcriptionRetention.count() <= 0) {
            throw std::invalid_argument{"ChatService: a dictation limit of 0 would refuse every dictation"};
        }
        if (limits.maxDictationBytes > kMaxDictationBytes) {
            throw std::invalid_argument{"ChatService: maxDictationBytes is above " +
                                        std::to_string(kMaxDictationBytes)};
        }
    }
```

Make `serves`:

```cpp
bool ChatService::serves(std::string_view kind) const {
    if (_shared->options.provider == nullptr) {
        return false;
    }
    if (kind == kTranscribeKind || kind == kTranscriptionKind) {
        return _shared->options.transcriber != nullptr;
    }
    return kind == kTurnKind || kind == kStateKind || kind == kCancelKind || kind == kEndKind;
}
```

In `handle`, after the `kStateKind` branch:

```cpp
    if (request.kind == kTranscribeKind) {
        return Shared::startTranscription(_shared, request, facts.cid);
    }
    if (request.kind == kTranscriptionKind) {
        return _shared->readTranscription(request);
    }
```

- [ ] **Step 5: Run them and expect PASS.**

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_tests
ctest --test-dir build -R '^chat_dictation_' --output-on-failure
ctest --test-dir build -R 'a_turn_runs_the_tool_and_finishes|the_probe_finds_a_chat_or_not' --output-on-failure
```

Expected: the eight new cases pass, and the two existing chat cases still pass.

- [ ] **Step 6: Mutation checks (the bound check, `busy`, the per-principal cap, the probe's field).**

```bash
cp src/ui/chat_service.cpp /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/chat_service.cpp.bak
```

One at a time, each followed by a `morph_tests` build, `ctest --test-dir build -R '<case>' --output-on-failure`, and a
restore (`cp …/chat_service.cpp.bak src/ui/chat_service.cpp && touch src/ui/chat_service.cpp`):

1. Delete the `if (parsed->audio.size() > ((limits.maxDictationBytes + 2) / 3) * 4)` block. Case
   `chat_dictation_refusals_name_their_bound`: FAIL at `CHECK( garbage.message == bound )` (it reads "the recording is
   not base64").
2. Delete the `if (onConnection > 0)` block. Case `chat_dictation_busy_and_the_principal_cap`: FAIL at
   `CHECK( busy.kind == "err" )`.
3. Delete the `if (ofPrincipal >= limits.maxTranscriptionsPerPrincipal)` block. Same case: FAIL at
   `CHECK( capped.kind == "err" )`.
4. Delete the `else if (options.transcriber != nullptr) { state.dictation = ...; }` block. Case
   `chat_dictation_is_offered_by_the_probe_only_with_a_transcriber`: FAIL at `REQUIRE( offer.has_value() )`.

Rebuild once more after the last restore.

- [ ] **Step 7: Update the spec.**

In `docs/spec/ui/chat.md`, add `- [Dictation](#dictation)` to the contents after "Conversations", and a section after
"Conversations":

```markdown
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
```

- [ ] **Step 8: Commit.**

```bash
git add include/morph/ui/chat/transcriber.hpp include/morph/ui/testing/fake_transcriber.hpp include/morph/ui/chat/service.hpp \
        src/ui/chat_service.cpp CMakeLists.txt tests/CMakeLists.txt tests/test_chat_transcription.cpp \
        tests/qt/test_qt_keywords.cpp docs/spec/ui/chat.md
git commit -m "$(cat <<'MSG'
Accept a dictated clip in the chat service and transcribe it on the worker

The clip is checked against its bounds before it is decoded, accepted at once,
and its text read from the state the transcription event points at.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznwgG54dkVGoxXz2Z4
MSG
)"
```

---

## Task 4: Every transcription ends exactly once

**Files:**
- Modify: `src/ui/chat_service.cpp:156-166` (`endChat`), the new `Shared` functions from Task 3 (add `armTranscriptionDeadline`, `stopTranscriptionsWhere`; call the first from `startTranscription`), `:1051-1060` (destructor), `:1106-1108` (`connectionClosed`)
- Test: `tests/test_chat_transcription.cpp` (seven cases at the end, one helper struct)
- Docs: `docs/spec/ui/chat.md` ("Dictation", second half)

**Interfaces:**
- Consumes: Task 3's `Shared::endTranscription`, `CancelCall`, `Tell`, `Shared::timers`, `ChatLimits::toolCallTimeout`, `ChatLimits::transcriptionRetention`, `ChatOptions::now`.
- Produces: `static void Shared::armTranscriptionDeadline(std::shared_ptr<Shared> const&, TranscriptionPtr const&);` and `template <typename Which> static void Shared::stopTranscriptionsWhere(std::shared_ptr<Shared> const&, Which const&, std::string const& reason, Tell tell);`. Behaviour: the deadline fails an entry with "the transcription did not answer in time" and cancels its call; `ui-chat-end` fails the principal's entries on that connection with "cancelled" and cancels them; a closing connection cancels its entries silently and drops them; destruction cancels every call and sends nothing.

- [ ] **Step 1: Write the failing tests.**

Append to `tests/test_chat_transcription.cpp`:

```cpp
namespace {

// A transcriber the test answers by hand, whenever it likes, even after the service cancelled the call: a real one
// can deliver while a cancel is on its way. `called` hands the sink over, since a kept sink would keep the service.
struct ManualTranscriber final : chat::ITranscriber {
    struct Call final : chat::ITranscriberCall {
        explicit Call(std::shared_ptr<std::atomic<bool>> flag) : cancelled{std::move(flag)} {}
        void cancel() override { cancelled->store(true); }
        std::shared_ptr<std::atomic<bool>> cancelled;
    };

    [[nodiscard]] std::shared_ptr<chat::ITranscriberCall> transcribe(chat::TranscriptionInput /*input*/,
                                                                     chat::TranscriberSink given) override {
        std::scoped_lock const lock{mutex};
        sink = std::move(given);
        return std::make_shared<Call>(cancelled);
    }

    [[nodiscard]] chat::TranscriberSink called() {
        REQUIRE(waitFor([&] {
            std::scoped_lock const lock{mutex};
            return sink.has_value();
        }));
        std::scoped_lock const lock{mutex};
        return *std::exchange(sink, std::nullopt);
    }

    std::mutex mutex;
    std::optional<chat::TranscriberSink> sink;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
};

std::size_t eventsFor(Rig& rig, std::uint64_t id) {
    std::size_t count = 0;
    for (chat::TranscriptionState const& told : rig.eventWire.transcriptions()) {
        count += told.id == id ? 1U : 0U;
    }
    return count;
}

}  // namespace

// Mutation: end the entry on the deadline without cancelling its call.
TEST_CASE("chat_transcription_deadline_fails_and_cancels", "[chat][dictation]") {
    chat::ChatLimits limits;
    limits.toolCallTimeout = 150ms;
    Rig rig{limits};
    rig.fake->hold(true);
    auto const cid = rig.gui();
    auto const id = rig.transcribe(kClip, cid);
    auto const state = rig.settled(id, cid);
    CHECK(state.status == chat::TranscriptionStatus::Failed);
    CHECK(state.error == "the transcription did not answer in time");
    CHECK(rig.fake->cancelled() == 1);
    REQUIRE(waitFor([&] { return eventsFor(rig, id) == 1; }));
}

// Mutation: end the entry on `ui-chat-end` without cancelling its call, or stop every connection's.
TEST_CASE("chat_transcription_is_cancelled_by_ending_the_chat", "[chat][dictation]") {
    Rig rig;
    rig.fake->hold(true);
    auto const cid = rig.gui();
    auto const elsewhere = rig.gui();
    auto const id = rig.transcribe(kClip, cid);
    auto const other = rig.transcribe(kClip, elsewhere);
    wire::Envelope end;
    end.kind = std::string{chat::kEndKind};
    REQUIRE(rig.call(std::move(end), cid).kind == "ok");
    auto const state = rig.settled(id, cid);
    CHECK(state.status == chat::TranscriptionStatus::Failed);
    CHECK(state.error == "cancelled");
    CHECK(rig.fake->cancelled() == 1);
    CHECK(rig.state(other, elsewhere).status == chat::TranscriptionStatus::Running);
    rig.fake->release();
}

// Mutation: keep a closed connection's entry, or leave its call running.
TEST_CASE("chat_transcription_is_dropped_with_its_connection", "[chat][dictation]") {
    Rig rig;
    rig.fake->hold(true);
    auto const cid = rig.gui();
    auto const id = rig.transcribe(kClip, cid);
    rig.server->closeConnection(cid);
    REQUIRE(waitFor([&] { return rig.fake->cancelled() == 1; }));
    auto const after = rig.state(id, rig.gui());
    CHECK(after.status == chat::TranscriptionStatus::Failed);
    CHECK(after.error == "no such transcription");
    CHECK(eventsFor(rig, id) == 0);
}

// Mutation: let a later answer overwrite an ended entry.
TEST_CASE("chat_transcription_answer_after_the_end_changes_nothing", "[chat][dictation]") {
    chat::ChatLimits limits;
    limits.toolCallTimeout = 150ms;
    auto const manual = std::make_shared<ManualTranscriber>();
    Rig rig{limits, manual};
    auto const cid = rig.gui();
    auto const id = rig.transcribe(kClip, cid);
    auto sink = manual->called();
    CHECK(rig.settled(id, cid).error == "the transcription did not answer in time");
    REQUIRE(waitFor([&] { return eventsFor(rig, id) == 1; }));
    sink.done(std::string{"too late"});
    auto const after = rig.state(id, cid);
    CHECK(after.status == chat::TranscriptionStatus::Failed);
    CHECK_FALSE(after.text.has_value());
    morph::testing::storageOwner().runFor(20ms);
    CHECK(eventsFor(rig, id) == 1);
}

// Mutation: leave the calls running when the service is destroyed, or announce them.
TEST_CASE("chat_transcription_answer_after_the_service_is_gone", "[chat][dictation]") {
    auto const manual = std::make_shared<ManualTranscriber>();
    Rig rig{chat::ChatLimits{}, manual};
    auto const cid = rig.gui();
    static_cast<void>(rig.transcribe(kClip, cid));
    auto sink = manual->called();
    std::size_t const before = rig.eventWire.transcriptions().size();
    rig.server.reset();
    rig.service.reset();
    CHECK(manual->cancelled->load());
    sink.done(std::string{"nobody listens"});
    CHECK(rig.eventWire.transcriptions().size() == before);
}

// Mutation: never forget a finished entry, or forget it at once.
TEST_CASE("chat_transcription_is_forgotten_after_its_retention", "[chat][dictation]") {
    chat::ChatLimits limits;
    limits.transcriptionRetention = std::chrono::seconds{60};
    Rig rig{limits};
    auto const cid = rig.gui();
    rig.fake->script(std::string{"kept a while"});
    auto const id = rig.transcribe(kClip, cid);
    CHECK(rig.settled(id, cid).text == "kept a while");
    rig.clock->fetch_add(59);
    CHECK(rig.state(id, cid).text == "kept a while");
    rig.clock->fetch_add(2);
    CHECK(rig.state(id, cid).error == "no such transcription");
}

// Mutation: add the transcript to the conversation.
TEST_CASE("chat_transcription_leaves_the_conversation_alone", "[chat][dictation]") {
    Rig rig;
    auto const cid = rig.gui();
    rig.fake->script(std::string{"heard words"});
    static_cast<void>(rig.settled(rig.transcribe(kClip, cid), cid));
    rig.provider->script(chat::Response{.text = "ok", .calls = {}, .opaque = {}});
    chat::TurnRequest turn;
    turn.screenId = "s";
    turn.text = "typed words";
    wire::Envelope start;
    start.kind = std::string{chat::kTurnKind};
    start.body = chat::writeMessage(turn);
    auto const accepted = chat::readTurnAccepted(rig.call(std::move(start), cid).body);
    REQUIRE(accepted.has_value());
    REQUIRE(waitFor([&] { return rig.provider->calls() == 1; }));
    auto const requests = rig.provider->requests();
    REQUIRE(requests.front().messages.size() == 1);
    CHECK(requests.front().messages.front().text == "typed words");
    CHECK_FALSE(requests.front().system.contains("heard words"));
}
```

- [ ] **Step 2: Run them and expect FAIL.**

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_tests
ctest --test-dir build -R '^chat_transcription_' --output-on-failure
```

Expected: `chat_transcription_deadline_fails_and_cancels` FAILs at `REQUIRE( waitFor(...) )` inside `settled` (no
deadline yet, the entry stays `running`); `chat_transcription_is_cancelled_by_ending_the_chat`,
`chat_transcription_is_dropped_with_its_connection`, `chat_transcription_answer_after_the_end_changes_nothing` and
`chat_transcription_answer_after_the_service_is_gone` FAIL likewise. The retention and conversation cases pass already;
they guard Task 3's sweep and the service's isolation from the conversation store.

- [ ] **Step 3: Implement the endings.**

In `src/ui/chat_service.cpp`, add to `Shared` after `callTranscriber`:

```cpp
    // Fails the entry when the transcriber has not answered within `toolCallTimeout`, and cancels its call. The timer
    // holds both weakly and hands the ending to the worker, as a turn's deadline does.
    static void armTranscriptionDeadline(std::shared_ptr<Shared> const& self, TranscriptionPtr const& entry) {
        auto const limit = self->options.limits.toolCallTimeout;
        if (limit.count() <= 0) {
            return;
        }
        // NOLINTNEXTLINE(misc-const-correctness): `schedule` is not const.
        ::morph::async::detail::TimeoutScheduler* timers = nullptr;
        {
            std::scoped_lock const lock{self->mutex};
            if (self->timers == nullptr) {
                self->timers = std::make_unique<::morph::async::detail::TimeoutScheduler>();
            }
            timers = self->timers.get();
        }
        auto const handle = timers->schedule(
            limit, [weakSelf = std::weak_ptr<Shared>{self}, weakEntry = std::weak_ptr<Transcription>{entry},
                    worker = self->options.worker] {
                auto lockedSelf = weakSelf.lock();
                auto lockedEntry = weakEntry.lock();
                if (lockedSelf == nullptr || lockedEntry == nullptr) {
                    return;
                }
                try {
                    worker->post([lockedSelf = std::move(lockedSelf), lockedEntry = std::move(lockedEntry)] {
                        endTranscription(lockedSelf, lockedEntry,
                                         std::unexpected(std::string{"the transcription did not answer in time"}),
                                         CancelCall::Yes, Tell::Yes);
                    });
                    // NOLINTNEXTLINE(bugprone-empty-catch): the worker is gone, so the service is being torn down.
                } catch (...) {
                }
            });
        std::optional<::morph::async::detail::TimeoutScheduler::Handle> stale;
        {
            std::scoped_lock const lock{self->mutex};
            if (entry->status == TranscriptionStatus::Running) {
                entry->deadline = handle;
            } else {
                stale = handle;
            }
        }
        cancelTimer(*self, stale);
    }

    // Ends each running transcription @p which picks, cancelling its call. Never blocks: a service callback runs on
    // the server strand.
    template <typename Which>
    static void stopTranscriptionsWhere(std::shared_ptr<Shared> const& self, Which const& which,
                                        std::string const& reason, Tell tell) {
        std::vector<TranscriptionPtr> matched;
        {
            std::scoped_lock const lock{self->mutex};
            for (auto const& [id, held] : self->transcriptions) {
                static_cast<void>(id);
                if (held->status == TranscriptionStatus::Running && which(*held)) {
                    matched.push_back(held);
                }
            }
        }
        for (TranscriptionPtr const& entry : matched) {
            endTranscription(self, entry, std::unexpected(reason), CancelCall::Yes, tell);
        }
    }
```

In `startTranscription`, after the `try { self->options.worker->post(...) } catch (...) { ... }` block and before the
`return`, add `armTranscriptionDeadline(self, entry);`.

In `endChat`, after the `stopWhere(...)` line:

```cpp
        stopTranscriptionsWhere(
            self, [&](Transcription const& entry) { return entry.cid == cid && entry.principal == principal; },
            "cancelled", Tell::Yes);
```

Make the destructor's `try` body:

```cpp
        Shared::stopWhere(_shared, [](Turn const&) { return true; });
        // Nobody is left to read a transcription, so it is cancelled and announced to no one.
        Shared::stopTranscriptionsWhere(
            _shared, [](Transcription const&) { return true; }, "the chat service stopped", Shared::Tell::No);
```

Make `connectionClosed`:

```cpp
void ChatService::connectionClosed(backend::ConnectionId cid) {
    Shared::stopWhere(_shared, [cid](Turn const& turn) { return turn.cid == cid; });
    // No one can read a closed connection's transcriptions: they are cancelled silently and forgotten.
    Shared::stopTranscriptionsWhere(
        _shared, [cid](Transcription const& entry) { return entry.cid == cid; }, "the connection closed",
        Shared::Tell::No);
    std::scoped_lock const lock{_shared->mutex};
    std::erase_if(_shared->transcriptions, [cid](auto const& held) { return held.second->cid == cid; });
}
```

- [ ] **Step 4: Run them and expect PASS.**

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_tests
ctest --test-dir build -R '^chat_transcription_|^chat_dictation_' --output-on-failure --repeat until-fail:5
```

Expected: all fifteen cases pass five times in a row (the deadline cases race a timer against the worker).

- [ ] **Step 5: Mutation checks (cancel on the deadline and on `ui-chat-end`).**

```bash
cp src/ui/chat_service.cpp /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/chat_service.cpp.bak
```

1. In `armTranscriptionDeadline`, change `CancelCall::Yes` to `CancelCall::No`. Rebuild, run
   `ctest --test-dir build -R 'chat_transcription_deadline_fails_and_cancels' --output-on-failure`: FAIL at
   `CHECK( rig.fake->cancelled() == 1 )`. Restore with `cp` and `touch src/ui/chat_service.cpp`.
2. In `stopTranscriptionsWhere`, change `CancelCall::Yes` to `CancelCall::No`. Rebuild, run
   `ctest --test-dir build -R 'chat_transcription_is_cancelled_by_ending_the_chat' --output-on-failure`: FAIL at
   `CHECK( rig.fake->cancelled() == 1 )`. Restore the same way and rebuild.

- [ ] **Step 6: Update the spec.**

Append to the "Dictation" section of `docs/spec/ui/chat.md`:

```markdown
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
```

- [ ] **Step 7: Commit.**

```bash
git add src/ui/chat_service.cpp tests/test_chat_transcription.cpp docs/spec/ui/chat.md
git commit -m "$(cat <<'MSG'
End every transcription exactly once: answer, deadline, chat end or close

The deadline and ui-chat-end cancel the call and say why; a closed connection's
entries are cancelled silently and dropped; the service's destruction cancels
every call and announces none.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznwgG54dkVGoxXz2Z4
MSG
)"
```

---

## Task 5: The recorder seam and the pure audio functions

**Files:**
- Create: `include/morph/ui/chat/dictation.hpp`, `tests/test_chat_dictation_audio.cpp`
- Modify: `CMakeLists.txt:678-687` (header file set), `tests/CMakeLists.txt:39` (source list), `tests/qt/test_qt_keywords.cpp:11-14` (includes)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces (namespace `morph::ui::chat`):
  - `inline constexpr int kSpeechSampleRate = 16000;` and `inline constexpr std::size_t kWavHeaderBytes = 44;`
  - `struct DictationLimits { std::chrono::seconds maxSeconds{0}; std::size_t maxBytes = 0; };`
  - `class IDictationRecorder` with `unavailable() const -> std::optional<std::string>`, `start(DictationLimits)`, `stop()`, and the callbacks `onStarted` (`std::function<void(std::expected<void, std::string>)>`), `onProgress` (`std::function<void(std::chrono::seconds)>`), `onClip` (`std::function<void(std::expected<std::string, std::string>)>`)
  - `[[nodiscard]] std::string wavFromPcm16(std::span<std::int16_t const> samples);`
  - `class SpeechResampler { public: SpeechResampler(int channels, int sampleRate); [[nodiscard]] std::vector<std::int16_t> push(std::span<float const> interleaved); };`
  - `[[nodiscard]] std::vector<std::int16_t> resampleToSpeech(std::span<float const> interleaved, int channels, int sampleRate);`
  - `enum class ClipState : std::uint8_t { Recording, Full };`
  - `class DictationClip { public: DictationClip(DictationLimits limits, std::chrono::steady_clock::time_point start); ClipState append(std::span<std::int16_t const> samples, std::chrono::steady_clock::time_point now); [[nodiscard]] std::chrono::seconds elapsed(std::chrono::steady_clock::time_point now) const; [[nodiscard]] std::size_t samples() const noexcept; [[nodiscard]] std::string wav() const; };`

- [ ] **Step 1: Write the failing tests.**

Create `tests/test_chat_dictation_audio.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// What a dictated clip is made of: 16 kHz mono 16-bit PCM in a WAV container, resampled from whatever the device gives,
// and stopped at the server's bounds.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <morph/ui/chat/dictation.hpp>
#include <numbers>
#include <string>
#include <vector>

namespace chat = morph::ui::chat;
using namespace std::chrono_literals;

namespace {

std::uint32_t le32(std::string const& bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes.at(at + index))) << (8U * index);
    }
    return value;
}

std::uint16_t le16(std::string const& bytes, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes.at(at)) |
                                      (static_cast<unsigned>(static_cast<unsigned char>(bytes.at(at + 1))) << 8U));
}

}  // namespace

// Mutation: write the data size into the RIFF size, or the sample rate into the byte rate.
TEST_CASE("chat_audio_wav_header_for_pcm16_speech", "[chat][dictation]") {
    std::array<std::int16_t, 3> const samples{1, -2, 32767};
    std::string const wav = chat::wavFromPcm16(samples);
    REQUIRE(wav.size() == chat::kWavHeaderBytes + 6);
    CHECK(wav.substr(0, 4) == "RIFF");
    CHECK(le32(wav, 4) == 36 + 6);
    CHECK(wav.substr(8, 8) == "WAVEfmt ");
    CHECK(le32(wav, 16) == 16);
    CHECK(le16(wav, 20) == 1);
    CHECK(le16(wav, 22) == 1);
    CHECK(le32(wav, 24) == 16000);
    CHECK(le32(wav, 28) == 32000);
    CHECK(le16(wav, 32) == 2);
    CHECK(le16(wav, 34) == 16);
    CHECK(wav.substr(36, 4) == "data");
    CHECK(le32(wav, 40) == 6);
    CHECK(static_cast<std::int16_t>(le16(wav, 44)) == 1);
    CHECK(static_cast<std::int16_t>(le16(wav, 46)) == -2);
    CHECK(static_cast<std::int16_t>(le16(wav, 48)) == 32767);
    std::vector<std::int16_t> const minute(static_cast<std::size_t>(chat::kSpeechSampleRate) * 60, 0);
    CHECK(chat::wavFromPcm16(minute).size() == 1920044);
}

// Mutation: pick every third sample's neighbour, or forget to divide the channels' sum.
TEST_CASE("chat_audio_resample_48k_stereo_to_16k_mono", "[chat][dictation]") {
    constexpr std::size_t kFrames = 4800;
    std::vector<float> stereo;
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        auto const value = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(frame) / 48000.0));
        stereo.push_back(value);
        stereo.push_back(value);
    }
    std::vector<std::int16_t> const speech = chat::resampleToSpeech(stereo, 2, 48000);
    REQUIRE(speech.size() == kFrames / 3);
    for (std::size_t index = 0; index < speech.size(); ++index) {
        // Each output averages input frames 3k..3k+2, centred on 3k+1.
        double const expected =
            0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>((3 * index) + 1) / 48000.0) * 32767.0;
        CHECK(std::abs(static_cast<double>(speech.at(index)) - expected) < 200.0);
    }
}

// Mutation: take the left channel alone.
TEST_CASE("chat_audio_resample_mixes_channels_and_passes_16k_through", "[chat][dictation]") {
    std::vector<float> const opposed{0.5F, -0.5F, 0.5F, -0.5F, 0.5F, -0.5F};
    for (std::int16_t const sample : chat::resampleToSpeech(opposed, 2, 16000)) {
        CHECK(sample == 0);
    }
    std::vector<float> const mono{0.0F, 0.25F, -0.25F, 1.0F, -1.0F};
    std::vector<std::int16_t> const passed = chat::resampleToSpeech(mono, 1, 16000);
    CHECK(passed == std::vector<std::int16_t>{0, 8192, -8192, 32767, -32767});
}

// Mutation: count output positions from each chunk's start rather than from the first sample.
TEST_CASE("chat_audio_resample_in_chunks_matches_one_pass", "[chat][dictation]") {
    std::vector<float> input;
    for (int index = 0; index < 4410; ++index) {
        input.push_back(static_cast<float>(std::sin(static_cast<double>(index) / 7.0) * 0.8));
    }
    std::vector<std::int16_t> const whole = chat::resampleToSpeech(input, 1, 44100);
    chat::SpeechResampler chunked{1, 44100};
    std::vector<std::int16_t> pieces;
    for (std::size_t at = 0; at < input.size(); at += 100) {
        std::size_t const count = std::min<std::size_t>(100, input.size() - at);
        auto const out = chunked.push(std::span<float const>{input}.subspan(at, count));
        pieces.insert(pieces.end(), out.begin(), out.end());
    }
    CHECK(pieces == whole);
}

// Mutation: let the clip pass its byte bound, or check the bound only once the clip is over it.
TEST_CASE("chat_audio_clip_stops_at_its_byte_bound", "[chat][dictation]") {
    auto const start = std::chrono::steady_clock::time_point{};
    chat::DictationClip clip{chat::DictationLimits{.maxSeconds = 60s, .maxBytes = chat::kWavHeaderBytes + 200}, start};
    std::vector<std::int16_t> const sixty(60, 7);
    CHECK(clip.append(sixty, start + 100ms) == chat::ClipState::Recording);
    CHECK(clip.append(sixty, start + 200ms) == chat::ClipState::Full);
    CHECK(clip.samples() == 100);
    CHECK(clip.wav().size() == chat::kWavHeaderBytes + 200);
    CHECK(clip.append(sixty, start + 300ms) == chat::ClipState::Full);
    CHECK(clip.samples() == 100);
}

// Mutation: measure the time bound from the last append rather than from the start.
TEST_CASE("chat_audio_clip_stops_at_its_time_bound", "[chat][dictation]") {
    auto const start = std::chrono::steady_clock::time_point{};
    chat::DictationClip clip{chat::DictationLimits{.maxSeconds = 2s, .maxBytes = 1U << 20U}, start};
    std::vector<std::int16_t> const some(10, 1);
    CHECK(clip.append(some, start + 1s) == chat::ClipState::Recording);
    CHECK(clip.elapsed(start + 1500ms) == 1s);
    CHECK(clip.append({}, start + 2s) == chat::ClipState::Full);
    CHECK(clip.samples() == 10);
}
```

In `tests/CMakeLists.txt`, add `test_chat_dictation_audio.cpp` after `test_chat_transcription.cpp`. In
`tests/qt/test_qt_keywords.cpp`, add `#include <morph/ui/chat/dictation.hpp>` after `#include <morph/ui/chat/client.hpp>`.

- [ ] **Step 2: Run them and expect FAIL.**

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_tests
```

Expected: `fatal error: 'morph/ui/chat/dictation.hpp' file not found`.

- [ ] **Step 3: Implement.**

Create `include/morph/ui/chat/dictation.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

/// @file
/// @brief What records a dictated clip, and the pure functions that make one: 16 kHz mono 16-bit PCM in a WAV
///        container, whatever the device offers, stopped at the server's bounds. Specified in `docs/spec/ui/chat.md`.

namespace morph::ui::chat {

/// @brief The sample rate a dictated clip has. At 16 kHz mono 16-bit a clip's size is a function of its length alone,
///        so the server's bounds hold on every device.
inline constexpr int kSpeechSampleRate = 16000;

/// @brief The size of the WAV header `wavFromPcm16` writes.
inline constexpr std::size_t kWavHeaderBytes = 44;

/// @brief The bounds a clip stops at, as the server's probe offers them.
struct DictationLimits {
    /// @brief The longest clip.
    std::chrono::seconds maxSeconds{0};
    /// @brief The largest clip, WAV header included.
    std::size_t maxBytes = 0;
};

/// @brief Records a dictated clip from the device's microphone.
///
/// Each `start` ends in exactly one of: `onStarted` with an error, when recording could not begin (nothing follows);
/// or `onClip`, possibly preceded by `onStarted` with success. A `stop` before recording began cancels the start, and
/// is answered by `onClip` with an error. The callbacks run on the thread that owns the recorder. The interface sits in
/// the UI layer, so what drives it is tested without a toolkit.
class IDictationRecorder {
public:
    IDictationRecorder() = default;
    IDictationRecorder(IDictationRecorder const&) = delete;
    IDictationRecorder& operator=(IDictationRecorder const&) = delete;
    IDictationRecorder(IDictationRecorder&&) = delete;
    IDictationRecorder& operator=(IDictationRecorder&&) = delete;
    virtual ~IDictationRecorder() = default;

    /// @brief What is known now, without asking, that keeps it from recording, such as no input device. It is not a
    ///        promise that `start` will succeed: permission is asked only by `start`.
    /// @return Why it cannot record, or nothing.
    [[nodiscard]] virtual std::optional<std::string> unavailable() const = 0;

    /// @brief Starts recording, asking for permission first where the platform does. Answers through `onStarted`.
    /// @param limits Where the clip stops by itself.
    virtual void start(DictationLimits limits) = 0;

    /// @brief Ends the clip; `onClip` follows.
    virtual void stop() = 0;

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): set by whoever drives the recorder.
    /// @brief Recording began, or why it did not.
    std::function<void(std::expected<void, std::string>)> onStarted;
    /// @brief The seconds recorded so far, as they pass.
    std::function<void(std::chrono::seconds)> onProgress;
    /// @brief The clip as WAV bytes, or why there is none.
    std::function<void(std::expected<std::string, std::string>)> onClip;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)
};

/// @brief 16 kHz mono 16-bit samples as a WAV file.
/// @param samples The samples.
/// @return The file's bytes: a 44-byte header, then the samples little-endian.
[[nodiscard]] inline std::string wavFromPcm16(std::span<std::int16_t const> samples) {
    std::string out;
    out.reserve(kWavHeaderBytes + (samples.size() * 2));
    auto const put32 = [&out](std::uint32_t const value) {
        for (unsigned shift = 0; shift < 32U; shift += 8U) {
            out.push_back(static_cast<char>((value >> shift) & 0xFFU));
        }
    };
    auto const put16 = [&out](std::uint16_t const value) {
        out.push_back(static_cast<char>(value & 0xFFU));
        out.push_back(static_cast<char>((static_cast<unsigned>(value) >> 8U) & 0xFFU));
    };
    auto const data = static_cast<std::uint32_t>(samples.size() * 2);
    out += "RIFF";
    put32(36U + data);
    out += "WAVEfmt ";
    put32(16U);
    put16(1U);
    put16(1U);
    put32(static_cast<std::uint32_t>(kSpeechSampleRate));
    put32(static_cast<std::uint32_t>(kSpeechSampleRate) * 2U);
    put16(2U);
    put16(16U);
    out += "data";
    put32(data);
    for (std::int16_t const sample : samples) {
        put16(static_cast<std::uint16_t>(sample));
    }
    return out;
}

/// @brief Turns a device's samples, at any rate and channel count, into 16 kHz mono 16-bit, one chunk at a time.
///
/// Channels are averaged. Down-sampling averages the input samples one output sample spans, a box filter that keeps
/// most of what lies above the new Nyquist frequency out; up-sampling interpolates between neighbours. Every position
/// is computed from the output sample's index and the samples consumed, never accumulated, so chunks give exactly what
/// one pass would.
class SpeechResampler {
public:
    /// @brief A resampler for one device format.
    /// @param channels   The device's channel count; less than 1 counts as 1.
    /// @param sampleRate The device's sample rate; less than 1 counts as `kSpeechSampleRate`.
    SpeechResampler(int channels, int sampleRate)
        : _channels{static_cast<std::size_t>(std::max(channels, 1))},
          _step{static_cast<double>(sampleRate > 0 ? sampleRate : kSpeechSampleRate) / kSpeechSampleRate} {}

    /// @brief Takes the next chunk.
    /// @param interleaved Samples in [-1, 1], frame by frame; a trailing partial frame is ignored.
    /// @return The 16 kHz mono samples the input so far completes.
    [[nodiscard]] std::vector<std::int16_t> push(std::span<float const> interleaved) {
        std::size_t const frames = interleaved.size() / _channels;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            float sum = 0.0F;
            for (std::size_t channel = 0; channel < _channels; ++channel) {
                sum += interleaved[(frame * _channels) + channel];
            }
            _pending.push_back(sum / static_cast<float>(_channels));
        }
        std::vector<std::int16_t> out;
        while (ready()) {
            out.push_back(toPcm16(next()));
            ++_produced;
        }
        // What the next output sample no longer needs.
        auto const done = std::min(static_cast<std::size_t>(startOf(_produced) - _consumed), _pending.size());
        _pending.erase(_pending.begin(), _pending.begin() + static_cast<std::ptrdiff_t>(done));
        _consumed += done;
        return out;
    }

private:
    // Where output sample @p index starts, counted in input samples from the first one ever pushed.
    [[nodiscard]] std::uint64_t startOf(std::uint64_t const index) const {
        return static_cast<std::uint64_t>(std::floor(static_cast<double>(index) * _step));
    }

    [[nodiscard]] bool ready() const {
        std::uint64_t const available = _consumed + _pending.size();
        return _step >= 1.0 ? startOf(_produced + 1) <= available : startOf(_produced) + 1 < available;
    }

    [[nodiscard]] float next() const {
        auto const first = static_cast<std::size_t>(startOf(_produced) - _consumed);
        if (_step >= 1.0) {
            auto const end = std::max(static_cast<std::size_t>(startOf(_produced + 1) - _consumed), first + 1);
            float sum = 0.0F;
            for (std::size_t index = first; index < end; ++index) {
                sum += _pending[index];
            }
            return sum / static_cast<float>(end - first);
        }
        double const position = static_cast<double>(_produced) * _step;
        auto const fraction = static_cast<float>(position - std::floor(position));
        return _pending[first] + ((_pending[first + 1] - _pending[first]) * fraction);
    }

    [[nodiscard]] static std::int16_t toPcm16(float const value) {
        return static_cast<std::int16_t>(std::lround(std::clamp(value, -1.0F, 1.0F) * 32767.0F));
    }

    std::size_t _channels;
    double _step;
    std::uint64_t _produced = 0;
    std::uint64_t _consumed = 0;
    std::vector<float> _pending;
};

/// @brief A whole buffer as 16 kHz mono 16-bit: `SpeechResampler::push` once.
/// @param interleaved Samples in [-1, 1], frame by frame.
/// @param channels    The channel count.
/// @param sampleRate  The sample rate.
/// @return The samples.
[[nodiscard]] inline std::vector<std::int16_t> resampleToSpeech(std::span<float const> interleaved, int channels,
                                                                int sampleRate) {
    SpeechResampler resampler{channels, sampleRate};
    return resampler.push(interleaved);
}

/// @brief Whether a clip can take more.
enum class ClipState : std::uint8_t {
    Recording,  ///< Under both bounds.
    Full,       ///< At a bound: the recorder stops and sends what it has.
};

/// @brief A clip being recorded, which stops at the server's bounds: its time from the start, and the size its WAV
///        would have. The clock is passed in, so the bounds are tested without waiting.
class DictationClip {
public:
    /// @brief An empty clip.
    /// @param limits The bounds.
    /// @param start  When recording began.
    DictationClip(DictationLimits limits, std::chrono::steady_clock::time_point start)
        : _limits{limits},
          _start{start},
          _capacity{limits.maxBytes > kWavHeaderBytes ? (limits.maxBytes - kWavHeaderBytes) / 2 : 0} {}

    /// @brief Adds samples, as many as fit the byte bound.
    /// @param samples 16 kHz mono samples; may be empty, to check the time bound alone.
    /// @param now     The time now.
    /// @return `Full` once a bound is reached; samples past the byte bound are dropped.
    ClipState append(std::span<std::int16_t const> samples, std::chrono::steady_clock::time_point now) {
        if (_full) {
            return ClipState::Full;
        }
        std::size_t const room = _capacity - _samples.size();
        std::size_t const taken = std::min(room, samples.size());
        _samples.insert(_samples.end(), samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(taken));
        _full = _samples.size() >= _capacity || now - _start >= _limits.maxSeconds;
        return _full ? ClipState::Full : ClipState::Recording;
    }

    /// @brief The whole seconds recorded so far.
    /// @param now The time now.
    /// @return They.
    [[nodiscard]] std::chrono::seconds elapsed(std::chrono::steady_clock::time_point now) const {
        return std::chrono::duration_cast<std::chrono::seconds>(now - _start);
    }

    /// @brief The samples held.
    /// @return Their number.
    [[nodiscard]] std::size_t samples() const noexcept { return _samples.size(); }

    /// @brief The clip as a WAV file.
    /// @return Its bytes, at most `DictationLimits::maxBytes`.
    [[nodiscard]] std::string wav() const { return wavFromPcm16(_samples); }

private:
    DictationLimits _limits;
    std::chrono::steady_clock::time_point _start;
    std::size_t _capacity;
    std::vector<std::int16_t> _samples;
    bool _full = false;
};

}  // namespace morph::ui::chat
```

In `CMakeLists.txt`, add `include/morph/ui/chat/dictation.hpp` after `include/morph/ui/chat/conversations.hpp`.

- [ ] **Step 4: Run them and expect PASS.**

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_tests
ctest --test-dir build -R '^chat_audio_' --output-on-failure
```

Expected: six cases pass.

- [ ] **Step 5: Commit.**

```bash
git add include/morph/ui/chat/dictation.hpp tests/test_chat_dictation_audio.cpp CMakeLists.txt tests/CMakeLists.txt \
        tests/qt/test_qt_keywords.cpp
git commit -m "$(cat <<'MSG'
Add the dictation recorder seam and the pure audio functions

A clip is 16 kHz mono 16-bit WAV whatever the device gives, resampled in chunks,
and stops at the server's time and byte bounds under an injected clock.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznwgG54dkVGoxXz2Z4
MSG
)"
```

---

## Task 6: The Qt transcriber, `openai-transcriptions`

**Files:**
- Create: `include/morph/qt/qt_transcriber.hpp`, `src/qt/qt_transcriber.cpp`, `tests/qt/fake_http_server.hpp`, `tests/qt/test_qt_transcriber.cpp`
- Modify: `tests/qt/test_qt_chat_provider.cpp:36-101` (its local `pumpUntil` and `FakeServer` move to the shared header)
- Modify: `CMakeLists.txt:1223-1236` (`morph_qt` headers, `morph_qt_impl` sources), `tests/qt/CMakeLists.txt:41-49` (sources)
- Modify: `examples/lims/src/server/main.cpp:90-107` (build the transcriber from the configuration)
- Docs: `docs/spec/ui/chat.md` ("Providers")

**Interfaces:**
- Consumes: `TranscriptionConfig` (Task 2); `ITranscriber`, `ITranscriberCall`, `TranscriptionInput`, `TranscriberSink` (Task 3); `wavFromPcm16`, `kSpeechSampleRate` (Task 5, live test only).
- Produces: `[[nodiscard]] std::expected<std::shared_ptr<ui::chat::ITranscriber>, std::string> morph::qt::makeQtTranscriber(ui::chat::TranscriptionConfig config, QObject* owner);` and, for tests, `qt_test::FakeHttpServer` and `qt_test::pumpUntil` in `tests/qt/fake_http_server.hpp`.

- [ ] **Step 1: Move the fake server into a shared header.**

Create `tests/qt/fake_http_server.hpp` holding what `tests/qt/test_qt_chat_provider.cpp:36-101` holds today, renamed and
with one accessor added:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

// A local server that speaks enough HTTP for one request and one canned answer, shared by the Qt provider tests. No
// network.

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>

namespace qt_test {

inline bool pumpUntil(std::function<bool()> const& done, std::chrono::milliseconds limit = std::chrono::seconds{10}) {
    auto const deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return true;
}

// Answers the first whole request with a canned response, and keeps what it was sent.
class FakeHttpServer {
public:
    explicit FakeHttpServer(std::string response) : _response{std::move(response)} {
        REQUIRE(_server.listen(QHostAddress::LocalHost));
        QObject::connect(&_server, &QTcpServer::newConnection, &_server, [this] {
            while (QTcpSocket* const socket = _server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] { read(*socket); });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, [this, socket] {
                    disconnected = true;
                    socket->deleteLater();
                });
            }
        });
    }

    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(static_cast<unsigned>(_server.serverPort()));
    }

    // Whether the whole request arrived and the canned response went out.
    [[nodiscard]] bool answered() const { return _answered; }

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): what a test sets and reads directly.
    // Hold the response's end, so the test can act during the call.
    bool holdOpen = false;
    bool disconnected = false;
    std::string received;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

private:
    void read(QTcpSocket& socket) {
        received += socket.readAll().toStdString();
        auto const headersEnd = received.find("\r\n\r\n");
        if (headersEnd == std::string::npos) {
            return;
        }
        auto const at = received.find("content-length: ");
        auto const atCapital = received.find("Content-Length: ");
        auto const start = at != std::string::npos ? at : atCapital;
        REQUIRE(start != std::string::npos);
        std::size_t const length = std::stoul(received.substr(start + 16));
        if (received.size() < headersEnd + 4 + length || _answered) {
            return;
        }
        _answered = true;
        socket.write(_response.data(), static_cast<qint64>(_response.size()));
        socket.flush();
        if (!holdOpen) {
            socket.disconnectFromHost();
        }
    }

    QTcpServer _server;
    std::string _response;
    bool _answered = false;
};

}  // namespace qt_test
```

In `tests/qt/test_qt_chat_provider.cpp`, delete `pumpUntil` and `class FakeServer` (lines 36-101) and the includes only
they used (`<QHostAddress>`, `<QTcpServer>`, `<QTcpSocket>`), add `#include "fake_http_server.hpp"` after the system
includes, and inside its anonymous namespace add:

```cpp
using qt_test::pumpUntil;
using FakeServer = qt_test::FakeHttpServer;
```

Build and run the provider suite to show the move changed nothing:

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_qt_tests
QT_QPA_PLATFORM=offscreen ctest --test-dir build -R 'text_deltas|tool_use_stream|replayed_unchanged|system_tools_key|http_error_is|truncated_stream|garbage_event|error_event_is|refusal_is_a_done|token_limit|cancel_aborts_the_request|unknown_name_or_missing' --output-on-failure
```

Expected: all thirteen provider cases pass.

- [ ] **Step 2: Write the failing tests.**

Create `tests/qt/test_qt_transcriber.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The configurable speech-to-text provider, against a local server answering one canned response. No network, except
// the live case, which runs only when asked for by name with a key:
//
//     MORPH_LIVE_STT_KEY=... morph_qt_tests "[.live][dictation]"

#include <QCoreApplication>
#include <QObject>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <memory>
#include <morph/qt/qt_transcriber.hpp>
#include <morph/ui/chat/config.hpp>
#include <morph/ui/chat/dictation.hpp>
#include <morph/ui/chat/transcriber.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fake_http_server.hpp"

namespace chat = morph::ui::chat;
using qt_test::FakeHttpServer;
using qt_test::pumpUntil;

namespace {

constexpr std::string_view kKey = "sk-stt-secret-key";

std::string jsonAnswer(int code, std::string const& reason, std::string const& body) {
    return "HTTP/1.1 " + std::to_string(code) + " " + reason +
           "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
}

chat::TranscriptionConfig configFor(std::string url) {
    return chat::TranscriptionConfig{.name = "openai-transcriptions",
                                     .baseUrl = std::move(url),
                                     .model = "whisper-1",
                                     .apiKey = std::string{kKey},
                                     .language = std::nullopt};
}

// The body of the multipart field @p name, or nothing when the request has none.
std::optional<std::string> partValue(std::string const& request, std::string const& name) {
    auto const at = request.find("name=\"" + name + "\"");
    if (at == std::string::npos) {
        return std::nullopt;
    }
    auto const start = request.find("\r\n\r\n", at);
    if (start == std::string::npos) {
        return std::nullopt;
    }
    auto const end = request.find("\r\n--", start + 4);
    return request.substr(start + 4, end == std::string::npos ? std::string::npos : end - start - 4);
}

struct Rig {
    QObject owner;
    std::unique_ptr<FakeHttpServer> server;
    std::shared_ptr<chat::ITranscriber> transcriber;
    std::optional<std::expected<std::string, std::string>> done;

    explicit Rig(std::string response) : server{std::make_unique<FakeHttpServer>(std::move(response))} {
        auto made = morph::qt::makeQtTranscriber(configFor(server->url()), &owner);
        REQUIRE(made.has_value());
        transcriber = *made;
    }

    std::shared_ptr<chat::ITranscriberCall> call(std::optional<std::string> language = std::nullopt) {
        return transcriber->transcribe(
            chat::TranscriptionInput{
                .audio = std::string{"RIFF-clip-bytes"}, .mediaType = "audio/wav", .language = std::move(language)},
            chat::TranscriberSink{
                .done = [this](std::expected<std::string, std::string> outcome) { done = std::move(outcome); }});
    }

    void finish() { REQUIRE(pumpUntil([this] { return done.has_value(); })); }
};

}  // namespace

// Mutation: leave out a part, or send the key in another header.
TEST_CASE("qt_transcriber_posts_the_clip_model_format_and_key", "[qt][dictation]") {
    Rig rig{jsonAnswer(200, "OK", R"({"text":"hello there"})")};
    static_cast<void>(rig.call());
    rig.finish();
    REQUIRE(rig.done->has_value());
    CHECK(**rig.done == "hello there");
    std::string const& sent = rig.server->received;
    CHECK(sent.starts_with("POST /v1/audio/transcriptions "));
    CHECK(sent.contains("Bearer " + std::string{kKey}));
    CHECK(sent.contains(R"(name="file"; filename="audio.wav")"));
    CHECK(partValue(sent, "file") == "RIFF-clip-bytes");
    CHECK(partValue(sent, "model") == "whisper-1");
    CHECK(partValue(sent, "response_format") == "json");
    CHECK_FALSE(partValue(sent, "language").has_value());
}

// Mutation: always send a language part, or never.
TEST_CASE("qt_transcriber_sends_a_language_only_when_given", "[qt][dictation]") {
    Rig rig{jsonAnswer(200, "OK", R"({"text":"hallo"})")};
    static_cast<void>(rig.call(std::string{"de"}));
    rig.finish();
    CHECK(partValue(rig.server->received, "language") == "de");
}

// Mutation: put the request or the configuration in the reason.
TEST_CASE("qt_transcriber_fails_with_the_providers_message_not_the_key", "[qt][dictation]") {
    Rig rig{jsonAnswer(401, "Unauthorized", R"({"error":{"message":"bad credentials","type":"invalid_request_error"}})")};
    static_cast<void>(rig.call());
    rig.finish();
    REQUIRE_FALSE(rig.done->has_value());
    CHECK(rig.done->error().contains("401"));
    CHECK(rig.done->error().contains("bad credentials"));
    CHECK_FALSE(rig.done->error().contains(kKey));
}

// Mutation: answer an unreadable body with an empty text.
TEST_CASE("qt_transcriber_fails_on_an_unreadable_answer", "[qt][dictation]") {
    Rig rig{jsonAnswer(200, "OK", "not json")};
    static_cast<void>(rig.call());
    rig.finish();
    REQUIRE_FALSE(rig.done->has_value());
    CHECK(rig.done->error() == "the transcription provider sent something unreadable");
}

// Mutation: leave the reply running after a cancel.
TEST_CASE("qt_transcriber_cancel_aborts_and_sends_no_done", "[qt][dictation]") {
    Rig rig{"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{\"te"};
    rig.server->holdOpen = true;
    auto const call = rig.call();
    REQUIRE(pumpUntil([&rig] { return rig.server->answered(); }));
    call->cancel();
    CHECK(pumpUntil([&rig] { return rig.server->disconnected; }));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    CHECK_FALSE(rig.done.has_value());
}

// Mutation: accept any name as the one transcriber there is.
TEST_CASE("qt_transcriber_refuses_a_bad_configuration", "[qt][dictation]") {
    QObject owner;
    auto config = configFor("http://127.0.0.1:1");
    config.name = "no-such-transcriber";
    CHECK_FALSE(morph::qt::makeQtTranscriber(config, &owner).has_value());
    config = configFor("http://127.0.0.1:1");
    config.apiKey.clear();
    CHECK_FALSE(morph::qt::makeQtTranscriber(config, &owner).has_value());
    config = configFor("http://127.0.0.1:1");
    config.model.clear();
    CHECK_FALSE(morph::qt::makeQtTranscriber(config, &owner).has_value());
    CHECK_FALSE(morph::qt::makeQtTranscriber(configFor("http://127.0.0.1:1"), nullptr).has_value());
    CHECK(morph::qt::makeQtTranscriber(configFor(""), &owner).has_value());
}

TEST_CASE("qt_transcriber_live_endpoint", "[.live][qt][dictation]") {
    // NOLINTBEGIN(concurrency-mt-unsafe): read before any thread starts.
    char const* const key = std::getenv("MORPH_LIVE_STT_KEY");
    char const* const url = std::getenv("MORPH_LIVE_STT_URL");
    char const* const model = std::getenv("MORPH_LIVE_STT_MODEL");
    // NOLINTEND(concurrency-mt-unsafe)
    if (key == nullptr) {
        SKIP("MORPH_LIVE_STT_KEY is not set");
    }
    QObject owner;
    auto made = morph::qt::makeQtTranscriber(chat::TranscriptionConfig{.name = "openai-transcriptions",
                                                                       .baseUrl = url != nullptr ? url : "",
                                                                       .model = model != nullptr ? model : "whisper-1",
                                                                       .apiKey = key,
                                                                       .language = "en"},
                                             &owner);
    REQUIRE(made.has_value());
    std::vector<std::int16_t> const second(static_cast<std::size_t>(chat::kSpeechSampleRate), 0);
    std::optional<std::expected<std::string, std::string>> done;
    auto const call = (*made)->transcribe(
        chat::TranscriptionInput{.audio = chat::wavFromPcm16(second), .mediaType = "audio/wav", .language = "en"},
        chat::TranscriberSink{.done = [&done](std::expected<std::string, std::string> outcome) { done = std::move(outcome); }});
    REQUIRE(pumpUntil([&done] { return done.has_value(); }, std::chrono::seconds{60}));
    INFO((done->has_value() ? std::string{} : done->error()));
    CHECK(done->has_value());
}
```

In `tests/qt/CMakeLists.txt`, add `test_qt_transcriber.cpp` after `test_qt_offline_queue.cpp` in `morph_qt_tests`.

- [ ] **Step 3: Run them and expect FAIL.**

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_qt_tests
```

Expected: `fatal error: 'morph/qt/qt_transcriber.hpp' file not found`.

- [ ] **Step 4: Implement.**

Create `include/morph/qt/qt_transcriber.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>
#include <string>

#include "../ui/chat/config.hpp"
#include "../ui/chat/transcriber.hpp"

class QObject;

/// @file
/// @brief The one place that names a speech-to-text provider. Specified in `docs/spec/ui/chat.md`.

namespace morph::qt {

/// @brief The transcriber @p config names. Its one name is `openai-transcriptions`: a multipart `POST` to
///        `{baseUrl}/v1/audio/transcriptions`, which hosted Whisper, self-hosted whisper.cpp and faster-whisper servers,
///        and most gateways speak.
/// @param config What it is reached with. An empty `baseUrl` is the provider's public address.
/// @param owner  A QObject on a thread running a Qt event loop, where its network calls run. Borrowed: it outlives
///               the transcriber. `ITranscriber::transcribe` may be called from any thread.
/// @return The transcriber, or why not: an unknown name, a missing key, model or owner.
[[nodiscard]] std::expected<std::shared_ptr<ui::chat::ITranscriber>, std::string> makeQtTranscriber(
    ui::chat::TranscriptionConfig config, QObject* owner);

}  // namespace morph::qt
```

Create `src/qt/qt_transcriber.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <QByteArray>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <atomic>
#include <expected>
#include <memory>
#include <morph/qt/qt_transcriber.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/chat/config.hpp>
#include <morph/ui/chat/transcriber.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace morph::qt {

namespace {

namespace chat = ui::chat;
using ui::Json;

// A transcription makes no progress for this long before it is given up on; the service's own deadline is shorter.
constexpr int kTransferTimeoutMs = 120000;

constexpr std::string_view kPublicAddress = "https://api.openai.com";

[[nodiscard]] Json const* memberOf(Json const& value, std::string_view key) {
    auto const* const object = std::get_if<Json::object_t>(&value.data);
    if (object == nullptr) {
        return nullptr;
    }
    auto const* const found = object->find(key);
    return found == object->end() ? nullptr : &found->second;
}

[[nodiscard]] std::string const* textAt(Json const& value, std::string_view key) {
    Json const* const found = memberOf(value, key);
    return found == nullptr ? nullptr : std::get_if<std::string>(&found->data);
}

[[nodiscard]] QHttpPart field(QString const& name, QByteArray const& value) {
    QHttpPart part;
    part.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant{QStringLiteral("form-data; name=\"%1\"").arg(name)});
    part.setBody(value);
    return part;
}

[[nodiscard]] QHttpPart clip(QByteArray const& audio) {
    QHttpPart part;
    part.setHeader(QNetworkRequest::ContentDispositionHeader,
                   QVariant{QStringLiteral("form-data; name=\"file\"; filename=\"audio.wav\"")});
    part.setHeader(QNetworkRequest::ContentTypeHeader, QVariant{QStringLiteral("audio/wav")});
    part.setBody(audio);
    return part;
}

// One transcription. Runs on the owner's thread, except `cancel`.
class Call final : public chat::ITranscriberCall, public std::enable_shared_from_this<Call> {
public:
    Call(QPointer<QObject> owner, chat::TranscriberSink sink) : _owner{std::move(owner)}, _sink{std::move(sink)} {}

    void cancel() override {
        _cancelled.store(true);
        QPointer<QObject> const owner = _owner;
        if (owner != nullptr) {
            QMetaObject::invokeMethod(
                owner.data(), [self = shared_from_this()] { self->abort(); }, Qt::QueuedConnection);
        }
    }

    void start(QNetworkAccessManager& network, QNetworkRequest const& request, QHttpMultiPart* body) {
        if (_cancelled.load()) {
            delete body;  // NOLINT(cppcoreguidelines-owning-memory): never handed to a reply.
            return;
        }
        _reply = network.post(request, body);
        body->setParent(_reply);
        QObject::connect(_reply, &QNetworkReply::finished, _reply, [self = shared_from_this()] { self->finished(); });
    }

    void fail(std::string reason) {
        if (_ended) {
            return;
        }
        _ended = true;
        if (!_cancelled.load() && _sink.done) {
            _sink.done(std::unexpected(std::move(reason)));
        }
    }

private:
    void abort() {
        _ended = true;
        if (_reply != nullptr) {
            _reply->abort();
        }
    }

    void finished() {
        QPointer<QNetworkReply> const reply = _reply;
        _reply = nullptr;
        if (reply == nullptr) {
            return;
        }
        reply->deleteLater();
        if (_ended) {
            return;
        }
        int const code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code == 0) {
            fail("the transcription provider could not be reached: " + reply->errorString().toStdString());
            return;
        }
        std::string const body = reply->readAll().toStdString();
        auto const parsed = ui::parseJson(body);
        if (code < 200 || code >= 300) {
            std::string message;
            if (parsed) {
                if (Json const* const error = memberOf(*parsed, "error")) {
                    if (std::string const* const text = textAt(*error, "message")) {
                        message = *text;
                    }
                }
            }
            fail("the transcription provider answered " + std::to_string(code) + (message.empty() ? "" : ": " + message));
            return;
        }
        std::string const* const text = parsed ? textAt(*parsed, "text") : nullptr;
        if (text == nullptr) {
            fail("the transcription provider sent something unreadable");
            return;
        }
        _ended = true;
        if (!_cancelled.load() && _sink.done) {
            _sink.done(*text);
        }
    }

    QPointer<QObject> _owner;
    chat::TranscriberSink _sink;
    QPointer<QNetworkReply> _reply;
    std::atomic<bool> _cancelled{false};
    bool _ended = false;
};

class OpenAiTranscriber final : public chat::ITranscriber {
public:
    OpenAiTranscriber(chat::TranscriptionConfig config, QObject* owner)
        : _config{std::move(config)}, _owner{owner}, _network{new QNetworkAccessManager(owner)} {
        if (_config.baseUrl.empty()) {
            _config.baseUrl = std::string{kPublicAddress};
        }
    }

    [[nodiscard]] std::shared_ptr<chat::ITranscriberCall> transcribe(chat::TranscriptionInput input,
                                                                     chat::TranscriberSink sink) override {
        auto call = std::make_shared<Call>(_owner, std::move(sink));
        QPointer<QObject> const owner = _owner;
        if (owner == nullptr) {
            call->fail("the transcription provider has shut down");
            return call;
        }
        QMetaObject::invokeMethod(
            owner.data(),
            [call, network = _network, config = _config, audio = QByteArray::fromStdString(input.audio),
             language = std::move(input.language)] {
                if (network == nullptr) {
                    call->fail("the transcription provider has shut down");
                    return;
                }
                auto* const body = new QHttpMultiPart(QHttpMultiPart::FormDataType);  // NOLINT(cppcoreguidelines-owning-memory)
                body->append(clip(audio));
                body->append(field(QStringLiteral("model"), QByteArray::fromStdString(config.model)));
                body->append(field(QStringLiteral("response_format"), QByteArrayLiteral("json")));
                if (language) {
                    body->append(field(QStringLiteral("language"), QByteArray::fromStdString(*language)));
                }
                QNetworkRequest http{QUrl{QString::fromStdString(config.baseUrl + "/v1/audio/transcriptions")}};
                http.setRawHeader("Authorization", QByteArray::fromStdString("Bearer " + config.apiKey));
                http.setTransferTimeout(kTransferTimeoutMs);
                call->start(*network, http, body);
            },
            Qt::QueuedConnection);
        return call;
    }

private:
    chat::TranscriptionConfig _config;
    QPointer<QObject> _owner;
    QPointer<QNetworkAccessManager> _network;
};

}  // namespace

std::expected<std::shared_ptr<ui::chat::ITranscriber>, std::string> makeQtTranscriber(
    ui::chat::TranscriptionConfig config, QObject* owner) {
    if (config.name != "openai-transcriptions") {
        return std::unexpected("no transcription provider is named `" + config.name + "`");
    }
    if (config.apiKey.empty()) {
        return std::unexpected(std::string{"the transcription provider needs an API key"});
    }
    if (config.model.empty()) {
        return std::unexpected(std::string{"the transcription provider needs a model"});
    }
    if (owner == nullptr) {
        return std::unexpected(std::string{"the transcription provider needs an owner on a Qt thread"});
    }
    return std::make_shared<OpenAiTranscriber>(std::move(config), owner);
}

}  // namespace morph::qt
```

In `CMakeLists.txt`, add `include/morph/qt/qt_transcriber.hpp` after `include/morph/qt/qt_tls.hpp` in the `morph_qt`
file set, and `src/qt/qt_transcriber.cpp` after `src/qt/qt_chat_provider.cpp` in `morph_qt_impl`.

- [ ] **Step 5: Run them and expect PASS.**

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_qt_tests
QT_QPA_PLATFORM=offscreen ctest --test-dir build -R '^qt_transcriber_' --output-on-failure
```

Expected: six cases pass; `qt_transcriber_live_endpoint` is hidden by `[.live]` and not discovered as a ctest case.

- [ ] **Step 6: Build the transcriber in the lims server.**

In `examples/lims/src/server/main.cpp`, add `#include <morph/qt/qt_transcriber.hpp>` beside
`#include <morph/qt/qt_chat_provider.hpp>`, and after `chat.bundle = *bundle;`:

```cpp
            if (chatConfig->transcription) {
                auto transcriber = morph::qt::makeQtTranscriber(*chatConfig->transcription, &providerOwner);
                if (!transcriber) {
                    std::cerr << "lims-server: " << transcriber.error() << '\n';
                    return 1;
                }
                chat.transcriber = *transcriber;
            }
```

This target exists only in a ladder build:

```bash
cmake -S . -B build-ladder -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DMORPH_BUILD_TESTS=OFF \
      -DMORPH_BUILD_QT=ON -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=lims
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build-ladder -j5 --target ladder_lims_server
```

Expected: it links.

- [ ] **Step 7: Update the spec.**

In `docs/spec/ui/chat.md`, "Providers", append:

```markdown
`morph::qt::makeQtTranscriber(config, owner)` in `qt/qt_transcriber.hpp` is the one place that names a speech-to-text
provider. Its one name is `openai-transcriptions`: a multipart `POST` to `{baseUrl}/v1/audio/transcriptions` over
`QNetworkAccessManager` on `owner`'s thread, with the parts `file` (the WAV, as `audio.wav`), `model`,
`response_format=json`, and `language` when the input carries one, and the header `Authorization: Bearer <key>`. It reads
`text`. A non-2xx answer fails with the status and the provider's `error.message` when it has one, never with the
request; an answer without a `text` fails as unreadable. `cancel` aborts the reply. A deployment that keeps audio on its
own network points `baseUrl` at its own server; an empty `baseUrl` is `https://api.openai.com`. `model` and the key have
no default, and the key appears in no log and no message.
```

- [ ] **Step 8: Commit.**

```bash
git add include/morph/qt/qt_transcriber.hpp src/qt/qt_transcriber.cpp tests/qt/fake_http_server.hpp \
        tests/qt/test_qt_transcriber.cpp tests/qt/test_qt_chat_provider.cpp tests/qt/CMakeLists.txt CMakeLists.txt \
        examples/lims/src/server/main.cpp docs/spec/ui/chat.md
git commit -m "$(cat <<'MSG'
Add the Qt transcriber: openai-transcriptions over QNetworkAccessManager

A multipart POST to {baseUrl}/v1/audio/transcriptions that reads `text`, fails
with the provider's message and never the key, and aborts on cancel. The lims
server builds it from the chat configuration's transcription block.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznwgG54dkVGoxXz2Z4
MSG
)"
```

---

## Task 7: `QtDictationRecorder` and `MORPH_ENABLE_DICTATION`

**Files:**
- Create: `include/morph/qt/qt_dictation_recorder.hpp`, `src/qt/qt_dictation_recorder.cpp`, `tests/qt/test_qt_dictation_recorder.cpp`
- Modify: `CMakeLists.txt:1209-1240` (find `Qt6::Multimedia`, the option, the sources), `tests/qt/CMakeLists.txt:41-49`
- Docs: `docs/spec/ui/chat.md` ("Dictation", the recorder paragraph)

**Interfaces:**
- Consumes: `IDictationRecorder`, `DictationLimits`, `SpeechResampler`, `DictationClip`, `ClipState` (Task 5).
- Produces: `class morph::qt::QtDictationRecorder final : public ui::chat::IDictationRecorder` with `QtDictationRecorder();`, `~QtDictationRecorder() override;`, `unavailable() const`, `start(DictationLimits)`, `stop()`. CMake: `option(MORPH_ENABLE_DICTATION ...)`, ON when `Qt6::Multimedia` is found; the compile definition `MORPH_ENABLE_DICTATION=1` on `morph_qt_impl` (PUBLIC) when ON.

- [ ] **Step 1: Write the failing test.**

Create `tests/qt/test_qt_dictation_recorder.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The Qt recorder on whatever audio input this machine has. Without one, `unavailable` names why and `start` answers
// with the same reason; with one, the time bound stops the clip and it arrives as WAV. What the clip is made of is
// tested without Qt in test_chat_dictation_audio.cpp.

#include <QCoreApplication>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <morph/qt/qt_dictation_recorder.hpp>
#include <morph/ui/chat/dictation.hpp>
#include <optional>
#include <string>

#include "fake_http_server.hpp"

using namespace std::chrono_literals;

// Mutation: answer `start` with nothing when there is no device, or let the time bound pass.
TEST_CASE("qt_dictation_recorder_answers_start_and_stop", "[qt][dictation]") {
    morph::qt::QtDictationRecorder recorder;
    std::optional<std::expected<void, std::string>> started;
    std::optional<std::expected<std::string, std::string>> clip;
    recorder.onStarted = [&started](std::expected<void, std::string> outcome) { started = std::move(outcome); };
    recorder.onClip = [&clip](std::expected<std::string, std::string> outcome) { clip = std::move(outcome); };
    std::optional<std::string> const why = recorder.unavailable();
    recorder.start(morph::ui::chat::DictationLimits{.maxSeconds = 1s, .maxBytes = 1U << 20U});
    REQUIRE(qt_test::pumpUntil([&] { return started.has_value() || clip.has_value(); }));
    if (why) {
        REQUIRE(started.has_value());
        REQUIRE_FALSE(started->has_value());
        CHECK(started->error() == *why);
        return;
    }
    if (started && !started->has_value()) {
        WARN("the input device would not open: " << started->error());
        return;
    }
    REQUIRE(qt_test::pumpUntil([&] { return clip.has_value(); }, 10s));
    REQUIRE(clip->has_value());
    CHECK((*clip)->starts_with("RIFF"));
    CHECK((*clip)->size() <= morph::ui::chat::kWavHeaderBytes + 2U * 16000U * 2U);
}
```

In `tests/qt/CMakeLists.txt`, after the `add_executable(morph_qt_tests ...)` block:

```cmake
if(MORPH_ENABLE_DICTATION)
    target_sources(morph_qt_tests PRIVATE test_qt_dictation_recorder.cpp)
endif()
```

- [ ] **Step 2: Run it and expect FAIL.**

```bash
cmake -S . -B build
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_qt_tests
```

Expected: configure leaves `MORPH_ENABLE_DICTATION` undefined, so the test file is not compiled, and
`ctest --test-dir build -R '^qt_dictation_recorder_'` reports `No tests were found`. That is the failure to fix.

- [ ] **Step 3: Implement.**

In `CMakeLists.txt`, inside `if(MORPH_BUILD_QT)`, after `find_package(Qt6 COMPONENTS Network WebSockets REQUIRED)`:

```cmake
    # The chat's dictation recorder needs Qt Multimedia's audio input; everything else about dictation does not.
    find_package(Qt6 COMPONENTS Multimedia QUIET)
    set(_morph_dictation_default OFF)
    if(TARGET Qt6::Multimedia)
        set(_morph_dictation_default ON)
    endif()
    option(MORPH_ENABLE_DICTATION "Build the chat's dictation recorder (needs Qt6::Multimedia)" ${_morph_dictation_default})
    if(MORPH_ENABLE_DICTATION AND NOT TARGET Qt6::Multimedia)
        message(FATAL_ERROR "MORPH_ENABLE_DICTATION=ON needs Qt6::Multimedia, which was not found.")
    endif()
```

After the `morph_qt_impl` `set_target_properties(... AUTOMOC ON)` line:

```cmake
    if(MORPH_ENABLE_DICTATION)
        target_sources(morph_qt INTERFACE FILE_SET HEADERS BASE_DIRS include
                       FILES include/morph/qt/qt_dictation_recorder.hpp)
        target_sources(morph_qt_impl PRIVATE src/qt/qt_dictation_recorder.cpp)
        target_link_libraries(morph_qt_impl PUBLIC Qt6::Multimedia)
        target_compile_definitions(morph_qt_impl PUBLIC MORPH_ENABLE_DICTATION=1)
    endif()
```

Create `include/morph/qt/qt_dictation_recorder.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "../ui/chat/dictation.hpp"

/// @file
/// @brief The chat's dictation recorder over Qt Multimedia. Built when `MORPH_ENABLE_DICTATION` is on. Specified in
///        `docs/spec/ui/chat.md`.

namespace morph::qt {

/// @brief Records a dictated clip from the default audio input with `QAudioSource`, at the device's preferred format,
///        converted to 16 kHz mono 16-bit as it arrives (`SpeechResampler`), and stopped at the server's bounds
///        (`DictationClip`). Asks for microphone permission where the platform has it. Audio stays in memory and is
///        never written to disk.
///
/// Lives on a thread running a Qt event loop, where it is made; its callbacks run there. Neither copyable nor movable.
class QtDictationRecorder final : public ui::chat::IDictationRecorder {
public:
    /// @brief A recorder on the calling thread.
    QtDictationRecorder();
    /// @brief Stops a recording without calling back.
    ~QtDictationRecorder() override;

    QtDictationRecorder(QtDictationRecorder const&) = delete;
    QtDictationRecorder& operator=(QtDictationRecorder const&) = delete;
    QtDictationRecorder(QtDictationRecorder&&) = delete;
    QtDictationRecorder& operator=(QtDictationRecorder&&) = delete;

    /// @brief Why it cannot record now: no input device.
    /// @return The reason, or nothing.
    [[nodiscard]] std::optional<std::string> unavailable() const override;

    /// @brief Asks for permission where the platform does, then records. Ignored while a recording runs.
    /// @param limits Where the clip stops by itself.
    void start(ui::chat::DictationLimits limits) override;

    /// @brief Ends the clip, or cancels a start still waiting for permission; `onClip` follows.
    void stop() override;

private:
    struct Take;

    void begin();
    void refuse(std::string reason);
    void read();
    void tick();
    void finish();

    std::unique_ptr<Take> _take;
    // Tells a permission answer for a start that was stopped from one for the current start.
    std::uint64_t _generation = 0;
};

}  // namespace morph::qt
```

Create `src/qt/qt_dictation_recorder.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QByteArray>
#include <QCoreApplication>
#include <QIODevice>
#include <QMediaDevices>
#include <QMetaObject>
#include <QObject>
#include <QTimer>
#include <QtGlobal>
#if QT_CONFIG(permissions)
#include <QPermissions>
#endif
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <morph/qt/qt_dictation_recorder.hpp>
#include <morph/ui/chat/dictation.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace morph::qt {

namespace {

constexpr std::chrono::milliseconds kTick{250};

constexpr char const* kNoDevice = "no microphone is connected";

// A source is destroyed from its own device's signal when the clip fills, so it goes once control returns.
struct LaterDelete {
    void operator()(QAudioSource* source) const {
        if (source != nullptr) {
            source->stop();
            source->deleteLater();
        }
    }
};

}  // namespace

// One recording, from `start` to its clip or its refusal.
struct QtDictationRecorder::Take {
    explicit Take(ui::chat::DictationLimits given) : limits{given} {}

    ui::chat::DictationLimits limits;
    // Scopes every connection and the permission answer: destroying it disconnects them.
    QObject context;
    std::unique_ptr<QAudioSource, LaterDelete> source;
    QIODevice* input = nullptr;
    QAudioFormat format;
    QByteArray leftover;
    std::optional<ui::chat::SpeechResampler> resampler;
    std::optional<ui::chat::DictationClip> clip;
    QTimer ticker;
    std::chrono::seconds shown{0};
    bool begun = false;
};

QtDictationRecorder::QtDictationRecorder() = default;

QtDictationRecorder::~QtDictationRecorder() = default;

std::optional<std::string> QtDictationRecorder::unavailable() const {
    if (QMediaDevices::defaultAudioInput().isNull()) {
        return std::string{kNoDevice};
    }
    return std::nullopt;
}

void QtDictationRecorder::start(ui::chat::DictationLimits limits) {
    if (_take != nullptr) {
        return;
    }
    _take = std::make_unique<Take>(limits);
    ++_generation;
#if QT_CONFIG(permissions)
    QMicrophonePermission const microphone;
    switch (qApp->checkPermission(microphone)) {
        case Qt::PermissionStatus::Granted:
            break;
        case Qt::PermissionStatus::Denied:
            refuse("the microphone permission was denied");
            return;
        case Qt::PermissionStatus::Undetermined:
            qApp->requestPermission(microphone, &_take->context,
                                    [this, generation = _generation](QPermission const& answer) {
                                        if (_take == nullptr || generation != _generation) {
                                            return;
                                        }
                                        if (answer.status() == Qt::PermissionStatus::Granted) {
                                            begin();
                                        } else {
                                            refuse("the microphone permission was denied");
                                        }
                                    });
            return;
    }
#endif
    begin();
}

void QtDictationRecorder::begin() {
    QAudioDevice const device = QMediaDevices::defaultAudioInput();
    if (device.isNull()) {
        refuse(kNoDevice);
        return;
    }
    Take& take = *_take;
    take.format = device.preferredFormat();
    take.resampler.emplace(take.format.channelCount(), take.format.sampleRate());
    take.clip.emplace(take.limits, std::chrono::steady_clock::now());
    take.source.reset(new QAudioSource(device, take.format));  // NOLINT(cppcoreguidelines-owning-memory)
    take.input = take.source->start();
    if (take.input == nullptr || take.source->error() != QAudio::NoError) {
        refuse("the microphone could not be opened");
        return;
    }
    QObject::connect(take.input, &QIODevice::readyRead, &take.context, [this] { read(); });
    QObject::connect(&take.ticker, &QTimer::timeout, &take.context, [this] { tick(); });
    take.ticker.start(kTick);
    take.begun = true;
    if (onStarted) {
        onStarted({});
    }
}

void QtDictationRecorder::refuse(std::string reason) {
    _take.reset();
    if (onStarted) {
        onStarted(std::unexpected(std::move(reason)));
    }
}

void QtDictationRecorder::read() {
    if (_take == nullptr || _take->input == nullptr) {
        return;
    }
    Take& take = *_take;
    take.leftover += take.input->readAll();
    int const frameBytes = take.format.bytesPerFrame();
    int const sampleBytes = take.format.bytesPerSample();
    if (frameBytes <= 0 || sampleBytes <= 0) {
        return;
    }
    qsizetype const whole = take.leftover.size() - (take.leftover.size() % frameBytes);
    std::vector<float> samples;
    samples.reserve(static_cast<std::size_t>(whole / sampleBytes));
    for (qsizetype at = 0; at < whole; at += sampleBytes) {
        samples.push_back(take.format.normalizedSampleValue(take.leftover.constData() + at));
    }
    take.leftover.remove(0, whole);
    std::vector<std::int16_t> const speech = take.resampler->push(samples);
    if (take.clip->append(speech, std::chrono::steady_clock::now()) == ui::chat::ClipState::Full) {
        finish();
    }
}

void QtDictationRecorder::tick() {
    if (_take == nullptr || !_take->clip) {
        return;
    }
    Take& take = *_take;
    auto const now = std::chrono::steady_clock::now();
    if (take.clip->append({}, now) == ui::chat::ClipState::Full) {
        finish();
        return;
    }
    if (auto const elapsed = take.clip->elapsed(now); elapsed != take.shown) {
        take.shown = elapsed;
        if (onProgress) {
            onProgress(elapsed);
        }
    }
}

void QtDictationRecorder::stop() {
    if (_take == nullptr) {
        return;
    }
    if (!_take->begun) {
        _take.reset();
        ++_generation;
        if (onClip) {
            onClip(std::unexpected(std::string{"the recording was stopped"}));
        }
        return;
    }
    finish();
}

void QtDictationRecorder::finish() {
    // `finish` runs inside the take's own `readyRead` or `timeout`, so the take, whose context receives them, is
    // destroyed once control is back in the event loop.
    std::shared_ptr<Take> const take{std::move(_take)};
    take->ticker.stop();
    QObject::disconnect(take->input, nullptr, &take->context, nullptr);
    QObject::disconnect(&take->ticker, nullptr, &take->context, nullptr);
    take->source.reset();
    QMetaObject::invokeMethod(QCoreApplication::instance(), [take] {}, Qt::QueuedConnection);
    std::string wav = take->clip->wav();
    if (onClip) {
        onClip(std::move(wav));
    }
}

}  // namespace morph::qt
```

- [ ] **Step 4: Run it and expect PASS.**

```bash
cmake -S . -B build
grep MORPH_ENABLE_DICTATION build/CMakeCache.txt
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_qt_tests
QT_QPA_PLATFORM=offscreen ctest --test-dir build -R '^qt_dictation_recorder_' --output-on-failure
```

Expected: `MORPH_ENABLE_DICTATION:BOOL=ON`, and the case passes. Record in the commit body which branch it took on this
machine (no device, a device that would not open, or a recorded clip), since the result depends on the hardware.

- [ ] **Step 5: Update the spec.**

Append to the "Dictation" section of `docs/spec/ui/chat.md`:

```markdown
**Recording.** `dictation.hpp`. `IDictationRecorder` records a clip: `unavailable()` reports what is known without
asking (no input device), `start(limits)` asks for permission where the platform does and answers through `onStarted`,
`onProgress` counts seconds, and `onClip` delivers the WAV or why there is none. Each `start` ends in exactly one of a
failed `onStarted` and an `onClip`; a `stop` before recording began is answered by an `onClip` error. A clip is 16 kHz
mono 16-bit whatever the device offers, so its size is a function of its length alone: `SpeechResampler` averages the
channels and the samples each output sample spans, `DictationClip` stops at `maxSeconds` or before the WAV would pass
`maxBytes`, and `wavFromPcm16` writes the 44-byte header. At this format 60 s is 1,920,044 bytes, 2,560,060 in base64.
`morph::qt::QtDictationRecorder` is the Qt one, in `morph_qt`, built when `Qt6::Multimedia` is found
(`MORPH_ENABLE_DICTATION`, ON when it is; ON without it is refused at configure time): `QAudioSource` on the owner thread
at the device's preferred format. Audio stays in memory and is never written to disk.
```

- [ ] **Step 6: Commit.**

```bash
git add include/morph/qt/qt_dictation_recorder.hpp src/qt/qt_dictation_recorder.cpp tests/qt/test_qt_dictation_recorder.cpp \
        tests/qt/CMakeLists.txt CMakeLists.txt docs/spec/ui/chat.md
git commit -m "$(cat <<'MSG'
Add QtDictationRecorder, built when Qt Multimedia is found

QAudioSource at the device's preferred format, converted to 16 kHz mono 16-bit
as it arrives and stopped at the server's bounds, with microphone permission
asked where the platform has it.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznwgG54dkVGoxXz2Z4
MSG
)"
```

---

## Task 8: `ChatClient` dictation: the offer, `transcribe` and notes

**Files:**
- Modify: `include/morph/ui/chat/client.hpp:4-20` (includes), `:74-84` (`probe`), `:115-131` (`end`), `:144-190` (`deliverEvent`, `reconnected`), `:204-352` (private state, `show`)
- Modify: `include/morph/ui/interpreter.hpp:2782-2786` (declare `base64Encode`), `src/ui/interpreter.cpp:4170-4192` (move `base64Encode` out of the anonymous namespace)
- Test: `tests/test_chat_client.cpp` (seven cases at the end)
- Docs: `docs/spec/ui/chat.md` ("The client")

**Interfaces:**
- Consumes: Task 2's protocol; `DictationLimits` (Task 5); `base64Encode`.
- Produces:
  - `[[nodiscard]] MORPH_API std::string morph::ui::base64Encode(std::string_view bytes);`
  - `[[nodiscard]] std::optional<DictationLimits> ChatClient::dictation() const noexcept;`
  - `void ChatClient::transcribe(std::string wav, std::optional<std::string> language, std::function<void(std::expected<std::string, std::string>)> done);`
  - `void ChatClient::note(std::string text);` (an `Entry` of kind `note`)
  - `end()` and `reconnected()` answer a waiting transcription with an error; `end()` forgets the offer.

- [ ] **Step 1: Write the failing tests.**

Append to `tests/test_chat_client.cpp` (add `#include <morph/ui/interpreter.hpp>` and `#include <chrono>` to the
includes):

```cpp
namespace {

using Outcome = std::optional<std::expected<std::string, std::string>>;

wire::EventBody transcriptionEvent(std::uint64_t id) {
    chat::TranscriptionEvent const event{
        .kind = std::string{chat::kTranscriptionEventKind},
        .state = {.id = id, .status = chat::TranscriptionStatus::Done, .text = std::nullopt, .error = std::nullopt}};
    return {.topic = std::string{chat::kChatTopic}, .payload = glz::raw_json{chat::writeMessage(event)}};
}

std::string transcriptionOf(std::uint64_t id, chat::TranscriptionStatus status, std::optional<std::string> text,
                            std::optional<std::string> error = std::nullopt) {
    return chat::writeMessage(
        chat::TranscriptionState{.id = id, .status = status, .text = std::move(text), .error = std::move(error)});
}

}  // namespace

// Mutation: keep reading only whether the probe's answer is ok.
TEST_CASE("chat_client_dictation_offer_comes_from_the_probe", "[chat][client][dictation]") {
    Rig rig;
    CHECK_FALSE(rig.client.dictation().has_value());
    rig.client.probe();
    chat::TurnState offered;
    offered.dictation = chat::DictationOffer{.maxSeconds = 60, .maxBytes = 1000};
    rig.fakeExchange.ok(chat::kStateKind, chat::writeMessage(offered));
    REQUIRE(rig.client.dictation().has_value());
    CHECK(rig.client.dictation()->maxSeconds == std::chrono::seconds{60});
    CHECK(rig.client.dictation()->maxBytes == 1000);
    rig.client.probe();
    rig.fakeExchange.ok(chat::kStateKind, chat::writeMessage(chat::TurnState{}));
    CHECK_FALSE(rig.client.dictation().has_value());
}

// Mutation: send the clip as raw bytes, or answer on the accept rather than on the state.
TEST_CASE("chat_client_dictation_transcribes_through_the_state", "[chat][client][dictation]") {
    Rig rig;
    Outcome outcome;
    int calls = 0;
    rig.client.transcribe(std::string{"RIFF\x00\x01", 6}, std::string{"de"},
                          [&](std::expected<std::string, std::string> text) {
                              ++calls;
                              outcome = std::move(text);
                          });
    auto const& sent = rig.fakeExchange.sent.back().request;
    REQUIRE(sent.kind == chat::kTranscribeKind);
    auto const request = chat::readTranscribeRequest(sent.body);
    REQUIRE(request.has_value());
    CHECK(ui::base64Decode(request->audio) == std::string{"RIFF\x00\x01", 6});
    CHECK(request->mediaType == "audio/wav");
    CHECK(request->language == "de");

    rig.fakeExchange.ok(chat::kTranscribeKind, chat::writeMessage(chat::TranscribeAccepted{.id = 5}));
    REQUIRE(rig.fakeExchange.sent.back().request.kind == chat::kTranscriptionKind);
    CHECK(chat::readTranscriptionQuery(rig.fakeExchange.sent.back().request.body)->id == 5);
    rig.fakeExchange.ok(chat::kTranscriptionKind, transcriptionOf(5, chat::TranscriptionStatus::Running, std::nullopt));
    CHECK(calls == 0);

    rig.client.deliverEvent(transcriptionEvent(99));
    CHECK(rig.fakeExchange.count(chat::kTranscriptionKind) == 1);
    rig.client.deliverEvent(transcriptionEvent(5));
    CHECK(rig.fakeExchange.count(chat::kTranscriptionKind) == 2);
    rig.fakeExchange.ok(chat::kTranscriptionKind, transcriptionOf(5, chat::TranscriptionStatus::Done, "words"));
    CHECK(calls == 1);
    REQUIRE(outcome.has_value());
    CHECK(*outcome == std::expected<std::string, std::string>{"words"});
}

// Mutation: drop a refusal or a failed state on the floor.
TEST_CASE("chat_client_dictation_reports_a_refused_or_failed_transcription", "[chat][client][dictation]") {
    Rig rig;
    Outcome refused;
    rig.client.transcribe("clip", std::nullopt, [&](std::expected<std::string, std::string> text) { refused = text; });
    rig.fakeExchange.answer(chat::kTranscribeKind, wire::makeErr("busy: a dictation is already being transcribed", 0));
    REQUIRE(refused.has_value());
    CHECK(refused->error().starts_with("busy"));

    Outcome failed;
    rig.client.transcribe("clip", std::nullopt, [&](std::expected<std::string, std::string> text) { failed = text; });
    rig.fakeExchange.ok(chat::kTranscribeKind, chat::writeMessage(chat::TranscribeAccepted{.id = 6}));
    rig.fakeExchange.ok(chat::kTranscriptionKind, transcriptionOf(6, chat::TranscriptionStatus::Failed, std::nullopt,
                                                                  "the transcription did not answer in time"));
    REQUIRE(failed.has_value());
    CHECK(failed->error() == "the transcription did not answer in time");
}

// Mutation: leave a waiting transcription waiting across a reconnect or an end.
TEST_CASE("chat_client_dictation_answers_a_waiting_transcription_on_reconnect_and_end", "[chat][client][dictation]") {
    Rig rig;
    rig.client.probe();
    chat::TurnState offered;
    offered.dictation = chat::DictationOffer{.maxSeconds = 60, .maxBytes = 1000};
    rig.fakeExchange.ok(chat::kStateKind, chat::writeMessage(offered));

    Outcome lost;
    rig.client.transcribe("clip", std::nullopt, [&](std::expected<std::string, std::string> text) { lost = text; });
    rig.client.reconnected();
    REQUIRE(lost.has_value());
    CHECK(lost->error().contains("connection was lost"));

    Outcome ended;
    rig.client.transcribe("clip", std::nullopt, [&](std::expected<std::string, std::string> text) { ended = text; });
    rig.client.end({});
    REQUIRE(ended.has_value());
    CHECK_FALSE(ended->has_value());
    CHECK_FALSE(rig.client.dictation().has_value());
}

// Mutation: read nothing on resync while a transcription waits.
TEST_CASE("chat_client_dictation_reads_a_waiting_transcription_on_resync", "[chat][client][dictation]") {
    Rig rig;
    rig.client.transcribe("clip", std::nullopt, [](std::expected<std::string, std::string> /*text*/) {});
    rig.fakeExchange.ok(chat::kTranscribeKind, chat::writeMessage(chat::TranscribeAccepted{.id = 8}));
    std::size_t const before = rig.fakeExchange.count(chat::kTranscriptionKind);
    rig.client.deliverEvent({.topic = std::string{wire::kResyncTopic}, .payload = glz::raw_json{"{}"}});
    CHECK(rig.fakeExchange.count(chat::kTranscriptionKind) == before + 1);
}

// Mutation: start a second transcription while one waits.
TEST_CASE("chat_client_dictation_refuses_a_second_transcription_while_one_waits", "[chat][client][dictation]") {
    Rig rig;
    rig.client.transcribe("clip", std::nullopt, [](std::expected<std::string, std::string> /*text*/) {});
    Outcome second;
    rig.client.transcribe("clip", std::nullopt, [&](std::expected<std::string, std::string> text) { second = text; });
    REQUIRE(second.has_value());
    CHECK_FALSE(second->has_value());
    CHECK(rig.fakeExchange.count(chat::kTranscribeKind) == 1);
}

// Mutation: let a turn's redraw cut the note off with the turn's old entries.
TEST_CASE("chat_client_dictation_note_survives_a_turn_redraw", "[chat][client][dictation]") {
    Rig rig;
    rig.started();
    rig.client.note("the microphone permission was denied");
    CHECK(rig.shown() == Shown{"user:hello", "note:the microphone permission was denied"});
    rig.fakeExchange.ok(chat::kStateKind, stateOf(7, chat::TurnStatus::Done, "hi there"));
    CHECK(rig.shown() ==
          Shown{"user:hello", "assistant:hi there", "note:the microphone permission was denied"});
    CHECK(rig.fakeExchange.count(chat::kTurnKind) == 1);
}
```

- [ ] **Step 2: Run them and expect FAIL.**

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_tests
```

Expected: `error: no member named 'dictation' in 'morph::ui::chat::ChatClient'`.

- [ ] **Step 3: Make `base64Encode` public.**

In `src/ui/interpreter.cpp`, cut the `base64Encode` definition (with its doc comment) out of the anonymous namespace
that ends at line 4192, and paste it right after that `}  // namespace`, before `base64Decode`, without the doc
comment. In `include/morph/ui/interpreter.hpp`, before `base64Decode`'s declaration:

```cpp
/// @brief Bytes as base64 (RFC 4648, with padding): how a document carries an inline file, and how a dictated clip
///        travels.
/// @param bytes The bytes.
/// @return The text.
[[nodiscard]] MORPH_API std::string base64Encode(std::string_view bytes);
```

- [ ] **Step 4: Implement the client side.**

In `include/morph/ui/chat/client.hpp`, add `#include <chrono>`, `#include "../interpreter.hpp"` and
`#include "dictation.hpp"`. Change the `Entry::kind` doc to "`user`, `assistant`, `tool`, `error`, or `note`: a line the
client adds itself, such as why a dictation failed, which the server never sees."

Replace `probe`:

```cpp
    /// @brief Asks whether this server offers this principal a chat, and dictation, which `available` and `dictation`
    ///        then answer.
    void probe() {
        send(std::string{kStateKind}, "{}", [this](ExchangeReply const& reply) {
            _available = reply.has_value() && reply->kind == "ok";
            _dictation.reset();
            if (_available) {
                auto const state = readTurnState(reply->body);
                if (state && state->dictation && state->dictation->maxSeconds > 0 && state->dictation->maxBytes > 0) {
                    _dictation = DictationLimits{
                        .maxSeconds = std::chrono::seconds{static_cast<std::int64_t>(state->dictation->maxSeconds)},
                        .maxBytes = static_cast<std::size_t>(state->dictation->maxBytes)};
                }
            }
            notify();
        });
    }
```

After `available()`:

```cpp
    /// @brief The dictation the last probe found offered.
    /// @return Its bounds, or nothing when the server offers none.
    [[nodiscard]] std::optional<DictationLimits> dictation() const noexcept { return _dictation; }

    /// @brief Hands a dictated clip to the server, and answers with its text once the server's state says it is done.
    ///
    /// @p done runs exactly once: with the text when the state reads `done`; with the reason when it reads `failed`,
    /// when the server refuses the clip, when the exchange fails, on a reconnect (the old connection's close dropped the
    /// transcription), when `end` runs, and at once when another transcription is still waiting.
    /// @param wav      The clip, WAV bytes.
    /// @param language The language spoken; nothing leaves it to the server's default.
    /// @param done     What gets the text or the reason.
    void transcribe(std::string wav, std::optional<std::string> language,
                    std::function<void(std::expected<std::string, std::string>)> done) {
        if (_transcribing) {
            if (done) {
                done(std::unexpected(std::string{"a dictation is already being transcribed"}));
            }
            return;
        }
        std::uint64_t const seq = ++_transcribeSeq;
        _transcribing = Transcribing{.seq = seq, .id = 0, .done = std::move(done)};
        TranscribeRequest const request{
            .audio = base64Encode(wav), .mediaType = std::string{kDictationMediaType}, .language = std::move(language)};
        wav = std::string{};
        send(std::string{kTranscribeKind}, writeMessage(request),
             [this, seq](ExchangeReply const& reply) { transcribeAccepted(seq, reply); });
    }

    /// @brief Adds a line of the client's own to the selected conversation, such as why a dictation failed. It stays
    ///        when a running turn is redrawn, after the turn's lines, and is never sent.
    /// @param text The line.
    void note(std::string text) {
        _conversations[_selected].push_back(Entry{.kind = "note", .text = std::move(text), .arguments = {}, .ok = false});
        notify();
    }
```

In `end`, after `_conversations.clear();`:

```cpp
        _dictation.reset();
        if (_transcribing) {
            transcribed(std::unexpected(std::string{"the chat ended, so the dictation stopped"}));
        }
```

Make the start of `deliverEvent`:

```cpp
    void deliverEvent(wire::EventBody const& event) {
        if (event.topic == wire::kResyncTopic) {
            readState();
            readTranscription();
            return;
        }
        if (event.topic != kChatTopic) {
            return;
        }
        if (auto const hint = readTranscriptionEvent(event.payload.str);
            hint && hint->kind == kTranscriptionEventKind) {
            if (_transcribing && _transcribing->id != 0 && hint->state.id == _transcribing->id) {
                readTranscription();
            }
            return;
        }
        if (!_active) {
            return;
        }
```

(the rest of the function is unchanged). In `reconnected`, before `probe();`:

```cpp
        if (_transcribing) {
            transcribed(std::unexpected(std::string{"the connection was lost, so the dictation stopped"}));
        }
```

In the private section, after `struct Active { ... };`:

```cpp
    // The transcription waiting for its text. `seq` tells this one's accept from an earlier one's; `id` is 0 until
    // the server accepted it.
    struct Transcribing {
        std::uint64_t seq = 0;
        std::uint64_t id = 0;
        std::function<void(std::expected<std::string, std::string>)> done;
    };

    void transcribeAccepted(std::uint64_t seq, ExchangeReply const& reply) {
        if (!_transcribing || _transcribing->seq != seq) {
            return;
        }
        if (!reply || reply->kind != "ok") {
            transcribed(std::unexpected(reply ? reply->message : reply.error()));
            return;
        }
        auto const read = readTranscribeAccepted(reply->body);
        if (!read || read->id == 0) {
            transcribed(std::unexpected(read ? std::string{"the dictation's reply names no transcription"} : read.error()));
            return;
        }
        _transcribing->id = read->id;
        readTranscription();
    }

    void readTranscription() {
        if (!_transcribing || _transcribing->id == 0) {
            return;
        }
        std::uint64_t const id = _transcribing->id;
        send(std::string{kTranscriptionKind}, writeMessage(TranscriptionQuery{.id = id}),
             [this, id](ExchangeReply const& reply) { transcriptionRead(id, reply); });
    }

    // A lost exchange leaves it waiting: the next event, a resync or a reconnect settles it.
    void transcriptionRead(std::uint64_t id, ExchangeReply const& reply) {
        if (!_transcribing || _transcribing->id != id || !reply) {
            return;
        }
        if (reply->kind != "ok") {
            transcribed(std::unexpected(reply->message));
            return;
        }
        auto const state = readTranscriptionState(reply->body);
        if (!state || state->status == TranscriptionStatus::Running) {
            return;
        }
        if (state->status == TranscriptionStatus::Done) {
            transcribed(state->text.value_or(std::string{}));
        } else {
            transcribed(std::unexpected(state->error.value_or(std::string{"the transcription failed"})));
        }
    }

    // Clears the waiting transcription before answering, so `done` may start the next one.
    void transcribed(std::expected<std::string, std::string> outcome) {
        auto done = std::move(_transcribing->done);
        _transcribing.reset();
        if (done) {
            done(std::move(outcome));
        }
    }
```

Replace `show` with:

```cpp
    // Redraws the running turn's entries. Notes added since the turn began are the client's own lines: they are kept,
    // after the turn's.
    void show(Active const& active) {
        Conversation& conversation = _conversations[active.key];
        std::vector<Entry> notes;
        for (std::size_t index = active.base; index < conversation.size(); ++index) {
            if (conversation[index].kind == "note") {
                notes.push_back(std::move(conversation[index]));
            }
        }
        conversation.resize(active.base);
        for (TurnPart const& part : active.parts) {
            if (part.kind == "tool") {
                conversation.push_back(Entry{
                    .kind = "tool", .text = part.step.name, .arguments = part.step.arguments, .ok = part.step.ok});
            } else if (!part.text.empty()) {
                conversation.push_back(Entry{.kind = "assistant", .text = part.text, .arguments = {}, .ok = false});
            }
        }
        if (!active.error.empty()) {
            conversation.push_back(Entry{.kind = "error", .text = active.error, .arguments = {}, .ok = false});
        }
        for (Entry& note : notes) {
            conversation.push_back(std::move(note));
        }
        notify();
    }
```

Add the members after `bool _available = false;`:

```cpp
    std::optional<DictationLimits> _dictation;
    std::optional<Transcribing> _transcribing;
    std::uint64_t _transcribeSeq = 0;
```

- [ ] **Step 5: Run them and expect PASS.**

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_tests
ctest --test-dir build -R '^chat_client_dictation_|probe_finds|resync|reconnect|end_calls_done|state_read' --output-on-failure
```

Expected: seven new cases and the existing client cases pass.

- [ ] **Step 6: Update the spec.**

In `docs/spec/ui/chat.md`, "The client", add:

```markdown
- **Dictation.** The probe's answer also carries the dictation offer, which `dictation()` returns, or nothing.
  `transcribe(wav, language, done)` sends `ui-chat-transcribe` with the clip as base64, keeps the id, and reads
  `ui-chat-transcription` when the `transcription` event for that id arrives and on `resync`. `done` runs exactly once:
  with the text when the state reads `done`, with the reason when it reads `failed`, when the server refuses the clip,
  when the exchange fails, on a reconnect, when `end` runs (which also forgets the offer until the next probe), and at
  once when another transcription still waits.
- **Notes.** `note(text)` adds an entry of kind `note` to the selected conversation: a line of the client's own, such as
  why a dictation failed. A running turn's redraw keeps it, after the turn's lines, and it is never sent.
```

- [ ] **Step 7: Commit.**

```bash
git add include/morph/ui/chat/client.hpp include/morph/ui/interpreter.hpp src/ui/interpreter.cpp tests/test_chat_client.cpp \
        docs/spec/ui/chat.md
git commit -m "$(cat <<'MSG'
Let the chat client transcribe a clip and show notes of its own

The probe's dictation offer, a transcription answered from the server's state
exactly once, and client-side notes a turn's redraw keeps.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznwgG54dkVGoxXz2Z4
MSG
)"
```

---

## Task 9: The Dictate button, the append watch and the binding

**Files:**
- Create: `include/morph/ui/testing/fake_dictation_recorder.hpp`
- Modify: `include/morph/ui/chat/panel.hpp:4-20` (includes), `:33-59` (`kChatPanelDocument`), `:71-177` (`ChatPanelBinding`)
- Modify: `CMakeLists.txt:696` (header file set)
- Test: `tests/test_chat_panel.cpp:31-87` (the `Rig`), seven cases at the end
- Docs: `docs/spec/ui/chat.md` ("The panel")

**Interfaces:**
- Consumes: `HostValue::documents`, `HostCommand::documents` (Task 1); `ChatClient::dictation`, `transcribe`, `note` (Task 8); `IDictationRecorder`, `DictationLimits` (Task 5).
- Produces:
  - `ChatPanelBinding(HostVocabulary& host, ChatClient& client, std::function<std::optional<TurnRequest>()> context, std::function<std::uint64_t()> origin = {}, std::function<std::optional<std::string>()> draft = {}, IDictationRecorder* recorder = nullptr, std::function<bool()> visible = {});`
  - Host values for `kChatPanelId` only: `chat.canDictate` (bool), `chat.dictation` (string `idle`/`recording`/`waiting`), `chat.dictationSeconds` (int), `chat.dictated` (record `{seq, text}`); host commands for `kChatPanelId` only: `chat.dictateStart`, `chat.dictateStop`.
  - Panel widgets: button `chat_dictate`; top-level `watch` on `{"host": "chat.dictated"}`.
  - `class morph::ui::chat::testing::FakeDictationRecorder final : public IDictationRecorder` with `why`, `starts`, `stops`, `limits`, `begin()`, `refuse(std::string)`, `tick(std::chrono::seconds)`, `clip(std::expected<std::string, std::string>)`.

- [ ] **Step 1: Write the fake and the failing tests.**

Create `include/morph/ui/testing/fake_dictation_recorder.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <utility>

#include "../chat/dictation.hpp"

/// @file
/// @brief An `IDictationRecorder` a test drives by hand.

namespace morph::ui::chat::testing {

/// @brief Records `start` and `stop`, and answers only when the test says: `begin`, `refuse`, `tick` and `clip` call
///        the callbacks as a real recorder would.
class FakeDictationRecorder final : public IDictationRecorder {
public:
    /// @brief What `unavailable` answers.
    /// @return `why`.
    [[nodiscard]] std::optional<std::string> unavailable() const override { return why; }

    /// @brief Counts the start and keeps its limits.
    /// @param given The limits.
    void start(DictationLimits given) override {
        ++starts;
        limits = given;
    }

    /// @brief Counts the stop; the test then calls `clip`.
    void stop() override { ++stops; }

    /// @brief Says recording began.
    void begin() {
        if (onStarted) {
            onStarted({});
        }
    }

    /// @brief Says recording could not begin.
    /// @param reason Why.
    void refuse(std::string reason) {
        if (onStarted) {
            onStarted(std::unexpected(std::move(reason)));
        }
    }

    /// @brief Says how long it has recorded.
    /// @param seconds The seconds.
    void tick(std::chrono::seconds seconds) {
        if (onProgress) {
            onProgress(seconds);
        }
    }

    /// @brief Delivers the clip, or why there is none.
    /// @param outcome The WAV bytes or the reason.
    void clip(std::expected<std::string, std::string> outcome) {
        if (onClip) {
            onClip(std::move(outcome));
        }
    }

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): what a test sets and reads directly.
    /// @brief Why it cannot record; nothing when it can.
    std::optional<std::string> why;
    /// @brief The starts so far.
    std::size_t starts = 0;
    /// @brief The stops so far.
    std::size_t stops = 0;
    /// @brief The last start's limits.
    std::optional<DictationLimits> limits;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)
};

}  // namespace morph::ui::chat::testing
```

In `CMakeLists.txt`, add `include/morph/ui/testing/fake_dictation_recorder.hpp` after
`include/morph/ui/testing/fake_chat_provider.hpp`.

In `tests/test_chat_panel.cpp`, add `#include <chrono>`, `#include <morph/ui/testing/fake_dictation_recorder.hpp>` and
`#include <algorithm>`. Give the `Rig` these members, declared before `binding`:

```cpp
    chat::testing::FakeDictationRecorder recorder;
    bool panelVisible = true;
```

and build the binding with them:

```cpp
        binding = std::make_unique<chat::ChatPanelBinding>(host, client, std::move(reader),
                                                           std::function<std::uint64_t()>{},
                                                           std::function<std::optional<std::string>()>{}, &recorder,
                                                           [this] { return panelVisible; });
```

Add these helpers to the `Rig`:

```cpp
    // Answers the probe with a dictation offer.
    void offer() {
        client.probe();
        chat::TurnState offered;
        offered.dictation = chat::DictationOffer{.maxSeconds = 60, .maxBytes = 1U << 20U};
        answers.back()(wire::makeOk(0, chat::writeMessage(offered)));
        harness.settle();
    }

    void clickDictate() {
        harness.backend.click(id("Button", "chat_dictate"));
        harness.settle();
    }

    [[nodiscard]] std::string dictateLabel() const { return harness.backend.prop(id("Button", "chat_dictate"), "label"); }

    [[nodiscard]] std::string input() const { return harness.backend.prop(id("TextInput", "chat_input"), "text"); }

    // Records a clip and answers its transcription with @p text, as the server would.
    void dictate(std::string const& text) {
        clickDictate();
        recorder.begin();
        clickDictate();
        recorder.clip(std::string{"RIFF"});
        harness.settle();
        REQUIRE(sent.back().kind == chat::kTranscribeKind);
        answers.back()(wire::makeOk(0, chat::writeMessage(chat::TranscribeAccepted{.id = ++transcriptions})));
        REQUIRE(sent.back().kind == chat::kTranscriptionKind);
        answers.back()(wire::makeOk(0, chat::writeMessage(chat::TranscriptionState{
                                           .id = transcriptions, .status = chat::TranscriptionStatus::Done,
                                           .text = text, .error = std::nullopt})));
        harness.settle();
    }

    std::uint64_t transcriptions = 0;
```

Append the cases:

```cpp
// Mutation: show the button without an offer, a recorder that can record, or a visible panel.
TEST_CASE("chat_panel_dictation_button_needs_an_offer_a_recorder_and_a_visible_panel", "[chat][panel][dictation]") {
    Rig rig;
    CHECK_FALSE(rig.visible("Button", "chat_dictate"));
    rig.offer();
    CHECK(rig.visible("Button", "chat_dictate"));
    CHECK(rig.dictateLabel() == "Dictate");
    rig.recorder.why = "no microphone is connected";
    rig.binding->refresh();
    rig.harness.settle();
    CHECK_FALSE(rig.visible("Button", "chat_dictate"));
    rig.recorder.why.reset();
    rig.panelVisible = false;
    rig.binding->refresh();
    rig.harness.settle();
    CHECK_FALSE(rig.visible("Button", "chat_dictate"));
    rig.panelVisible = true;
    rig.binding->refresh();
    rig.harness.settle();
    CHECK(rig.visible("Button", "chat_dictate"));
}

// Mutation: leave the button's label on "Dictate", or keep it enabled while transcribing.
TEST_CASE("chat_panel_dictation_button_goes_through_recording_and_transcribing", "[chat][panel][dictation]") {
    Rig rig;
    rig.offer();
    rig.clickDictate();
    REQUIRE(rig.recorder.starts == 1);
    CHECK(rig.recorder.limits->maxSeconds == std::chrono::seconds{60});
    CHECK(rig.dictateLabel() == "Stop (0 s)");
    rig.recorder.begin();
    rig.recorder.tick(std::chrono::seconds{12});
    rig.harness.settle();
    CHECK(rig.dictateLabel() == "Stop (12 s)");
    rig.clickDictate();
    CHECK(rig.recorder.stops == 1);
    rig.recorder.clip(std::string{"RIFF"});
    rig.harness.settle();
    CHECK(rig.dictateLabel() == "Transcribing…");
    CHECK(rig.harness.backend.prop(rig.id("Button", "chat_dictate"), "enabled") == "false");
    rig.answers.back()(wire::makeOk(0, chat::writeMessage(chat::TranscribeAccepted{.id = 1})));
    rig.answers.back()(wire::makeOk(0, chat::writeMessage(chat::TranscriptionState{
                                           .id = 1, .status = chat::TranscriptionStatus::Done, .text = "x",
                                           .error = std::nullopt})));
    rig.harness.settle();
    CHECK(rig.dictateLabel() == "Dictate");
    CHECK(rig.harness.backend.prop(rig.id("Button", "chat_dictate"), "enabled") != "false");
    CHECK(rig.sent.size() == 3);
}

// Mutation: drop the panel's append watch, or join without the space rule.
TEST_CASE("chat_panel_dictation_appends_with_the_spacing_rules", "[chat][panel][dictation]") {
    Rig rig;
    rig.offer();
    rig.dictate("  hello ");
    CHECK(rig.input() == "hello");
    int const input = rig.id("TextInput", "chat_input");
    rig.harness.backend.edit(input, "typed");
    rig.harness.settle();
    rig.dictate("world");
    CHECK(rig.input() == "typed world");
    rig.harness.backend.edit(input, "typed ");
    rig.harness.settle();
    rig.dictate("again");
    CHECK(rig.input() == "typed again");
    CHECK(std::ranges::none_of(rig.sent, [](wire::Envelope const& env) { return env.kind == chat::kTurnKind; }));
}

// Mutation: append on the seconds' change, or append only the first transcript.
TEST_CASE("chat_panel_dictation_appends_two_in_order_and_the_tick_appends_nothing", "[chat][panel][dictation]") {
    Rig rig;
    rig.offer();
    rig.dictate("one");
    rig.dictate("two");
    CHECK(rig.input() == "one two");
    rig.clickDictate();
    rig.recorder.begin();
    rig.recorder.tick(std::chrono::seconds{3});
    rig.harness.settle();
    CHECK(rig.input() == "one two");
}

// Mutation: drop a failure silently, or put the note inside the turn's range.
TEST_CASE("chat_panel_dictation_failure_note_survives_a_turn_redraw", "[chat][panel][dictation]") {
    Rig rig;
    rig.offer();
    rig.submit("hello");
    rig.answers.back()(wire::makeOk(0, chat::writeMessage(chat::TurnAccepted{.turnId = 7})));
    rig.harness.settle();
    rig.clickDictate();
    rig.recorder.refuse("the microphone permission was denied");
    rig.harness.settle();
    CHECK(rig.shows("the microphone permission was denied"));
    CHECK(rig.dictateLabel() == "Dictate");
    chat::TurnState state;
    state.turnId = 7;
    state.status = chat::TurnStatus::Done;
    state.text = "hi";
    rig.answers.back()(wire::makeOk(0, chat::writeMessage(state)));
    rig.harness.settle();
    CHECK(rig.shows("hi"));
    CHECK(rig.shows("the microphone permission was denied"));
}

// Mutation: keep recording when the panel is hidden, or send the clip it then delivers.
TEST_CASE("chat_panel_dictation_hiding_the_panel_stops_and_drops", "[chat][panel][dictation]") {
    Rig rig;
    rig.offer();
    rig.clickDictate();
    rig.recorder.begin();
    rig.panelVisible = false;
    rig.binding->refresh();
    rig.harness.settle();
    CHECK(rig.recorder.stops == 1);
    std::size_t const before = rig.sent.size();
    rig.recorder.clip(std::string{"RIFF"});
    rig.harness.settle();
    CHECK(rig.sent.size() == before);
    rig.panelVisible = true;
    rig.binding->refresh();
    rig.dictate("back again");
    CHECK(rig.input() == "back again");
}

// Mutation: register the dictation names for every document.
TEST_CASE("chat_panel_dictation_another_document_cannot_start_or_read_it", "[chat][panel][dictation]") {
    Rig rig;
    rig.offer();
    auto const other = rig.harness.mount(R"({"vocab":"ui/1","id":"lims.results","view":{"kind":"column","children":[
      {"kind":"button","testId":"steal","label":"steal","onClick":{"host":"chat.dictateStart"}},
      {"kind":"text","testId":"heard","text":{"fn":"coalesce","args":[{"get":{"host":"chat.dictated"},"path":"text"},"none"]}},
      {"kind":"text","testId":"can","text":{"fn":"format","args":["{0}",{"can":"host:chat.dictateStart"}]}}]}})");
    rig.harness.backend.click(rig.id("Button", "steal"));
    rig.harness.settle();
    CHECK(rig.recorder.starts == 0);
    CHECK(rig.harness.text("can") == "false");
    rig.dictate("private words");
    CHECK(rig.harness.text("heard") == "none");
    CHECK(std::ranges::any_of(rig.harness.diagnostics, [](auto const& refusal) {
        return refusal.message().contains("host command `chat.dictateStart` is missing");
    }));
}
```

- [ ] **Step 2: Run them and expect FAIL.**

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_tests
```

Expected: `error: no matching constructor for initialization of 'morph::ui::chat::ChatPanelBinding'` (seven arguments).

- [ ] **Step 3: Implement the document and the binding.**

In `include/morph/ui/chat/panel.hpp`, add `#include <set>` and `#include "dictation.hpp"`, and replace
`kChatPanelDocument` with:

```cpp
inline constexpr std::string_view kChatPanelDocument = R"({
  "vocab": "ui/1", "id": "morph.chat",
  "state": { "draft": { "init": "" } },
  "watch": [
    { "on": { "host": "chat.dictated" },
      "do": [ { "set": "draft", "value": { "if": [
        { "fn": "or", "args": [
          { "fn": "isEmpty", "args": [ { "ref": "draft" } ] },
          { "fn": "lt", "args": [
            { "fn": "length", "args": [ { "fn": "trim", "args": [ { "fn": "concat", "args": [ "x", { "ref": "draft" } ] } ] } ] },
            { "fn": "length", "args": [ { "fn": "concat", "args": [ "x", { "ref": "draft" } ] } ] } ] } ] },
        { "fn": "concat", "args": [ { "ref": "draft" },
          { "fn": "trim", "args": [ { "get": { "host": "chat.dictated" }, "path": "text" } ] } ] },
        { "fn": "concat", "args": [ { "ref": "draft" }, " ",
          { "fn": "trim", "args": [ { "get": { "host": "chat.dictated" }, "path": "text" } ] } ] } ] } } ] } ],
  "view": { "kind": "column", "testId": "chat_panel", "gap": 2,
    "sizing": { "lit": { "width": { "stretch": 1 }, "height": { "stretch": 1 } } },
    "children": [
      { "kind": "scroll", "testId": "chat_entries",
        "sizing": { "lit": { "width": { "stretch": 1 }, "height": { "stretch": 1 } } },
        "children": [
          { "kind": "forEach", "gap": 2, "key": { "row": "key" },
            "rows": { "fn": "coalesce", "args": [ { "host": "chat.entries" }, { "lit": [] } ] },
            "children": [
              { "kind": "text", "testId": "chat_entry", "text": { "row": "text" }, "role": { "row": "role" } } ] } ] },
      { "kind": "row", "gap": 1, "align": "center",
        "sizing": { "lit": { "width": { "stretch": 1 } } },
        "children": [
          { "kind": "textInput", "testId": "chat_input", "placeholder": "Message",
            "sizing": { "lit": { "width": { "stretch": 1 } } },
            "value": { "ref": "draft" }, "enabled": { "host": "chat.onScreen" },
            "onChange": [ { "set": "draft", "value": { "ref": "event" } } ],
            "onSubmit": [ { "host": "chat.send", "args": { "obj": { "text": { "ref": "event" } } } },
                          { "set": "draft", "value": "" } ] },
          { "kind": "button", "testId": "chat_dictate", "icon": "chat.dictate",
            "label": { "if": [ { "fn": "eq", "args": [ { "host": "chat.dictation" }, "recording" ] },
                               { "fn": "format", "args": [ "Stop ({0} s)", { "host": "chat.dictationSeconds" } ] },
                               { "if": [ { "fn": "eq", "args": [ { "host": "chat.dictation" }, "waiting" ] },
                                         "Transcribing…", "Dictate" ] } ] },
            "visible": { "fn": "coalesce", "args": [ { "host": "chat.canDictate" }, false ] },
            "enabled": { "fn": "ne", "args": [ { "host": "chat.dictation" }, "waiting" ] },
            "onClick": [ { "if": [ { "fn": "eq", "args": [ { "host": "chat.dictation" }, "recording" ] },
                                   [ { "host": "chat.dictateStop" } ],
                                   [ { "host": "chat.dictateStart" } ] ] } ] },
          { "kind": "button", "testId": "chat_send", "label": "Send", "enabled": { "host": "chat.canSend" },
            "onClick": [ { "host": "chat.send", "args": { "obj": { "text": { "ref": "draft" } } } },
                         { "set": "draft", "value": "" } ] },
          { "kind": "button", "testId": "chat_cancel", "label": "Stop", "emphasis": "ghost",
            "visible": { "host": "chat.running" }, "onClick": [ { "host": "chat.cancel" } ] } ] } ] } })";
```

Replace the `ChatPanelBinding` class doc's first sentence with "Connects the panel document to a client, through a
`HostVocabulary`: `chat.entries`, `chat.running`, `chat.onScreen` and `chat.canSend` to read, `chat.send` and
`chat.cancel` to run, and, for the panel document alone, the dictation names." Replace the constructor, destructor and
`refresh`, and add the dictation members, so the class reads:

```cpp
class ChatPanelBinding {
public:
    /// @brief Registers the values and the commands.
    /// @param host     The vocabulary. Borrowed: it outlives this binding.
    /// @param client   The client. Borrowed: it outlives this binding.
    /// @param context  Where the person is now: the open screen's context, with no text. Read at each send, so a
    ///                 message goes to the screen they are looking at when they send it. Nothing for a screen with no
    ///                 document behind it, such as a host screen, which disables the input.
    /// @param origin   What the client hands back with each draft the turn proposes (`ChatClient::send`): read at
    ///                 each send, with the context, so it names the tab the message goes from. May be empty, which is 0.
    /// @param draft    The text of the draft the open screen holds, read at each send and carried as the turn's
    ///                 `TurnRequest::draft`; nothing for a screen with no draft. May be empty, which is nothing.
    /// @param recorder What records a dictated clip; null offers no dictation. Borrowed: it outlives this binding,
    ///                 which sets its callbacks and clears them when it goes.
    /// @param visible  Whether the panel is in view, read at each `refresh`. A microphone never runs without its
    ///                 indicator in view, so empty, like `false`, offers no dictation.
    ChatPanelBinding(HostVocabulary& host, ChatClient& client, std::function<std::optional<TurnRequest>()> context,
                     std::function<std::uint64_t()> origin = {},
                     std::function<std::optional<std::string>()> draft = {}, IDictationRecorder* recorder = nullptr,
                     std::function<bool()> visible = {})
        : _host{&host},
          _client{&client},
          _context{std::move(context)},
          _origin{std::move(origin)},
          _draft{std::move(draft)},
          _recorder{recorder},
          _visible{std::move(visible)} {
        host.add(
            HostValue{.name = "chat.entries", .version = 1, .type = "list<record>", .initial = Value{ValueList{}}});
        host.add(HostValue{.name = "chat.running", .version = 1, .type = "bool", .initial = Value{false}});
        host.add(HostValue{.name = "chat.onScreen", .version = 1, .type = "bool", .initial = Value{false}});
        host.add(HostValue{.name = "chat.canSend", .version = 1, .type = "bool", .initial = Value{false}});
        host.add(HostCommand{.name = "chat.send",
                             .version = 1,
                             .args = {HostArg{.name = "text", .type = "string"}},
                             .safeForUntrusted = true,
                             .run = [this](ValueRecord const& args) { send(args); }});
        host.add(HostCommand{.name = "chat.cancel",
                             .version = 1,
                             .args = {},
                             .safeForUntrusted = true,
                             .run = [this](ValueRecord const& /*args*/) { _client->cancel(); }});
        addDictation(host);
        refresh();
    }

    ChatPanelBinding(ChatPanelBinding const&) = delete;
    ChatPanelBinding& operator=(ChatPanelBinding const&) = delete;
    ChatPanelBinding(ChatPanelBinding&&) = delete;
    ChatPanelBinding& operator=(ChatPanelBinding&&) = delete;

    /// @brief Clears the recorder's callbacks, then stops a recording, whose clip then reaches no one.
    ~ChatPanelBinding() {
        if (_recorder == nullptr) {
            return;
        }
        _recorder->onStarted = nullptr;
        _recorder->onProgress = nullptr;
        _recorder->onClip = nullptr;
        if (_state == DictationState::Recording) {
            _recorder->stop();
        }
    }

    /// @brief Publishes what the client shows now, whether the person is on a screen, and the dictation's state. Stops
    ///        a recording and drops its clip when the panel is no longer visible or the dictation offer is gone, as it
    ///        is after the client's `end` at sign-out.
    void refresh() {
        ValueList entries;
        std::int64_t key = 0;
        for (ChatClient::Entry const& entry : _client->entries()) {
            entries.emplace_back(ValueRecord{{"key", Value{key++}}, {"role", roleOf(entry)}, {"text", textOf(entry)}});
        }
        bool const onScreen = _context && _context().has_value();
        bool const running = _client->running();
        if (_state == DictationState::Recording && (!shown() || !_client->dictation())) {
            dropRecording();
        }
        _host->set("chat.entries", Value{std::move(entries)});
        _host->set("chat.running", Value{running});
        _host->set("chat.onScreen", Value{onScreen});
        _host->set("chat.canSend", Value{onScreen && !running});
        publishDictation();
    }

private:
    enum class DictationState : std::uint8_t { Idle, Recording, Waiting };

    // Registered for the panel document alone: no screen, custom screen or draft may start the microphone or read a
    // transcript. Safe for an untrusted connection, since the panel document is the client's own.
    void addDictation(HostVocabulary& host) {
        std::set<std::string, std::less<>> const panel{std::string{kChatPanelId}};
        host.add(HostValue{
            .name = "chat.canDictate", .version = 1, .type = "bool", .initial = Value{false}, .documents = panel});
        host.add(HostValue{.name = "chat.dictation",
                           .version = 1,
                           .type = "string",
                           .initial = Value{std::string{"idle"}},
                           .documents = panel});
        host.add(HostValue{.name = "chat.dictationSeconds",
                           .version = 1,
                           .type = "int",
                           .initial = Value{std::int64_t{0}},
                           .documents = panel});
        host.add(HostValue{.name = "chat.dictated",
                           .version = 1,
                           .type = "record",
                           .initial = Value{ValueRecord{{"seq", Value{std::int64_t{0}}}, {"text", Value{std::string{}}}}},
                           .documents = panel});
        host.add(HostCommand{.name = "chat.dictateStart",
                             .version = 1,
                             .args = {},
                             .safeForUntrusted = true,
                             .run = [this](ValueRecord const& /*args*/) { startDictation(); },
                             .documents = panel});
        host.add(HostCommand{.name = "chat.dictateStop",
                             .version = 1,
                             .args = {},
                             .safeForUntrusted = true,
                             .run = [this](ValueRecord const& /*args*/) { stopDictation(); },
                             .documents = panel});
        if (_recorder == nullptr) {
            return;
        }
        _recorder->onStarted = [this](std::expected<void, std::string> started) {
            if (!started && _state == DictationState::Recording) {
                _state = DictationState::Idle;
                _client->note(std::move(started.error()));
                publishDictation();
            }
        };
        _recorder->onProgress = [this](std::chrono::seconds seconds) {
            if (_state == DictationState::Recording) {
                _seconds = seconds.count();
                publishDictation();
            }
        };
        _recorder->onClip = [this](std::expected<std::string, std::string> clip) { clipped(std::move(clip)); };
    }

    [[nodiscard]] bool shown() const { return _visible && _visible(); }

    [[nodiscard]] bool canDictate() const {
        return _recorder != nullptr && _client->dictation().has_value() && !_recorder->unavailable().has_value() &&
               _context && _context().has_value() && shown();
    }

    void publishDictation() {
        static constexpr std::array<std::string_view, 3> kNames{"idle", "recording", "waiting"};
        _host->set("chat.canDictate", Value{canDictate()});
        _host->set("chat.dictation", Value{std::string{kNames.at(static_cast<std::size_t>(_state))}});
        _host->set("chat.dictationSeconds", Value{_seconds});
    }

    void startDictation() {
        if (_state != DictationState::Idle || !canDictate()) {
            return;
        }
        DictationLimits const limits = *_client->dictation();
        _state = DictationState::Recording;
        _seconds = 0;
        publishDictation();
        _recorder->start(limits);
    }

    void stopDictation() {
        if (_state == DictationState::Recording) {
            _recorder->stop();
        }
    }

    // The clip a stop now delivers is dropped: `stop` is always answered by exactly one `onClip`.
    void dropRecording() {
        _state = DictationState::Idle;
        _seconds = 0;
        ++_droppedClips;
        _recorder->stop();
    }

    void clipped(std::expected<std::string, std::string> clip) {
        if (_droppedClips > 0) {
            --_droppedClips;
            return;
        }
        if (_state != DictationState::Recording) {
            return;
        }
        if (!clip) {
            _state = DictationState::Idle;
            _client->note(std::move(clip.error()));
            publishDictation();
            return;
        }
        _state = DictationState::Waiting;
        publishDictation();
        _client->transcribe(std::move(*clip), std::nullopt,
                            [this, alive = std::weak_ptr<bool>{_alive}](std::expected<std::string, std::string> text) {
                                if (!alive.expired()) {
                                    transcribed(std::move(text));
                                }
                            });
    }

    // A transcript goes into the input through `chat.dictated`, never into a turn: the panel's watch appends it.
    void transcribed(std::expected<std::string, std::string> text) {
        _state = DictationState::Idle;
        if (!text) {
            _client->note(std::move(text.error()));
        } else if (std::ranges::all_of(*text, [](unsigned char const letter) { return std::isspace(letter); })) {
            _client->note("Nothing was heard in the recording");
        } else {
            ++_dictatedSeq;
            _host->set("chat.dictated",
                       Value{ValueRecord{{"seq", Value{_dictatedSeq}}, {"text", Value{std::move(*text)}}}});
        }
        publishDictation();
    }

    [[nodiscard]] static Value roleOf(ChatClient::Entry const& entry) {
        if (entry.kind == "tool") {
            return Value{std::string{"muted"}};
        }
        return Value{std::string{entry.kind == "error" || entry.kind == "note" ? "error" : "normal"}};
    }
```

Keep `textOf` and `send` as they are, and make the members:

```cpp
    HostVocabulary* _host;
    ChatClient* _client;
    std::function<std::optional<TurnRequest>()> _context;
    std::function<std::uint64_t()> _origin;
    std::function<std::optional<std::string>()> _draft;
    IDictationRecorder* _recorder;
    std::function<bool()> _visible;
    DictationState _state = DictationState::Idle;
    std::int64_t _seconds = 0;
    std::int64_t _dictatedSeq = 0;
    std::size_t _droppedClips = 0;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
};
```

Add `#include <array>` to the header's includes.

- [ ] **Step 4: Run them and expect PASS.**

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_tests
ctest --test-dir build -R '^chat_panel_dictation_|panel_|the_frames_chat_slot|a_host_screen_disables|blank_text|sending_with_the_button' --output-on-failure
```

Expected: the seven new cases and the existing panel cases pass.

- [ ] **Step 5: Mutation checks (the append watch, the document scoping).**

```bash
cp include/morph/ui/chat/panel.hpp /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/panel.hpp.bak
```

1. Delete the whole `"watch": [ ... ],` member from `kChatPanelDocument`. Rebuild, run
   `ctest --test-dir build -R 'chat_panel_dictation_appends_with_the_spacing_rules' --output-on-failure`: FAIL at
   `CHECK( rig.input() == "hello" )`. Restore with `cp` and `touch include/morph/ui/chat/panel.hpp`.
2. In `addDictation`, change `.documents = panel` on `chat.dictateStart` to `.documents = {}`. Rebuild, run
   `ctest --test-dir build -R 'chat_panel_dictation_another_document' --output-on-failure`: FAIL at
   `CHECK( rig.recorder.starts == 0 )`. Restore the same way and rebuild.

- [ ] **Step 6: Update the spec.**

In `docs/spec/ui/chat.md`, "The panel": add the dictate button (`chat_dictate`) to the first paragraph's list of
widgets, and add after the paragraph about `ChatPanelBinding`:

```markdown
**Dictation.** Beside Send, a **Dictate** button (`chat_dictate`, theme icon `chat.dictate`, the label alone when the
theme has no such icon) shows when the server offers dictation, the client can record, the person is on a screen and
the panel is visible. Pressing it records, and it reads **Stop (12 s)**, counting seconds; pressing again stops, and so
do `maxDictationSeconds` and `maxDictationBytes`. It then reads **Transcribing…** and is disabled. The transcript is
appended to the input, trimmed, after one space when the input is not empty and does not already end in whitespace; it
is never sent, since a misheard word sent to an assistant whose tools change data costs more than one more press of
Send. Dictating is allowed while a turn runs. A failure (no microphone, permission denied, a refusal, a provider error, a
timeout, a lost connection, or nothing heard) adds a `note` drawn in the `error` role and leaves the input as it was.

The binding takes the recorder and whether the panel is visible, and registers for the panel document alone
(`HostValue::documents`, `HostCommand::documents`): `chat.canDictate`, `chat.dictation` (`idle`, `recording`,
`waiting`), `chat.dictationSeconds`, `chat.dictated` (`{seq, text}`, `seq` growing by one per transcript) and the commands
`chat.dictateStart` and `chat.dictateStop`. No screen, custom screen or draft can start the microphone or read a
transcript. `refresh` stops a recording and drops its clip when the panel stops being visible or the offer is gone, as it
is after `end` at sign-out. The document's top-level `watch` on `{"host": "chat.dictated"}` appends: a watch on a host
value does not fire on its first evaluation and fires on each change of the record, so each new `seq` appends once and
the seconds' tick, a different value, appends nothing.
```

- [ ] **Step 7: Commit.**

```bash
git add include/morph/ui/chat/panel.hpp include/morph/ui/testing/fake_dictation_recorder.hpp CMakeLists.txt \
        tests/test_chat_panel.cpp docs/spec/ui/chat.md
git commit -m "$(cat <<'MSG'
Add the Dictate button to the chat panel and append the transcript

The binding drives the recorder and the client through host names only the panel
document sees; the panel's watch appends each transcript to the input with one
space, and never sends it.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznwgG54dkVGoxXz2Z4
MSG
)"
```

---

## Task 10: The shell records, and hides the microphone with the panel

**Files:**
- Modify: `include/morph/ui/shell_frame.hpp:209-211` (add `chatVisible` after `compact`), `:279` (`showChat`)
- Modify: `include/morph/client/shell.hpp:1-20` (includes), `:141-172` (`ShellOptions`)
- Modify: `src/client/shell.cpp:415-428` (width listener), `:520-533` (`buildFrame`), `:701-710` (`buildChat`), `:993-996` (`showSignedIn`), `:1510` (members)
- Modify: `examples/client/main.cpp:38-50` (includes), `:312-330` (`ShellOptions`), `examples/client/CMakeLists.txt:81-83`
- Test: `tests/client/test_custom_screens.cpp:540-650` (`ChatServedSource`, `ChatWindow`), two cases at the end of the assistant block
- Docs: `docs/spec/client/client.md` ("Chat")

**Interfaces:**
- Consumes: the seven-argument `ChatPanelBinding` (Task 9); `FakeTranscriber` (Task 3); `FakeDictationRecorder` (Task 9); `QtDictationRecorder` (Task 7).
- Produces:
  - `[[nodiscard]] bool ui::ShellFrame::chatVisible() const noexcept;` (the chat column is shown: wanted, signed in and not compact)
  - `std::function<std::unique_ptr<ui::chat::IDictationRecorder>()> client::ShellOptions::dictation;` (empty: no dictation; called once, on the owner, when the chat is built)

- [ ] **Step 1: Write the failing tests.**

In `tests/client/test_custom_screens.cpp`, add `#include <morph/ui/testing/fake_dictation_recorder.hpp>` and
`#include <morph/ui/testing/fake_transcriber.hpp>`. Give `ChatServedSource` a transcriber:

```cpp
    ChatServedSource(Offered const& offered, std::shared_ptr<chat::testing::FakeChatProvider> provider,
                     std::shared_ptr<chat::ITranscriber> transcriber = nullptr)
        : _offered{&offered}, _provider{std::move(provider)}, _transcriber{std::move(transcriber)} {
        requestSession().token = "ann";
    }
```

with `options.transcriber = _transcriber;` after `options.bundle = *served;` in `connect`, and the member
`std::shared_ptr<chat::ITranscriber> _transcriber;` after `_provider`. Give `ChatWindow` a transcriber and the shell a
recorder factory:

```cpp
    explicit ChatWindow(Offered const& offered, std::shared_ptr<chat::ITranscriber> transcriber = nullptr)
        : source{offered, provider, std::move(transcriber)} {
        std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
        source.open(context,
                    [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
        REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
        REQUIRE(opened->has_value());
        client::ShellOptions options{.startScreen = "customs.home"};
        options.dictation = [this] {
            auto made = std::make_unique<chat::testing::FakeDictationRecorder>();
            recorder = made.get();
            return made;
        };
        shell = client::mountShell(context, backend, std::move(*opened), source, std::move(options));
        mineId = offered.mineId;
        REQUIRE(wait([this] { return find("Button", "nav_" + mineId).has_value(); }));
        REQUIRE(wait([this] { return find("Button", "chat_send").has_value() && chatEnabled(); }));
    }
```

with the member `chat::testing::FakeDictationRecorder* recorder = nullptr;` declared before `shell`, and add:

```cpp
    void clickDictate() {
        auto const button = find("Button", "chat_dictate");
        REQUIRE(button.has_value());
        backend.click(*button);
    }
    [[nodiscard]] std::string input() const {
        auto const widget = find("TextInput", "chat_input");
        return widget ? backend.prop(*widget, "text") : std::string{};
    }
```

Append, after the last assistant case:

```cpp
// Mutations: hand the binding no recorder (no button); send the transcript as a turn (the provider is called).
TEST_CASE("Client shell: a dictated clip lands in the chat input through the server", "[client][chat][dictation]") {
    Offered const offered;
    auto const transcriber = std::make_shared<chat::testing::FakeTranscriber>();
    transcriber->script(std::string{"spoken words"});
    ChatWindow window{offered, transcriber};
    REQUIRE(window.wait([&window] { return window.shown("Button", "chat_dictate"); }));
    REQUIRE(window.recorder != nullptr);
    window.clickDictate();
    REQUIRE(window.wait([&window] { return window.recorder->starts == 1; }));
    window.recorder->begin();
    window.clickDictate();
    REQUIRE(window.wait([&window] { return window.recorder->stops == 1; }));
    window.recorder->clip(std::string{"RIFF-clip"});
    REQUIRE(window.wait([&window] { return window.input() == "spoken words"; }));
    CHECK(transcriber->calls() == 1);
    CHECK(window.provider->calls() == 0);
}

// Mutation: leave the binding unrefreshed when the frame turns compact.
TEST_CASE("Client shell: a compact frame stops a recording and drops its clip", "[client][chat][dictation]") {
    Offered const offered;
    auto const transcriber = std::make_shared<chat::testing::FakeTranscriber>();
    ChatWindow window{offered, transcriber};
    REQUIRE(window.wait([&window] { return window.shown("Button", "chat_dictate"); }));
    window.clickDictate();
    REQUIRE(window.wait([&window] { return window.recorder->starts == 1; }));
    window.recorder->begin();
    window.backend.setWidthClass("compact");
    REQUIRE(window.wait([&window] { return window.recorder->stops == 1; }));
    window.recorder->clip(std::string{"RIFF-clip"});
    static_cast<void>(window.wait([] { return false; }));
    CHECK(transcriber->calls() == 0);
}
```

- [ ] **Step 2: Run them and expect FAIL.**

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_client_tests
```

Expected: `error: no member named 'dictation' in 'morph::client::ShellOptions'`.

- [ ] **Step 3: Implement.**

In `include/morph/ui/shell_frame.hpp`, after `compact()`:

```cpp
    /// @brief Whether the chat column is in view: there is a chat, someone is signed in, and the frame is not compact.
    /// @return It.
    [[nodiscard]] bool chatVisible() const noexcept { return _chatWanted && _signedIn && !_compact; }
```

and make `showChat` `void showChat() { _chat->setVisible(chatVisible()); }`.

In `include/morph/client/shell.hpp`, add `#include "../ui/chat/dictation.hpp"`, and at the end of `ShellOptions`:

```cpp
    /// @brief Makes the recorder the chat panel dictates with, once, on the owner, when the chat is built; empty offers
    ///        no dictation. The shell owns what it returns and stops a recording whenever the panel leaves view.
    std::function<std::unique_ptr<ui::chat::IDictationRecorder>()> dictation;
```

In `src/client/shell.cpp`:

- Add the member before `_chat`, so it outlives the binding that sets its callbacks:
  ```cpp
      // What the chat panel dictates with; made once, before the binding, which goes first.
      std::unique_ptr<ui::chat::IDictationRecorder> _recorder;
  ```
- In `buildChat`, before `_chatBinding = std::make_unique<...>(...)`, add
  `if (_options.dictation) { _recorder = _options.dictation(); }`, and build the binding with:
  ```cpp
            _chatBinding = std::make_unique<ui::chat::ChatPanelBinding>(
                _chatHost, *_chat, [this] { return chatContext(); }, [this] { return chatOrigin(); },
                [this] { return chatDraft(); }, _recorder.get(),
                [this] { return _frame != nullptr && _frame->chatVisible(); });
  ```
- In the width-class listener, after `_frame->setCompact(compactFrame(widthClass));` add
  `if (_chatBinding) { _chatBinding->refresh(); }` (inside the `if (_frame)` block).
- In `showSignedIn()`, after `_frame->showSignedIn(_signedIn);` add `if (_chatBinding) { _chatBinding->refresh(); }`.
- In `buildFrame()`, after `_frame.reset();` add, with its comment:
  ```cpp
        // No frame, no panel in view: a recording stops before the new frame is built.
        if (_chatBinding) {
            _chatBinding->refresh();
        }
  ```

In `examples/client/CMakeLists.txt`, before the `target_compile_definitions(${_morph_client} PRIVATE MORPH_CLIENT_HAS_QT_QUICK=...`
block:

```cmake
    set(_morph_client_dictation 0)
    if(_morph_client_qt_quick AND MORPH_ENABLE_DICTATION)
        set(_morph_client_dictation 1)
    endif()
```

and add `MORPH_CLIENT_HAS_DICTATION=${_morph_client_dictation}` to that block. In `examples/client/main.cpp`, under the
`#if MORPH_CLIENT_HAS_QT_QUICK` includes:

```cpp
#if MORPH_CLIENT_HAS_DICTATION
#include <morph/qt/qt_dictation_recorder.hpp>
#endif
```

and after `options.dev = args->dev;`:

```cpp
#if MORPH_CLIENT_HAS_DICTATION
        options.dictation = [] { return std::make_unique<morph::qt::QtDictationRecorder>(); };
#endif
```

- [ ] **Step 4: Run them and expect PASS.**

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5 --target morph_client_tests morph_tests morph_remote_client
ctest --test-dir build -R 'Client shell: a (dictated clip|compact frame stops)|the_shell_shows_the_panel|the_frames_chat_slot' --output-on-failure
```

Expected: both new cases and the two existing chat-slot cases pass, and `morph_remote_client` links with the recorder.

- [ ] **Step 5: Mutation check (the shell's refresh on compact).**

```bash
cp src/client/shell.cpp /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/shell.cpp.bak
```

Delete the `_chatBinding->refresh()` added in the width-class listener. Rebuild `morph_client_tests`, run
`ctest --test-dir build -R 'Client shell: a compact frame stops' --output-on-failure`: FAIL at
`REQUIRE( window.wait(...) )` waiting for `stops == 1`. Restore with `cp` and `touch src/client/shell.cpp`, rebuild.

- [ ] **Step 6: Update the spec.**

In `docs/spec/client/client.md`, "Chat", append:

```markdown
Dictation needs a recorder, which the client application hands the shell as `ShellOptions::dictation`, a factory the
shell calls once when it builds the chat; the generic Qt Quick client passes a `QtDictationRecorder` when
`MORPH_ENABLE_DICTATION` is on, and a client without one shows no Dictate button. The binding reads whether the panel is
in view from `ShellFrame::chatVisible()` (a chat, signed in, not compact), and the shell refreshes it when the frame turns
compact, when the sign-in state changes and when the frame is rebuilt, so a recording never runs without the panel in
view. Signing out sends `ui-chat-end` first, which answers a waiting transcription with an error and forgets the offer
until the next probe.
```

- [ ] **Step 7: Verify the whole change.**

```bash
flock /tmp/claude-1000/-home-yaraslau-repo-morph/c1c9e9fc-f439-4fd4-a48b-bc49c0072deb/scratchpad/morph-build.lock timeout 580 cmake --build build -j5
ctest --test-dir build -R 'chat|host vocabulary|UiRegistry|Client shell|qt_transcriber|qt_dictation' --output-on-failure
QT_QPA_PLATFORM=offscreen ctest --test-dir build -L client --output-on-failure
git diff origin/master --name-only -- include/morph | xargs grep -nwE 'slots|signals|emit' || echo "no Qt keyword identifiers"
cmake -S . -B build-docs -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF
cmake --build build-docs --target doc
grep -n '[^[:print:][:space:]]' $(git diff origin/master --name-only -- 'tests/*.cpp') | grep TEST_CASE || echo "ASCII test names"
```

Expected: the full build succeeds; every listed test passes; no Qt keyword identifiers; Doxygen ends without a warning;
every test name is ASCII.

- [ ] **Step 8: Commit.**

```bash
git add include/morph/ui/shell_frame.hpp include/morph/client/shell.hpp src/client/shell.cpp examples/client/main.cpp \
        examples/client/CMakeLists.txt tests/client/test_custom_screens.cpp docs/spec/client/client.md
git commit -m "$(cat <<'MSG'
Let the client shell dictate into the chat, only while the panel is in view

The shell makes the recorder from ShellOptions::dictation, hands the binding the
frame's chat visibility, and refreshes it when the frame turns compact, the
sign-in state changes or the frame is rebuilt. The Qt Quick client passes a
QtDictationRecorder when dictation is built.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznwgG54dkVGoxXz2Z4
MSG
)"
```

---

## Design coverage

| Design section | Task |
|---|---|
| Goal, out of scope | Header; nothing built for spoken replies, wake words, partial transcripts or voice commands |
| Decisions table | Global Constraints; each decision lands in the task listed below |
| The person's view: button, Stop (12 s), Transcribing…, append rule, during a turn, failure notes | 9 (document, binding), 8 (notes), 10 (in the real shell) |
| Wire: two kinds, event, probe's `dictation`, events as hints, retention, "no such transcription" | 2, 3, 4, 8 |
| `ITranscriber`, `FakeTranscriber` | 3 |
| `makeQtTranscriber` | 6 |
| `ChatService`: accepting, language, table, ending exactly once, nothing kept | 3, 4 |
| Configuration and limits | 2 (file), 3 (`configure`, construction checks) |
| Framework seam: host names for one document; bundle refuses `morph.chat` | 1, 9 (panel-only registration) |
| `IDictationRecorder` | 5 |
| `QtDictationRecorder`, `resampleToSpeech`, `wavFromPcm16`, `MORPH_ENABLE_DICTATION` | 5, 7 |
| `ChatClient`: `dictation`, `transcribe`, `note` | 8 |
| `ChatPanelBinding` and the panel document | 9 |
| Failure modes table | 9 (no device, permission, refusal, provider error, hidden panel), 8 (lost connection, sign-out), 3 (refusals), 7 (clip at a bound) |
| Testing: service, config, Qt transcriber (live gated on `MORPH_LIVE_STT_KEY`), audio, host seam, binding and panel | 3, 4, 2, 6, 5, 1 and 9, 9 |
| Mutation checks: bound, `busy`, principal cap, cancel on deadline and on `ui-chat-end`, append watch, probe field, document scoping | 3, 3, 3, 4, 4, 9, 3, 1 and 9 |
| Where it lands: one PR from `feat/chat-dictation` with `chat.md` and the host-vocabulary spec | Every task's spec step; 10's verification |
