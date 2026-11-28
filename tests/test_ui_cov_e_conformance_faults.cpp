// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <morph/ui/backend.hpp>
#include <morph/ui/testing/backend_conformance.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_recording_probe.hpp"

namespace ui = morph::ui;

namespace {

using morph::testing::conformance_probe::Fault;
using morph::testing::conformance_probe::RecordingProbe;

// How a probe misreads or mis-drives the backend behind it. Each distortion corrupts one family of probe operations;
// a case that depends on that family must report it.
enum class Distort : std::uint8_t {
    ZeroCounts,          // every container reports no children
    NullChildren,        // every container hands out null in place of each child
    FewerChildren,       // every container reports one child fewer than it has
    MoreChildren,        // every container reports one child more than it has
    InvertVisible,       // every widget's visibility reads inverted
    InvertEnabled,       // every widget's enabled flag reads inverted
    WrongText,           // every widget shows "?"
    EmptyText,           // every widget shows nothing
    DropInput,           // every driving operation does nothing
    NoSelection,         // a table shows no rows selected
    ExtraSelection,      // a table shows row 0 selected as well
    CursorOff,           // a field's cursor reads one byte further
    FocusNowhere,        // no widget has focus
    FocusEverywhere,     // every widget has focus
    BoundsNone,          // the probe reads no geometry
    BoundsZero,          // every widget is drawn with no size
    BoundsRamp,          // widgets are drawn at places and sizes that grow with the order they are asked about
    BoundsStack,         // every widget is drawn in the same place, 12 by 3
    BoundsNarrow,        // every widget is 3 wide and 1 high, side by side
    BoundsWide,          // every widget is 80 wide and 1 high, one below the other
    ExtentOffset,        // a text's lines are drawn 5 units right of the widget
    TargetNowhere,       // a spotlight rings nothing
    HeadersEmpty,        // every header reads empty
    HeadersWrong,        // every header reads another text
    MenuEmpty,           // a header's menu has no entries
    ClipboardEmpty,      // the clipboard reads empty
    DescriptionNone,     // no description is read
    NullSecond,          // a container hands out null for its second child
    NullThird,           // a container hands out null for its third child
    NullFourth,          // a container hands out null for its fourth child
    NullFirst,           // a container hands out null for its first child
    MenuRelabelled,      // a header menu's first entry carries another label
    MenuNoSeparator,     // a header menu's second entry is no separator
    MenuLastRelabelled,  // a header menu's last entry carries another label
    TargetOther,         // a spotlight rings a widget other than its target
};

constexpr std::size_t kAll = static_cast<std::size_t>(-1);

// A reference probe decorated with one distortion applied to a window of the calls of its family.
class DistortedProbe final : public ui::testing::ConformanceProbe {
public:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the window is a first call and a count, named as such.
    DistortedProbe(Distort mode, std::size_t from, std::size_t count) : _mode{mode}, _from{from}, _count{count} {}

    // How many calls of the distorted family the case made.
    [[nodiscard]] std::size_t seen() const { return _seen; }

    ui::IViewBackend& backend() override { return _inner.backend(); }
    morph::reactive::Runtime& runtime() override { return _inner.runtime(); }
    void settle() override { _inner.settle(); }

    [[nodiscard]] std::string textOf(ui::Widget const& widget) override {
        if (_mode == Distort::WrongText && hit()) {
            return "?";
        }
        if (_mode == Distort::EmptyText && hit()) {
            return {};
        }
        return _inner.textOf(widget);
    }
    [[nodiscard]] bool visibleOf(ui::Widget const& widget) override {
        bool const visible = _inner.visibleOf(widget);
        return _mode == Distort::InvertVisible && hit() ? !visible : visible;
    }
    [[nodiscard]] bool enabledOf(ui::Widget const& widget) override {
        bool const enabled = _inner.enabledOf(widget);
        return _mode == Distort::InvertEnabled && hit() ? !enabled : enabled;
    }
    [[nodiscard]] std::size_t childCount(ui::ContainerWidget const& container) override {
        std::size_t const count = _inner.childCount(container);
        if (_mode == Distort::ZeroCounts && hit()) {
            return 0;
        }
        if (_mode == Distort::FewerChildren && hit()) {
            return count == 0 ? 0 : count - 1;
        }
        if (_mode == Distort::MoreChildren && hit()) {
            return count + 1;
        }
        return count;
    }
    [[nodiscard]] ui::Widget* childAt(ui::ContainerWidget const& container, std::size_t index) override {
        if (_mode == Distort::NullChildren && hit()) {
            return nullptr;
        }
        bool const first = _mode == Distort::NullFirst && index == 0;
        bool const second = _mode == Distort::NullSecond && index == 1;
        bool const third = _mode == Distort::NullThird && index == 2;
        bool const fourth = _mode == Distort::NullFourth && index == 3;
        if ((first || second || third || fourth) && hit()) {
            return nullptr;
        }
        return _inner.childAt(container, index);
    }
    void click(ui::Widget& widget) override {
        if (!dropped()) {
            _inner.click(widget);
        }
    }
    void clickAt(ui::Widget& across, ui::Widget& along) override {
        if (!dropped()) {
            _inner.clickAt(across, along);
        }
    }
    void type(ui::Widget& widget, std::string_view text) override {
        if (!dropped()) {
            _inner.type(widget, text);
        }
    }
    void drag(ui::Widget& source, ui::Widget& target) override {
        if (!dropped()) {
            _inner.drag(source, target);
        }
    }
    void dismiss(ui::Widget& dialog) override {
        if (!dropped()) {
            _inner.dismiss(dialog);
        }
    }
    void selectRows(ui::Widget& table, std::vector<std::size_t> const& rows) override {
        if (!dropped()) {
            _inner.selectRows(table, rows);
        }
    }
    void clickRow(ui::Widget& table, std::size_t row, ui::RowClick click) override {
        if (!dropped()) {
            _inner.clickRow(table, row, click);
        }
    }
    void clickHeader(ui::Widget& table, std::size_t column, ui::HeaderClick click) override {
        if (!dropped()) {
            _inner.clickHeader(table, column, click);
        }
    }
    [[nodiscard]] std::vector<std::size_t> selectedRows(ui::Widget const& table) override {
        std::vector<std::size_t> rows = _inner.selectedRows(table);
        if (_mode == Distort::NoSelection && hit()) {
            return {};
        }
        if (_mode == Distort::ExtraSelection && hit() && !std::ranges::contains(rows, std::size_t{0})) {
            rows.insert(rows.begin(), 0);
        }
        return rows;
    }
    void moveCursor(ui::Widget& field, std::size_t position) override {
        if (!dropped()) {
            _inner.moveCursor(field, position);
        }
    }
    [[nodiscard]] std::size_t cursorOf(ui::Widget const& field) override {
        std::size_t const cursor = _inner.cursorOf(field);
        return _mode == Distort::CursorOff && hit() ? cursor + 1 : cursor;
    }
    void press(ui::Widget& focused, std::string_view chord) override {
        if (!dropped()) {
            _inner.press(focused, chord);
        }
    }
    [[nodiscard]] bool hasFocus(ui::Widget const& widget) override {
        if (_mode == Distort::FocusNowhere && hit()) {
            return false;
        }
        if (_mode == Distort::FocusEverywhere && hit()) {
            return true;
        }
        return _inner.hasFocus(widget);
    }
    void chooseMenuEntry(ui::Widget& menu, std::vector<std::size_t> const& path) override {
        if (!dropped()) {
            _inner.chooseMenuEntry(menu, path);
        }
    }
    void expand(ui::Widget& section, bool open) override {
        if (!dropped()) {
            _inner.expand(section, open);
        }
    }
    void dropFiles(ui::Widget& zone, std::vector<std::string> const& paths) override {
        if (!dropped()) {
            _inner.dropFiles(zone, paths);
        }
    }
    [[nodiscard]] std::optional<std::string> descriptionOf(ui::Widget const& widget) override {
        if (_mode == Distort::DescriptionNone && hit()) {
            return std::nullopt;
        }
        return _inner.descriptionOf(widget);
    }
    [[nodiscard]] std::optional<ui::testing::WidgetBounds> boundsOf(ui::Widget const& widget) override {
        std::optional<ui::testing::WidgetBounds> const real = _inner.boundsOf(widget);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
        switch (_mode) {
            case Distort::BoundsNone:
                return hit() ? std::nullopt : real;
            case Distort::BoundsZero:
                return hit() ? std::optional<ui::testing::WidgetBounds>{ui::testing::WidgetBounds{}} : real;
            case Distort::BoundsRamp: {
                if (!hit()) {
                    return real;
                }
                auto const [place, fresh] = _ordinals.try_emplace(&widget, _ordinals.size());
                static_cast<void>(fresh);
                auto const ordinal = static_cast<double>(place->second);
                return ui::testing::WidgetBounds{
                    .x = ordinal * 4, .y = ordinal * 2, .width = 6 + (ordinal * 5), .height = 1 + ordinal};
            }
            case Distort::BoundsStack:
                return hit() ? std::optional<ui::testing::WidgetBounds>{ui::testing::WidgetBounds{
                                   .x = 0, .y = 0, .width = 12, .height = 3}}
                             : real;
            case Distort::BoundsNarrow: {
                if (!hit()) {
                    return real;
                }
                auto const [place, fresh] = _ordinals.try_emplace(&widget, _ordinals.size());
                static_cast<void>(fresh);
                return ui::testing::WidgetBounds{
                    .x = static_cast<double>(place->second) * 3, .y = 0, .width = 3, .height = 1};
            }
            case Distort::BoundsWide: {
                if (!hit()) {
                    return real;
                }
                auto const [place, fresh] = _ordinals.try_emplace(&widget, _ordinals.size());
                static_cast<void>(fresh);
                return ui::testing::WidgetBounds{
                    .x = 0, .y = static_cast<double>(place->second), .width = 80, .height = 1};
            }
            default:
                return real;
        }
#pragma GCC diagnostic pop
    }
    [[nodiscard]] std::optional<ui::testing::WidgetBounds> textExtentOf(ui::Widget const& widget) override {
        std::optional<ui::testing::WidgetBounds> extent = _inner.textExtentOf(widget);
        if (_mode == Distort::ExtentOffset && hit()) {
            ui::testing::WidgetBounds shifted = _inner.boundsOf(widget).value_or(ui::testing::WidgetBounds{});
            shifted.x += 5;
            return shifted;
        }
        return extent;
    }
    [[nodiscard]] ui::Widget const* targetOf(ui::SpotlightWidget const& spotlight) override {
        if (_mode == Distort::TargetNowhere && hit()) {
            return nullptr;
        }
        if (_mode == Distort::TargetOther && hit()) {
            return &spotlight;
        }
        return _inner.targetOf(spotlight);
    }
    [[nodiscard]] std::vector<ui::MenuEntry> headerMenu(ui::Widget& table,
                                                        std::optional<std::size_t> column) override {
        if (_mode == Distort::MenuEmpty && hit()) {
            return {};
        }
        std::vector<ui::MenuEntry> entries = _inner.headerMenu(table, column);
        if (entries.size() >= 2 && _mode == Distort::MenuRelabelled && hit()) {
            entries.front().label += "!";
        }
        if (entries.size() >= 2 && _mode == Distort::MenuNoSeparator && hit()) {
            entries.at(1).separator = !entries.at(1).separator;
        }
        if (entries.size() >= 2 && _mode == Distort::MenuLastRelabelled && hit()) {
            entries.back().label += "!";
        }
        return entries;
    }
    void chooseHeaderMenuEntry(ui::Widget& table, std::optional<std::size_t> column,
                               std::vector<std::size_t> const& path) override {
        if (!dropped()) {
            _inner.chooseHeaderMenuEntry(table, column, path);
        }
    }
    void moveColumn(ui::Widget& table, std::size_t column, std::size_t position) override {
        if (!dropped()) {
            _inner.moveColumn(table, column, position);
        }
    }
    [[nodiscard]] std::string headerText(ui::Widget const& table, std::size_t column) override {
        return headerOf(_inner.headerText(table, column));
    }
    [[nodiscard]] std::string headerName(ui::Widget const& table, std::size_t column) override {
        return headerOf(_inner.headerName(table, column));
    }
    [[nodiscard]] std::string headerDescription(ui::Widget const& table, std::size_t column) override {
        return headerOf(_inner.headerDescription(table, column));
    }
    void pressFilter(ui::Widget& table, std::size_t column) override {
        if (!dropped()) {
            _inner.pressFilter(table, column);
        }
    }
    void pressGroupFilter(ui::Widget& table, std::size_t group) override {
        if (!dropped()) {
            _inner.pressGroupFilter(table, group);
        }
    }
    [[nodiscard]] std::string clipboardText() override {
        std::string const text = _inner.clipboardText();
        return _mode == Distort::ClipboardEmpty && hit() ? std::string{} : text;
    }

private:
    // Counts a call of the distorted family and says whether it falls in the window.
    bool hit() {
        std::size_t const call = _seen++;
        return call >= _from && (_count == kAll || call - _from < _count);
    }
    bool dropped() { return _mode == Distort::DropInput && hit(); }
    std::string headerOf(std::string text) {
        if (_mode == Distort::HeadersEmpty && hit()) {
            return {};
        }
        if (_mode == Distort::HeadersWrong && hit()) {
            return text + "!";
        }
        return text;
    }

    Distort _mode;
    std::size_t _from;
    std::size_t _count;
    std::size_t _seen = 0;
    RecordingProbe _inner{Fault::None};
    std::map<ui::Widget const*, std::size_t> _ordinals;
};

struct Run {
    std::optional<std::string> failure;
    std::size_t seen = 0;
    bool threw = false;
};

Run run(ui::testing::ConformanceCase const& testCase, Distort mode, std::size_t from, std::size_t count) {
    DistortedProbe probe{mode, from, count};
    Run result;
    try {
        result.failure = testCase.run(probe);
    } catch (std::exception const&) {
        result.threw = true;
    }
    result.seen = probe.seen();
    return result;
}

std::vector<ui::testing::ConformanceCase> allCases() {
    std::vector<ui::testing::ConformanceCase> cases;
    for (auto const group : {ui::testing::conformanceCases(), ui::testing::layoutConformanceCases(),
                             ui::testing::spotlightConformanceCases()}) {
        cases.insert(cases.end(), group.begin(), group.end());
    }
    return cases;
}

}  // namespace

namespace {

// NOLINTNEXTLINE(cert-err58-cpp,bugprone-throwing-static-initialization): a test table; an allocation failure at start-up ends the run either way.
const std::set<std::string> kZeroSized{"a row centres a child across it",
                                       "a table's heading row spans every column",
                                       "a table cell that spans three rows is drawn over all three",
                                       "a stretching scroll above a bar keeps the bar at its column's end",
                                       "a sheet fills the surface",
                                       "a slot showing a one-row ForEach whose holder stretches fills the slot",
                                       "a wrapping row starts a line only when out of room",
                                       "a column with wrap: false keeps its cells to one line",
                                       "a badge given a fixed width below its label is raised to its natural width",
                                       "a badge given a percent width below its label is raised to its natural width",
                                       "a stretch push button keeps its label's width"};

// NOLINTNEXTLINE(cert-err58-cpp,bugprone-throwing-static-initialization): a test table; an allocation failure at start-up ends the run either way.
const std::set<std::string> kStacked{
    "a table's heading row spans every column",
    "a stretching scroll above a bar keeps the bar at its column's end",
    "a sheet fills the surface",
    "a slot showing a one-row ForEach whose holder stretches fills the slot",
    "a column with wrap: false keeps its cells to one line",
    "a stretch row nested in a content-sized stretch row keeps its children's naturals",
    "a panel's stretch-height children share its height by weight, whatever their content",
    "a stretch push button keeps its label's width"};

std::vector<ui::testing::ConformanceCase> layoutCases() {
    auto const cases = ui::testing::layoutConformanceCases();
    return {cases.begin(), cases.end()};
}

// The cases that call the distorted family at least once, run with every call distorted, and that neither fail nor
// throw: a case that measures what the distortion destroys and still passes.
std::vector<std::string> passing(std::vector<ui::testing::ConformanceCase> const& cases, Distort mode) {
    std::vector<std::string> names;
    for (auto const& testCase : cases) {
        Run const result = run(testCase, mode, 0, kAll);
        if (result.seen > 0 && !result.threw && !result.failure.has_value()) {
            names.emplace_back(testCase.name);
        }
    }
    return names;
}

}  // namespace

// Mutation: make `detail::Checks::that` record nothing; every layout case then passes a destroyed structure.
TEST_CASE("cove: a layout case fails when the container it measures reports no, fewer or more children",
          "[ui][conformance][cove]") {
    auto const cases = layoutCases();
    for (Distort const mode : {Distort::ZeroCounts, Distort::NullChildren, Distort::FewerChildren}) {
        INFO(static_cast<int>(mode));
        CHECK(passing(cases, mode).empty());
    }
}

// Mutation: make `Geometry::of` return its bounds without checking that the probe read any.
TEST_CASE("cove: a layout case fails when the probe reads no bounds", "[ui][conformance][cove]") {
    CHECK(passing(layoutCases(), Distort::BoundsNone).empty());
}

// With every widget drawn zero-sized, or every widget drawn on top of the others, these cases still pass: what they
// compare holds trivially of such a layout. The sets name the blind spots; a case that stops being blind leaves its
// set, a case that becomes blind enters it.
TEST_CASE("cove: the layout cases that a degenerate layout still passes are the known ones",
          "[ui][conformance][cove]") {
    auto const cases = layoutCases();
    std::vector<std::string> const zero = passing(cases, Distort::BoundsZero);
    std::vector<std::string> const stacked = passing(cases, Distort::BoundsStack);
    CHECK(std::set<std::string>{zero.begin(), zero.end()} == kZeroSized);
    CHECK(std::set<std::string>{stacked.begin(), stacked.end()} == kStacked);
}

// A distortion of one call at a time: whichever case fails, it says why, and the suite catches each family of
// distortion somewhere. Mutation: make `Checks::fail` store an empty message.
TEST_CASE("cove: a single distorted call is reported with a message, in every family the suite reads",
          "[ui][conformance][cove]") {
    auto const cases = allCases();
    for (Distort m = Distort::ZeroCounts; m <= Distort::TargetOther;
         m = static_cast<Distort>(static_cast<int>(m) + 1)) {
        std::size_t caught = 0;
        std::size_t read = 0;
        for (auto const& testCase : cases) {
            Run const whole = run(testCase, m, 0, kAll);
            read += whole.seen;
            for (std::size_t call = 0; call < std::min<std::size_t>(whole.seen, 40); ++call) {
                for (std::size_t const count : {std::size_t{1}, kAll}) {
                    Run const one = run(testCase, m, call, count);
                    if (one.failure.has_value()) {
                        ++caught;
                        INFO(std::string{testCase.name} + " / " + std::to_string(static_cast<int>(m)) + " / " +
                             std::to_string(call));
                        CHECK_FALSE(one.failure->empty());
                    }
                }
            }
        }
        INFO(static_cast<int>(m));
        CHECK(read > 0);
        CHECK(caught > 0);
    }
}
