// SPDX-License-Identifier: Apache-2.0
//
// The custom screens chrome (docs/spec/ui/custom_screens.md, "The client"): Source on every document screen, Preview
// through the client's load and the server's check into a draft, the refusal and its line, the draft bar's Save,
// Save as… and Discard, and the store's refusal shown to the person.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/ui/custom_screen_chrome.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/json_source.hpp>
#include <optional>
#include <string>
#include <vector>

#include "ui_custom_client_support.hpp"

using custom_client::ChromeRig;
using custom_client::draftHeading;
using custom_client::kEdit;
using custom_client::kHome;
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
TEST_CASE("custom screen chrome: Save as... names the draft, saves it, and the tab becomes the saved screen",
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

// Mutation: end the draft before openInPlace answers (the tab that stayed open shows the draft with no bar).
TEST_CASE("custom screen chrome: Save as... on a screen that stays open leaves the tab drafted",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    // A screen that is dirty of itself: its close goes to the guard, which keeps it open.
    std::string const dirty = rig.saveOnServer(
        "Alpha", "notes.edit",
        screens_test::replaced(std::string{kEdit}, R"("params")", R"("dirty":{"lit":true},"params")"));
    rig.sync->documents().forget(dirty);
    std::optional<bool> fetched;
    rig.sync->open(dirty, [&fetched](auto loaded) { fetched = loaded.has_value(); });
    REQUIRE(rig.pump([&fetched] { return fetched.has_value(); }));
    REQUIRE(*fetched);
    rig.open(dirty, note(3));
    draftHeading(rig, dirty, "Edited by hand");
    REQUIRE(rig.shown("Row", "draft_bar_" + dirty));
    std::string const draft = *rig.chrome->draftOf(rig.tabAt(0));
    rig.click("draft_save_as_" + dirty);
    rig.type("save_as_title", "My note");
    rig.click("save_as_confirm");
    REQUIRE(rig.pump([&rig] { return rig.guarded > 0; }));
    // The save outcome is told once the replacement answered.
    REQUIRE(rig.pump([&rig] { return !rig.notices.empty(); }));
    CHECK(rig.notices.back().contains("stays a draft"));
    CHECK(rig.workspace->tabs().size() == 1);
    CHECK(rig.screenOf(0) == dirty);
    CHECK(rig.text("heading") == "Edited by hand");
    CHECK(rig.shown("Row", "draft_bar_" + dirty));
    CHECK(rig.chrome->draftOf(rig.tabAt(0)) == std::optional<std::string>{draft});
}

// Mutation: save from the source view without previewing (the refused document is saved: the list has a screen).
TEST_CASE("custom screen chrome: Save as... from the source view previews first, and saves nothing it refuses",
          "[ui][custom-screens][chrome]") {
    morph::exec::ThreadPoolExecutor storePool{1};
    auto const store = std::make_shared<custom_client::GatedStore>(storePool);
    ChromeRig rig{Offer::Store, store};
    rig.open("notes.edit", note(1));
    rig.click("source_button_notes.edit");
    rig.type("source_text", screens_test::replaced(rig.field("source_text"), "\"notes.edit\"", "\"notes.other\""));
    rig.click("source_save_as");
    rig.type("save_as_title", "Never");
    rig.click("save_as_confirm");
    REQUIRE(rig.pump([&rig] { return rig.shown("Button", "source_refusal"); }));
    // A round trip after the refused press: a save sent by it would have reached the store before this answers.
    std::optional<bool> barrier;
    rig.sync->check("notes.edit", std::string{kEdit},
                    [&barrier](auto const& answered) { barrier = answered.has_value(); });
    REQUIRE(rig.pump([&barrier] { return barrier.has_value(); }));
    CHECK(store->puts == 0);
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
    REQUIRE_FALSE(rig.notices.empty());
    CHECK(rig.notices.back().starts_with("Saved"));
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

// Mutation: drop the read-only flag while a check runs (the editor stays editable, `readonly` is "false").
TEST_CASE("custom screen chrome: while a check runs the editor is read only, and a second Preview does nothing",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("source_button_notes.edit");
    int const editor = *rig.find("TextInput", "source_text");
    CHECK(rig.harness.backend.prop(editor, "readonly") != "true");
    rig.click("source_preview");
    REQUIRE(rig.find("TextInput", "source_text").has_value());
    CHECK(rig.harness.backend.prop(*rig.find("TextInput", "source_text"), "readonly") == "true");
    CHECK_FALSE(rig.enabled("Button", "source_preview"));
    std::optional<bool> again;
    rig.chrome->preview([&again](bool drafted) { again = drafted; });
    REQUIRE(again.has_value());
    CHECK_FALSE(*again);
    REQUIRE(rig.pump([&rig] { return rig.shown("Row", "draft_bar_notes.edit"); }));
}

// Mutation: take a check's reply whichever view asked it (the reply of the closed view mounts a draft the reopened
// view never asked for).
TEST_CASE("custom screen chrome: a reply to a check made for a view that was reopened does nothing",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("source_button_notes.edit");
    rig.type("source_text", screens_test::replaced(rig.field("source_text"), "\"Edit note\"", "\"Stale\""));
    rig.click("source_preview");
    REQUIRE(rig.find("Dialog", "source_dialog").has_value());
    rig.chrome->showSource(rig.tabAt(0));
    REQUIRE(rig.pump([&rig] { return rig.field("source_text") == morph::ui::prettyJson(kEdit); }));
    static_cast<void>(
        rig.pump([&rig] { return rig.shown("Row", "draft_bar_notes.edit"); }, std::chrono::milliseconds{300}));
    CHECK(rig.text("heading") == "Edit note");
    CHECK_FALSE(rig.shown("Row", "draft_bar_notes.edit"));
    CHECK(rig.find("Dialog", "source_dialog").has_value());
}

// Mutation: leave the editor read only after a refusal (`refuse` does not clear the busy flag).
TEST_CASE(
    "custom screen chrome: after a refusal the editor takes edits again, and the refusal is labelled with its line",
    "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("source_button_notes.edit");
    std::string const edited = screens_test::replaced(rig.field("source_text"), "\"notes.edit\"", "\"notes.other\"");
    rig.type("source_text", edited);
    rig.click("source_preview");
    REQUIRE(rig.pump([&rig] { return rig.shown("Button", "source_refusal"); }));
    int const refusal = *rig.find("Button", "source_refusal");
    CHECK(rig.harness.backend.prop(refusal, "label")
              .starts_with("Line " + std::to_string(morph::ui::lineAt(edited, edited.find("\"id\""))) + ":"));
    CHECK(rig.harness.backend.prop(*rig.find("TextInput", "source_text"), "readonly") != "true");
}

// Mutation: move the cursor for a refusal whose pointer is empty (the cursor jumps to the start of the text).
TEST_CASE("custom screen chrome: a refusal that names no place leaves the cursor where it is",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("source_button_notes.edit");
    rig.type("source_text", "{ not json");
    rig.click("source_preview");
    REQUIRE(rig.pump([&rig] { return rig.shown("Button", "source_refusal"); }));
    int const editor = *rig.find("TextInput", "source_text");
    std::size_t const before = rig.harness.backend.cursor(editor);
    rig.click("source_refusal");
    CHECK(rig.harness.backend.cursor(editor) == before);
}

// Mutation: show the source of a custom screen the cache dropped as an empty editor.
TEST_CASE("custom screen chrome: Source of a custom screen the cache dropped fetches it first",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    std::string const id = rig.saveOnServer("Mine", "notes.edit", std::string{kEdit});
    std::optional<bool> fetched;
    rig.sync->open(id, [&fetched](auto loaded) { fetched = loaded.has_value(); });
    REQUIRE(rig.pump([&fetched] { return fetched.has_value(); }));
    rig.open(id, note(1));
    rig.sync->documents().forget(id);
    REQUIRE(rig.sync->documents().entry(id) == nullptr);
    rig.click("source_button_" + id);
    REQUIRE(rig.pump([&rig] { return rig.find("TextInput", "source_text").has_value(); }));
    REQUIRE(rig.sync->documents().entry(id) != nullptr);
    CHECK(rig.field("source_text") == morph::ui::prettyJson(rig.sync->documents().entry(id)->text));
}

// Mutation: show Save whenever custom screens are offered (`canSave` returns the offer).
TEST_CASE("custom screen chrome: Save is hidden on a draft of an application screen and of another person's screen",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    draftHeading(rig, "notes.edit", "Edited by hand");
    CHECK(rig.shown("Row", "draft_bar_notes.edit"));
    CHECK_FALSE(rig.shown("Button", "draft_save_notes.edit"));
    CHECK(rig.shown("Button", "draft_save_as_notes.edit"));

    std::string const id = rig.saveOnServer("Mine", "notes.edit", std::string{kEdit});
    std::optional<bool> fetched;
    rig.sync->open(id, [&fetched](auto loaded) { fetched = loaded.has_value(); });
    REQUIRE(rig.pump([&fetched] { return fetched.has_value(); }));
    rig.open(id, note(2));
    draftHeading(rig, id, "Edited by hand");
    CHECK(rig.shown("Button", "draft_save_" + id));
    morph::ui::CustomScreensListed others = rig.sync->listed();
    others.screens.front().mine = false;
    rig.chrome->listed(others);
    rig.harness.settle();
    CHECK_FALSE(rig.shown("Button", "draft_save_" + id));
}

// Mutation: skip the generation check in the save reply (the tab is replaced by the saved screen although the draft
// that was saved is gone). The server runs one action of the model at a time, so a newer draft cannot be previewed
// before a held save answers; a discard is what ends the draft meanwhile.
TEST_CASE("custom screen chrome: a save that finishes after its draft was discarded leaves the tab as it is",
          "[ui][custom-screens][chrome]") {
    morph::exec::ThreadPoolExecutor storePool{1};
    auto const store = std::make_shared<custom_client::GatedStore>(storePool);
    ChromeRig rig{Offer::Store, store};
    custom_client::GateGuard const guard{*store};
    rig.open("notes.edit", note(1));
    draftHeading(rig, "notes.edit", "First");
    store->hold();
    rig.click("draft_save_as_notes.edit");
    rig.type("save_as_title", "First");
    rig.click("save_as_confirm");
    CHECK_FALSE(rig.enabled("Button", "draft_save_as_notes.edit"));
    rig.click("draft_discard_notes.edit");
    REQUIRE(rig.pump([&rig] { return rig.text("heading") == "Edit note"; }));
    store->release();
    REQUIRE(rig.pump([&rig] { return !rig.notices.empty(); }));
    CHECK(rig.notices.back().contains("is gone"));
    CHECK(rig.screenOf(0) == "notes.edit");
    CHECK(rig.text("heading") == "Edit note");
    CHECK_FALSE(rig.shown("Row", "draft_bar_notes.edit"));
    CHECK(rig.workspace->tabs().size() == 1);
    CHECK(rig.guarded == 0);
    rig.click("source_button_notes.edit");
    rig.type("source_text", screens_test::replaced(rig.field("source_text"), "\"Edit note\"", "\"Next\""));
    rig.click("source_preview");
    REQUIRE(rig.pump([&rig] { return rig.text("heading") == "Next"; }));
    CHECK(rig.enabled("Button", "draft_save_as_notes.edit"));
}

// Mutation: let Save run while one is pending (the store gets two puts).
TEST_CASE("custom screen chrome: a second Save while one is pending is ignored", "[ui][custom-screens][chrome]") {
    morph::exec::ThreadPoolExecutor storePool{1};
    auto const store = std::make_shared<custom_client::GatedStore>(storePool);
    ChromeRig rig{Offer::Store, store};
    custom_client::GateGuard const guard{*store};
    std::string const id = rig.saveOnServer("Mine", "notes.edit", std::string{kEdit});
    std::optional<bool> fetched;
    rig.sync->open(id, [&fetched](auto loaded) { fetched = loaded.has_value(); });
    REQUIRE(rig.pump([&fetched] { return fetched.has_value(); }));
    rig.open(id, note(1));
    draftHeading(rig, id, "Saved by hand");
    int const before = store->puts;
    store->hold();
    rig.click("draft_save_" + id);
    CHECK_FALSE(rig.enabled("Button", "draft_save_" + id));
    rig.chrome->save(rig.tabAt(0));
    rig.chrome->save(rig.tabAt(0));
    store->release();
    REQUIRE(rig.pump([&rig, &id] { return !rig.shown("Row", "draft_bar_" + id); }));
    CHECK(store->puts == before + 1);
}

// Mutation: let a second Save as… replace the dialog while one is saving (the typed name is lost).
TEST_CASE("custom screen chrome: a second Save as... while one is pending does not replace the dialog",
          "[ui][custom-screens][chrome]") {
    morph::exec::ThreadPoolExecutor storePool{1};
    auto const store = std::make_shared<custom_client::GatedStore>(storePool);
    ChromeRig rig{Offer::Store, store};
    custom_client::GateGuard const guard{*store};
    rig.open("notes.edit", note(1));
    draftHeading(rig, "notes.edit", "Edited by hand");
    store->hold();
    rig.click("draft_save_as_notes.edit");
    rig.type("save_as_title", "Once");
    rig.click("save_as_confirm");
    rig.chrome->askSaveAs(rig.tabAt(0), false);
    REQUIRE(rig.find("TextInput", "save_as_title").has_value());
    CHECK(rig.field("save_as_title") == "Once");
    store->release();
    REQUIRE(rig.pump([&rig] { return morph::ui::isCustomScreenId(rig.screenOf(0)); }));
    CHECK(rig.sync->listed().screens.size() == 1);
    CHECK(store->puts == 1);
}

// Mutation: act on the tab's id without finding it (the reply dereferences or reopens a closed tab).
TEST_CASE("custom screen chrome: a save that finishes after its tab closed tells the person and opens nothing",
          "[ui][custom-screens][chrome]") {
    morph::exec::ThreadPoolExecutor storePool{1};
    auto const store = std::make_shared<custom_client::GatedStore>(storePool);
    ChromeRig rig{Offer::Store, store};
    custom_client::GateGuard const guard{*store};
    rig.open("notes.edit", note(1));
    draftHeading(rig, "notes.edit", "Edited by hand");
    store->hold();
    rig.click("draft_save_as_notes.edit");
    rig.type("save_as_title", "Late");
    rig.click("save_as_confirm");
    rig.guardChoice = morph::ui::CloseChoice::Discard;
    std::optional<bool> closed;
    rig.workspace->close(0, [&closed](bool outcome) { closed = outcome; });
    REQUIRE(rig.pump([&closed] { return closed.has_value(); }));
    REQUIRE(*closed);
    CHECK(rig.workspace->tabs().empty());
    store->release();
    REQUIRE(rig.pump([&rig] { return !rig.notices.empty(); }));
    CHECK(rig.notices.back().contains("tab was closed"));
    CHECK(rig.workspace->tabs().empty());
}

// Mutation: key a pending reply by tab id alone (a tab of the replacement workspace with the same id is acted on).
TEST_CASE("custom screen chrome: a reply asked under a replaced workspace does nothing to the new one",
          "[ui][custom-screens][chrome]") {
    morph::exec::ThreadPoolExecutor storePool{1};
    auto const store = std::make_shared<custom_client::GatedStore>(storePool);
    ChromeRig rig{Offer::Store, store};
    custom_client::GateGuard const guard{*store};
    rig.open("notes.edit", note(1));
    draftHeading(rig, "notes.edit", "Edited by hand");
    store->hold();
    rig.click("draft_save_as_notes.edit");
    rig.type("save_as_title", "Old");
    rig.click("save_as_confirm");
    morph::ui::WorkspaceTabId const oldTab = rig.tabAt(0);

    rig.chrome->attach(nullptr);
    rig.workspace = std::make_unique<morph::ui::Workspace>(rig.harness.runtime, rig.harness.backend, *rig.host,
                                                           rig.workspaceOptions());
    rig.chrome->attach(&rig.workspace->model());
    rig.open("notes.edit", note(1));
    REQUIRE(rig.tabAt(0) == oldTab);
    store->release();
    REQUIRE(rig.pump([&rig] { return rig.sync->listed().screens.size() == 1; }));
    // Round trips after the save's reply: a reply that acted would have opened a custom screen by now.
    std::string const id = rig.sync->listed().screens.front().id;
    std::optional<bool> fetched;
    rig.sync->open(id, [&fetched](auto loaded) { fetched = loaded.has_value(); });
    REQUIRE(rig.pump([&fetched] { return fetched.has_value(); }));
    CHECK(rig.screenOf(0) == "notes.edit");
    CHECK(rig.notices.empty());
}

// Mutation: drop the pointer of the host's mount refusal (`refuse("", ...)`, the label names no line).
TEST_CASE("custom screen chrome: a refusal from the host's mount keeps its pointer", "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("source_button_notes.edit");
    std::string const edited = screens_test::replaced(rig.field("source_text"), "\"notes.edit\"", "\"notes.other\"");
    rig.type("source_text", edited);
    rig.click("source_preview");
    REQUIRE(rig.pump([&rig] { return rig.shown("Button", "source_refusal"); }));
    CHECK(rig.harness.backend.prop(*rig.find("Button", "source_refusal"), "label").starts_with("Line "));
}

// Mutation: drop the dialog-busy guard of askSaveAs (the dialog is replaced while its source is being previewed).
TEST_CASE("custom screen chrome: Save as... asked again while the source is being previewed keeps the dialog",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("source_button_notes.edit");
    rig.click("source_save_as");
    rig.type("save_as_title", "Once");
    rig.click("save_as_confirm");
    rig.chrome->askSaveAs(rig.tabAt(0), true);
    REQUIRE(rig.find("TextInput", "save_as_title").has_value());
    CHECK(rig.field("save_as_title") == "Once");
    REQUIRE(rig.pump([&rig] { return morph::ui::isCustomScreenId(rig.screenOf(0)); }));
    CHECK(rig.sync->listed().screens.size() == 1);
}

// Mutation: open the chosen screen in a tab of its own (two tabs remain).
TEST_CASE("custom screen chrome: Open as lists the screens based on this one and opens one in the same tab and params",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    std::string const alpha = rig.saveOnServer("Alpha", "notes.edit", std::string{kEdit});
    static_cast<void>(rig.saveOnServer("Beta", "notes.edit", std::string{kEdit}));
    static_cast<void>(rig.saveOnServer("Homely", "notes.home", std::string{kHome}));
    rig.open("notes.edit", note(7));
    REQUIRE(rig.pump([&rig] { return rig.find("Menu", "open_as_notes.edit").has_value(); }));
    int const menu = *rig.find("Menu", "open_as_notes.edit");
    std::string const items = rig.harness.backend.prop(menu, "items");
    CHECK(items.contains("Note"));
    CHECK(items.contains("Alpha"));
    CHECK(items.contains("Beta"));
    CHECK_FALSE(items.contains("Homely"));

    // The base first, then the custom screens of that base in the list's order.
    std::size_t place = 1;
    for (morph::ui::CustomScreenSummary const& screen : rig.sync->listed().screens) {
        if (screen.basedOn != "notes.edit") {
            continue;
        }
        if (screen.id == alpha) {
            break;
        }
        ++place;
    }
    rig.harness.backend.chooseMenuEntry(menu, {place});
    REQUIRE(rig.pump([&rig, &alpha] { return rig.workspace->tabs().size() == 1 && rig.screenOf(0) == alpha; }));
    CHECK(DocumentScreenHost::paramsOf(*rig.workspace->handleOf(0)) == std::optional<ParamMap>{note(7)});
    CHECK(rig.text("heading") == "Edit note");
}

// Mutation: show Open as whatever the offer (the menu stays after custom screens are no longer offered).
TEST_CASE("custom screen chrome: Open as and the navigator's entries go when custom screens are not offered",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    static_cast<void>(rig.saveOnServer("Alpha", "notes.edit", std::string{kEdit}));
    static_cast<void>(rig.saveOnServer("Homely", "notes.home", std::string{kHome}));
    rig.open("notes.edit", note(1));
    REQUIRE(rig.pump([&rig] { return rig.find("Menu", "open_as_notes.edit").has_value(); }));
    CHECK(rig.chrome->navigatorEntries().size() == 1);
    rig.chrome->setOffered(false);
    rig.harness.settle();
    CHECK_FALSE(rig.find("Menu", "open_as_notes.edit").has_value());
    CHECK(rig.chrome->navigatorEntries().empty());
}

// Mutations: leave broken screens out of the entries (`Old` is not listed); mount a broken screen instead of opening
// its source (no source view shows, and a tab is opened).
TEST_CASE(
    "custom screen chrome: navigator entries are the screens of a base without params, a broken one opens its "
    "source",
    "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    std::string const homely = rig.saveOnServer("Homely", "notes.home", std::string{kHome});
    static_cast<void>(rig.saveOnServer("Alpha", "notes.edit", std::string{kEdit}));
    std::string const old = custom_client::customId('1');
    custom_client::putDirectly(rig, morph::ui::CustomScreen{.id = old,
                                                            .title = "Old",
                                                            .basedOn = "notes.home",
                                                            .document = R"({"vocab":"ui/1","id":")" + old +
                                                                        R"(","view":{"kind":"hologram"}})",
                                                            .savedAt = {},
                                                            .savedBy = "ann"});
    rig.sync->relist();
    REQUIRE(rig.pump([&rig] { return rig.sync->listed().screens.size() == 3; }));

    std::vector<morph::ui::NavigatorCustomEntry> const entries = rig.chrome->navigatorEntries();
    REQUIRE(entries.size() == 2);
    auto const entryOf = [&entries](std::string const& id) {
        auto const found = std::ranges::find(entries, id, &morph::ui::NavigatorCustomEntry::id);
        REQUIRE(found != entries.end());
        return *found;
    };
    CHECK(entryOf(homely).title == "Homely");
    CHECK(entryOf(homely).mine);
    CHECK(entryOf(homely).broken.empty());
    CHECK_FALSE(entryOf(old).broken.empty());
    CHECK(rig.chrome->isBroken(old));
    CHECK_FALSE(rig.chrome->isBroken(homely));

    rig.chrome->openCustom(old, {});
    REQUIRE(rig.pump([&rig] { return rig.find("Dialog", "source_dialog").has_value(); }));
    CHECK(rig.shown("Button", "source_refusal"));
    CHECK(rig.find("Button", "source_save").has_value());
    CHECK(rig.workspace->tabs().empty());

    // A screen whose base takes no params opens from the navigator, and has no Open as.
    rig.chrome->openCustom(homely, {});
    REQUIRE(rig.pump([&rig, &homely] { return !rig.workspace->tabs().empty() && rig.screenOf(0) == homely; }));
    CHECK_FALSE(rig.find("Menu", "open_as_" + homely).has_value());
}

// Mutation: report the kept screen as an error (the notice starts with "Cannot open").
TEST_CASE("custom screen chrome: Open as on a dirty screen that is kept open says it was cancelled",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    std::string const dirty = rig.saveOnServer(
        "Alpha", "notes.edit",
        screens_test::replaced(std::string{kEdit}, R"("params")", R"("dirty":{"lit":true},"params")"));
    rig.sync->documents().forget(dirty);
    std::optional<bool> fetched;
    rig.sync->open(dirty, [&fetched](auto loaded) { fetched = loaded.has_value(); });
    REQUIRE(rig.pump([&fetched] { return fetched.has_value(); }));
    REQUIRE(*fetched);
    rig.open(dirty, note(5));
    REQUIRE(rig.pump([&rig, &dirty] { return rig.find("Menu", "open_as_" + dirty).has_value(); }));
    rig.harness.backend.chooseMenuEntry(*rig.find("Menu", "open_as_" + dirty), {0});
    REQUIRE(rig.pump([&rig] { return !rig.notices.empty(); }));
    CHECK(rig.notices.back().starts_with("Open as cancelled: "));
    CHECK(rig.notices.back().ends_with(" stayed open"));
    CHECK(rig.workspace->tabs().size() == 1);
    CHECK(rig.screenOf(0) == dirty);
}

// Mutation: mount a broken screen chosen from Open as (the tab shows it, and no source view opens).
TEST_CASE("custom screen chrome: Open as onto a broken screen opens its source and leaves the tab as it is",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    static_cast<void>(rig.saveOnServer("Alpha", "notes.edit", std::string{kEdit}));
    std::string const old = custom_client::customId('3');
    custom_client::putDirectly(rig, morph::ui::CustomScreen{.id = old,
                                                            .title = "Old",
                                                            .basedOn = "notes.edit",
                                                            .document = R"({"vocab":"ui/1","id":")" + old +
                                                                        R"(","view":{"kind":"hologram"}})",
                                                            .savedAt = {},
                                                            .savedBy = "ann"});
    rig.sync->relist();
    REQUIRE(rig.pump([&rig] { return rig.sync->listed().screens.size() == 2; }));
    rig.open("notes.edit", note(7));
    REQUIRE(rig.pump([&rig] { return rig.find("Menu", "open_as_notes.edit").has_value(); }));
    std::size_t place = 1;
    for (morph::ui::CustomScreenSummary const& screen : rig.sync->listed().screens) {
        if (screen.id == old) {
            break;
        }
        ++place;
    }
    rig.harness.backend.chooseMenuEntry(*rig.find("Menu", "open_as_notes.edit"), {place});
    REQUIRE(rig.pump([&rig] { return rig.find("Dialog", "source_dialog").has_value(); }));
    CHECK(rig.shown("Button", "source_refusal"));
    CHECK(rig.workspace->tabs().size() == 1);
    CHECK(rig.screenOf(0) == "notes.edit");
}

// Mutation: treat every stored screen as the person's own (Check and save shows on another person's screen).
TEST_CASE("custom screen chrome: the stored source of another person's screen is read only, with no Check and save",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    std::string const theirs = custom_client::customId('4');
    custom_client::putDirectly(rig, morph::ui::CustomScreen{.id = theirs,
                                                            .title = "Theirs",
                                                            .basedOn = "notes.home",
                                                            .document = R"({"vocab":"ui/1","id":")" + theirs +
                                                                        R"(","view":{"kind":"hologram"}})",
                                                            .savedAt = {},
                                                            .savedBy = "bob"});
    rig.sync->relist();
    REQUIRE(rig.pump([&rig] { return rig.sync->listed().screens.size() == 1; }));
    CHECK_FALSE(rig.sync->listed().screens.front().mine);
    rig.chrome->showStoredSource(theirs);
    REQUIRE(rig.pump([&rig] { return rig.find("Dialog", "source_dialog").has_value(); }));
    CHECK_FALSE(rig.find("Button", "source_save").has_value());
    CHECK(rig.harness.backend.prop(*rig.find("TextInput", "source_text"), "readonly") == "true");
}

// Mutation: close the source view after a passing check without saving (the screen stays broken).
TEST_CASE("custom screen chrome: the source of a broken screen of mine is fixed and saved from the source view",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    std::string const old = custom_client::customId('2');
    custom_client::putDirectly(rig, morph::ui::CustomScreen{.id = old,
                                                            .title = "Old",
                                                            .basedOn = "notes.home",
                                                            .document = R"({"vocab":"ui/1","id":")" + old +
                                                                        R"(","view":{"kind":"hologram"}})",
                                                            .savedAt = {},
                                                            .savedBy = "ann"});
    rig.sync->relist();
    REQUIRE(rig.pump([&rig] { return rig.sync->listed().screens.size() == 1; }));
    rig.chrome->showStoredSource(old);
    REQUIRE(rig.pump([&rig] { return rig.find("Dialog", "source_dialog").has_value(); }));
    rig.type("source_text",
             morph::ui::prettyJson(screens_test::replaced(std::string{kHome}, "\"notes.home\"", "\"" + old + "\"")));
    rig.click("source_save");
    REQUIRE(rig.pump([&rig, &old] {
        auto const& screens = rig.sync->listed().screens;
        return screens.size() == 1 && screens.front().id == old && !screens.front().broken.has_value();
    }));
    REQUIRE(rig.pump([&rig] { return !rig.find("Dialog", "source_dialog").has_value(); }));
}

// Mutation: open a custom screen without fetching it first (the mount finds no document, and openCustom answers why).
TEST_CASE(
    "custom screen chrome: a custom screen opens once fetched, is recorded by its route, and reads back once fetched "
    "again",
    "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    std::string const alpha = rig.saveOnServer("Alpha", "notes.edit", std::string{kEdit});
    REQUIRE(rig.sync->documents().find(alpha) == nullptr);
    std::optional<std::expected<void, std::string>> opened;
    rig.chrome->openCustom(alpha, note(3),
                           [&opened](std::expected<void, std::string> answer) { opened = std::move(answer); });
    REQUIRE(rig.pump([&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    REQUIRE(rig.workspace->tabs().size() == 1);
    std::string const route = alpha + "?noteId=3";
    CHECK(rig.workspace->record() == std::vector<std::string>{route});

    rig.sync->documents().forget(alpha);
    CHECK_FALSE(rig.host->parseRoute(route).has_value());
    std::optional<std::vector<std::string>> missing;
    rig.sync->prefetchRoutes({route}, [&missing](std::vector<std::string> ids) { missing = std::move(ids); });
    REQUIRE(rig.pump([&missing] { return missing.has_value(); }));
    CHECK(missing->empty());
    CHECK(rig.host->parseRoute(route).has_value());
}

namespace {

std::string editedBy(std::string const& heading) {
    return screens_test::replaced(std::string{kEdit}, "\"Edit note\"", "\"" + heading + "\"");
}

}  // namespace

// Mutation: skip mountAsDraft in proposeDraft (no draft, no bar, and the heading stays "Edit note").
TEST_CASE("custom screen chrome: a proposed draft mounts as the tab's draft, with the draft bar and its text",
          "[ui][custom-screens][chrome][assistant]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    CHECK_FALSE(rig.chrome->draftOf(rig.tabAt(0)).has_value());
    REQUIRE(rig.chrome->proposeDraft(rig.tabAt(0), "notes.edit", editedBy("By the assistant")));
    rig.harness.settle();
    CHECK(rig.text("heading") == "By the assistant");
    CHECK(rig.shown("Row", "draft_bar_notes.edit"));
    CHECK(DocumentScreenHost::drafted(*rig.workspace->handleOf(0)));
    CHECK(rig.chrome->draftOf(rig.tabAt(0)) == std::optional<std::string>{editedBy("By the assistant")});

    // A second proposal replaces the first, and Discard returns the screen's own document.
    REQUIRE(rig.chrome->proposeDraft(rig.tabAt(0), "notes.edit", editedBy("Second")));
    rig.harness.settle();
    CHECK(rig.text("heading") == "Second");
    rig.click("draft_discard_notes.edit");
    REQUIRE(rig.pump([&rig] { return rig.text("heading") == "Edit note"; }));
    CHECK_FALSE(rig.chrome->draftOf(rig.tabAt(0)).has_value());
}

// Mutation: drop the screen-id comparison (a draft of another screen is handed to the host, which refuses it and the
// person is told), or drop the offered check (a client without custom screens mounts it).
TEST_CASE("custom screen chrome: a proposal for another screen, a closed tab or an unoffered client mounts nothing",
          "[ui][custom-screens][chrome][assistant]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    morph::ui::WorkspaceTabId const tab = rig.tabAt(0);
    CHECK_FALSE(rig.chrome->proposeDraft(tab, "notes.home", editedBy("Elsewhere")));
    CHECK_FALSE(rig.chrome->proposeDraft(morph::ui::WorkspaceTabId{999}, "notes.edit", editedBy("Nobody's")));
    CHECK_FALSE(DocumentScreenHost::drafted(*rig.workspace->handleOf(0)));
    CHECK(rig.text("heading") == "Edit note");

    rig.chrome->setOffered(false);
    CHECK_FALSE(rig.chrome->proposeDraft(tab, "notes.edit", editedBy("Unoffered")));
    CHECK_FALSE(DocumentScreenHost::drafted(*rig.workspace->handleOf(0)));
    rig.chrome->setOffered(true);

    bool closed = false;
    rig.workspace->close(0, [&closed](bool done) { closed = done; });
    REQUIRE(rig.pump([&closed] { return closed; }));
    CHECK_FALSE(rig.chrome->proposeDraft(tab, "notes.edit", editedBy("Closed")));
    CHECK(rig.notices.empty());
}

// Mutation: mount without loading (a document the client cannot load reaches the host), or skip the notify.
TEST_CASE("custom screen chrome: a proposal the client cannot load leaves the tab as it was and tells the person",
          "[ui][custom-screens][chrome][assistant]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    CHECK_FALSE(rig.chrome->proposeDraft(rig.tabAt(0), "notes.edit",
                                         screens_test::replaced(std::string{kEdit}, "\"column\"", "\"hologram\"")));
    CHECK_FALSE(DocumentScreenHost::drafted(*rig.workspace->handleOf(0)));
    CHECK(rig.text("heading") == "Edit note");
    REQUIRE(rig.notices.size() == 1);
    CHECK(rig.notices.front().starts_with("The assistant's draft could not be shown"));
}

// Mutation: drop the source-view close in proposeDraft (the view of the replaced text stays open).
TEST_CASE("custom screen chrome: a proposal closes the tab's source view, whose text it replaced",
          "[ui][custom-screens][chrome][assistant]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("source_button_notes.edit");
    REQUIRE(rig.find("Dialog", "source_dialog").has_value());
    REQUIRE(rig.chrome->proposeDraft(rig.tabAt(0), "notes.edit", editedBy("Proposed")));
    REQUIRE(rig.pump([&rig] { return !rig.find("Dialog", "source_dialog").has_value(); }));
    CHECK(rig.text("heading") == "Proposed");
}

// Mutation: skip the generation bump in proposeDraft (the Preview reply in flight mounts over the proposal).
TEST_CASE("custom screen chrome: a proposal made while a Preview is checking is not mounted over by its reply",
          "[ui][custom-screens][chrome][assistant]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("source_button_notes.edit");
    rig.type("source_text", screens_test::replaced(rig.field("source_text"), "\"Edit note\"", "\"By hand\""));
    rig.click("source_preview");
    REQUIRE(rig.chrome->proposeDraft(rig.tabAt(0), "notes.edit", editedBy("Proposed")));
    // A round trip after the proposal: the Preview's check was sent before it and answers first.
    std::optional<bool> barrier;
    rig.sync->check("notes.edit", std::string{kEdit},
                    [&barrier](auto const& answered) { barrier = answered.has_value(); });
    REQUIRE(rig.pump([&barrier] { return barrier.has_value(); }));
    CHECK(rig.pump([&rig] { return rig.text("heading") == "By hand"; }, std::chrono::milliseconds{200}) == false);
    CHECK(rig.text("heading") == "Proposed");
}

// Mutation: close the source view whatever it holds (the person's unpreviewed edits are thrown away).
TEST_CASE("custom screen chrome: a proposal keeps a source view with unpreviewed edits and says so",
          "[ui][custom-screens][chrome][assistant]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("source_button_notes.edit");
    std::string const edited = screens_test::replaced(rig.field("source_text"), "\"Edit note\"", "\"By hand\"");
    rig.type("source_text", edited);
    REQUIRE(rig.chrome->proposeDraft(rig.tabAt(0), "notes.edit", editedBy("Proposed")));
    std::optional<bool> barrier;
    rig.sync->check("notes.edit", std::string{kEdit},
                    [&barrier](auto const& answered) { barrier = answered.has_value(); });
    REQUIRE(rig.pump([&barrier] { return barrier.has_value(); }));
    CHECK(rig.text("heading") == "Proposed");
    CHECK(rig.find("Dialog", "source_dialog").has_value());
    CHECK(rig.field("source_text") == edited);
    REQUIRE_FALSE(rig.notices.empty());
    CHECK(rig.notices.back().contains("source view"));
}
