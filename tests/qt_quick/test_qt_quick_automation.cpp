// SPDX-License-Identifier: Apache-2.0
//
// Automation conformance (spec 6 §12): a generated screen, mounted by the interpreter on the Qt Quick backend in a
// shown window, read back the way an automation driver reads it, through QAccessible, never through the QML items'
// properties. It checks what spec 6 §6 promises: the document's `a11y` name and role, a button's `button` role and
// press action, a read-only value named "<caption>: <value>", `testId` as the node's identity, and every input kind
// as one node found by its name and role that a driver focuses and types into or sets.

#include <QAccessible>
#include <QAccessibleActionInterface>
#include <QAccessibleInterface>
#include <QAccessibleValueInterface>
#include <QObject>
#include <QString>
#include <QtGlobal>
#include <algorithm>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <iterator>
#include <memory>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/ui/interpreter.hpp>
#include <string_view>
#include <utility>
#include <vector>

#include "qt_quick_harness.hpp"
#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using morph::qt_quick::testing::Scene;
using ui::testing::corpusCatalog;
using ui::testing::loadOrFail;
using ui::testing::ScriptedDispatcher;
using namespace Qt::StringLiterals;

namespace {

/// What a driver sees of one accessible node.
struct Seen {
    QAccessibleInterface* node = nullptr;
    QString name;
    QAccessible::Role role = QAccessible::NoRole;
    QString objectName;
    QString identifier;
};

// NOLINTNEXTLINE(misc-no-recursion): an accessibility tree, walked down.
void walk(QAccessibleInterface* node, std::vector<Seen>& out) {
    if (node == nullptr || !node->isValid()) {
        return;
    }
    QObject const* const object = node->object();
    out.push_back(Seen{.node = node,
                       .name = node->text(QAccessible::Name),
                       .role = node->role(),
                       .objectName = object != nullptr ? object->objectName() : QString{},
                       .identifier = node->text(QAccessible::Identifier)});
    for (int index = 0; index < node->childCount(); ++index) {
        walk(node->child(index), out);
    }
}

/// Every node of the window's accessibility tree, depth first, from the window's own interface.
std::vector<Seen> treeOf(QQuickWindow& window) {
    std::vector<Seen> out;
    walk(QAccessible::queryAccessibleInterface(&window), out);
    return out;
}

/// The nodes whose objectName, the generator's test id, is @p testId.
std::vector<Seen> byTestId(std::vector<Seen> const& tree, QString const& testId) {
    std::vector<Seen> found;
    std::ranges::copy_if(tree, std::back_inserter(found), [&](Seen const& seen) { return seen.objectName == testId; });
    return found;
}

/// The nodes named @p name in role @p role.
std::vector<Seen> byNameAndRole(std::vector<Seen> const& tree, QString const& name, QAccessible::Role role) {
    std::vector<Seen> found;
    std::ranges::copy_if(tree, std::back_inserter(found),
                         [&](Seen const& seen) { return seen.name == name && seen.role == role; });
    return found;
}

/// The nodes named @p name, in any role.
std::vector<Seen> byName(std::vector<Seen> const& tree, QString const& name) {
    std::vector<Seen> found;
    std::ranges::copy_if(tree, std::back_inserter(found), [&](Seen const& seen) { return seen.name == name; });
    return found;
}

/// Turns the accessibility machinery on for one test, as an assistive technology connecting does: Qt Quick Controls
/// fill in their accessible properties only while it is on.
class ActiveAccessibility {
public:
    ActiveAccessibility() : _was{QAccessible::isActive()} { QAccessible::setActive(true); }
    ~ActiveAccessibility() { QAccessible::setActive(_was); }
    ActiveAccessibility(ActiveAccessibility const&) = delete;
    ActiveAccessibility& operator=(ActiveAccessibility const&) = delete;
    ActiveAccessibility(ActiveAccessibility&&) = delete;
    ActiveAccessibility& operator=(ActiveAccessibility&&) = delete;

private:
    bool _was;
};

/// A generated screen on the Qt Quick backend in a shown window.
struct Mounted {
    ActiveAccessibility active;
    Scene scene;
    morph::reactive::testing::ManualScheduler scheduler;
    ui::Session session{scene.runtime(), ui::SessionOptions{.scheduler = &scheduler,
                                                            .clock = [this] { return scheduler.now(); },
                                                            .catalog = corpusCatalog()}};
    ScriptedDispatcher dispatcher{scene.owner()};
    ui::Interpreter interpreter{session, dispatcher};
    std::unique_ptr<ui::Screen> screen;

    explicit Mounted(std::string_view text) {
        auto mounted = interpreter.mount(loadOrFail(text, corpusCatalog()), {}, scene.backend());
        if (!mounted) {
            FAIL(mounted.error().message());
        }
        screen = std::move(*mounted);
        scene.settle();
    }

    [[nodiscard]] std::vector<Seen> tree() { return treeOf(scene.window()); }

    /// The one node with @p testId.
    [[nodiscard]] Seen node(QString const& testId) {
        std::vector<Seen> const found = byTestId(tree(), testId);
        INFO("nodes with test id " << testId.toStdString() << ": " << found.size());
        REQUIRE(found.size() == 1);
        return found.front();
    }
};

constexpr char const* kScreen = R"({
  "vocab": "ui/1", "id": "automation",
  "state": { "count": { "init": 0 } },
  "view": { "kind": "column", "testId": "root", "a11y": { "name": "Automation", "role": "group" }, "children": [
    { "kind": "text", "testId": "title", "text": "Accounts", "a11y": { "role": "heading" } },
    { "kind": "button", "testId": "add", "label": "Add",
      "onClick": [ { "set": "count", "value": { "fn": "add", "args": [ { "ref": "count" }, 1 ] } } ] },
    { "kind": "button", "testId": "close", "label": "X", "a11y": { "name": "Close the account" },
      "onClick": [ { "set": "count", "value": 0 } ] },
    { "kind": "textInput", "testId": "amount", "a11y": { "name": "Amount", "role": "edit" } },
    { "kind": "keyValue", "testId": "facts",
      "items": { "lit": [ { "label": "Balance", "value": "12.50" }, { "label": "Owner", "value": "Ada" } ] } },
    { "kind": "text", "testId": "status", "text": "Ready", "a11y": { "role": "status" } },
    { "kind": "text", "testId": "count", "text": { "fn": "format", "args": [ "{0}", { "ref": "count" } ] } } ] } })";

// One of each input kind, named by the document, except the check box, which its label names; and a drop-down in a
// role the document names.
constexpr char const* kInputs = R"({
  "vocab": "ui/1", "id": "inputs",
  "view": { "kind": "column", "testId": "inputs", "children": [
    { "kind": "textInput", "testId": "line", "a11y": { "name": "Line" } },
    { "kind": "textInput", "testId": "notes", "mode": "multiline", "a11y": { "name": "Notes" } },
    { "kind": "select", "testId": "drop", "options": { "lit": [ "a", "b" ] }, "a11y": { "name": "Drop" } },
    { "kind": "select", "testId": "radios", "style": "radio", "options": { "lit": [ "x", "y" ] },
      "a11y": { "name": "Radios" } },
    { "kind": "checkbox", "testId": "agree", "label": "I agree" },
    { "kind": "slider", "testId": "level", "min": 0, "max": 10, "a11y": { "name": "Level" } },
    { "kind": "dateTimeInput", "testId": "when", "a11y": { "name": "When" } },
    { "kind": "filePicker", "testId": "file", "a11y": { "name": "File" } },
    { "kind": "select", "testId": "pick", "options": { "lit": [ "c" ] }, "a11y": { "name": "Pick", "role": "list" } } ] } })";

/// An input as a driver looks it up: its accessible name, its role and its test id.
struct Input {
    QString name;
    QAccessible::Role role = QAccessible::NoRole;
    QString testId;
};

}  // namespace

// Mutation: drop `Accessible.role`'s binding from MorphWidget.qml (every node takes Qt's default role).
TEST_CASE("qt_quick automation: a generated screen exposes the document's names and roles", "[qt_quick][automation]") {
    Mounted m{kScreen};
    std::vector<Seen> const tree = m.tree();

    CHECK(byNameAndRole(tree, u"Automation"_s, QAccessible::Grouping).size() == 1);
    CHECK(byNameAndRole(tree, u"Accounts"_s, QAccessible::Heading).size() == 1);
    CHECK(byNameAndRole(tree, u"Close the account"_s, QAccessible::Button).size() == 1);
    CHECK(byNameAndRole(tree, u"Amount"_s, QAccessible::EditableText).size() == 1);
    CHECK(byNameAndRole(tree, u"Ready"_s, QAccessible::StatusBar).size() == 1);
    // A name the document gives replaces the kind's own text: the label is not a second name.
    CHECK(byName(tree, u"X"_s).empty());
}

// Mutation: give MorphButton no default role (the button reads as a plain item).
TEST_CASE("qt_quick automation: a click handler is a button, named by its label, with the press action",
          "[qt_quick][automation]") {
    Mounted m{kScreen};
    std::vector<Seen> const tree = m.tree();
    std::vector<Seen> const buttons = byNameAndRole(tree, u"Add"_s, QAccessible::Button);
    REQUIRE(buttons.size() == 1);
    CHECK(buttons.front().objectName == u"add"_s);
    // One node per button: the control inside the widget is not announced a second time.
    CHECK(byName(tree, u"Add"_s).size() == 1);

    QAccessibleActionInterface* const actions = buttons.front().node->actionInterface();
    REQUIRE(actions != nullptr);
    CHECK(actions->actionNames().contains(QAccessibleActionInterface::pressAction()));
    actions->doAction(QAccessibleActionInterface::pressAction());
    m.scene.settle();
    CHECK(m.node(u"count"_s).name == u"1"_s);
}

// Mutation: name MorphKeyValue's caption by its caption alone (no node reads "Balance: 12.50").
TEST_CASE("qt_quick automation: a read-only value is named \"<caption>: <value>\"", "[qt_quick][automation]") {
    Mounted m{kScreen};
    std::vector<Seen> const tree = m.tree();
    CHECK(byNameAndRole(tree, u"Balance: 12.50"_s, QAccessible::StaticText).size() == 1);
    CHECK(byNameAndRole(tree, u"Owner: Ada"_s, QAccessible::StaticText).size() == 1);
    // The value is part of its pair's name, not a node of its own.
    CHECK(byName(tree, u"12.50"_s).empty());
}

// Mutation: drop showTestId's objectName (no node carries the test id).
TEST_CASE("qt_quick automation: testId identifies the node", "[qt_quick][automation]") {
    Mounted m{kScreen};
    for (QString const& testId : {u"root"_s, u"title"_s, u"add"_s, u"close"_s, u"amount"_s, u"facts"_s, u"status"_s}) {
        INFO(testId.toStdString());
        Seen const seen = m.node(testId);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        // Where Qt has `Accessible.id`, the test id is also the automation id a platform driver reads.
        CHECK(seen.identifier == testId);
#endif
    }
    CHECK(m.node(u"title"_s).name == u"Accounts"_s);
    CHECK(m.node(u"close"_s).role == QAccessible::Button);
}

// Mutation: drop the name or the test id from one input's control, or the combo box's role binding (the document's
// role no longer reaches it), and that input's case fails; let an input's root present itself again, and its name
// names two nodes.
TEST_CASE("qt_quick automation: each input kind is one node, found by its name and role", "[qt_quick][automation]") {
    Mounted m{kInputs};
    std::vector<Seen> const tree = m.tree();
    for (Input const& input : {Input{.name = u"Line"_s, .role = QAccessible::EditableText, .testId = u"line"_s},
                               Input{.name = u"Notes"_s, .role = QAccessible::EditableText, .testId = u"notes"_s},
                               Input{.name = u"Drop"_s, .role = QAccessible::ComboBox, .testId = u"drop"_s},
                               Input{.name = u"Radios"_s, .role = QAccessible::Grouping, .testId = u"radios"_s},
                               Input{.name = u"I agree"_s, .role = QAccessible::CheckBox, .testId = u"agree"_s},
                               Input{.name = u"Level"_s, .role = QAccessible::Slider, .testId = u"level"_s},
                               Input{.name = u"When"_s, .role = QAccessible::EditableText, .testId = u"when"_s},
                               Input{.name = u"File"_s, .role = QAccessible::EditableText, .testId = u"file"_s},
                               Input{.name = u"Pick"_s, .role = QAccessible::List, .testId = u"pick"_s}}) {
        INFO(input.name.toStdString());
        std::vector<Seen> const found = byNameAndRole(tree, input.name, input.role);
        CHECK(found.size() == 1);
        // The name names nothing else: the widget's root is not a second node.
        CHECK(byName(tree, input.name).size() == 1);
        // The test id leads to the same node.
        std::vector<Seen> const identified = byTestId(tree, input.testId);
        CHECK(identified.size() == 1);
        if (found.size() == 1 && identified.size() == 1) {
            CHECK(identified.front().node == found.front().node);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
            CHECK(found.front().identifier == input.testId);
#endif
        }
    }
}

// Mutation: leave the text input's field without the widget's name (no node to focus and type into).
TEST_CASE("qt_quick automation: a driver focuses a text input by its name and role and types into it",
          "[qt_quick][automation]") {
    Mounted m{kInputs};
    std::vector<Seen> const found = byNameAndRole(m.tree(), u"Line"_s, QAccessible::EditableText);
    REQUIRE(found.size() == 1);
    QAccessibleActionInterface* const actions = found.front().node->actionInterface();
    REQUIRE(actions != nullptr);
    REQUIRE(actions->actionNames().contains(QAccessibleActionInterface::setFocusAction()));
    actions->doAction(QAccessibleActionInterface::setFocusAction());
    m.scene.settle();
    CHECK(found.front().node->state().focused);
    m.scene.type(u"abc"_s);
    CHECK(found.front().node->text(QAccessible::Value) == u"abc"_s);
}

// Mutation: leave the slider without its role (no node with a value to set).
TEST_CASE("qt_quick automation: a driver sets a slider found by its name and role", "[qt_quick][automation]") {
    Mounted m{kInputs};
    std::vector<Seen> const found = byNameAndRole(m.tree(), u"Level"_s, QAccessible::Slider);
    REQUIRE(found.size() == 1);
    QAccessibleValueInterface* const value = found.front().node->valueInterface();
    REQUIRE(value != nullptr);
    value->setCurrentValue(7);
    m.scene.settle();
    CHECK(value->currentValue().toInt() == 7);
}

// Mutation: leave the radio buttons without their role (no option to press).
TEST_CASE("qt_quick automation: a radio select's options are radio buttons named by their labels",
          "[qt_quick][automation]") {
    Mounted m{kInputs};
    std::vector<Seen> const tree = m.tree();
    std::vector<Seen> const option = byNameAndRole(tree, u"y"_s, QAccessible::RadioButton);
    REQUIRE(option.size() == 1);
    CHECK(byNameAndRole(tree, u"x"_s, QAccessible::RadioButton).size() == 1);
    // A drop-down's hidden radio buttons are no nodes.
    CHECK(byNameAndRole(tree, u"a"_s, QAccessible::RadioButton).empty());
    QAccessibleActionInterface const* const actions = option.front().node->actionInterface();
    REQUIRE(actions != nullptr);
    CHECK(actions->actionNames().contains(QAccessibleActionInterface::pressAction()));
}
