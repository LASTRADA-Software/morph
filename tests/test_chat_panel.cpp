// SPDX-License-Identifier: Apache-2.0
//
// The chat panel, a document like any screen, on the recording backend, and its place in the shell frame.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/chat/client.hpp>
#include <morph/ui/chat/panel.hpp>
#include <morph/ui/chat/protocol.hpp>
#include <morph/ui/host_vocabulary.hpp>
#include <morph/ui/service_client.hpp>
#include <morph/ui/shell_frame.hpp>
#include <morph/ui/testing/fake_dictation_recorder.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
namespace chat = morph::ui::chat;
namespace wire = morph::wire;
using morph::ui::testing::Harness;

namespace {

struct Rig {
    Harness harness;
    ui::HostVocabulary host;
    std::vector<wire::Envelope> sent;
    std::vector<std::function<void(ui::ExchangeReply)>> answers;
    std::optional<chat::TurnRequest> context = chat::TurnRequest{};
    chat::testing::FakeDictationRecorder recorder;
    bool panelVisible = true;
    std::unique_ptr<chat::ChatPanelBinding> binding;
    chat::ChatClient client{[this](wire::Envelope request, std::function<void(ui::ExchangeReply)> done) {
                                sent.push_back(std::move(request));
                                answers.push_back(std::move(done));
                            },
                            [] { return morph::session::Context{}; },
                            [this] {
                                if (binding != nullptr) {
                                    binding->refresh();
                                }
                            }};
    ui::Interpreter interpreter{harness.session, harness.dispatcher, ui::InterpreterOptions{.host = &harness.host}};
    std::unique_ptr<ui::Screen> screen;

    explicit Rig(bool const withContext = true) {
        context->screenId = "the.screen";
        host.connect(harness.session);
        std::function<std::optional<chat::TurnRequest>()> reader;
        if (withContext) {
            reader = [this] { return context; };
        }
        binding = std::make_unique<chat::ChatPanelBinding>(
            host, client, std::move(reader), std::function<std::uint64_t()>{},
            std::function<std::optional<std::string>()>{}, &recorder, [this] { return panelVisible; });
        auto const document = chat::chatPanelDocument();
        INFO((document ? std::string{} : document.error().message()));
        REQUIRE(document.has_value());
        auto mounted = interpreter.mount(*document, {}, harness.backend);
        REQUIRE(mounted.has_value());
        screen = std::move(*mounted);
        harness.settle();
    }

    [[nodiscard]] int id(std::string_view kind, std::string_view testId) const {
        auto const found = harness.backend.find(kind, "testId", testId);
        REQUIRE(found.has_value());
        return *found;
    }

    [[nodiscard]] bool visible(std::string_view kind, std::string_view testId) const {
        return harness.backend.prop(id(kind, testId), "visible") != "false";
    }

    [[nodiscard]] bool shows(std::string_view text) const { return harness.backend.dump().contains(text); }

    void submit(std::string text) {
        harness.backend.submit(id("TextInput", "chat_input"), std::move(text));
        harness.settle();
    }

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

    [[nodiscard]] std::string dictateLabel() const {
        return harness.backend.prop(id("Button", "chat_dictate"), "label");
    }

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
        answers.back()(
            wire::makeOk(0, chat::writeMessage(chat::TranscriptionState{.id = transcriptions,
                                                                        .status = chat::TranscriptionStatus::Done,
                                                                        .text = text,
                                                                        .error = std::nullopt})));
        harness.settle();
    }

    std::uint64_t transcriptions = 0;
};

}  // namespace

// Mutation: write the panel with a screen-specific host, or leave the document out of the vocabulary every screen
// uses.
TEST_CASE("the_panel_document_loads_with_no_refusal", "[chat][panel]") {
    auto const document = chat::chatPanelDocument();
    INFO((document ? std::string{} : document.error().message()));
    REQUIRE(document.has_value());
    CHECK((*document)->id == chat::kChatPanelId);
}

// Mutation: read the context when the binding is built rather than at the send.
TEST_CASE("the_panel_shows_entries_and_sends_with_fresh_context", "[chat][panel]") {
    Rig rig;
    rig.context->screenId = "changed.since.building";
    rig.submit("hello");
    REQUIRE(rig.sent.size() == 1);
    auto const body = chat::readTurnRequest(rig.sent.front().body);
    REQUIRE(body.has_value());
    CHECK(body->screenId == "changed.since.building");
    CHECK(body->text == "hello");
    CHECK(rig.shows("You: hello"));
}

// Mutation: leave the draft out of what the button sends.
TEST_CASE("sending_with_the_button_uses_what_was_typed_and_clears_it", "[chat][panel]") {
    Rig rig;
    int const input = rig.id("TextInput", "chat_input");
    rig.harness.backend.edit(input, "typed");
    rig.harness.settle();
    rig.harness.backend.click(rig.id("Button", "chat_send"));
    rig.harness.settle();
    REQUIRE(rig.sent.size() == 1);
    CHECK(chat::readTurnRequest(rig.sent.front().body)->text == "typed");
    CHECK(rig.harness.backend.prop(input, "text").empty());
}

// Mutation: send whatever the input holds.
TEST_CASE("blank_text_is_not_sent", "[chat][panel]") {
    Rig rig;
    rig.submit("   ");
    CHECK(rig.sent.empty());
}

// Mutation: leave the stop button always shown.
TEST_CASE("the_panel_shows_stop_only_while_running", "[chat][panel]") {
    Rig rig;
    CHECK_FALSE(rig.visible("Button", "chat_cancel"));
    rig.submit("hello");
    CHECK(rig.visible("Button", "chat_cancel"));
    CHECK(rig.harness.backend.prop(rig.id("Button", "chat_send"), "enabled") == "false");
    rig.answers.at(0)(wire::makeErr("refused", 0));
    rig.harness.settle();
    CHECK_FALSE(rig.visible("Button", "chat_cancel"));
    CHECK(rig.shows("refused"));
    CHECK(rig.harness.backend.prop(rig.id("Button", "chat_send"), "enabled") != "false");
}

// Mutation: leave the input enabled with no screen behind it.
TEST_CASE("a_host_screen_disables_the_input", "[chat][panel]") {
    Rig rig;
    rig.context.reset();
    rig.binding->refresh();
    rig.harness.settle();
    CHECK(rig.harness.backend.prop(rig.id("TextInput", "chat_input"), "enabled") == "false");
    CHECK(rig.harness.backend.prop(rig.id("Button", "chat_send"), "enabled") == "false");
    rig.submit("hello");
    CHECK(rig.sent.empty());
}

// Mutation: key the lines so one is left behind.
TEST_CASE("switching_to_a_shorter_conversation_hides_the_extra_lines", "[chat][panel]") {
    Rig rig;
    rig.submit("hello");
    REQUIRE(rig.shows("You: hello"));
    rig.client.selectConversation("elsewhere", "");
    rig.harness.settle();
    CHECK_FALSE(rig.shows("You: hello"));
}

// Mutation: leave the slot's visibility to the caller alone.
TEST_CASE("the_frames_chat_slot_hides_signed_out_and_compact", "[chat][panel]") {
    ui::testing::RecordingBackend backend;
    ui::ShellFrame frame{backend, nullptr, ui::ShellFrameOptions{}};
    int const chatColumn = backend.find("Row", "testId", "shell_chat").value();
    CHECK(backend.prop(chatColumn, "visible") == "false");
    frame.setChatShown(true);
    CHECK(backend.prop(chatColumn, "visible") != "false");
    frame.showSignedIn(false);
    CHECK(backend.prop(chatColumn, "visible") == "false");
    frame.showSignedIn(true);
    CHECK(backend.prop(chatColumn, "visible") != "false");
    frame.setCompact(true);
    CHECK(backend.prop(chatColumn, "visible") == "false");
}

// Mutation: show a tool step's line like any other.
TEST_CASE("the_panel_shows_a_tool_step_as_what_ran", "[chat][panel]") {
    Rig rig;
    rig.submit("hello");
    rig.answers.at(0)(wire::makeOk(0, chat::writeMessage(chat::TurnAccepted{.turnId = 7})));
    rig.harness.settle();
    chat::TurnState state;
    state.turnId = 7;
    state.status = chat::TurnStatus::Done;
    state.steps.push_back({.name = "lab__Add", .ok = true, .message = "{}"});
    rig.answers.at(1)(wire::makeOk(0, chat::writeMessage(state)));
    rig.harness.settle();
    CHECK(rig.shows("Ran lab__Add"));
}

// Mutation: send while a turn is running.
TEST_CASE("the_panel_does_not_send_while_a_turn_runs", "[chat][panel]") {
    Rig rig;
    rig.submit("hello");
    rig.submit("again");
    CHECK(rig.sent.size() == 1);
}

// Mutation: enable the input whenever a screen is open. Enter in the input would then send nothing while a turn runs
// and clear what was typed, so the message is lost.
TEST_CASE("the_input_takes_text_only_while_a_message_can_be_sent", "[chat][panel]") {
    Rig rig;
    int const input = rig.id("TextInput", "chat_input");
    CHECK(rig.harness.backend.prop(input, "enabled") != "false");
    rig.submit("hello");
    CHECK(rig.harness.backend.prop(input, "enabled") == "false");
    rig.answers.at(0)(wire::makeErr("refused", 0));
    rig.harness.settle();
    CHECK(rig.harness.backend.prop(input, "enabled") != "false");
}

// Mutation: call the context without asking whether there is one.
TEST_CASE("a_panel_with_no_context_disables_the_input", "[chat][panel]") {
    Rig rig{false};
    CHECK(rig.harness.backend.prop(rig.id("TextInput", "chat_input"), "enabled") == "false");
    rig.submit("hello");
    CHECK(rig.sent.empty());
}

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
    rig.answers.back()(
        wire::makeOk(0, chat::writeMessage(chat::TranscriptionState{
                            .id = 1, .status = chat::TranscriptionStatus::Done, .text = "x", .error = std::nullopt})));
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
    // Beside it: the panel itself received the same transcript.
    CHECK(rig.input() == "private words");
    CHECK(std::ranges::any_of(rig.harness.diagnostics, [](auto const& refusal) {
        return refusal.message().contains("host command `chat.dictateStart` is missing");
    }));
}
