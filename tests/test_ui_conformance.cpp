// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <morph/reactive/runtime.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/testing/backend_conformance.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_support.hpp"
#include "ui_echoing_backend.hpp"

namespace ui = morph::ui;

namespace {

// Undoes `detail::escape`: a backslash and `n` or `r` is a line ending, a backslash and anything else is that
// character.
std::string unescape(std::string_view text) {
    std::string plain;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char const character = text.at(i);
        if (character != '\\' || i + 1 == text.size()) {
            plain += character;
            continue;
        }
        switch (char const escaped = text.at(++i)) {
            case 'n':
                plain += '\n';
                break;
            case 'r':
                plain += '\r';
                break;
            default:
                plain += escaped;
                break;
        }
    }
    return plain;
}

// The elements of a `detail::formatList` list, still escaped: split at the commas no backslash escapes.
std::vector<std::string> listItems(std::string_view list) {
    std::vector<std::string> items;
    if (list.size() < 2 || list == "[]") {
        return items;
    }
    std::string item;
    for (std::size_t i = 1; i + 1 < list.size(); ++i) {
        char const character = list.at(i);
        if (character == '\\' && i + 2 < list.size()) {
            item += character;
            item += list.at(++i);
        } else if (character == ',') {
            items.push_back(std::move(item));
            item.clear();
        } else {
            item += character;
        }
    }
    items.push_back(std::move(item));
    return items;
}

// Undoes `detail::formatKey`.
ui::Key parseKey(std::string_view text) {
    if (text.starts_with('"') && text.size() >= 2) {
        return ui::Key{unescape(text.substr(1, text.size() - 2))};
    }
    return ui::Key{std::int64_t{std::stoll(std::string{text})}};
}

// How a probe, or the backend behind it, breaks the contract on purpose. A suite that passes a correct backend
// measures something only if it fails a broken one.
enum class Fault : std::uint8_t {
    None,           // the reference probe
    NoChildren,     // a container reports its children but hands none of them out
    WrongText,      // every widget reads as showing other text
    DeadInput,      // user actions never reach the widgets
    EchoingSelect,  // the backend's Select reports a choice when its options are replaced
};

// The reference probe: what a TUI or Qt Quick probe does with a rendered screen or a QQuickItem tree, this one does
// with the recording backend's records. Given a fault, it is a broken backend the suite must flag.
class RecordingProbe final : public ui::testing::ConformanceProbe {
public:
    explicit RecordingProbe(Fault fault = Fault::None) : _fault{fault} {}

    ui::IViewBackend& backend() override {
        return _fault == Fault::EchoingSelect ? static_cast<ui::IViewBackend&>(_echoing) : _backend;
    }
    morph::reactive::Runtime& runtime() override { return _rt; }
    void settle() override { _owner.runAll(); }

    [[nodiscard]] std::string textOf(ui::Widget const& widget) override {
        if (_fault == Fault::WrongText) {
            return "?";
        }
        int const widgetId = _backend.idOf(widget);
        std::string const kind = _backend.kindOf(widgetId);
        if (kind == "Select") {
            return markedLabel(widgetId);
        }
        return unescape(_backend.prop(widgetId, kind == "Button" || kind == "Checkbox" ? "label" : "text"));
    }
    [[nodiscard]] bool visibleOf(ui::Widget const& widget) override {
        return _backend.prop(_backend.idOf(widget), "visible") != "false";
    }
    [[nodiscard]] bool enabledOf(ui::Widget const& widget) override {
        return _backend.prop(_backend.idOf(widget), "enabled") != "false";
    }
    [[nodiscard]] std::size_t childCount(ui::ContainerWidget const& container) override {
        return _backend.children(_backend.idOf(container)).size();
    }
    [[nodiscard]] ui::Widget* childAt(ui::ContainerWidget const& container, std::size_t index) override {
        if (_fault == Fault::NoChildren) {
            return nullptr;
        }
        auto const children = _backend.children(_backend.idOf(container));
        return index < children.size() ? _backend.widget(children.at(index)) : nullptr;
    }
    void click(ui::Widget& widget) override {
        if (_fault != Fault::DeadInput) {
            _backend.click(_backend.idOf(widget));
        }
    }
    void type(ui::Widget& widget, std::string_view text) override {
        if (_fault != Fault::DeadInput) {
            _backend.edit(_backend.idOf(widget), std::string{text});
        }
    }
    void drag(ui::Widget& source, ui::Widget& target) override {
        if (_fault != Fault::DeadInput) {
            static_cast<void>(_backend.drag(_backend.idOf(source), _backend.idOf(target)));
        }
    }
    void dismiss(ui::Widget& dialog) override {
        if (_fault != Fault::DeadInput) {
            _backend.dismiss(_backend.idOf(dialog));
        }
    }
    void moveCursor(ui::Widget& field, std::size_t position) override {
        _backend.moveCursor(_backend.idOf(field), position);
    }
    [[nodiscard]] std::size_t cursorOf(ui::Widget const& field) override {
        return _backend.cursor(_backend.idOf(field));
    }
    void selectRows(ui::Widget& table, std::vector<std::size_t> const& rows) override {
        if (_fault == Fault::DeadInput) {
            return;
        }
        int const tableId = _backend.idOf(table);
        std::vector<int> const children = _backend.children(tableId);
        std::vector<ui::Key> keys;
        keys.reserve(rows.size());
        for (std::size_t const row : rows) {
            keys.push_back(parseKey(_backend.prop(children.at(row), "rowKey")));
        }
        _backend.selectRows(tableId, std::move(keys));
    }
    [[nodiscard]] std::vector<std::size_t> selectedRows(ui::Widget const& table) override {
        int const tableId = _backend.idOf(table);
        std::vector<std::string> const marked = listItems(_backend.prop(tableId, "selection"));
        std::vector<int> const children = _backend.children(tableId);
        std::vector<std::size_t> rows;
        for (std::size_t i = 0; i < children.size(); ++i) {
            if (std::ranges::find(marked, _backend.prop(children.at(i), "rowKey")) != marked.end()) {
                rows.push_back(i);
            }
        }
        return rows;
    }
    void press(ui::Widget& focused, std::string_view chord) override {
        if (_fault != Fault::DeadInput) {
            static_cast<void>(_backend.press(_backend.idOf(focused), std::string{chord}));
        }
    }
    [[nodiscard]] bool hasFocus(ui::Widget const& widget) override {
        return _backend.focused() == _backend.idOf(widget);
    }
    void chooseMenuEntry(ui::Widget& menu, std::vector<std::size_t> const& path) override {
        if (_fault != Fault::DeadInput) {
            _backend.chooseMenuEntry(_backend.idOf(menu), path);
        }
    }
    void expand(ui::Widget& section, bool open) override {
        if (_fault != Fault::DeadInput) {
            _backend.expand(_backend.idOf(section), open);
        }
    }
    void dropFiles(ui::Widget& zone, std::vector<std::string> const& paths) override {
        if (_fault != Fault::DeadInput) {
            static_cast<void>(_backend.dropFiles(_backend.idOf(zone), paths));
        }
    }

private:
    // The label of the option whose key `selected` shows, from `options`' `key:label` elements.
    [[nodiscard]] std::string markedLabel(int selectId) const {
        std::string const selected = _backend.prop(selectId, "selected");
        if (selected.empty() || selected == "none") {
            return {};
        }
        for (std::string const& option : listItems(_backend.prop(selectId, "options"))) {
            if (option.starts_with(selected + ":")) {
                return unescape(std::string_view{option}.substr(selected.size() + 1));
            }
        }
        return {};
    }

    Fault _fault;
    morph::testing::StepExecutor _owner;
    morph::testing::EchoingBackend _echoing;
    ui::testing::RecordingBackend& _backend = _echoing.recording();
    morph::reactive::Runtime _rt{_owner};
};

// The names of the cases a probe with @p fault fails.
std::set<std::string_view> failingCases(Fault fault) {
    std::set<std::string_view> failing;
    for (auto const& testCase : ui::testing::conformanceCases()) {
        RecordingProbe probe{fault};
        if (testCase.run(probe).has_value()) {
            failing.insert(testCase.name);
        }
    }
    return failing;
}

}  // namespace

TEST_CASE("ui conformance: RecordingBackend passes every case", "[ui][conformance]") {
    auto const cases = ui::testing::conformanceCases();
    REQUIRE(cases.size() == 34);
    std::set<std::string_view> names;
    for (auto const& testCase : cases) {
        CHECK(names.insert(testCase.name).second);
        RecordingProbe probe;
        std::optional<std::string> const failure = testCase.run(probe);
        INFO(std::string{testCase.name} + ": " + failure.value_or("passed"));
        CHECK_FALSE(failure.has_value());
    }
}

// Each fault breaks one part of the contract, and the cases that check that part must say so. Mutation: make
// `detail::Checks::that` or `detail::Checks::text` record nothing (every fault then passes everything).
TEST_CASE("ui conformance: the suite flags a backend that breaks the contract", "[ui][conformance]") {
    std::set<std::string_view> const noChildren = failingCases(Fault::NoChildren);
    INFO("no children: " + std::to_string(noChildren.size()) + " cases failed");
    for (std::string_view const name :
         {"Tabs mount a page on first selection and show only the selected one",
          "a ForEach updates a kept row in place", "a ForEach follows inserts, removals and reorders",
          "a moved widget keeps its native state", "a Dialog holds its content only while open",
          "a dismissal the document refuses leaves the dialog open",
          "a widget inside a hidden, disabled or collapsed container takes no input",
          "a hidden Table cell keeps its column"}) {
        INFO(name);
        CHECK(noChildren.contains(name));
    }

    std::set<std::string_view> const wrongText = failingCases(Fault::WrongText);
    for (std::string_view const name :
         {"a Text shows its constant text", "a bound Text updates once per batch and not for an equal write",
          "a Switch shows the selected case and nothing for a key without one",
          "a text field shows what the document makes of an edit"}) {
        INFO(name);
        CHECK(wrongText.contains(name));
    }

    std::set<std::string_view> const deadInput = failingCases(Fault::DeadInput);
    for (std::string_view const name :
         {"a click runs the button's action once",
          "typing reaches onChange and a value set by the application is not echoed",
          "a drag onto an accepting target delivers the key",
          "a Table reports a user's selection as exactly the existing rows selected",
          "a dismissal the document refuses leaves the dialog open",
          "a chord goes to the innermost widget that declares it",
          "a chord pressed inside a hidden container runs nothing",
          "a menu reports only an enabled entry without a submenu", "a read-only input takes no edit",
          "a dismissed banner reports it and stays shown",
          "a closed collapsible's content takes no input, its header does",
          "a drop zone takes a drop whole or refuses it whole"}) {
        INFO(name);
        CHECK(deadInput.contains(name));
    }

    // Only the setter case is run against the echoing backend: the probe reads widgets through the recording
    // backend's ids, which the echoing Select's wrapper does not have.
    auto const cases = ui::testing::conformanceCases();
    auto const setters =
        std::ranges::find(cases, std::string_view{"no setter calls a handler"}, &ui::testing::ConformanceCase::name);
    REQUIRE(setters != cases.end());
    RecordingProbe echoing{Fault::EchoingSelect};
    std::optional<std::string> const failure = setters->run(echoing);
    REQUIRE(failure.has_value());
    CHECK(failure->contains("Select onSelect"));
}

TEST_CASE("ui conformance: a failure message names keys of either kind", "[ui][conformance]") {
    CHECK(ui::testing::detail::keyTexts({ui::Key{std::int64_t{7}}, ui::Key{std::string{"b"}}}) == "7,\"b\"");
}
