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

// The reference probe: what a TUI or Qt Quick probe does with a rendered screen or a QQuickItem tree, this one does
// with the recording backend's records.
class RecordingProbe final : public ui::testing::ConformanceProbe {
public:
    ui::IViewBackend& backend() override { return _backend; }
    morph::reactive::Runtime& runtime() override { return _rt; }
    void settle() override { _owner.runAll(); }

    [[nodiscard]] std::string textOf(ui::Widget const& widget) override {
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
        auto const children = _backend.children(_backend.idOf(container));
        return index < children.size() ? _backend.widget(children.at(index)) : nullptr;
    }
    void click(ui::Widget& widget) override { _backend.click(_backend.idOf(widget)); }
    void type(ui::Widget& widget, std::string_view text) override {
        _backend.edit(_backend.idOf(widget), std::string{text});
    }
    void drag(ui::Widget& source, ui::Widget& target) override {
        static_cast<void>(_backend.drag(_backend.idOf(source), _backend.idOf(target)));
    }
    void dismiss(ui::Widget& dialog) override { _backend.dismiss(_backend.idOf(dialog)); }
    void selectRows(ui::Widget& table, std::vector<std::size_t> const& rows) override {
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

    morph::testing::StepExecutor _owner;
    ui::testing::RecordingBackend _backend;
    morph::reactive::Runtime _rt{_owner};
};

}  // namespace

TEST_CASE("ui conformance: RecordingBackend passes every case", "[ui][conformance]") {
    auto const cases = ui::testing::conformanceCases();
    REQUIRE(cases.size() == 23);
    std::set<std::string_view> names;
    for (auto const& testCase : cases) {
        CHECK(names.insert(testCase.name).second);
        RecordingProbe probe;
        std::optional<std::string> const failure = testCase.run(probe);
        INFO(std::string{testCase.name} + ": " + failure.value_or("passed"));
        CHECK_FALSE(failure.has_value());
    }
}
