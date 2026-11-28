// SPDX-License-Identifier: Apache-2.0

// Source of a custom screen the client's cache dropped and the server no longer has: the person is told it could not
// be read, and no source view opens.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/ui/custom_screen_chrome.hpp>
#include <morph/ui/custom_screens.hpp>
#include <optional>
#include <stdexcept>
#include <string>

#include "ui_custom_client_support.hpp"

using custom_client::ChromeRig;
using custom_client::kEdit;
using custom_client::note;

TEST_CASE("covj custom screen source: a dropped document the server no longer has is reported unreadable",
          "[covj][ui][custom-screens][chrome]") {
    ChromeRig rig;
    std::string const id = rig.saveOnServer("Mine", "notes.edit", std::string{kEdit});
    std::optional<bool> fetched;
    rig.sync->open(id, [&fetched](auto loaded) { fetched = loaded.has_value(); });
    REQUIRE(rig.pump([&fetched] { return fetched.has_value(); }));
    rig.open(id, note(1));
    rig.sync->documents().forget(id);
    // The list still names the screen, so its title is known; the server answers the fetch with a refusal.
    auto const removed = rig.server->call(morph::ui::kCustomScreensRemove,
                                          R"({"app":"notes","id":)" + screens_test::quoted(id) + "}", "ann");
    REQUIRE(removed.kind == "ok");
    rig.click("source_button_" + id);
    REQUIRE(rig.pump([&rig] {
        for (std::string const& line : rig.notices) {
            if (line.contains("could not be read")) {
                return true;
            }
        }
        return false;
    }));
    CHECK(rig.notices.back().contains("Mine"));
    CHECK_FALSE(rig.find("TextInput", "source_text").has_value());
    CHECK(rig.sync->documents().entry(id) == nullptr);
}

TEST_CASE("covj custom screen chrome: options it cannot work without are refused at construction",
          "[covj][ui][custom-screens][chrome]") {
    ChromeRig rig;
    auto options = rig.chromeOptions();
    SECTION("no runtime") { options.runtime = nullptr; }
    SECTION("no backend") { options.backend = nullptr; }
    SECTION("no host") { options.host = nullptr; }
    SECTION("no sync") { options.sync = nullptr; }
    SECTION("no documents") { options.documents = nullptr; }
    SECTION("no sourceOf") { options.sourceOf = nullptr; }
    SECTION("no catalog") { options.catalog = nullptr; }
    SECTION("no openTab") { options.openTab = nullptr; }
    CHECK_THROWS_AS(morph::ui::CustomScreenChrome{std::move(options)}, std::invalid_argument);
}

TEST_CASE("covj custom screen chrome: a refusal at a place the text lacks is named by its pointer",
          "[covj][ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("source_button_notes.edit");
    std::string const withoutParams =
        screens_test::replaced(std::string{kEdit}, R"("params":{"noteId":"int"},"identity":{"ref":"noteId"},)", "");
    rig.type("source_text", withoutParams);
    rig.click("source_preview");
    REQUIRE(rig.pump([&rig] { return rig.shown("Button", "source_refusal"); }));
    CHECK(rig.harness.backend.prop(*rig.find("Button", "source_refusal"), "label").starts_with("At /params: "));
    CHECK(rig.text("heading") == "Edit note");
}

TEST_CASE(
    "covj custom screen chrome: the stored source of my screen that the server refuses stays open with the refusal",
    "[covj][ui][custom-screens][chrome]") {
    ChromeRig rig;
    std::string const id = rig.saveOnServer("Mine", "notes.edit", std::string{kEdit});
    rig.chrome->showStoredSource(id);
    REQUIRE(rig.pump([&rig] { return rig.find("Dialog", "source_dialog").has_value(); }));
    std::string const printing =
        screens_test::replaced(screens_test::replaced(std::string{kEdit}, "\"notes.edit\"", "\"" + id + "\""),
                               R"({"set":"content","value":"changed"})", R"({"host":"print"})");
    rig.type("source_text", morph::ui::prettyJson(printing));
    rig.click("source_save");
    REQUIRE(rig.pump([&rig] { return rig.shown("Button", "source_refusal"); }));
    CHECK(rig.find("Dialog", "source_dialog").has_value());
    CHECK(rig.sync->documents().find(id) == nullptr);
}

TEST_CASE("covj custom screen chrome: Save as and Remove of a tab that closed, or with nothing to ask, do nothing",
          "[covj][ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    morph::ui::WorkspaceTabId const tab = rig.tabAt(0);
    std::optional<bool> closed;
    rig.workspace->close(0, [&closed](bool outcome) { closed = outcome; });
    REQUIRE(rig.pump([&closed] { return closed.has_value(); }));
    rig.chrome->askSaveAs(tab, false);
    rig.chrome->askRemove(tab);
    rig.chrome->confirmSaveAs();
    rig.chrome->confirmRemove();
    rig.chrome->save(tab);
    rig.chrome->discard(tab);
    rig.chrome->openAs(tab, "notes.home");
    rig.harness.settle();
    CHECK_FALSE(rig.find("Dialog", "save_as_dialog").has_value());
    CHECK_FALSE(rig.find("Dialog", "remove_dialog").has_value());
    CHECK(rig.notices.empty());
}

TEST_CASE("covj custom screen chrome: Save as with no name saves nothing and keeps the dialog",
          "[covj][ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    custom_client::draftHeading(rig, "notes.edit", "Edited by hand");
    rig.click("draft_save_as_notes.edit");
    rig.type("save_as_title", "");
    rig.click("save_as_confirm");
    rig.harness.settle();
    CHECK(rig.find("Dialog", "save_as_dialog").has_value());
    CHECK(rig.sync->listed().screens.empty());
    CHECK(rig.shown("Row", "draft_bar_notes.edit"));
}

TEST_CASE(
    "covj custom screen chrome: Open as the screen already shown, and Remove of an application screen, do nothing",
    "[covj][ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    morph::ui::WorkspaceTabId const tab = rig.tabAt(0);
    rig.chrome->openAs(tab, "notes.edit");
    rig.chrome->askRemove(tab);
    rig.harness.settle();
    CHECK(rig.screenOf(0) == "notes.edit");
    CHECK_FALSE(rig.find("Dialog", "remove_dialog").has_value());
    CHECK(rig.notices.empty());
}
