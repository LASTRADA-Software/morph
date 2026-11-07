// SPDX-License-Identifier: Apache-2.0
//
// The chat panel, on the recording backend, and its place in the shell frame.

#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/chat/client.hpp>
#include <morph/ui/chat/panel.hpp>
#include <morph/ui/chat/protocol.hpp>
#include <morph/ui/service_client.hpp>
#include <morph/ui/shell_frame.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ui = morph::ui;
namespace chat = morph::ui::chat;
namespace wire = morph::wire;

namespace {

struct Rig {
    ui::testing::RecordingBackend backend;
    std::vector<wire::Envelope> sent;
    std::vector<std::function<void(ui::ExchangeReply)>> answers;
    std::optional<chat::TurnRequest> context = chat::TurnRequest{};
    std::unique_ptr<ui::StackWidget> root = backend.createStack(nullptr, ui::Axis::Vertical);
    std::unique_ptr<chat::ChatPanel> panel;
    chat::ChatClient client{[this](wire::Envelope request, std::function<void(ui::ExchangeReply)> done) {
                                sent.push_back(std::move(request));
                                answers.push_back(std::move(done));
                            },
                            [] { return morph::session::Context{}; },
                            [this] {
                                if (panel != nullptr) {
                                    panel->refresh();
                                }
                            }};

    Rig() {
        context->screenId = "the.screen";
        panel = std::make_unique<chat::ChatPanel>(backend, *root, client, [this] { return context; });
    }

    [[nodiscard]] int id(std::string_view kind, std::string_view testId) const {
        auto const found = backend.find(kind, "testId", testId);
        REQUIRE(found.has_value());
        return *found;
    }

    [[nodiscard]] bool visible(std::string_view kind, std::string_view testId) const {
        return backend.prop(id(kind, testId), "visible") != "false";
    }

    [[nodiscard]] std::string entry(int index) const {
        return backend.prop(id("Text", "chat_entry_" + std::to_string(index)), "text");
    }

    void accept() { answers.at(0)(wire::makeOk(0, R"({"turnId":1})")); }
};

}  // namespace

// Mutation: read the context when the panel is built rather than at the send.
TEST_CASE("the_panel_shows_entries_and_sends_with_fresh_context", "[chat][panel]") {
    Rig rig;
    rig.context->screenId = "changed.since.building";
    rig.backend.submit(rig.id("TextInput", "chat_input"), "hello");
    REQUIRE(rig.sent.size() == 1);
    auto const body = chat::readTurnRequest(rig.sent.front().body);
    REQUIRE(body.has_value());
    CHECK(body->screenId == "changed.since.building");
    CHECK(body->text == "hello");
    CHECK(rig.entry(0).contains("hello"));
}

// Mutation: leave the draft in the input after sending.
TEST_CASE("sending_with_the_button_uses_what_was_typed_and_clears_it", "[chat][panel]") {
    Rig rig;
    int const input = rig.id("TextInput", "chat_input");
    rig.backend.edit(input, "typed");
    rig.backend.click(rig.id("Button", "chat_send"));
    REQUIRE(rig.sent.size() == 1);
    CHECK(chat::readTurnRequest(rig.sent.front().body)->text == "typed");
    CHECK(rig.backend.prop(input, "text").empty());
}

// Mutation: send whatever the input holds.
TEST_CASE("blank_text_is_not_sent", "[chat][panel]") {
    Rig rig;
    rig.backend.submit(rig.id("TextInput", "chat_input"), "   ");
    CHECK(rig.sent.empty());
}

// Mutation: leave the cancel button always shown.
TEST_CASE("the_panel_shows_cancel_only_while_running", "[chat][panel]") {
    Rig rig;
    CHECK_FALSE(rig.visible("Button", "chat_cancel"));
    rig.backend.submit(rig.id("TextInput", "chat_input"), "hello");
    CHECK(rig.visible("Button", "chat_cancel"));
    rig.answers.at(0)(wire::makeErr("refused", 0));
    CHECK_FALSE(rig.visible("Button", "chat_cancel"));
    CHECK(rig.entry(1).contains("refused"));
}

// Mutation: leave the input enabled with no screen behind it.
TEST_CASE("a_host_screen_disables_the_input", "[chat][panel]") {
    Rig rig;
    rig.context.reset();
    rig.panel->refresh();
    CHECK(rig.backend.prop(rig.id("TextInput", "chat_input"), "enabled") == "false");
    CHECK(rig.backend.prop(rig.id("Button", "chat_send"), "enabled") == "false");
    rig.backend.submit(rig.id("TextInput", "chat_input"), "hello");
    CHECK(rig.sent.empty());
}

// Mutation: never hide an entry past the current conversation's end.
TEST_CASE("switching_to_a_shorter_conversation_hides_the_extra_lines", "[chat][panel]") {
    Rig rig;
    rig.backend.submit(rig.id("TextInput", "chat_input"), "hello");
    rig.client.selectConversation("elsewhere", "");
    CHECK_FALSE(rig.backend.find("Text", "testId", "chat_entry_0").has_value());
}

// Mutation: leave the slot's visibility to the caller alone.
TEST_CASE("the_frames_chat_slot_hides_signed_out_and_compact", "[chat][panel]") {
    ui::testing::RecordingBackend backend;
    ui::ShellFrame frame{backend, nullptr, ui::ShellFrameOptions{}};
    int const chat = backend.find("Row", "testId", "shell_chat").value();
    CHECK(backend.prop(chat, "visible") == "false");
    frame.setChatShown(true);
    CHECK(backend.prop(chat, "visible") != "false");
    frame.showSignedIn(false);
    CHECK(backend.prop(chat, "visible") == "false");
    frame.showSignedIn(true);
    CHECK(backend.prop(chat, "visible") != "false");
    frame.setCompact(true);
    CHECK(backend.prop(chat, "visible") == "false");
}
