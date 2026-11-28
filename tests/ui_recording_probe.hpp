// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <morph/reactive/runtime.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/testing/backend_conformance.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "test_support.hpp"
#include "ui_echoing_backend.hpp"

namespace morph::testing::conformance_probe {

namespace ui = ::morph::ui;

// Undoes `detail::escape`: a backslash and `n` or `r` is a line ending, a backslash and anything else is that
// character.
inline std::string unescape(std::string_view text) {
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
inline std::vector<std::string> listItems(std::string_view list) {
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
inline ui::Key parseKey(std::string_view text) {
    if (text.starts_with('"') && text.size() >= 2) {
        return ui::Key{unescape(text.substr(1, text.size() - 2))};
    }
    return ui::Key{std::int64_t{std::stoll(std::string{text})}};
}

// The characters of runs as `formatSpans` records them: each run's text up to its first unescaped `;`, joined.
inline std::string spansText(std::string_view recorded) {
    std::string joined;
    std::string_view const inner = recorded.substr(1, recorded.size() - 2);
    bool flags = false;
    for (std::size_t i = 0; i < inner.size(); ++i) {
        char const character = inner.at(i);
        if (character == '\\' && i + 1 < inner.size()) {
            if (!flags) {
                joined += unescape(inner.substr(i, 2));
            }
            ++i;
        } else if (character == ',') {
            flags = false;
        } else if (character == ';') {
            flags = true;
        } else if (!flags) {
            joined += character;
        }
    }
    return joined;
}

// How a probe, or the backend behind it, breaks the contract on purpose. A suite that passes a correct backend
// measures something only if it fails a broken one.
enum class Fault : std::uint8_t {
    None,                // the reference probe
    NoChildren,          // a container reports its children but hands none of them out
    WrongText,           // every widget reads as showing other text
    DeadInput,           // user actions never reach the widgets
    EchoingSelect,       // the backend's Select reports a choice when its options are replaced
    NoModifiers,         // a click on a table row arrives without its modifiers, as a plain click
    NarrowBadge,         // a badge sized by a fixed or percent width is laid out narrower than its label
    OverlappedText,      // the text after a badge starts inside it
    TinyBounds,          // every widget is 5 units wide
    HugeBounds,          // every widget is 40 units wide
    HollowGrid,          // a grid reports no cells
    SecondMissing,       // a column hands out only its first child
    HollowNarrow,        // a row whose first child is sized by a fixed or percent width reports no children
    HollowNatural,       // a row whose first child is sized by its content reports no children
    NarrowStretch,       // a button sized by a stretch width is laid out narrower than its label
    PlainHeaderClicks,   // every header click is reported as Plain
    FirstSortKeyOnly,    // a header's mark and description answer for the chain's first key only
    MenuColumnOffByOne,  // a header menu's choice is made on the next column's menu
    MoveTargetIsSource,  // a column move reports the column's own place
    UnnamedFilter,       // a filtered header is named by its bare label
    SilentClipboard,     // copyText reports success and the clipboard reads empty
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

    // Only the narrow-badge fault reads geometry: a badge told a fixed or percent width is 5 units wide against 11 for
    // its content, and the text after it starts at 2, inside it.
    [[nodiscard]] std::optional<ui::testing::WidgetBounds> boundsOf(ui::Widget const& widget) override {
        if (_fault == Fault::TinyBounds || _fault == Fault::HugeBounds) {
            return ui::testing::WidgetBounds{
                .x = 0, .y = 0, .width = _fault == Fault::TinyBounds ? 5.0 : 40.0, .height = 1};
        }
        if (_fault == Fault::NarrowStretch) {
            int const buttonId = _backend.idOf(widget);
            bool const narrow =
                _backend.kindOf(buttonId) == "Button" && _backend.prop(buttonId, "layout").starts_with("stretch");
            return ui::testing::WidgetBounds{.x = 0, .y = 0, .width = narrow ? 5.0 : 20.0, .height = 1};
        }
        if (_fault != Fault::NarrowBadge && _fault != Fault::OverlappedText) {
            return std::nullopt;
        }
        int const widgetId = _backend.idOf(widget);
        std::string const layout = _backend.prop(widgetId, "layout");
        bool const sized =
            _fault == Fault::NarrowBadge && (layout.starts_with("fixed") || layout.starts_with("percent"));
        if (_backend.kindOf(widgetId) == "Badge") {
            return ui::testing::WidgetBounds{.x = 0, .y = 0, .width = sized ? 5.0 : 11.0, .height = 1};
        }
        return ui::testing::WidgetBounds{.x = 2, .y = 0, .width = 20, .height = 1};
    }

    [[nodiscard]] std::string textOf(ui::Widget const& widget) override {
        if (_fault == Fault::WrongText) {
            return "?";
        }
        int const widgetId = _backend.idOf(widget);
        std::string const kind = _backend.kindOf(widgetId);
        if (kind == "Select") {
            return markedLabel(widgetId);
        }
        if (std::string const spans = _backend.prop(widgetId, "spans"); !spans.empty()) {
            return spansText(spans);
        }
        if (kind == "Tabs") {
            return highlightedLabel(widgetId);
        }
        if (kind == "Chart") {
            return unescape(_backend.prop(widgetId, "summary"));
        }
        if (kind == "Steps") {
            return currentStepLabel(widgetId);
        }
        return unescape(
            _backend.prop(widgetId, kind == "Button" || kind == "Checkbox" || kind == "Menu" ? "label" : "text"));
    }
    // A field's messages, then its description, as assistive technology would read them.
    [[nodiscard]] std::optional<std::string> descriptionOf(ui::Widget const& widget) override {
        int const widgetId = _backend.idOf(widget);
        // A field's messages, then its description; an overlay has only its description.
        std::vector<std::string> lines;
        for (std::string const& error : listItems(_backend.prop(widgetId, "errors"))) {
            lines.push_back(unescape(error));
        }
        if (std::string const description = unescape(_backend.prop(widgetId, "description")); !description.empty()) {
            lines.push_back(description);
        }
        std::string joined;
        for (std::string const& line : lines) {
            joined += (joined.empty() ? "" : "\n") + line;
        }
        return joined;
    }
    [[nodiscard]] bool visibleOf(ui::Widget const& widget) override {
        return _backend.prop(_backend.idOf(widget), "visible") != "false";
    }
    [[nodiscard]] bool enabledOf(ui::Widget const& widget) override {
        return _backend.prop(_backend.idOf(widget), "enabled") != "false";
    }
    [[nodiscard]] std::size_t childCount(ui::ContainerWidget const& container) override {
        int const id = _backend.idOf(container);
        if (_fault == Fault::HollowGrid && _backend.kindOf(id) == "Grid") {
            return 0;
        }
        if ((_fault == Fault::HollowNarrow || _fault == Fault::HollowNatural) && _backend.kindOf(id) == "Row" &&
            !_backend.children(id).empty()) {
            std::string const layout = _backend.prop(_backend.children(id).front(), "layout");
            bool const content = layout.empty() || layout.starts_with("content");
            if (content == (_fault == Fault::HollowNatural)) {
                return 0;
            }
        }
        return _backend.children(id).size();
    }
    [[nodiscard]] ui::Widget* childAt(ui::ContainerWidget const& container, std::size_t index) override {
        if (_fault == Fault::NoChildren) {
            return nullptr;
        }
        auto const children = _backend.children(_backend.idOf(container));
        if (_fault == Fault::SecondMissing && index == 1 && _backend.kindOf(_backend.idOf(container)) == "Column") {
            return nullptr;
        }
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
    void clickRow(ui::Widget& table, std::size_t row, ui::RowClick click) override {
        if (_fault == Fault::DeadInput) {
            return;
        }
        int const tableId = _backend.idOf(table);
        ui::Key const key = parseKey(_backend.prop(_backend.children(tableId).at(row), "rowKey"));
        _backend.clickRow(tableId, key, _fault == Fault::NoModifiers ? ui::RowClick::Only : click);
    }
    void clickHeader(ui::Widget& table, std::size_t column, ui::HeaderClick click) override {
        if (_fault != Fault::DeadInput) {
            _backend.clickHeader(_backend.idOf(table), column,
                                 _fault == Fault::PlainHeaderClicks ? ui::HeaderClick::Plain : click);
        }
    }
    [[nodiscard]] std::vector<ui::MenuEntry> headerMenu(ui::Widget& table,
                                                        std::optional<std::size_t> column) override {
        return _fault == Fault::DeadInput ? std::vector<ui::MenuEntry>{}
                                          : _backend.menuOf(_backend.idOf(table), column);
    }
    // The path's labels, from the top, as a user reads them on the way to the entry.
    void chooseHeaderMenuEntry(ui::Widget& table, std::optional<std::size_t> column,
                               std::vector<std::size_t> const& path) override {
        if (_fault == Fault::DeadInput) {
            return;
        }
        int const tableId = _backend.idOf(table);
        std::optional<std::size_t> const chosen =
            column && _fault == Fault::MenuColumnOffByOne ? std::optional<std::size_t>{*column + 1} : column;
        std::vector<ui::MenuEntry> const entries = _backend.menuOf(tableId, chosen);
        std::vector<ui::MenuEntry> const* level = &entries;
        std::vector<std::string_view> labels;
        for (std::size_t const index : path) {
            if (index >= level->size()) {
                return;
            }
            labels.emplace_back(level->at(index).label);
            level = &level->at(index).items;
        }
        _backend.chooseMenu(tableId, chosen, labels);
    }
    void moveColumn(ui::Widget& table, std::size_t column, std::size_t position) override {
        if (_fault != Fault::DeadInput) {
            _backend.moveColumn(_backend.idOf(table), column, _fault == Fault::MoveTargetIsSource ? column : position);
        }
    }
    [[nodiscard]] std::string headerText(ui::Widget const& table, std::size_t column) override {
        return _fault == Fault::FirstSortKeyOnly ? firstKeyOnly(table, column, true)
                                                 : _backend.headerText(_backend.idOf(table), column);
    }
    [[nodiscard]] std::string headerName(ui::Widget const& table, std::size_t column) override {
        if (_fault == Fault::UnnamedFilter) {
            return _backend.tableColumns(_backend.idOf(table)).at(column).label;
        }
        return _backend.headerName(_backend.idOf(table), column);
    }
    [[nodiscard]] std::string headerDescription(ui::Widget const& table, std::size_t column) override {
        return _fault == Fault::FirstSortKeyOnly ? firstKeyOnly(table, column, false)
                                                 : _backend.headerDescription(_backend.idOf(table), column);
    }
    void pressFilter(ui::Widget& table, std::size_t column) override {
        if (_fault != Fault::DeadInput) {
            _backend.clickFilter(_backend.idOf(table), column);
        }
    }
    void pressGroupFilter(ui::Widget& table, std::size_t group) override {
        if (_fault != Fault::DeadInput) {
            _backend.clickGroupFilter(_backend.idOf(table), group);
        }
    }
    [[nodiscard]] std::string clipboardText() override {
        return _fault == Fault::SilentClipboard ? std::string{} : _backend.clipboardText();
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
    [[nodiscard]] ui::Widget const* targetOf(ui::SpotlightWidget const& spotlight) override {
        std::string const ringed = _backend.prop(_backend.idOf(spotlight), "ringed");
        int widgetId = 0;
        char const* const first = ringed.data();
        static_cast<void>(std::from_chars(first, std::next(first, std::ssize(ringed)), widgetId));
        return widgetId == 0 || !_backend.exists(widgetId) ? nullptr : _backend.widget(widgetId);
    }

private:
    // A header's text or description as a backend that draws only the chain's first key shows it: the first key of
    // the recorded `sort`, as a chain of one.
    [[nodiscard]] std::string firstKeyOnly(ui::Widget const& table, std::size_t column, bool text) {
        int const tableId = _backend.idOf(table);
        std::string const sort = _backend.prop(tableId, "sort");
        std::string const label = _backend.tableColumns(tableId).at(column).label;
        std::size_t const colon = sort.find(':');
        if (sort == "none" || colon == std::string::npos || std::stoul(sort.substr(0, colon)) != column) {
            return text ? label : std::string{};
        }
        auto const direction = sort.compare(colon + 1, 9, "Ascending") == 0 ? ui::SortDirection::Ascending
                                                                            : ui::SortDirection::Descending;
        return text ? label + ui::sortMarkText(direction, 1, 1) : ui::sortDescription(direction, 1, 1);
    }

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

    // The label of the tab `selected` highlights, from `tabs`' elements; empty when it highlights none.
    [[nodiscard]] std::string highlightedLabel(int tabsId) const {
        std::string const selected = _backend.prop(tabsId, "selected");
        std::size_t index = 0;
        char const* const first = selected.data();
        if (std::from_chars(first, std::next(first, std::ssize(selected)), index).ec != std::errc{}) {
            return {};
        }
        std::vector<std::string> const labels = listItems(_backend.prop(tabsId, "tabs"));
        return index < labels.size() ? unescape(labels.at(index)) : std::string{};
    }

    // The label of a steps indicator's current step: the entries read `label:State`.
    [[nodiscard]] std::string currentStepLabel(int stepsId) const {
        std::string const current = _backend.prop(stepsId, "current");
        std::size_t index = 0;
        char const* const first = current.data();
        if (std::from_chars(first, std::next(first, std::ssize(current)), index).ec != std::errc{}) {
            return {};
        }
        std::vector<std::string> const entries = listItems(_backend.prop(stepsId, "steps"));
        if (index >= entries.size()) {
            return {};
        }
        std::string const& entry = entries.at(index);
        return unescape(entry.substr(0, entry.rfind(':')));
    }

    Fault _fault;
    morph::testing::StepExecutor _owner;
    morph::testing::EchoingBackend _echoing;
    ui::testing::RecordingBackend& _backend = _echoing.recording();
    morph::reactive::Runtime _rt{_owner};
};

}  // namespace morph::testing::conformance_probe
