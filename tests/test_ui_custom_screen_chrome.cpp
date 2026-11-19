// SPDX-License-Identifier: Apache-2.0
//
// The custom screens chrome (docs/spec/ui/custom_screens.md, "The client"): Source on every document screen, Preview
// through the client's load and the server's check into a draft, the refusal and its line, the draft bar's Save,
// Save as… and Discard, and the store's refusal shown to the person.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/ui/custom_screen_chrome.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/json_source.hpp>
#include <optional>
#include <string>

#include "ui_custom_client_support.hpp"

using custom_client::ChromeRig;
using custom_client::draftHeading;
using custom_client::kEdit;
using custom_client::note;
using custom_client::Offer;
using morph::ui::DocumentScreenHost;
using morph::ui::ParamMap;

// Mutation: skip mountDraft after the server's check (the heading stays "Edit note").
TEST_CASE(
    "custom screen chrome: Source shows the screen's document, and Preview mounts the edit as the window's draft",
    "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("change");
    REQUIRE(rig.text("body") == "changed");
    CHECK_FALSE(rig.shown("Row", "draft_bar_notes.edit"));

    rig.click("source_button_notes.edit");
    std::string const shown = rig.field("source_text");
    CHECK(shown == morph::ui::prettyJson(kEdit));
    rig.type("source_text", screens_test::replaced(shown, "\"Edit note\"", "\"Edited by hand\""));
    rig.click("source_preview");
    REQUIRE(rig.pump([&rig] { return rig.text("heading") == "Edited by hand"; }));
    CHECK(rig.text("body") == "changed");
    CHECK(rig.shown("Row", "draft_bar_notes.edit"));
    CHECK_FALSE(rig.shown("Row", "screen_tools_notes.edit"));
    CHECK(rig.screenOf(0) == "notes.edit");
    REQUIRE(rig.pump([&rig] { return !rig.find("Dialog", "source_dialog").has_value(); }));
}

// Mutations: skip placeCursor when the refusal is chosen (the cursor stays at the end of the text); skip the client's
// id check (the host refuses the mount instead, with no pointer, so the label names no line of `"id"`).
TEST_CASE("custom screen chrome: a refusal names its line, and choosing it moves the cursor there",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("source_button_notes.edit");
    std::string const edited = screens_test::replaced(rig.field("source_text"), "\"notes.edit\"", "\"notes.other\"");
    rig.type("source_text", edited);
    rig.click("source_preview");
    REQUIRE(rig.pump([&rig] { return rig.shown("Button", "source_refusal"); }));
    std::size_t const idAt = edited.find("\"id\"");
    int const refusal = *rig.find("Button", "source_refusal");
    CHECK(rig.harness.backend.prop(refusal, "label")
              .starts_with("Line " + std::to_string(morph::ui::lineAt(edited, idAt)) + ":"));
    rig.click("source_refusal");
    CHECK(rig.harness.backend.cursor(*rig.find("TextInput", "source_text")) == idAt);
    CHECK(rig.text("heading") == "Edit note");
    CHECK_FALSE(rig.shown("Row", "draft_bar_notes.edit"));
}

// Mutation: skip the server's check (the printing document mounts as the draft).
TEST_CASE("custom screen chrome: what the client cannot load, and what the server refuses, stay out of the window",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("source_button_notes.edit");
    rig.type("source_text", screens_test::replaced(rig.field("source_text"), "\"column\"", "\"hologram\""));
    rig.click("source_preview");
    REQUIRE(rig.pump([&rig] { return rig.shown("Button", "source_refusal"); }));
    CHECK(rig.text("heading") == "Edit note");

    std::string const printing =
        screens_test::replaced(std::string{kEdit}, R"({"set":"content","value":"changed"})", R"({"host":"print"})");
    rig.type("source_text", morph::ui::prettyJson(printing));
    CHECK_FALSE(rig.shown("Button", "source_refusal"));
    rig.click("source_preview");
    REQUIRE(rig.pump([&rig] { return rig.shown("Button", "source_refusal"); }));
    CHECK(rig.text("heading") == "Edit note");
    CHECK_FALSE(rig.shown("Row", "draft_bar_notes.edit"));
}

// Mutation: open the source editable whatever the offer (`readonly` is not set, and Preview is there).
TEST_CASE("custom screen chrome: without a store, Source is read only, with no Preview and no Save as",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig{Offer::NoStore};
    CHECK(rig.sync->offer() == morph::ui::CustomScreensOffer::Off);
    rig.open("notes.edit", note(1));
    rig.click("source_button_notes.edit");
    REQUIRE(rig.find("TextInput", "source_text").has_value());
    CHECK(rig.harness.backend.prop(*rig.find("TextInput", "source_text"), "readonly") == "true");
    CHECK_FALSE(rig.find("Button", "source_preview").has_value());
    CHECK_FALSE(rig.find("Button", "source_save_as").has_value());
}

// Mutation: skip endDraft before opening the saved screen (the old tab's close goes to the guard, which keeps it, and
// the tab stays `notes.edit`).
TEST_CASE("custom screen chrome: Save as… names the draft, saves it, and the tab becomes the saved screen",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(3));
    draftHeading(rig, "notes.edit", "Edited by hand");
    rig.click("draft_save_as_notes.edit");
    rig.type("save_as_title", "My note");
    rig.click("save_as_confirm");
    REQUIRE(rig.pump([&rig] { return morph::ui::isCustomScreenId(rig.screenOf(0)); }));
    CHECK(rig.workspace->tabs().size() == 1);
    CHECK(rig.text("heading") == "Edited by hand");
    std::string const id = rig.screenOf(0);
    CHECK_FALSE(rig.shown("Row", "draft_bar_" + id));
    CHECK(DocumentScreenHost::paramsOf(*rig.workspace->handleOf(0)) == std::optional<ParamMap>{note(3)});
    REQUIRE(rig.sync->listed().screens.size() == 1);
    CHECK(rig.sync->listed().screens.front().title == "My note");
    CHECK(rig.guarded == 0);
}

// Mutation: save from the source view without previewing (the refused document is saved: the list has a screen).
TEST_CASE("custom screen chrome: Save as… from the source view previews first, and saves nothing it refuses",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("source_button_notes.edit");
    rig.type("source_text", screens_test::replaced(rig.field("source_text"), "\"notes.edit\"", "\"notes.other\""));
    rig.click("source_save_as");
    rig.type("save_as_title", "Never");
    rig.click("save_as_confirm");
    REQUIRE(rig.pump([&rig] { return rig.shown("Button", "source_refusal"); }));
    static_cast<void>(rig.pump([] { return false; }, std::chrono::milliseconds{200}));
    CHECK(rig.sync->listed().screens.empty());

    rig.type("source_text", screens_test::replaced(rig.field("source_text"), "\"notes.other\"", "\"notes.edit\""));
    rig.type("save_as_title", "Mine");
    rig.click("save_as_confirm");
    REQUIRE(rig.pump([&rig] { return morph::ui::isCustomScreenId(rig.screenOf(0)); }));
    CHECK(rig.sync->listed().screens.size() == 1);
}

// Mutation: hide Save on every draft (the draft of my own custom screen has no Save button).
TEST_CASE("custom screen chrome: Save overwrites a draft of my own custom screen", "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    std::string const id = rig.saveOnServer("Mine", "notes.edit", std::string{kEdit});
    std::optional<bool> fetched;
    rig.sync->open(id, [&fetched](auto loaded) { fetched = loaded.has_value(); });
    REQUIRE(rig.pump([&fetched] { return fetched.has_value(); }));
    REQUIRE(*fetched);
    rig.open(id, note(1));
    draftHeading(rig, id, "Saved by hand");
    REQUIRE(rig.shown("Button", "draft_save_" + id));
    std::string const before = rig.sync->listed().screens.front().hash;
    rig.click("draft_save_" + id);
    REQUIRE(rig.pump([&rig, &id, &before] {
        return !rig.shown("Row", "draft_bar_" + id) && rig.sync->listed().screens.front().hash != before;
    }));
    CHECK(rig.text("heading") == "Saved by hand");
    REQUIRE(rig.sync->documents().entry(id) != nullptr);
    CHECK(rig.sync->documents().entry(id)->text.contains("Saved by hand"));
    CHECK(rig.workspace->tabs().size() == 1);
}

// Mutation: drop the bar's message on a refused save (`draft_message_notes.edit` stays hidden).
TEST_CASE("custom screen chrome: a store's refusal shows in the draft's bar, and the draft stays",
          "[ui][custom-screens][chrome]") {
    morph::exec::ThreadPoolExecutor storePool{1};
    ChromeRig rig{Offer::Store, std::make_shared<custom_client::RefusingStore>(storePool)};
    rig.open("notes.edit", note(1));
    draftHeading(rig, "notes.edit", "Edited by hand");
    rig.click("draft_save_as_notes.edit");
    rig.type("save_as_title", "Too many");
    rig.click("save_as_confirm");
    REQUIRE(rig.pump([&rig] { return rig.shown("Text", "draft_message_notes.edit"); }));
    CHECK(rig.text("draft_message_notes.edit").contains("over your limit of custom screens"));
    CHECK(rig.shown("Row", "draft_bar_notes.edit"));
    CHECK(rig.text("heading") == "Edited by hand");
    CHECK(rig.screenOf(0) == "notes.edit");
}

// Mutation: make Discard end the draft without remounting (the heading stays "Edited by hand").
TEST_CASE("custom screen chrome: Discard restores the screen, and closing a drafted tab asks first",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("change");
    draftHeading(rig, "notes.edit", "Edited by hand");
    rig.click("draft_discard_notes.edit");
    REQUIRE(rig.pump([&rig] { return rig.text("heading") == "Edit note"; }));
    CHECK(rig.text("body") == "changed");
    CHECK_FALSE(rig.shown("Row", "draft_bar_notes.edit"));

    draftHeading(rig, "notes.edit", "Again");
    std::optional<bool> closed;
    rig.workspace->close(0, [&closed](bool outcome) { closed = outcome; });
    REQUIRE(rig.pump([&closed] { return closed.has_value(); }));
    CHECK(rig.guarded == 1);
    CHECK_FALSE(*closed);
    CHECK(rig.workspace->tabs().size() == 1);
}
