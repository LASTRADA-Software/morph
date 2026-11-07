// SPDX-License-Identifier: Apache-2.0
//
// The chat panel, a document like any screen, on the recording backend, and its place in the shell frame.

#include <catch2/catch_test_macros.hpp>
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

    Rig() {
        context->screenId = "the.screen";
        host.connect(harness.session);
        binding = std::make_unique<chat::ChatPanelBinding>(host, client, [this] { return context; });
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
