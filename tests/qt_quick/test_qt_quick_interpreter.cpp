// SPDX-License-Identifier: Apache-2.0
//
// UI documents mounted by the interpreter on the Qt Quick backend and driven through the real engine (spec 3 §11):
// a bound property follows its value, a flush is one frame, a refused selection snaps back, typing keeps the cursor,
// a dialog dismisses and reopens; lists reconcile with the fewest moves, keep focus across a reorder and carry an
// int64 key above 2^53; and spec 5 §7's server-calculated values show in the rendered cells, stale while in flight.

#include <QQuickItem>
#include <QQuickWindow>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/interpreter.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "qt_quick/kinds.hpp"
#include "qt_quick_harness.hpp"
#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
namespace detail = morph::qt_quick::detail;
using morph::qt_quick::testing::Scene;
using ui::testing::corpusCatalog;
using ui::testing::KindedError;
using ui::testing::loadOrFail;
using ui::testing::readJson;
using ui::testing::RecordingHost;
using ui::testing::ScriptedDispatcher;
using namespace std::chrono_literals;

namespace {

/// One client on the Qt Quick backend: the scene, a session over a manual clock, a scripted dispatcher and a host.
struct QtHarness {
    Scene scene;
    morph::reactive::testing::ManualScheduler scheduler;
    ui::Session session{scene.runtime(),
                        ui::SessionOptions{.scheduler = &scheduler,
                                           .clock = [this] { return scheduler.now(); },
                                           .keepUnused = std::chrono::minutes{5},
                                           .catalog = corpusCatalog(),
                                           .classify = [](std::exception_ptr const& error) -> std::string {
                                               try {
                                                   std::rethrow_exception(error);
                                               } catch (KindedError const& kinded) {
                                                   return kinded.kind;
                                               } catch (...) {
                                                   return "error";
                                               }
                                           }}};
    ScriptedDispatcher dispatcher{scene.owner()};
    RecordingHost host;
    ui::Interpreter interpreter{session, dispatcher, ui::InterpreterOptions{.host = &host}};

    std::unique_ptr<ui::Screen> mount(std::string_view text) {
        auto screen = interpreter.mount(loadOrFail(text, corpusCatalog()), {}, scene.backend());
        if (!screen) {
            FAIL(screen.error().message());
        }
        scene.settle();
        return std::move(*screen);
    }

    void reply(std::size_t index, std::string json) {
        dispatcher.reply(index, std::move(json));
        scene.settle();
    }

    void advance(std::chrono::milliseconds delta) {
        scheduler.advance(delta);
        scene.settle();
    }

    /// The widget whose test id is @p testId.
    static detail::ItemBase const& widget(std::string_view testId) {
        detail::ItemBase const* const found = detail::findWidget(detail::toQString(testId));
        if (found == nullptr) {
            FAIL("no widget with test id " << testId);
        }
        return *found;
    }

    /// Every widget whose test id is @p testId, in the order the window shows them, top to bottom.
    static std::vector<detail::ItemBase const*> widgets(std::string_view testId) {
        std::vector<detail::ItemBase const*> found;
        for (detail::ItemBase const* const candidate : detail::liveWidgets()) {
            if (candidate->item() != nullptr && candidate->item()->objectName() == detail::toQString(testId)) {
                found.push_back(candidate);
            }
        }
        std::ranges::sort(found, [](detail::ItemBase const* lhs, detail::ItemBase const* rhs) {
            return lhs->item()->mapToScene(QPointF{0, 0}).y() < rhs->item()->mapToScene(QPointF{0, 0}).y();
        });
        return found;
    }

    /// Clicks a widget, at its control when it has one.
    void click(std::string_view testId) {
        detail::ItemBase const& target = widget(testId);
        QQuickItem const* clicked = target.focusTarget();
        if (clicked == nullptr) {
            clicked = target.item();
        }
        REQUIRE(clicked != nullptr);
        scene.click(*clicked);
    }
};

constexpr char const* kBehaviour = R"({
  "vocab": "ui/1", "id": "behaviour",
  "state": { "count": { "init": 0 }, "pick": { "init": "a" }, "kept": { "init": "a" }, "name": { "init": "" },
             "keepOpen": { "init": false } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "count", "text": { "fn": "format", "args": [ "{0}", { "ref": "count" } ] } },
    { "kind": "button", "testId": "add", "label": "Add",
      "onClick": [ { "set": "count", "value": { "fn": "add", "args": [ { "ref": "count" }, 1 ] } } ] },
    { "kind": "select", "testId": "refused", "style": "radio", "options": { "lit": [ "a", "b" ] },
      "selected": { "ref": "kept" } },
    { "kind": "select", "testId": "taken", "style": "radio", "options": { "lit": [ "a", "b" ] },
      "selected": { "ref": "pick" }, "onChoose": [ { "set": "pick", "value": { "ref": "event" } } ] },
    { "kind": "textInput", "testId": "name", "value": { "ref": "name" },
      "onChange": [ { "set": "name", "value": { "ref": "event" } } ] },
    { "kind": "text", "testId": "echo", "text": { "ref": "name" } },
    { "kind": "button", "testId": "open", "label": "Open", "onClick": { "open": "dlg" } },
    { "kind": "checkbox", "testId": "keepOpen", "label": "Keep open", "checked": { "ref": "keepOpen" },
      "onToggle": [ { "set": "keepOpen", "value": { "ref": "event" } } ] },
    { "kind": "dialog", "id": "dlg", "title": "Hello",
      "onDismiss": [ { "if": [ { "ref": "keepOpen" }, [ { "open": "dlg" } ] ] } ],
      "children": [ { "kind": "text", "testId": "inside", "text": "Inside" } ] } ] } })";

QList<QQuickItem*> radiosOf(std::string_view testId) {
    QQuickItem* const control = qobject_cast<QQuickItem*>(QtHarness::widget(testId).part("control"));
    REQUIRE(control != nullptr);
    return Scene::itemsOf(*control, "QQuickRadioButton");
}

}  // namespace

TEST_CASE("qt_quick interpreter: a bound property follows its value", "[qt_quick][interpreter]") {
    QtHarness h;
    auto screen = h.mount(kBehaviour);
    CHECK(QtHarness::widget("count").shownText() == "0");
    h.click("add");
    h.click("add");
    CHECK(QtHarness::widget("count").shownText() == "2");
    CHECK(readJson(*screen, R"({"ref": "count"})") == "2");
}

TEST_CASE("qt_quick interpreter: a refused selection snaps back, an accepted one stays", "[qt_quick][interpreter]") {
    QtHarness h;
    auto screen = h.mount(kBehaviour);
    auto const& refused = dynamic_cast<detail::SelectImpl const&>(QtHarness::widget("refused"));
    auto const& taken = dynamic_cast<detail::SelectImpl const&>(QtHarness::widget("taken"));
    CHECK(refused.shownText() == "a");
    h.scene.click(*radiosOf("refused").at(1));
    CHECK(refused.shownText() == "a");
    CHECK(radiosOf("refused").at(0)->property("checked").toBool());
    CHECK_FALSE(radiosOf("refused").at(1)->property("checked").toBool());
    h.scene.click(*radiosOf("taken").at(1));
    CHECK(taken.shownText() == "b");
    CHECK(readJson(*screen, R"({"ref": "pick"})") == R"("b")");
}

TEST_CASE("qt_quick interpreter: typing into a controlled input keeps the cursor", "[qt_quick][interpreter]") {
    QtHarness h;
    auto screen = h.mount(kBehaviour);
    auto const& name = dynamic_cast<detail::TextInputImpl const&>(QtHarness::widget("name"));
    h.click("name");
    h.scene.type(QStringLiteral("abc"));
    CHECK(QtHarness::widget("echo").shownText() == "abc");
    name.moveCursor(1);
    h.scene.type(QStringLiteral("X"));
    CHECK(name.shownText() == "aXbc");
    CHECK(QtHarness::widget("echo").shownText() == "aXbc");
    CHECK(name.cursor() == 2);
}

TEST_CASE("qt_quick interpreter: a dialog dismisses and reopens, and a document that keeps it open keeps it open",
          "[qt_quick][interpreter]") {
    QtHarness h;
    auto screen = h.mount(kBehaviour);
    h.click("open");
    REQUIRE(detail::findWidget(QStringLiteral("inside")) != nullptr);
    CHECK(QtHarness::widget("inside").item()->isVisible());
    h.scene.key(Qt::Key_Escape);
    CHECK(detail::findWidget(QStringLiteral("inside")) == nullptr);
    h.click("open");
    REQUIRE(detail::findWidget(QStringLiteral("inside")) != nullptr);
    h.scene.key(Qt::Key_Escape);
    CHECK(detail::findWidget(QStringLiteral("inside")) == nullptr);
    h.click("keepOpen");
    h.click("open");
    h.scene.key(Qt::Key_Escape);
    REQUIRE(detail::findWidget(QStringLiteral("inside")) != nullptr);
    CHECK(QtHarness::widget("inside").item()->isVisible());
}

namespace {

constexpr char const* kFrames = R"({
  "vocab": "ui/1", "id": "frames",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": { "one": { "model": "sample", "action": "Lims_ListResults" },
               "two": { "model": "sample", "action": "Lims_ListSamples" } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "a", "text": { "fn": "format", "args": [ "{0}", { "ref": "one.a" } ] } },
    { "kind": "text", "testId": "b", "text": { "fn": "format", "args": [ "{0}", { "ref": "one.b" } ] } },
    { "kind": "text", "testId": "c", "text": { "fn": "format", "args": [ "{0}", { "ref": "one.c" } ] } },
    { "kind": "text", "testId": "d", "text": { "fn": "format", "args": [ "{0}", { "ref": "two.d" } ] } } ] } })";

}  // namespace

TEST_CASE("qt_quick interpreter: a flush that changes several values is one frame", "[qt_quick][interpreter]") {
    QtHarness h;
    auto screen = h.mount(kFrames);
    int frames = 0;
    QObject::connect(&h.scene.window(), &QQuickWindow::frameSwapped, &h.scene.window(), [&frames] { ++frames; });
    h.scene.settle();
    frames = 0;
    std::size_t const one = h.dispatcher.last("Lims_ListResults");
    std::size_t const two = h.dispatcher.last("Lims_ListSamples");
    h.reply(one, R"({"a":1,"b":2,"c":3})");
    CHECK(QtHarness::widget("a").shownText() == "1");
    CHECK(QtHarness::widget("c").shownText() == "3");
    CHECK(frames == 1);
    // A second flush in a later turn of the loop is a frame of its own: the count measures frames.
    h.reply(two, R"({"d":4})");
    CHECK(QtHarness::widget("d").shownText() == "4");
    CHECK(frames == 2);
}

namespace {

constexpr char const* kList = R"({
  "vocab": "ui/1", "id": "list",
  "state": { "rows": { "init": { "lit": [ { "id": 1, "name": "one" }, { "id": 2, "name": "two" }, { "id": 3, "name": "three" } ] } } },
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "reverse", "label": "reverse",
      "onClick": [ { "set": "rows", "value": { "lit": [ { "id": 3, "name": "three" }, { "id": 2, "name": "two" }, { "id": 1, "name": "one" } ] } } ] },
    { "kind": "button", "testId": "front", "label": "front",
      "onClick": [ { "set": "rows", "value": { "lit": [ { "id": 0, "name": "zero" }, { "id": 1, "name": "one" }, { "id": 2, "name": "two" }, { "id": 3, "name": "three" } ] } } ] },
    { "kind": "button", "testId": "append", "label": "append",
      "onClick": [ { "set": "rows", "value": { "lit": [ { "id": 1, "name": "one" }, { "id": 2, "name": "two" }, { "id": 3, "name": "three" }, { "id": 4, "name": "four" } ] } } ] },
    { "kind": "button", "testId": "remove", "label": "remove",
      "onClick": [ { "set": "rows", "value": { "lit": [ { "id": 1, "name": "one" }, { "id": 3, "name": "three" } ] } } ] },
    { "kind": "button", "testId": "rename", "label": "rename",
      "onClick": [ { "set": "rows", "value": { "lit": [ { "id": 1, "name": "uno" }, { "id": 2, "name": "two" }, { "id": 3, "name": "three" } ] } } ] },
    { "kind": "forEach", "rows": { "ref": "rows" }, "key": { "row": "id" },
      "children": [ { "kind": "textInput", "testId": "cell", "value": { "row": "name" } } ] } ] } })";

std::vector<std::string> cellTexts() {
    std::vector<detail::ItemBase const*> const cells = QtHarness::widgets("cell");
    std::vector<std::string> texts;
    texts.reserve(cells.size());
    for (detail::ItemBase const* const cell : cells) {
        texts.push_back(cell->shownText());
    }
    return texts;
}

std::vector<QQuickItem*> cellItems() {
    std::vector<detail::ItemBase const*> const cells = QtHarness::widgets("cell");
    std::vector<QQuickItem*> items;
    items.reserve(cells.size());
    for (detail::ItemBase const* const cell : cells) {
        items.push_back(cell->item());
    }
    return items;
}

// The container the rows are in: the forEach's stack, the first container up from a cell that holds every row.
detail::ContainerCore& rowsContainer() {
    detail::ContainerCore* container = QtHarness::widget("cell").container();
    while (container != nullptr && container->children().size() < 3) {
        auto const* const holder = dynamic_cast<detail::ItemBase const*>(container);
        container = holder != nullptr ? holder->container() : nullptr;
    }
    REQUIRE(container != nullptr);
    return *container;
}

}  // namespace

TEST_CASE("qt_quick interpreter: a list change moves the fewest rows and keeps every kept row's item",
          "[qt_quick][interpreter]") {
    QtHarness h;
    auto screen = h.mount(kList);
    CHECK(cellTexts() == std::vector<std::string>{"one", "two", "three"});
    std::vector<QQuickItem*> const before = cellItems();
    detail::ContainerCore const& rows = rowsContainer();

    SECTION("reversing three rows is two moves") {
        h.click("reverse");
        CHECK(cellTexts() == std::vector<std::string>{"three", "two", "one"});
        CHECK(rows.restacks() == 2);
        std::vector<QQuickItem*> const after = cellItems();
        CHECK(after == std::vector<QQuickItem*>{before.at(2), before.at(1), before.at(0)});
    }
    SECTION("an insert at the front is one move") {
        h.click("front");
        CHECK(cellTexts() == std::vector<std::string>{"zero", "one", "two", "three"});
        CHECK(rows.restacks() == 1);
    }
    SECTION("an append and a removal move nothing") {
        h.click("append");
        CHECK(cellTexts() == std::vector<std::string>{"one", "two", "three", "four"});
        h.click("remove");
        CHECK(cellTexts() == std::vector<std::string>{"one", "three"});
        CHECK(rows.restacks() == 0);
        CHECK(cellItems() == std::vector<QQuickItem*>{before.at(0), before.at(2)});
    }
    SECTION("an update changes the row in place") {
        h.click("rename");
        CHECK(cellTexts() == std::vector<std::string>{"uno", "two", "three"});
        CHECK(rows.restacks() == 0);
        CHECK(cellItems() == before);
    }
}

TEST_CASE("qt_quick interpreter: focus and a half-typed edit survive a reorder", "[qt_quick][interpreter]") {
    QtHarness h;
    auto screen = h.mount(kList);
    detail::ItemBase const& two = *QtHarness::widgets("cell").at(1);
    h.scene.click(*two.focusTarget());
    h.scene.type(QStringLiteral("!"));
    REQUIRE(two.focusTarget()->hasActiveFocus());
    // The reverse button takes no focus from a click: focus moves only when the user moves it.
    two.focusTarget()->forceActiveFocus();
    detail::ItemBase const& reverse = QtHarness::widget("reverse");
    reverse.relay().fire(QStringLiteral("click"));
    h.scene.settle();
    CHECK(cellTexts() == std::vector<std::string>{"three", "two!", "one"});
    CHECK(two.focusTarget()->hasActiveFocus());
}

namespace {

constexpr char const* kKeys = R"({
  "vocab": "ui/1", "id": "keys",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": { "results": { "model": "sample", "action": "Lims_ListResults" } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "picked", "text": { "fn": "format", "args": [ "{0}", { "ref": "grid.selection" } ] } },
    { "kind": "table", "id": "grid", "rows": { "query": "results" }, "key": { "row": "id" }, "selectionMode": "multiple",
      "columns": [ { "label": "Name", "cell": { "kind": "text", "testId": "name", "text": { "row": "name" } } } ] } ] } })";

}  // namespace

TEST_CASE("qt_quick interpreter: a table row keyed above 2^53 is selected and reported exactly",
          "[qt_quick][interpreter]") {
    QtHarness h;
    auto screen = h.mount(kKeys);
    h.reply(0, R"([{"id":9007199254740993,"name":"big"},{"id":9007199254740994,"name":"bigger"}])");
    std::vector<detail::ItemBase const*> const names = QtHarness::widgets("name");
    REQUIRE(names.size() == 2);
    // The row is the cell's container's container: the cell sits in the column that holds the row's scope.
    detail::ContainerCore* const holder = names.at(1)->container();
    REQUIRE(holder != nullptr);
    auto* const row = dynamic_cast<detail::TableRowImpl*>(holder);
    detail::ItemBase const* rowBase = row;
    if (rowBase == nullptr) {
        auto const* const inner = dynamic_cast<detail::ItemBase const*>(holder);
        REQUIRE(inner != nullptr);
        rowBase = dynamic_cast<detail::TableRowImpl*>(inner->container());
    }
    REQUIRE(rowBase != nullptr);
    CHECK(rowBase->read("rowKey").toString() == QStringLiteral("i:9007199254740994"));
    h.scene.click(*rowBase->item());
    CHECK(readJson(*screen, R"({"ref": "grid.selection"})") == "[9007199254740994]");
}

namespace {

// Spec 5 §7: each row keeps a draft, previews a calculation through a debounced row query, and shows the result,
// stale while a newer one is in flight.
constexpr char const* kCalc = R"({
  "vocab": "ui/1", "id": "calc", "showErrors": "always",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": { "results": { "model": "sample", "action": "Lims_ListResults" } },
  "view": { "kind": "table", "id": "grid", "rows": { "query": "results" }, "key": { "row": "id" },
    "scope": {
      "state": { "draft": { "init": { "row": "inputs" } } },
      "queries": { "calc": { "model": "sample", "action": "Lims_EvaluateResult", "debounce": 300,
                             "body": { "obj": { "resultId": { "row": "id" }, "inputs": { "ref": "draft" } } } } } },
    "columns": [
      { "label": "Mass", "cell": { "kind": "textInput", "id": "mass", "testId": "mass", "commitOn": "change",
                                   "value": { "ref": "draft.mass" },
                                   "onChange": [ { "set": "draft.mass", "value": { "ref": "event" } } ] } },
      { "label": "Result", "cell": { "kind": "text", "testId": "result",
                                     "text": { "fn": "format", "args": [ "{0}", { "fn": "coalesce", "args": [ { "ref": "calc.concentration" }, { "row": "concentration" } ] } ] },
                                     "stale": { "pending": "calc" } } },
      { "label": "Error", "cell": { "kind": "text", "testId": "error", "text": { "fn": "coalesce", "args": [ { "error": "calc" }, "" ] } } } ] } })";

constexpr char const* kRows = R"([
  {"id":1,"inputs":{"mass":"10"},"mass":10,"concentration":1},
  {"id":2,"inputs":{"mass":"20"},"mass":20,"concentration":2}])";

detail::ItemBase const& cell(std::string_view testId, std::size_t row) {
    std::vector<detail::ItemBase const*> const found = QtHarness::widgets(testId);
    REQUIRE(found.size() > row);
    return *found.at(row);
}

std::unique_ptr<ui::Screen> mountRows(QtHarness& h) {
    auto screen = h.mount(kCalc);
    h.reply(0, kRows);
    REQUIRE(h.dispatcher.count("Lims_EvaluateResult") == 2);
    h.reply(1, R"({"concentration":1})");
    h.reply(2, R"({"concentration":2})");
    return screen;
}

}  // namespace

TEST_CASE("qt_quick interpreter: server-calculated values show in their row, stale while a newer one is in flight",
          "[qt_quick][interpreter]") {
    QtHarness h;
    auto screen = mountRows(h);
    CHECK(cell("result", 0).shownText() == "1");
    CHECK(cell("result", 1).shownText() == "2");
    CHECK(cell("mass", 0).shownText() == "10");
    CHECK(cell("result", 0).item()->opacity() == 1.0);

    // The user types into row 1: its result is stale, never blank, until the calculation replies.
    h.scene.click(*cell("mass", 0).focusTarget());
    h.scene.type(QStringLiteral("5"));
    CHECK(cell("mass", 0).shownText() == "105");
    CHECK(cell("result", 0).read("stale").toBool());
    CHECK(cell("result", 0).item()->opacity() < 1.0);
    CHECK(cell("result", 0).shownText() == "1");
    CHECK_FALSE(cell("result", 1).read("stale").toBool());
    h.advance(300ms);
    std::size_t const first = h.dispatcher.last("Lims_EvaluateResult");
    CHECK(h.dispatcher.calls.at(first).body == R"({"inputs":{"mass":"105"},"resultId":1})");

    // A newer edit supersedes it: the older reply is dropped, the newer one shows.
    h.scene.type(QStringLiteral("0"));
    h.advance(300ms);
    std::size_t const second = h.dispatcher.last("Lims_EvaluateResult");
    REQUIRE(second == first + 1);
    h.reply(second, R"({"concentration":1050})");
    h.reply(first, R"({"concentration":105})");
    CHECK(cell("result", 0).shownText() == "1050");
    CHECK_FALSE(cell("result", 0).read("stale").toBool());
    CHECK(cell("result", 1).shownText() == "2");
}

TEST_CASE("qt_quick interpreter: a failed calculation keeps the stale value and shows the row's error",
          "[qt_quick][interpreter]") {
    QtHarness h;
    auto screen = mountRows(h);
    h.scene.click(*cell("mass", 1).focusTarget());
    h.scene.type(QStringLiteral("1"));
    h.advance(300ms);
    std::size_t const call = h.dispatcher.last("Lims_EvaluateResult");
    h.dispatcher.fail(call, "offline", "no connection");
    h.scene.settle();
    CHECK(cell("result", 1).shownText() == "2");
    CHECK(cell("error", 1).shownText() == "no connection");
    CHECK(cell("error", 0).shownText().empty());
}

// A `form` node renders through Qt Quick: the forms engine's view tree, typed into on the real engine, submits the
// exact body through the model alias.
TEST_CASE("qt_quick interpreter: a form document renders and submits", "[qt_quick][interpreter][forms]") {
    QtHarness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "capture",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "view": { "kind": "column", "children": [
        { "kind": "form", "id": "cap", "model": "sample", "action": "Lims_CaptureConcentration",
          "fields": { "note": { "label": "Remarks" } } },
        { "kind": "text", "testId": "ready", "text": { "if": [ { "ref": "cap.ready" }, "ready", "not ready" ] } } ] } })");
    CHECK(QtHarness::widget("label_note").shownText() == "Remarks");
    CHECK(QtHarness::widget("ready").shownText() == "not ready");
    h.click("field_sampleId");
    h.scene.type(QStringLiteral("9007199254740993"));
    h.click("field_concentration");
    h.scene.type(QStringLiteral("12.5"));
    CHECK(QtHarness::widget("ready").shownText() == "ready");
    REQUIRE(h.dispatcher.count("Lims_CaptureConcentration") >= 1);
    auto const& call = h.dispatcher.calls.at(h.dispatcher.last("Lims_CaptureConcentration"));
    CHECK(call.body == R"({"sampleId":9007199254740993,"concentration":{"num":1250,"den":100,"dp":2}})");
}

// Mutation: put the ` *` suffix back on the cell's label (the label reads `Sample *` and the cell shows two marks).
TEST_CASE("qt_quick interpreter: a required form field shows one required mark, its control's",
          "[qt_quick][interpreter][forms]") {
    QtHarness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "capture",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "view": { "kind": "form", "id": "cap", "model": "sample", "action": "Lims_CaptureConcentration" } })");
    CHECK(QtHarness::widget("label_sampleId").shownText() == "Sample");
    QQuickItem* const cell = QtHarness::widget("cell_sampleId").item();
    REQUIRE(cell != nullptr);
    int marks = 0;
    for (QQuickItem const* const text : Scene::itemsOf(*cell, "QQuickText")) {
        if (text->isVisible() && text->property("text").toString().contains(QLatin1Char('*'))) {
            ++marks;
        }
    }
    CHECK(marks == 1);
}

// Mutation: tell the inputs their label is never shown (no caption is visible).
TEST_CASE("qt_quick interpreter: a standalone input draws its caption above the control when the document asks",
          "[qt_quick][interpreter][forms]") {
    QtHarness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "captions",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "view": { "kind": "column", "children": [
        { "kind": "textInput", "testId": "shownText", "label": "Name", "showLabel": true },
        { "kind": "select", "testId": "shownSelect", "label": "Grade", "showLabel": true },
        { "kind": "dateTimeInput", "testId": "shownWhen", "label": "When", "showLabel": true },
        { "kind": "textInput", "testId": "named", "label": "Hidden caption" },
        { "kind": "form", "id": "cap", "model": "sample", "action": "Lims_CaptureConcentration" } ] } })");
    auto const captionOf = [](std::string_view testId) -> QQuickItem const& {
        QQuickItem const* const item = QtHarness::widget(testId).item();
        REQUIRE(item != nullptr);
        auto const* const caption = item->findChild<QQuickItem*>(QStringLiteral("caption"));
        REQUIRE(caption != nullptr);
        return *caption;
    };
    for (auto const& [testId, text] :
         {std::pair{"shownText", "Name"}, std::pair{"shownSelect", "Grade"}, std::pair{"shownWhen", "When"}}) {
        INFO(testId);
        QQuickItem const& caption = captionOf(testId);
        CHECK(caption.isVisible());
        CHECK(caption.property("text").toString() == QString::fromUtf8(text));
        // The control sits under it.
        QQuickItem const* const control = qobject_cast<QQuickItem*>(QtHarness::widget(testId).part("control"));
        REQUIRE(control != nullptr);
        CHECK(control->y() >= caption.height());
    }
    CHECK_FALSE(captionOf("named").isVisible());
    // In a form the cell's label is the caption, and the input draws none.
    CHECK_FALSE(captionOf("field_sampleId").isVisible());
    CHECK_FALSE(QtHarness::widget("field_sampleId").read("labelShown").toBool());
    CHECK(QtHarness::widget("label_sampleId").shownText() == "Sample");
}

// A `collection` node renders through Qt Quick: the rows' exact cells, the confirm dialog gating a row action, and
// the row editor in its dialog, submitting through the model alias.
TEST_CASE("qt_quick interpreter: a collection document lists, confirms and edits", "[qt_quick][interpreter][forms]") {
    QtHarness h;
    h.dispatcher.autoReply.emplace("Lims_ListSamples", R"({"rows":[{"id":9007199254740993,"name":"Water"}]})");
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "samples",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "view": { "kind": "collection", "model": "sample", "action": "Lims_ListSamples" } })");
    CHECK(QtHarness::widget("collection_title").shownText() == "Samples");
    CHECK(QtHarness::widget("cell_name_9007199254740993").shownText() == "Water");
    h.click("rowAction_Lims_DeleteSample_9007199254740993");
    h.scene.settle();
    CHECK(h.dispatcher.count("Lims_DeleteSample") == 0);
    h.click("confirm_accept");
    h.scene.settle();
    REQUIRE(h.dispatcher.count("Lims_DeleteSample") == 1);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_DeleteSample")).body == R"({"id":9007199254740993})");
    h.click("rowOpen_9007199254740993");
    h.scene.settle();
    CHECK(QtHarness::widget("field_sampleId").shownText() == "9007199254740993");
    h.click("field_concentration");
    h.scene.type(QStringLiteral("2"));
    REQUIRE(h.dispatcher.count("Lims_CaptureConcentration") >= 1);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_CaptureConcentration")).body ==
          R"({"sampleId":9007199254740993,"concentration":{"num":200,"den":100,"dp":2}})");
}

// A `wizard` node renders through Qt Quick: Next waits for the step's success, and the second step is prefilled from
// the first, exactly.
TEST_CASE("qt_quick interpreter: a wizard document steps on", "[qt_quick][interpreter][forms]") {
    QtHarness h;
    h.dispatcher.autoReply.emplace("Lims_CaptureConcentration", R"({"ok":true})");
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "flow",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "view": { "kind": "wizard", "model": "sample", "action": "Lims_CaptureConcentration" } })");
    CHECK(QtHarness::widget("wizard_title").shownText() == "Capture flow  (1 / 2)");
    h.click("field_sampleId");
    h.scene.type(QStringLiteral("9007199254740993"));
    h.click("field_concentration");
    h.scene.type(QStringLiteral("1"));
    REQUIRE(h.dispatcher.count("Lims_CaptureConcentration") >= 1);
    h.click("wizardNext");
    h.scene.settle();
    CHECK(QtHarness::widget("wizard_title").shownText() == "Capture flow  (2 / 2)");
    CHECK(QtHarness::widget("field_sampleId").shownText() == "9007199254740993");
    REQUIRE(h.dispatcher.count("Lims_ConfirmSample") == 1);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_ConfirmSample")).body == R"({"sampleId":9007199254740993})");
}

namespace {

/// A host that places screens itself: it only records where a document asks to go.
class PlacingHost final : public ui::DocumentScreenHost {
public:
    PlacingHost(QtHarness& harness, ui::DocumentScreenHostOptions options)
        : DocumentScreenHost{harness.session, harness.dispatcher, std::move(options)} {}

    using DocumentScreenHost::onNavigate;

    void onNavigate(std::string_view screenId, ui::ParamMap const& params) override {
        static_cast<void>(params);
        navigated.emplace_back(screenId);
    }

    std::vector<std::string> navigated;
};

constexpr char const* kPlaced = R"({
  "vocab": "ui/1", "id": "placed", "title": "Placed",
  "state": { "count": { "init": 0 } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "placed.count", "text": { "fn": "format", "args": [ "{0}", { "ref": "count" } ] } },
    { "kind": "button", "testId": "placed.more", "label": "more",
      "onClick": [ { "set": "count", "value": { "fn": "add", "args": [ { "ref": "count" }, 1 ] } } ] } ] } })";

}  // namespace

// Mutation: have rootOf answer null for a document screen (the host cannot reach the screen's item).
TEST_CASE("qt_quick interpreter: a host mounts a document through DocumentScreenHost and places its root item",
          "[qt_quick]") {
    QtHarness h;
    std::shared_ptr<ui::Document const> const placed = loadOrFail(kPlaced, corpusCatalog());
    PlacingHost host{h, ui::DocumentScreenHostOptions{
                            .documents = [&placed](std::string_view id) -> std::shared_ptr<ui::Document const> {
                                return id == "placed" ? placed : nullptr;
                            },
                            .backend = &h.scene.backend(),
                            .guard = {},
                            .application = nullptr,
                            .diagnostics = {}}};
    auto mounted = host.mount("placed", ui::ParamMap{});
    REQUIRE(mounted.has_value());
    h.scene.settle();
    ui::Widget const* const root = ui::DocumentScreenHost::rootOf(**mounted);
    REQUIRE(root != nullptr);
    QQuickItem& item = Scene::itemOf(*root);

    // The host puts the item in its own layout; the screen keeps working there.
    QQuickItem* const original = item.parentItem();
    QQuickItem place{h.scene.window().contentItem()};
    item.setParentItem(&place);
    QQuickItem const* const count = QtHarness::widget("placed.count").item();
    REQUIRE(count != nullptr);
    CHECK(place.isAncestorOf(count));
    CHECK((*mounted)->title().get() == "Placed");
    h.scene.click(*QtHarness::widget("placed.more").item());
    h.scene.settle();
    CHECK(count->property("text").toString() == QStringLiteral("1"));
    item.setParentItem(original);
}
