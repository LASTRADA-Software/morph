// SPDX-License-Identifier: Apache-2.0

// The commands corpus of spec 5 §8 and §14, with the lifecycle keys of spec 6 §3: navigate as a dialog answering
// through onResult or onCancel, focus, a missing host command, switchBackend for a listed and an unlisted backend,
// setLocale, cancelClose, transfers, the mount refusals, and every node kind lowered once.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;
using ui::testing::readJson;

namespace {

constexpr char const* kCommands = R"({
  "vocab": "ui/1", "id": "cmds", "title": { "t": "cmds.title", "fallback": "Commands" },
  "identity": { "ref": "who" }, "dirty": { "ref": "changed" },
  "params": { "who": "int" },
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "picked": { "init": null }, "changed": { "init": false }, "url": { "init": null },
             "ran": { "init": 0 }, "flag": { "type": "bool?", "init": null } },
  "queries": { "results": { "model": "sample", "action": "Lims_ListResults" } },
  "mutations": { "attach": { "model": "sample", "action": "Lims_Attach", "body": { "obj": { "kind": "csv" } } } },
  "onMount": [ { "set": "ran", "value": 1 } ],
  "onClose": [ { "if": [ { "ref": "changed" }, [ { "cancelClose": true } ] ] } ],
  "onSave": [ { "set": "changed", "value": false } ],
  "onBackendChange": [ { "set": "ran", "value": 2 } ],
  "keys": { "F5": [ { "refetch": "results" } ] },
  "view": { "kind": "column", "testId": "root", "children": [
    { "kind": "button", "testId": "pick", "label": "pick", "onClick": { "navigate": "lims.pick", "as": "dialog",
        "params": { "obj": { "who": { "ref": "who" } } },
        "onResult": [ { "set": "picked", "value": { "ref": "event" } }, { "set": "changed", "value": true } ],
        "onCancel": [ { "set": "picked", "value": "cancelled" } ] } },
    { "kind": "button", "testId": "focus", "label": "focus", "onClick": { "focus": "note" } },
    { "kind": "textInput", "id": "note", "testId": "note" },
    { "kind": "button", "testId": "host", "label": "host", "onClick": { "seq": [
        { "host": "openPath", "args": { "obj": { "path": "/tmp" } } }, { "set": "ran", "value": 5 } ] } },
    { "kind": "button", "testId": "missing", "label": "missing", "onClick": { "seq": [
        { "host": "nope" }, { "set": "ran", "value": 6 } ] } },
    { "kind": "button", "testId": "switch", "label": "switch", "onClick": { "switchBackend": "staging" } },
    { "kind": "button", "testId": "badswitch", "label": "bad", "onClick": { "switchBackend": "nowhere" } },
    { "kind": "button", "testId": "locale", "label": "de", "onClick": { "setLocale": "de" } },
    { "kind": "text", "testId": "hello", "text": { "t": "hello", "fallback": "Hello" } },
    { "kind": "button", "testId": "notify", "label": "n", "onClick": { "notify": { "fn": "concat", "args": [ "hi ", { "ref": "note.value" } ] }, "role": "success" } },
    { "kind": "button", "testId": "nullif", "label": "if", "onClick": { "if": [ { "ref": "flag" }, [ { "set": "ran", "value": 7 } ], [ { "set": "ran", "value": 8 } ] ] } },
    { "kind": "button", "testId": "return", "label": "ret", "onClick": [ { "return": { "ref": "who" } } ] },
    { "kind": "button", "testId": "self", "label": "self", "onClick": [ { "close": "self" } ] },
    { "kind": "button", "testId": "app", "label": "app", "onClick": [ { "set": "app.banner", "value": "hey" } ] },
    { "kind": "button", "testId": "upload", "label": "up", "onClick": [ { "upload": "/tmp/a.csv", "to": "attach", "as": "ref", "id": "up" } ] },
    { "kind": "text", "testId": "progress", "text": { "fn": "format", "args": [ "{0}/{1}", { "ref": "up.progress" }, { "ref": "up.pending" } ] } },
    { "kind": "button", "testId": "download", "label": "down", "onClick": [ { "download": "ref-1", "to": "preview", "as": "url" } ] },
    { "kind": "button", "testId": "save", "label": "save", "onClick": [ { "download": "ref-1", "save": "dialog", "name": "a.csv" } ] },
    { "kind": "button", "testId": "signout", "label": "out", "onClick": [ { "signOut": true } ] },
    { "kind": "button", "testId": "reset", "label": "reset", "onClick": [ { "reset": "ran" } ] } ] } })";

ui::ValueRecord who(std::int64_t value) { return ui::ValueRecord{{"who", ui::Value{value}}}; }

}  // namespace

TEST_CASE("interpreter commands: lifecycle keys run and read", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kCommands, who(3));
    CHECK(readJson(*screen, R"({"ref": "ran"})") == "1");
    CHECK(screen->title() == ui::Value{"Commands"});
    CHECK(screen->identity() == ui::Value{std::int64_t{3}});
    CHECK_FALSE(screen->dirty());
    CHECK(screen->requestClose());
    screen->runLifecycle("onBackendChange");
    CHECK(readJson(*screen, R"({"ref": "ran"})") == "2");
    CHECK(screen->id() == "cmds");
    CHECK(readJson(*screen, R"({"nonsense": 1})") == "null");
}

TEST_CASE("interpreter commands: navigate as a dialog returns through onResult or onCancel", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kCommands, who(3));
    int const pick = h.byTestId("Button", "pick");
    h.backend.click(pick);
    h.settle();
    CHECK(h.host.log.back() == R"(navigate lims.pick as dialog {"who":3})");
    h.host.onResult(ui::Value{"S-1"});
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "picked"})") == R"("S-1")");
    CHECK(screen->dirty());
    // onClose keeps a dirty screen open; onSave clears it.
    CHECK_FALSE(screen->requestClose());
    screen->runLifecycle("onSave");
    CHECK(screen->requestClose());
    h.backend.click(pick);
    h.settle();
    h.host.onCancel();
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "picked"})") == R"("cancelled")");
}

TEST_CASE("interpreter commands: focus moves to the element itself; host commands, return and close go to the host",
          "[ui][interpreter]") {
    Harness h;
    h.host.commands = {"openPath"};
    auto screen = h.mount(kCommands, who(3));
    h.backend.click(h.byTestId("Button", "focus"));
    h.settle();
    // The interpreter carries `focus` out in the mounted tree; the host is not asked.
    CHECK(h.backend.focused() == h.byTestId("TextInput", "note"));
    h.backend.click(h.byTestId("Button", "host"));
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "ran"})") == "5");
    h.backend.click(h.byTestId("Button", "missing"));
    h.settle();
    // A missing host command fails its step: the sequence stops there.
    CHECK(readJson(*screen, R"({"ref": "ran"})") == "5");
    REQUIRE_FALSE(h.diagnostics.empty());
    CHECK(h.diagnostics.back().reason == "host command `nope` is missing");
    h.backend.click(h.byTestId("Button", "return"));
    h.backend.click(h.byTestId("Button", "self"));
    h.backend.click(h.byTestId("Button", "app"));
    h.settle();
    CHECK(h.host.log == std::vector<std::string>{R"(host openPath {"path":"/tmp"})", "host nope null", "close cmds 3",
                                                 "close cmds", R"(app banner "hey")"});
}

TEST_CASE("interpreter commands: switchBackend for a listed and an unlisted backend", "[ui][interpreter]") {
    Harness h;
    h.host.backends = {"staging"};
    auto screen = h.mount(kCommands, who(3));
    h.reply(0, "[]");
    h.backend.click(h.byTestId("Button", "badswitch"));
    h.settle();
    CHECK(h.dispatcher.calls.size() == 1);
    CHECK(h.diagnostics.back().reason == "backend `nowhere` is not listed");
    h.backend.click(h.byTestId("Button", "switch"));
    h.settle();
    // A backend switch marks every entry stale and refetches the mounted queries.
    CHECK(h.dispatcher.calls.size() == 2);
}

TEST_CASE("interpreter commands: setLocale re-resolves every t", "[ui][interpreter]") {
    Harness h;
    ui::FormatContext format;
    format.translations = [](std::string_view key, std::string_view locale) -> std::optional<std::string> {
        if (key == "hello" && locale == "de") {
            return "Hallo";
        }
        return std::nullopt;
    };
    h.session.setFormatting(format);
    auto screen = h.mount(kCommands, who(3));
    CHECK(h.text("hello") == "Hello");
    h.backend.click(h.byTestId("Button", "locale"));
    h.settle();
    CHECK(h.text("hello") == "Hallo");
    CHECK(h.host.log.back() == "setLocale de");
}

TEST_CASE("interpreter commands: notify, a null `if`, reset, signOut and a document chord", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kCommands, who(3));
    h.backend.edit(h.byTestId("TextInput", "note"), "ann");
    h.settle();
    h.backend.click(h.byTestId("Button", "notify"));
    h.settle();
    CHECK(h.host.log.back() == "notify success hi ann");
    h.backend.click(h.byTestId("Button", "nullif"));
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "ran"})") == "1");
    h.backend.press(h.byTestId("TextInput", "note"), "F5");
    h.settle();
    CHECK(h.dispatcher.count("Lims_ListResults") == 2);
    h.reply(1, "[]");
    CHECK(h.session.cacheSize() == 1);
    h.backend.click(h.byTestId("Button", "signout"));
    h.settle();
    CHECK(h.session.cacheSize() == 0);
    CHECK(h.host.log.back() == "signOut");
    h.backend.click(h.byTestId("Button", "reset"));
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "ran"})") == "0");
}

TEST_CASE("interpreter commands: an upload reports progress, then runs its mutation with the reference",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kCommands, who(3));
    h.backend.click(h.byTestId("Button", "upload"));
    h.settle();
    // The host learns the mutation the upload is for, which the server authorises it as.
    CHECK(std::ranges::count(h.host.log, std::string{"uploadFor Lims_SampleModel Lims_Attach"}) == 1);
    CHECK(h.text("progress") == "0/true");
    h.host.uploadProgress(ui::Value{std::int64_t{1}});
    h.settle();
    CHECK(h.text("progress") == "1/true");
    h.host.uploadDone(ui::Value{"file-9"});
    h.settle();
    CHECK(h.text("progress") == "1/false");
    REQUIRE(h.dispatcher.count("Lims_Attach") == 1);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_Attach")).body == R"({"kind":"csv","ref":"file-9"})");

    h.backend.click(h.byTestId("Button", "upload"));
    h.settle();
    h.host.uploadDone(std::unexpected(std::string{"too large"}));
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "up.error"})") == R"("too large")");
}

TEST_CASE("interpreter commands: a download to preview binds its local reference", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kCommands, who(3));
    h.backend.click(h.byTestId("Button", "download"));
    h.settle();
    CHECK(h.host.log.back() == R"(download "ref-1" preview null)");
    h.host.downloadDone(ui::Value{"/tmp/local"});
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "url"})") == R"("/tmp/local")");
    h.backend.click(h.byTestId("Button", "save"));
    h.settle();
    CHECK(h.host.log.back() == R"(download "ref-1" dialog "a.csv")");
}

TEST_CASE("interpreter commands: without a host every host command is reported", "[ui][interpreter]") {
    Harness h;
    std::vector<ui::Refusal> reported;
    ui::Interpreter bare{h.session, h.dispatcher,
                         ui::InterpreterOptions{.host = nullptr, .diagnostics = [&](ui::Refusal const& refusal) {
                                                    reported.push_back(refusal);
                                                }}};
    auto mounted = bare.mount(ui::testing::loadOrFail(kCommands, ui::testing::corpusCatalog()), who(3), h.backend);
    REQUIRE(mounted);
    h.settle();
    // `focus` is not among them: the interpreter carries it out in the mounted tree, host or not.
    for (char const* const button :
         {"pick", "host", "switch", "notify", "return", "self", "app", "upload", "download", "locale", "signout"}) {
        h.backend.click(h.byTestId("Button", button));
        h.settle();
    }
    CHECK(reported.size() >= 9);
    CHECK((*mounted)->diagnostics().size() == reported.size());
}

TEST_CASE("interpreter commands: mounting refuses what only the opener can get wrong", "[ui][interpreter]") {
    Harness h;
    auto const doc = ui::testing::loadOrFail(kCommands, ui::testing::corpusCatalog());
    auto missing = h.interpreter.mount(doc, {}, h.backend);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().path == "/params/who");
    auto wrong = h.interpreter.mount(doc, ui::ValueRecord{{"who", ui::Value{"x"}}}, h.backend);
    REQUIRE_FALSE(wrong);
    CHECK(wrong.error().reason == "param `who` is not a int");
    auto app = ui::testing::loadOrFail(R"({"vocab": "ui/1", "id": "app"})");
    auto noView = h.interpreter.mount(app, {}, h.backend);
    REQUIRE_FALSE(noView);
    CHECK(noView.error().reason == "the document has no view");
}

TEST_CASE("interpreter commands: every node kind lowers to the view tree", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "palette",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "state": { "on": { "init": "b" }, "tick": { "init": 0 }, "date": { "init": null } },
      "components": { "Chart": { "qml": "sha256:4b1e0000000000000000000000000000000000000000000000000000000000aa",
                                 "props": { "title": "string" },
                                 "fallback": { "kind": "text", "testId": "chart", "text": { "ref": "props.title" } } } },
      "view": { "kind": "scroll", "axis": "horizontal", "children": [
        { "kind": "grid", "columns": 2, "gap": 1, "children": [
          { "kind": "spacer", "span": 2, "sizing": { "lit": { "fixed": 3 } } },
          { "kind": "busy", "active": true, "label": "wait", "sizing": { "lit": { "stretch": 2 } } } ] },
        { "kind": "panel", "title": "P", "padding": 1, "collapsible": true, "collapsed": false, "children": [
          { "kind": "row", "gap": 2, "children": [
            { "kind": "text", "testId": "role", "text": "e", "role": "error" },
            { "kind": "checkbox", "id": "agree", "testId": "agree", "label": "agree",
              "onToggle": [ { "set": "tick", "value": 1 } ] },
            { "kind": "text", "testId": "agreed", "text": { "fn": "format", "args": [ "{0}", { "ref": "agree.checked" } ] } } ] } ] },
        { "kind": "switch", "on": { "ref": "on" }, "cases": [
            { "when": "a", "children": [ { "kind": "text", "testId": "case", "text": "A" } ] },
            { "when": "b", "scope": { "state": { "inner": { "init": "B" } } },
              "children": [ { "kind": "text", "testId": "case", "text": { "ref": "inner" } } ] } ],
          "default": [ { "kind": "text", "testId": "case", "text": "other" } ] },
        { "kind": "tabs", "tabs": [ { "label": "one", "children": [ { "kind": "text", "text": "t1" } ] },
                                    { "label": "two", "children": [ { "kind": "text", "text": "t2" } ] } ] },
        { "kind": "menu", "items": [ { "label": "File", "items": [
            { "label": "Tick", "keys": "Ctrl+T", "icon": "clock", "checked": true, "onClick": [ { "set": "tick", "value": 2 } ] } ] } ] },
        { "kind": "banner", "tone": "warn", "text": "b", "dismissible": true,
          "action": { "label": "fix", "onClick": [ { "set": "tick", "value": 3 } ] }, "onDismiss": [ { "set": "tick", "value": 4 } ] },
        { "kind": "badge", "tone": "ok", "text": "ok", "icon": "check" },
        { "kind": "progress", "value": { "lit": { "num": 1, "den": 2, "dp": 1 } }, "label": "half" },
        { "kind": "steps", "items": { "lit": [ { "label": "a", "state": "done" }, { "label": "b", "state": "active" } ] }, "current": 1 },
        { "kind": "keyValue", "items": { "lit": [ { "label": "k", "value": 1 } ] } },
        { "kind": "emptyState", "title": "none", "text": "nothing", "icon": "box" },
        { "kind": "splitter", "orientation": "vertical", "children": [ { "kind": "spacer" }, { "kind": "spacer" } ] },
        { "kind": "collapsible", "title": "c", "open": true, "header": [ { "kind": "spacer" } ],
          "onToggle": [ { "set": "tick", "value": 5 } ], "children": [ { "kind": "spacer" } ] },
        { "kind": "dropZone", "accept": { "lit": [ ".csv" ] }, "multiple": true, "onDrop": [ { "set": "tick", "value": 6 } ],
          "children": [ { "kind": "spacer" } ] },
        { "kind": "filePicker", "id": "file", "testId": "file", "mode": "save", "onPick": [ { "set": "tick", "value": 7 } ] },
        { "kind": "dateTimeInput", "id": "when", "testId": "when", "mode": "date", "onChange": [ { "set": "date", "value": { "ref": "event" } } ] },
        { "kind": "slider", "testId": "slider", "min": 1, "max": 5, "step": 1 },
        { "kind": "text", "testId": "date", "text": { "ref": "date" } },
        { "kind": "drawer", "id": "drawer", "side": "start", "children": [ { "kind": "spacer" } ] },
        { "kind": "custom", "name": "Chart", "props": { "title": "Trend" } },
        { "kind": "form", "model": "sample", "action": "Lims_CaptureConcentration" },
        { "kind": "table", "model": "sample", "action": "Lims_ListResults" },
        { "kind": "table", "id": "derived", "rows": { "lit": [ { "id": 1, "name": "x" } ] },
          "columns": { "lit": [ { "id": "name", "label": "Name" } ] }, "selectionMode": "multiple" },
        { "kind": "text", "testId": "selection", "text": { "fn": "format", "args": [ "{0}", { "ref": "derived.selectedCount" } ] } },
        { "kind": "text", "testId": "dragged", "text": "drag", "drag": { "payload": 5 } },
        { "kind": "text", "testId": "target", "text": "drop",
          "drop": { "accepts": { "lit": [ 5 ] }, "onDrop": [ { "set": "tick", "value": { "ref": "event" } } ] } } ] } })");
    CHECK(h.text("chart") == "Trend");
    CHECK(h.text("case") == "B");
    CHECK(h.text("agreed") == "false");
    h.backend.toggle(h.byTestId("Checkbox", "agree"));
    h.settle();
    CHECK(h.text("agreed") == "true");
    CHECK(readJson(*screen, R"({"ref": "tick"})") == "1");
    h.backend.pick(h.byTestId("FilePicker", "file"), "/tmp/out.csv");
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "file.value"})") == R"("/tmp/out.csv")");
    h.backend.slide(h.byTestId("Slider", "slider"), 4);
    h.backend.setDateTime(h.byTestId("DateTimeInput", "when"), morph::time::Timestamp{morph::time::DateTime{}});
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "date"})") != "null");
    CHECK(h.text("date").contains('T'));
    h.backend.selectRows(h.backend.all("Table").front(), {ui::Key{std::int64_t{1}}});
    h.settle();
    CHECK(h.text("selection") == "1");
    int const banner = h.backend.all("Banner").front();
    h.backend.click(banner);
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "tick"})") == "3");
    h.backend.dismiss(banner);
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "tick"})") == "4");
    h.backend.chooseMenuEntry(h.backend.all("Menu").front(), {0, 0});
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "tick"})") == "2");
    h.backend.expand(h.backend.all("Collapsible").front(), false);
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "tick"})") == "5");
    CHECK(h.backend.dropFiles(h.backend.all("DropZone").front(), {"/tmp/a.csv"}));
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "tick"})") == "6");
    int const dragged = h.byTestId("Text", "dragged");
    CHECK(h.backend.drag(dragged, h.byTestId("Text", "target")));
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "tick"})") == "5");
    // The form lowers through the forms engine; the table declared from a list action is reported, not lowered.
    CHECK(h.diagnostics.size() == 1);
}

TEST_CASE("interpreter commands: roles, tones and step states map by name", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "names",
      "state": { "flag": { "init": true } },
      "view": { "kind": "column", "children": [
        { "kind": "text", "testId": "success", "text": { "fn": "format", "args": [ "{0}", { "ref": "flag" } ] }, "role": "success" },
        { "kind": "text", "testId": "muted", "text": "m", "role": "muted" },
        { "kind": "text", "testId": "heading", "text": "h", "role": "heading" },
        { "kind": "text", "testId": "error", "text": "e", "role": "error" },
        { "kind": "text", "testId": "normal", "text": "n", "role": "plain" },
        { "kind": "badge", "testId": "info", "tone": "info" },
        { "kind": "badge", "testId": "success", "tone": "success" },
        { "kind": "badge", "testId": "warning", "tone": "warning" },
        { "kind": "badge", "testId": "err", "tone": "err" },
        { "kind": "badge", "testId": "error", "tone": "error" },
        { "kind": "badge", "testId": "neutral", "tone": "other" },
        { "kind": "steps", "items": { "lit": [ { "label": "x", "state": "failed" }, { "label": "y", "state": "pending" }, { "state": "done" } ] } } ] } })");
    CHECK(h.backend.prop(h.byTestId("Text", "success"), "role") == "Success");
    CHECK(h.text("success") == "true");
    CHECK(h.backend.prop(h.byTestId("Text", "muted"), "role") == "Muted");
    CHECK(h.backend.prop(h.byTestId("Text", "heading"), "role") == "Heading");
    CHECK(h.backend.prop(h.byTestId("Text", "error"), "role") == "Error");
    CHECK(h.backend.prop(h.byTestId("Text", "normal"), "role") == "Normal");
    CHECK(h.backend.prop(h.byTestId("Badge", "info"), "tone") == "Info");
    CHECK(h.backend.prop(h.byTestId("Badge", "success"), "tone") == "Ok");
    CHECK(h.backend.prop(h.byTestId("Badge", "warning"), "tone") == "Warn");
    CHECK(h.backend.prop(h.byTestId("Badge", "err"), "tone") == "Err");
    CHECK(h.backend.prop(h.byTestId("Badge", "error"), "tone") == "Err");
    CHECK(h.backend.prop(h.byTestId("Badge", "neutral"), "tone") == "Neutral");
}

TEST_CASE("interpreter commands: a table's element values, and onActivate in the row's scope", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "grid",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "state": { "picked": { "init": null } },
      "queries": { "rows": { "model": "sample", "action": "Lims_ListResults" } },
      "view": { "kind": "column", "children": [
        { "kind": "table", "id": "t", "rows": { "query": "rows" }, "selectionMode": "single",
          "scope": { "state": { "label": { "init": { "fn": "concat", "args": [ "row ", { "row": "name" } ] } } } },
          "columns": [ { "id": "name", "label": "Name", "width": { "lit": { "stretch": 1 } } } ],
          "onActivate": [ { "set": "picked", "value": { "ref": "label" } } ] },
        { "kind": "text", "testId": "values", "text": { "fn": "format", "args": [ "{0}/{1}/{2}/{3}",
            { "ref": "t.viewCount" }, { "ref": "t.sourceCount" }, { "ref": "t.pending" }, { "ref": "t.selectedCount" } ] } },
        { "kind": "text", "testId": "error", "text": { "fn": "coalesce", "args": [ { "ref": "t.error" }, { "ref": "t.sort" }, "-" ] } } ] } })");
    CHECK(h.text("values") == "0/0/true/0");
    h.reply(0, R"([{"id":1,"name":"a"},{"id":2,"name":"b"}])");
    CHECK(h.text("values") == "2/2/false/0");
    int const table = h.backend.all("Table").front();
    h.backend.activateRow(table, ui::Key{std::int64_t{2}});
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "picked"})") == R"("row b")");
    CHECK(readJson(*screen, R"({"ref": "t.activeKey"})") == "2");
    h.backend.selectRows(table, {ui::Key{std::int64_t{1}}});
    h.settle();
    CHECK(h.text("values") == "2/2/false/1");
    CHECK(readJson(*screen, R"({"ref": "t.selection"})") == "[1]");
    CHECK(h.text("error") == "-");
}

TEST_CASE("interpreter commands: transfers a host cannot make fail their command", "[ui][interpreter]") {
    Harness h;
    h.host.refuseTransfers = true;
    auto screen = h.mount(kCommands, who(3));
    h.backend.click(h.byTestId("Button", "upload"));
    h.backend.click(h.byTestId("Button", "download"));
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "up.pending"})") == "false");
    CHECK(readJson(*screen, R"({"ref": "up.progress"})") == "null");
    REQUIRE(h.diagnostics.size() == 2);
    CHECK(h.diagnostics.at(0).reason == "`upload` is not supported by the host");
    h.host.refuseTransfers = false;
    h.backend.click(h.byTestId("Button", "download"));
    h.settle();
    h.host.downloadDone(std::unexpected(std::string{"gone"}));
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "url"})") == "null");
    // A transfer whose screen is gone settles nothing.
    h.backend.click(h.byTestId("Button", "upload"));
    h.settle();
    auto progress = h.host.uploadProgress;
    auto done = h.host.uploadDone;
    screen.reset();
    progress(ui::Value{std::int64_t{1}});
    done(ui::Value{"late"});
    h.settle();
    CHECK(h.dispatcher.count("Lims_Attach") == 0);
}

TEST_CASE("interpreter commands: a host that refuses a request fails the command and reports it once",
          "[ui][interpreter]") {
    Harness h;
    h.host.refuseAll = true;
    auto screen = h.mount(kCommands, who(3));
    for (char const* const button : {"pick", "focus", "notify", "return", "self", "app", "pick"}) {
        h.backend.click(h.byTestId("Button", button));
        h.settle();
    }
    std::vector<std::string> reasons;
    reasons.reserve(h.diagnostics.size());
    for (ui::Refusal const& refusal : h.diagnostics) {
        reasons.push_back(refusal.reason);
    }
    CHECK(reasons == std::vector<std::string>{
                         "`navigate` is not supported by the host", "`notify` is not supported by the host",
                         "`return` is not supported by the host", "`close: self` is not supported by the host",
                         "`set` of `app` is not supported by the host"});
    CHECK(screen->diagnostics().size() == reasons.size());
}

TEST_CASE("interpreter commands: optional parts of nodes fall back to their defaults", "[ui][interpreter]") {
    Harness h;
    ui::Interpreter quiet{h.session, h.dispatcher};
    auto doc = ui::testing::loadOrFail(R"({
      "vocab": "ui/1", "id": "defaults",
      "state": { "flag": { "init": true }, "typed": { "type": "int", "init": 1 }, "key": { "init": "k-1" }, "dlg": { "init": false } },
      "models": { "byName": { "type": "Lims_SampleModel", "instance": { "ref": "key" } } },
      "queries": { "q": { "model": "byName", "action": "Lims_Info" } },
      "view": { "kind": "column", "children": [
        { "kind": "text", "testId": "flag", "text": { "ref": "flag" } },
        { "kind": "text", "testId": "app", "text": { "fn": "coalesce", "args": [ { "ref": "app.tenant" }, "-" ] } },
        { "kind": "select", "testId": "options", "options": { "lit": [ { "value": "v" }, { "id": 2, "label": "two" }, { "label": "none" } ] } },
        { "kind": "tabs", "tabs": [ { "children": [ { "kind": "spacer" } ] }, { "label": "empty" } ] },
        { "kind": "keyValue", "items": { "lit": [ { "label": "only" }, { "value": 1 } ] } },
        { "kind": "steps", "items": { "lit": [ { "label": "s" }, { } ] } },
        { "kind": "grid", "children": [ { "kind": "spacer" } ] },
        { "kind": "scroll", "children": [ { "kind": "spacer" } ] },
        { "kind": "splitter", "children": [ { "kind": "spacer" } ] },
        { "kind": "drawer", "id": "d", "open": { "ref": "dlg" }, "children": [ { "kind": "spacer" } ] },
        { "kind": "table", "rows": { "lit": [ { "name": "x" } ] }, "columns": { "lit": [ { "id": "name" }, { "label": "No id" } ] } },
        { "kind": "table", "rows": { "lit": [ { "id": 1 } ] }, "columns": [ { "label": "A" }, { "id": "id" } ] },
        { "kind": "text", "testId": "drop", "text": "drop",
          "drop": { "accepts": 5, "onDrop": [ { "set": "typed", "value": { "ref": "event" } } ] } },
        { "kind": "text", "testId": "dropAny", "text": "any", "drop": { "onDrop": [ { "set": "typed", "value": 9 } ] } },
        { "kind": "text", "testId": "five", "text": "5", "drag": { "payload": 5 } },
        { "kind": "text", "testId": "six", "text": "6", "drag": { "payload": "six" } },
        { "kind": "button", "testId": "wrong", "label": "w", "onClick": [ { "set": "typed", "value": { "fn": "first", "args": [ { "lit": [ "text" ] } ] } } ] },
        { "kind": "button", "testId": "closeDrawer", "label": "c", "onClick": [ { "close": "d" } ] },
        { "kind": "button", "testId": "same", "label": "s", "onClick": [ { "set": "typed", "value": { "fn": "first", "args": [ { "lit": [ "text" ] } ] } } ] } ] } })");
    auto screen = quiet.mount(doc, {}, h.backend);
    REQUIRE(screen);
    h.settle();
    CHECK(h.text("flag") == "true");
    CHECK(h.text("app") == "-");
    CHECK(h.backend.prop(h.byTestId("Select", "options"), "options") == R"(["v":v,2:two,"null":none])");
    REQUIRE(h.dispatcher.calls.size() == 1);
    CHECK(h.dispatcher.calls.at(0).instanceKey == "k-1");
    int const drop = h.byTestId("Text", "drop");
    CHECK_FALSE(h.backend.drag(h.byTestId("Text", "six"), drop));
    CHECK(h.backend.drag(h.byTestId("Text", "five"), drop));
    CHECK(h.backend.drag(h.byTestId("Text", "six"), h.byTestId("Text", "dropAny")));
    h.settle();
    CHECK(readJson(**screen, R"({"ref": "typed"})") == "9");
    h.backend.click(h.byTestId("Button", "wrong"));
    h.backend.click(h.byTestId("Button", "wrong"));
    h.backend.click(h.byTestId("Button", "closeDrawer"));
    h.settle();
    CHECK(readJson(**screen, R"({"ref": "typed"})") == "9");
    REQUIRE((*screen)->diagnostics().size() == 2);
    CHECK((*screen)->diagnostics().at(0).reason == "`set` writes a value that is not a int");
    CHECK((*screen)->diagnostics().at(1).reason == "the dialog's `open` is bound to an expression");
}

// Mutations: leave out the label fallback (every unnamed field reads empty); let the label overwrite an explicit
// `a11y.name` (the "Explicit" checks fail); skip the fallback when an `a11y` part carries only a role.
TEST_CASE("interpreter commands: a field's label is its accessible name unless `a11y.name` gives one",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "named",
      "state": { "unit": { "init": "Mass" } },
      "view": { "kind": "column", "children": [
        { "kind": "textInput", "testId": "text", "label": { "ref": "unit" } },
        { "kind": "select", "testId": "select", "label": "Unit", "options": { "lit": [ "g", "kg" ] } },
        { "kind": "slider", "testId": "slider", "label": "Dilution" },
        { "kind": "dateTimeInput", "testId": "when", "label": "Since" },
        { "kind": "filePicker", "testId": "file", "label": "Report" },
        { "kind": "textInput", "testId": "explicitText", "label": "Label", "a11y": { "name": "Explicit" } },
        { "kind": "select", "testId": "explicitSelect", "label": "Label", "a11y": { "name": "Explicit" } },
        { "kind": "textInput", "testId": "roleOnly", "label": "Note", "a11y": { "role": "textbox" } },
        { "kind": "button", "testId": "rename", "label": "rename",
          "onClick": [ { "set": "unit", "value": "Volume" } ] } ] } })");
    auto name = [&](std::string_view kind, std::string_view testId) {
        return h.backend.prop(h.byTestId(kind, testId), "a11yName");
    };
    CHECK(name("TextInput", "text") == "Mass");
    CHECK(name("Select", "select") == "Unit");
    CHECK(name("Slider", "slider") == "Dilution");
    CHECK(name("DateTimeInput", "when") == "Since");
    CHECK(name("FilePicker", "file") == "Report");
    CHECK(name("TextInput", "explicitText") == "Explicit");
    CHECK(name("Select", "explicitSelect") == "Explicit");
    CHECK(name("TextInput", "roleOnly") == "Note");
    h.backend.click(h.byTestId("Button", "rename"));
    h.settle();
    CHECK(name("TextInput", "text") == "Volume");
}

// Mutation: never call setRowTone (the rows show no tone).
TEST_CASE("table: rowTone tones each row in its own scope, follows the row, and reports a value that is no tone",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "toned",
      "state": { "items": { "init": { "lit": [ { "id": 1, "status": "ok" }, { "id": 2, "status": "err" },
                                              { "id": 3, "status": null }, { "id": 4, "status": "purple" },
                                              { "id": 5, "status": "purple" } ] } } },
      "view": { "kind": "column", "children": [
        { "kind": "table", "rows": { "ref": "items" }, "rowTone": { "row": "status" },
          "columns": [ { "id": "id", "label": "Id" } ] },
        { "kind": "button", "testId": "retone", "label": "retone",
          "onClick": [ { "set": "items", "value": { "lit": [ { "id": 1, "status": "warn" }, { "id": 2, "status": "err" },
                                                          { "id": 3, "status": "info" }, { "id": 4, "status": "purple" },
                                                          { "id": 5, "status": "purple" } ] } } ] } ] } })");
    auto const tones = [&h] {
        std::vector<std::string> out;
        for (int const row : h.backend.all("Row")) {
            out.push_back(h.backend.prop(row, "tone"));
        }
        return out;
    };
    CHECK(tones() == std::vector<std::string>{"Ok", "Err", "none", "none", "none"});
    auto const purple = std::ranges::count_if(h.diagnostics, [](morph::ui::Refusal const& refusal) {
        return refusal.reason.contains(R"(rowTone is "purple")");
    });
    CHECK(purple == 1);
    h.backend.click(h.byTestId("Button", "retone"));
    h.settle();
    CHECK(tones() == std::vector<std::string>{"Warn", "Err", "Info", "none", "none"});
}

namespace {

/// Whether @p widget or anything under it has the test id @p testId.
// NOLINTNEXTLINE(misc-no-recursion): one level per nesting of the recorded widgets.
bool holds(ui::testing::RecordingBackend const& backend, int widget, std::string const& testId) {
    if (backend.prop(widget, "testId") == testId) {
        return true;
    }
    auto const children = backend.children(widget);
    return std::ranges::any_of(children, [&](int child) {  // NOLINT(misc-no-recursion)
        return holds(backend, child, testId);
    });
}

}  // namespace

// Mutation: mount the action bar in place, in the content, instead of in the overlay's footer.
TEST_CASE("interpreter commands: a dialog's actions are buttons in its footer, run in the dialog's scope",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "overlay.actions", "state": { "saved": { "init": "no" } },
      "view": { "kind": "column", "children": [
        { "kind": "button", "testId": "open", "label": "open", "onClick": { "open": "dlg" } },
        { "kind": "text", "testId": "saved", "text": { "ref": "saved" } },
        { "kind": "dialog", "id": "dlg", "scope": { "state": { "draft": { "init": "inner" } } },
          "default": [ { "set": "saved", "value": "by enter" } ],
          "actions": [
            { "id": "save", "label": "Save", "emphasis": "primary", "onClick": [ { "set": "saved", "value": { "ref": "draft" } } ] },
            { "id": "later", "label": "Later", "enabled": false } ],
          "children": [ { "kind": "text", "testId": "body", "text": "Body" } ] } ] } })");
    CHECK(h.backend.all("Footer").empty());  // closed, its content and its actions are not mounted
    h.backend.click(h.byTestId("Button", "open"));
    h.settle();
    int const dialog = h.backend.all("Dialog").front();
    std::vector<int> const footers = h.backend.all("Footer");
    REQUIRE(footers.size() == 1);
    CHECK(holds(h.backend, footers.front(), "action_save"));
    CHECK(holds(h.backend, footers.front(), "action_later"));
    for (int const child : h.backend.children(dialog)) {
        if (child != footers.front()) {
            CHECK(holds(h.backend, child, "body"));
            CHECK_FALSE(holds(h.backend, child, "action_save"));
        }
    }
    CHECK(h.backend.prop(h.byTestId("Button", "action_later"), "enabled") == "false");
    CHECK(h.backend.prop(h.byTestId("Button", "action_save"), "emphasis") == "Primary");

    h.backend.click(h.byTestId("Button", "action_save"));
    h.settle();
    CHECK(h.text("saved") == "inner");
    REQUIRE(h.backend.find("Column", "keys", "Enter").has_value());
    CHECK(h.backend.press(*h.backend.find("Column", "keys", "Enter"), "Enter"));
    h.settle();
    CHECK(h.text("saved") == "by enter");
}

// Mutation: let an action bar mount wherever it is (it would show in the column, under no overlay).
TEST_CASE("mount: an action bar outside a dialog's or drawer's content is refused", "[ui][interpreter]") {
    Harness h;
    ui::Node const bar = ui::actionBar(ui::ActionBar{.children = {ui::button(ui::Button{.label = "Save"})}});
    CHECK_THROWS_AS(ui::Mounted(h.runtime, h.backend, ui::column(ui::Column{.children = {bar}})), std::logic_error);
}

namespace {

/// Files moved inline: three downloads from bytes and an upload into bytes.
constexpr char const* kInline = R"({
  "vocab": "ui/1", "id": "inline", "params": { "blob": "string?" },
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "picked": { "init": "/data/readings.csv" }, "saved": { "init": 0 } },
  "mutations": { "attach": { "model": "sample", "action": "Lims_Attach", "body": { "obj": { "kind": "csv" } } } },
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "save", "label": "save",
      "onClick": [ { "download": "AQID", "from": "bytes", "save": "dialog", "name": "three.bin",
                     "type": "application/octet-stream" } ] },
    { "kind": "button", "testId": "saveList", "label": "saveList",
      "onClick": [ { "download": { "lit": [ 1, 2, 255 ] }, "from": "bytes", "save": "dialog", "name": "list.bin" } ] },
    { "kind": "button", "testId": "saveBig", "label": "saveBig",
      "onClick": [ { "download": { "ref": "blob" }, "from": "bytes", "save": "dialog", "name": "big.bin" } ] },
    { "kind": "button", "testId": "send", "label": "send",
      "onClick": [ { "upload": { "ref": "picked" }, "to": "attach", "into": "bytes", "as": "content",
                     "nameAs": "fileName", "id": "up" } ] },
    { "kind": "text", "testId": "progress",
      "text": { "fn": "format", "args": [ "{0}/{1}", { "ref": "up.progress" }, { "ref": "up.pending" } ] } } ] } })";

}  // namespace

// Mutation: skip the cap check (the oversize download reaches the host).
TEST_CASE("interpreter commands: a download from bytes reaches saveFile decoded, and one over the cap never does",
          "[ui][interpreter]") {
    Harness h;
    std::string const big(((ui::kDefaultInlineFileBytes + 3) / 3) * 4, 'A');
    auto screen = h.mount(kInline, ui::ValueRecord{{"blob", ui::Value{big}}});
    h.backend.click(h.byTestId("Button", "save"));
    h.backend.click(h.byTestId("Button", "saveList"));
    h.settle();
    REQUIRE(h.host.saved.size() == 2);
    CHECK(h.host.saved.at(0).name == "three.bin");
    CHECK(h.host.saved.at(0).mediaType == "application/octet-stream");
    CHECK(h.host.saved.at(0).bytes == std::string{"\x01\x02\x03", 3});
    CHECK(h.host.saved.at(1).bytes == std::string{"\x01\x02\xff", 3});
    CHECK(h.diagnostics.empty());

    h.backend.click(h.byTestId("Button", "saveBig"));
    h.settle();
    CHECK(h.host.saved.size() == 2);
    REQUIRE(h.diagnostics.size() == 1);
    CHECK(h.diagnostics.front().reason ==
          "download: 1 MiB is over the 1 MiB inline cap; serve it through the file store");
}

// Mutation: skip the cap check on the host's reply (the oversize file runs the mutation).
TEST_CASE("interpreter commands: an upload into bytes runs its mutation with the base64 and the name",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kInline);
    h.backend.click(h.byTestId("Button", "send"));
    h.settle();
    CHECK(h.host.readCap == ui::kDefaultInlineFileBytes);
    CHECK(h.text("progress") == "0/true");
    h.host.readDone(ui::InlineFile{.name = "readings.csv", .mediaType = "text/csv", .bytes = "a,b"});
    h.settle();
    CHECK(h.text("progress") == "1/false");
    REQUIRE(h.dispatcher.count("Lims_Attach") == 1);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_Attach")).body ==
          R"({"content":"YSxi","fileName":"readings.csv","kind":"csv"})");

    // A host that answers more than the cap is refused all the same.
    h.backend.click(h.byTestId("Button", "send"));
    h.settle();
    h.host.readDone(ui::InlineFile{
        .name = "huge.csv", .mediaType = {}, .bytes = std::string(ui::kDefaultInlineFileBytes + 1, 'x')});
    h.settle();
    CHECK(h.dispatcher.count("Lims_Attach") == 1);
    CHECK(readJson(*screen, R"({"ref": "up.error"})") ==
          R"("upload: 1 MiB is over the 1 MiB inline cap; send it through the file store")");
}

TEST_CASE("interpreter commands: the session's inline cap is 1 to kMaxInlineFileBytes", "[ui][interpreter]") {
    STATIC_REQUIRE(ui::kMaxInlineFileBytes == 6'242'304);
    morph::testing::StepExecutor owner;
    morph::reactive::Runtime runtime{owner};
    CHECK_THROWS_AS(ui::Session(runtime, ui::SessionOptions{.maxInlineFileBytes = 0}), std::invalid_argument);
    CHECK_THROWS_AS(ui::Session(runtime, ui::SessionOptions{.maxInlineFileBytes = ui::kMaxInlineFileBytes + 1}),
                    std::invalid_argument);
    CHECK_NOTHROW(ui::Session(runtime, ui::SessionOptions{.maxInlineFileBytes = ui::kMaxInlineFileBytes}));
}

namespace {

/// A table whose rows a button empties, fills and nulls, with an empty state in its place while they are an empty
/// list.
constexpr std::string_view kEmptyTable = R"({
  "vocab": "ui/1", "id": "empty",
  "state": { "rows": { "init": { "lit": [] } } },
  "view": { "kind": "column", "children": [
    { "kind": "table", "id": "list", "testId": "list", "rows": { "ref": "rows" },
      "columns": [ { "id": "name", "label": "Name" } ],
      "empty": [ { "kind": "emptyState", "testId": "nothing", "title": "No rows yet" } ] },
    { "kind": "button", "testId": "fill", "label": "fill", "onClick": { "set": "rows", "value": { "lit": [ { "id": 1, "name": "a" } ] } } },
    { "kind": "button", "testId": "none", "label": "none", "onClick": { "set": "rows", "value": null } },
    { "kind": "button", "testId": "focus", "label": "focus", "onClick": { "focus": "list" } } ] }
})";

/// Whether @p widget shows: it and every column around it are visible.
bool shownThroughColumns(morph::ui::testing::RecordingBackend const& backend, int widget) {
    for (std::optional<int> current = widget; current;) {
        if (backend.prop(*current, "visible") == "false") {
            return false;
        }
        std::optional<int> parent;
        for (int const column : backend.all("Column")) {
            if (std::ranges::contains(backend.children(column), *current)) {
                parent = column;
            }
        }
        current = parent;
    }
    return true;
}

}  // namespace

// Mutation: lower `table` without `empty` (no empty state is drawn, and the table shows with no rows).
TEST_CASE("table: its empty content shows while the rows are an empty list, the table otherwise",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(kEmptyTable);
    INFO((h.diagnostics.empty() ? std::string{} : h.diagnostics.front().message()));
    REQUIRE(h.diagnostics.empty());
    int const table = h.byTestId("Table", "list");
    int const nothing = h.byTestId("EmptyState", "nothing");
    CHECK_FALSE(shownThroughColumns(h.backend, table));
    CHECK(shownThroughColumns(h.backend, nothing));
    CHECK(h.backend.prop(nothing, "title") == "No rows yet");

    h.backend.click(h.byTestId("Button", "fill"));
    h.settle();
    CHECK(shownThroughColumns(h.backend, table));
    CHECK_FALSE(shownThroughColumns(h.backend, nothing));

    h.backend.click(h.byTestId("Button", "none"));
    h.settle();
    CHECK(shownThroughColumns(h.backend, table));  // not answered yet: the table, not the empty state
    CHECK_FALSE(shownThroughColumns(h.backend, nothing));

    // The table, not the column around it, is the element `focus` finds.
    h.backend.click(h.byTestId("Button", "focus"));
    h.settle();
    CHECK(h.backend.focused() == table);
}

// Mutation: lower `sizing` to the width alone (the record's height is lost); or drop the bounds in commonOf.
TEST_CASE(
    "sizing: a record sizes both dimensions, the bounds reach the widget, and a computed sizing of no known "
    "form is reported once",
    "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "sized", "state": { "w": { "init": "wide" } },
      "view": { "kind": "column", "testId": "root",
                "sizing": { "lit": { "width": { "stretch": 1 }, "height": { "stretch": 1 } } },
                "children": [
        { "kind": "text", "testId": "tall", "text": "x", "sizing": { "lit": { "height": { "stretch": 2 } } },
          "maxWidth": 44, "minHeight": 1 },
        { "kind": "forEach", "rows": { "lit": [ 1, 2 ] }, "children": [
          { "kind": "text", "testId": "odd", "text": "y", "sizing": { "ref": "w" } } ] } ] } })");
    CHECK(h.backend.prop(h.byTestId("Column", "root"), "layout") == "stretch(1)/stretch(1)");
    CHECK(h.backend.prop(h.byTestId("Text", "tall"), "layout") == "content max=44/stretch(2) min=1");
    CHECK(h.backend.prop(h.byTestId("Text", "odd"), "layout").empty());  // content, so no setLayout call
    REQUIRE(h.diagnostics.size() == 1);  // once for the path, though two rows lowered it
    CHECK(h.diagnostics.front().reason.starts_with(R"(sizing is "wide"; a sizing is)"));
}

// Mutation: lower `align`, `justify` and `alignSelf` to their defaults (the recording loses them).
TEST_CASE("align, justify and alignSelf: a document's alignment reaches the stacks, the grid and the child",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "aligned",
      "view": { "kind": "column", "testId": "root", "align": "center", "justify": "end", "children": [
        { "kind": "row", "testId": "bar", "align": "end", "justify": "between", "children": [
          { "kind": "text", "testId": "own", "text": "x", "alignSelf": "start" } ] },
        { "kind": "grid", "testId": "cells", "columns": 2, "align": "center", "children": [
          { "kind": "text", "text": "y" } ] },
        { "kind": "row", "testId": "plain", "children": [] } ] } })");
    CHECK(h.backend.prop(h.byTestId("Column", "root"), "align") == "center");
    CHECK(h.backend.prop(h.byTestId("Column", "root"), "justify") == "end");
    CHECK(h.backend.prop(h.byTestId("Row", "bar"), "align") == "end");
    CHECK(h.backend.prop(h.byTestId("Row", "bar"), "justify") == "between");
    CHECK(h.backend.prop(h.byTestId("Text", "own"), "layout") == "content/content self=start");
    CHECK(h.backend.prop(h.byTestId("Grid", "cells"), "align") == "center");
    CHECK(h.backend.prop(h.byTestId("Row", "plain"), "align").empty());  // the default makes no call
    CHECK(h.backend.prop(h.byTestId("Row", "plain"), "justify").empty());
    CHECK(h.diagnostics.empty());
}
