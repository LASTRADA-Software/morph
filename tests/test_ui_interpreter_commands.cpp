// SPDX-License-Identifier: Apache-2.0

// The commands corpus of spec 5 §8 and §14, with the lifecycle keys of spec 6 §3: navigate as a dialog answering
// through onResult or onCancel, focus, a missing host command, switchBackend for a listed and an unlisted backend,
// setLocale, cancelClose, transfers, the mount refusals, and every node kind lowered once.

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>
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

TEST_CASE("interpreter commands: focus, host commands, return and close go to the host", "[ui][interpreter]") {
    Harness h;
    h.host.commands = {"openPath"};
    auto screen = h.mount(kCommands, who(3));
    h.backend.click(h.byTestId("Button", "focus"));
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
    CHECK(h.host.log == std::vector<std::string>{"focus cmds note", R"(host openPath {"path":"/tmp"})",
                                                 "host nope null", "close cmds 3", "close cmds",
                                                 R"(app banner "hey")"});
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
    for (char const* const button : {"pick", "focus", "host", "switch", "notify", "return", "self", "app", "upload",
                                     "download", "locale", "signout"}) {
        h.backend.click(h.byTestId("Button", button));
        h.settle();
    }
    CHECK(reported.size() >= 10);
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
    // The forms engine and the table engine nodes are reported, not lowered.
    CHECK(h.diagnostics.size() == 2);
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
                         "`navigate` is not supported by the host", "`focus` is not supported by the host",
                         "`notify` is not supported by the host", "`return` is not supported by the host",
                         "`close: self` is not supported by the host", "`set` of `app` is not supported by the host"});
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
