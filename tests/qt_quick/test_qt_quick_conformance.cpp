// SPDX-License-Identifier: Apache-2.0
//
// The backend conformance suite (docs/spec/ui/backend_contract.md, "The conformance suite") run against the Qt Quick
// backend, on the real engine. The probe drives it as a user would where Qt Quick gives a way to: mouse clicks,
// presses, moves and releases sent to the window at a widget's place, and keys sent to the item with focus. A menu
// entry and a file drop go through the component's own entry point, which the menu's items and the drop area call.

#include <QPointF>
#include <QQuickItem>
#include <QTest>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <morph/qt_quick/backend.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/testing/backend_conformance.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "qt_quick/keys.hpp"
#include "qt_quick/kinds.hpp"
#include "qt_quick_harness.hpp"

namespace ui = morph::ui;
namespace detail = morph::qt_quick::detail;
using morph::qt_quick::testing::Scene;

namespace {

// How a probe breaks the contract on purpose. A suite that passes the Qt Quick backend measures something only if it
// fails a probe that drops the input or misreads what the widgets show.
enum class Fault : std::uint8_t {
    None,       // the Qt Quick probe
    DeadInput,  // no input reaches the window
    WrongText,  // every widget reads as showing other text
};

class QtProbe final : public ui::testing::ConformanceProbe {
public:
    explicit QtProbe(Fault fault = Fault::None) : _fault{fault} {}

    ui::IViewBackend& backend() override { return _scene.backend(); }
    morph::reactive::Runtime& runtime() override { return _scene.runtime(); }
    void settle() override { _scene.settle(); }

    std::string textOf(ui::Widget const& widget) override {
        settle();
        if (_fault == Fault::WrongText) {
            return "something else";
        }
        return base(widget).shownText();
    }

    bool visibleOf(ui::Widget const& widget) override { return base(widget).ownVisible(); }
    bool enabledOf(ui::Widget const& widget) override { return base(widget).ownEnabled(); }

    std::size_t childCount(ui::ContainerWidget const& container) override { return core(container).children().size(); }

    ui::Widget* childAt(ui::ContainerWidget const& container, std::size_t index) override {
        auto const& children = core(container).children();
        return index < children.size() ? dynamic_cast<ui::Widget*>(children.at(index)) : nullptr;
    }

    void click(ui::Widget& widget) override {
        settle();
        QQuickItem const* target = Backend::itemOf(widget);
        if (auto* const part =
                target != nullptr ? target->findChild<QQuickItem*>(QStringLiteral("action")) : nullptr) {
            target = part;
        }
        mouseClick(target);
    }

    void type(ui::Widget& widget, std::string_view text) override {
        settle();
        detail::ItemBase const& field = base(widget);
        QQuickItem const* const control = field.focusTarget();
        if (control == nullptr || !control->isVisible() || _fault == Fault::DeadInput) {
            return;
        }
        mouseClick(control);
        _scene.key(Qt::Key_A, Qt::ControlModifier);
        if (text.empty()) {
            _scene.key(Qt::Key_Delete);
        } else {
            _scene.enter(morph::qt_quick::detail::toQString(text));
        }
    }

    void drag(ui::Widget& source, ui::Widget& target) override {
        settle();
        QQuickItem const* const from = Backend::itemOf(source);
        QQuickItem const* const to = Backend::itemOf(target);
        if (from == nullptr || to == nullptr || !from->isVisible() || _fault == Fault::DeadInput) {
            return;
        }
        QPoint const start = Scene::centreOf(*from);
        QPoint const end = Scene::centreOf(*to);
        QTest::mousePress(&_scene.window(), Qt::LeftButton, {}, start);
        constexpr int steps = 10;
        for (int step = 1; step <= steps; ++step) {
            QPoint const at = start + ((end - start) * step / steps);
            QTest::mouseMove(&_scene.window(), at);
        }
        QTest::mouseRelease(&_scene.window(), Qt::LeftButton, {}, end);
        settle();
    }

    void dismiss(ui::Widget& widget) override {
        settle();
        if (_fault == Fault::DeadInput) {
            return;
        }
        if (auto* const banner = dynamic_cast<detail::BannerImpl*>(&widget)) {
            QQuickItem const* const item = banner->item();
            mouseClick(item != nullptr ? item->findChild<QQuickItem*>(QStringLiteral("dismiss")) : nullptr);
            return;
        }
        _scene.key(Qt::Key_Escape);
    }

    void selectRows(ui::Widget& table, std::vector<std::size_t> const& rows) override {
        settle();
        auto const& children = core(dynamic_cast<ui::ContainerWidget&>(table)).children();
        bool first = true;
        for (std::size_t const row : rows) {
            if (row < children.size()) {
                Qt::KeyboardModifiers const modifiers = first ? Qt::NoModifier : Qt::ControlModifier;
                mouseClick(children.at(row)->item(), modifiers);
            }
            first = false;
        }
    }

    void clickRow(ui::Widget& table, std::size_t row, ui::RowClick click) override {
        settle();
        auto const& children = core(dynamic_cast<ui::ContainerWidget&>(table)).children();
        if (row >= children.size()) {
            return;
        }
        Qt::KeyboardModifiers modifiers = Qt::NoModifier;
        if (click == ui::RowClick::Toggle) {
            modifiers = Qt::ControlModifier;
        } else if (click == ui::RowClick::Range) {
            modifiers = Qt::ShiftModifier;
        }
        mouseClick(children.at(row)->item(), modifiers);
    }

    // A mouse click on the column's header label.
    void clickHeader(ui::Widget& table, std::size_t column) override {
        settle();
        // The labels are a Repeater's delegates: items under the table's item, not QObject children of it.
        QString const name = QStringLiteral("header%1").arg(column);
        QList<QQuickItem const*> pending{Backend::itemOf(table)};
        while (!pending.isEmpty()) {
            QQuickItem const* const item = pending.takeLast();
            if (item->objectName() == name) {
                mouseClick(item, Qt::NoModifier);
                return;
            }
            for (QQuickItem const* const child : item->childItems()) {
                pending.append(child);
            }
        }
    }

    std::vector<std::size_t> selectedRows(ui::Widget const& table) override {
        settle();
        std::vector<std::size_t> rows;
        auto const& children = core(dynamic_cast<ui::ContainerWidget const&>(table)).children();
        for (std::size_t index = 0; index < children.size(); ++index) {
            if (detail::TableImpl::marked(*children.at(index))) {
                rows.push_back(index);
            }
        }
        return rows;
    }

    void moveCursor(ui::Widget& field, std::size_t position) override {
        settle();
        auto* const input = dynamic_cast<detail::TextInputImpl*>(&field);
        if (input == nullptr || _fault == Fault::DeadInput) {
            return;
        }
        // Home, then one Right per character up to the byte offset, as the arrow keys move it.
        input->takeFocus();
        _scene.standardKey(QKeySequence::MoveToStartOfLine);
        QString const text = morph::qt_quick::detail::toQString(input->shownText());
        qsizetype characters = 0;
        while (characters < text.size() && std::cmp_less_equal(text.left(characters + 1).toUtf8().size(), position)) {
            ++characters;
        }
        for (qsizetype step = 0; step < characters; ++step) {
            _scene.key(Qt::Key_Right);
        }
    }

    std::size_t cursorOf(ui::Widget const& field) override {
        auto const* const input = dynamic_cast<detail::TextInputImpl const*>(&field);
        return input != nullptr ? input->cursor() : 0;
    }

    void press(ui::Widget& focused, std::string_view chord) override {
        settle();
        if (_fault == Fault::DeadInput) {
            return;
        }
        base(focused).takeFocus();
        settle();
        std::optional<QKeyCombination> const key = detail::chordKey(chord);
        REQUIRE(key.has_value());
        _scene.key(key->key(), key->keyboardModifiers());
    }

    bool hasFocus(ui::Widget const& widget) override {
        settle();
        QQuickItem const* const target = base(widget).focusTarget();
        return target != nullptr && target->hasActiveFocus();
    }

    void chooseMenuEntry(ui::Widget& menu, std::vector<std::size_t> const& path) override {
        settle();
        auto const* const impl = dynamic_cast<detail::MenuImpl const*>(&menu);
        if (impl != nullptr && impl->reachable() && _fault != Fault::DeadInput) {
            impl->choose(path);
        }
        settle();
    }

    void expand(ui::Widget& section, bool open) override {
        settle();
        detail::ItemBase const& collapsible = base(section);
        if (collapsible.read("open").toBool() == open) {
            return;
        }
        QQuickItem const* const item = collapsible.item();
        mouseClick(item != nullptr ? item->findChild<QQuickItem*>(QStringLiteral("toggle")) : nullptr);
    }

    void dropFiles(ui::Widget& zone, std::vector<std::string> const& paths) override {
        settle();
        auto const* const impl = dynamic_cast<detail::DropZoneImpl const*>(&zone);
        if (impl != nullptr && _fault != Fault::DeadInput) {
            static_cast<void>(impl->offer(paths));
        }
        settle();
    }

    // In units: pixels over the theme's unit width and height.
    std::optional<ui::testing::WidgetBounds> boundsOf(ui::Widget const& widget) override {
        settle();
        QQuickItem const* const item = base(widget).item();
        auto* const theme =
            _scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
        if (item == nullptr || theme == nullptr) {
            return std::nullopt;
        }
        qreal const unitWidth = theme->property("unitWidth").toReal();
        qreal const unitHeight = theme->property("unitHeight").toReal();
        QPointF const corner = item->mapToScene(QPointF{0, 0});
        return ui::testing::WidgetBounds{.x = corner.x() / unitWidth,
                                         .y = corner.y() / unitHeight,
                                         .width = item->width() / unitWidth,
                                         .height = item->height() / unitHeight};
    }

private:
    using Backend = morph::qt_quick::Backend;

    static detail::ItemBase const& base(ui::Widget const& widget) {
        detail::ItemBase const* const found = detail::baseOf(widget);
        REQUIRE(found != nullptr);
        return *found;
    }

    static detail::ContainerCore const& core(ui::ContainerWidget const& container) {
        auto const* const found = dynamic_cast<detail::ContainerCore const*>(&container);
        REQUIRE(found != nullptr);
        return *found;
    }

    // A real click, at the item's centre; an item the user cannot see takes none.
    void mouseClick(QQuickItem const* item, Qt::KeyboardModifiers modifiers = {}) {
        if (item == nullptr || !item->isVisible() || _fault == Fault::DeadInput) {
            return;
        }
        _scene.click(*item, modifiers);
    }

    Fault _fault;
    Scene _scene;
};

std::set<std::string_view> failingCases(Fault fault) {
    std::set<std::string_view> failing;
    for (auto const& testCase : ui::testing::conformanceCases()) {
        QtProbe probe{fault};
        if (testCase.run(probe).has_value()) {
            failing.insert(testCase.name);
        }
    }
    return failing;
}

}  // namespace

TEST_CASE("qt_quick conformance: the Qt Quick backend passes every case", "[qt_quick][conformance]") {
    for (auto const& testCase : ui::testing::conformanceCases()) {
        QtProbe probe;
        std::optional<std::string> const failure = testCase.run(probe);
        INFO(std::string{testCase.name} + ": " + failure.value_or("passed"));
        CHECK_FALSE(failure.has_value());
    }
}

TEST_CASE("qt_quick conformance: the Qt Quick backend passes every layout case", "[qt_quick][conformance]") {
    for (auto const& testCase : ui::testing::layoutConformanceCases()) {
        QtProbe probe;
        std::optional<std::string> const failure = testCase.run(probe);
        INFO(std::string{testCase.name} + ": " + failure.value_or("passed"));
        CHECK_FALSE(failure.has_value());
    }
}

// A probe that delivers no input, or misreads every text, must fail the cases that check input and text: otherwise
// the passes above say nothing about the Qt Quick backend.
TEST_CASE("qt_quick conformance: the suite flags a probe that drops input or misreads text",
          "[qt_quick][conformance]") {
    std::set<std::string_view> const deadInput = failingCases(Fault::DeadInput);
    for (std::string_view const name :
         {"a click runs the button's action once",
          "typing reaches onChange and a value set by the application is not echoed",
          "a drag onto an accepting target delivers the key",
          "a Table reports a user's selection as exactly the existing rows selected",
          "a chord goes to the innermost widget that declares it",
          "a menu reports only an enabled entry without a submenu", "a dismissed banner reports it and stays shown",
          "a closed collapsible's content takes no input, its header does",
          "a drop zone takes a drop whole or refuses it whole"}) {
        INFO(name);
        CHECK(deadInput.contains(name));
    }
    std::set<std::string_view> const wrongText = failingCases(Fault::WrongText);
    for (std::string_view const name :
         {"a Text shows its constant text", "a bound Text updates once per batch and not for an equal write"}) {
        INFO(name);
        CHECK(wrongText.contains(name));
    }
}
