// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <morph/reactive/runtime.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/testing/backend_conformance.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "test_support.hpp"
#include "ui_echoing_backend.hpp"
#include "ui_recording_probe.hpp"

namespace ui = morph::ui;

namespace {

using morph::testing::conformance_probe::Fault;
using morph::testing::conformance_probe::RecordingProbe;

// A probe that renders through a real backend but delivers no input and reads nothing back: what a probe whose
// driving and reading operations are stubs would report.
class InertProbe final : public ui::testing::ConformanceProbe {
public:
    ui::IViewBackend& backend() override { return _backend; }
    morph::reactive::Runtime& runtime() override { return _rt; }
    void settle() override { _owner.runAll(); }
    [[nodiscard]] std::string textOf(ui::Widget const&) override { return {}; }
    [[nodiscard]] bool visibleOf(ui::Widget const&) override { return false; }
    [[nodiscard]] bool enabledOf(ui::Widget const&) override { return false; }
    [[nodiscard]] std::size_t childCount(ui::ContainerWidget const&) override { return 0; }
    [[nodiscard]] ui::Widget* childAt(ui::ContainerWidget const&, std::size_t) override { return nullptr; }
    void click(ui::Widget&) override {}
    void type(ui::Widget&, std::string_view) override {}
    void drag(ui::Widget&, ui::Widget&) override {}
    void dismiss(ui::Widget&) override {}
    void selectRows(ui::Widget&, std::vector<std::size_t> const&) override {}
    void clickRow(ui::Widget&, std::size_t, ui::RowClick) override {}
    void clickHeader(ui::Widget&, std::size_t, ui::HeaderClick) override {}
    [[nodiscard]] std::vector<std::size_t> selectedRows(ui::Widget const&) override { return {}; }
    void moveCursor(ui::Widget&, std::size_t) override {}
    [[nodiscard]] std::size_t cursorOf(ui::Widget const&) override { return 0; }
    void press(ui::Widget&, std::string_view) override {}
    [[nodiscard]] bool hasFocus(ui::Widget const&) override { return false; }
    void chooseMenuEntry(ui::Widget&, std::vector<std::size_t> const&) override {}
    void expand(ui::Widget&, bool) override {}
    void dropFiles(ui::Widget&, std::vector<std::string> const&) override {}

private:
    morph::testing::StepExecutor _owner;
    ui::testing::RecordingBackend _backend;
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
    REQUIRE(cases.size() == 67);
    std::set<std::string_view> names;
    for (auto const& testCase : cases) {
        CHECK(names.insert(testCase.name).second);
        RecordingProbe probe;
        std::optional<std::string> const failure = testCase.run(probe);
        INFO(std::string{testCase.name} + ": " + failure.value_or("passed"));
        CHECK_FALSE(failure.has_value());
    }
}

// A probe that reads no geometry fails every layout case rather than passing it unmeasured.
TEST_CASE("ui conformance: a layout case run on a backend that lays nothing out fails", "[ui][conformance]") {
    for (auto const& testCase : ui::testing::layoutConformanceCases()) {
        RecordingProbe probe;
        std::optional<std::string> const failure = testCase.run(probe);
        INFO(testCase.name);
        REQUIRE(failure.has_value());
        CHECK(failure->contains("reads no bounds"));
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
          "a text field shows what the document makes of an edit",
          "a button-style menu reports the entry past a separator, and never the separator",
          "a chart shows its current summary"}) {
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
          "a chord the focused widget declares comes before its own key",
          "a focused button takes Enter before its ancestors' chords",
          "a submitting field in a dialog takes Enter from the dialog",
          "a field without a submit commits on Enter and lets the dialog have it",
          "a check box toggles on Space and lets the dialog have Enter",
          "a field's own Enter chord comes before its commit and submit",
          "a menu reports only an enabled entry without a submenu",
          "a read-only input takes no edit",
          "a dismissed banner reports it and stays shown",
          "a closed collapsible's content takes no input, its header does",
          "a drop zone takes a drop whole or refuses it whole",
          "a click on a sortable header asks to sort by it, and no other header or setter does",
          "a button-style menu reports the entry past a separator, and never the separator",
          "a removable badge reports its removal once",
          "table: the table's menu is the menu with no column",
          "table: Ctrl+C on a focused table reports a copy",
          "table: a filterable header's filter button reports its column",
          "table: a group's filter button reports the group"}) {
        INFO(name);
        CHECK(deadInput.contains(name));
    }

    std::set<std::string_view> const noModifiers = failingCases(Fault::NoModifiers);
    for (std::string_view const name :
         {"a click selects only its row, and a Ctrl-click toggles one and keeps the rest",
          "a Shift-click selects the rows from the anchor, in place of the rest"}) {
        INFO(name);
        CHECK(noModifiers.contains(name));
    }

    // Each configuring fault fails the case its control stands beside. Mutation: drop a fault's branch from the
    // probe (the case then passes under it).
    for (auto const& [fault, name] : std::initializer_list<std::pair<Fault, std::string_view>>{
             {Fault::PlainHeaderClicks,
              "table: a Shift-click on a sortable header reports Shift, a plain click Plain"},
             {Fault::FirstSortKeyOnly, "table: a chain of three keys marks every header with its place"},
             {Fault::MenuColumnOffByOne,
              "table: a header's menu shows its entries and choosing one reports the column and the path"},
             {Fault::MoveTargetIsSource, "table: a column moved by the user reports its column and new place"},
             {Fault::UnnamedFilter, "table: a filtered column's header is named so"},
             {Fault::SilentClipboard, "backend: copyText puts its text on the clipboard"}}) {
        INFO(name);
        CHECK(failingCases(fault).contains(name));
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

// A suite that a probe doing nothing passes measures nothing. The two cases that still pass check only that something
// does not happen. Mutation: give any other case's failure path an early `return std::nullopt` (it then passes here).
TEST_CASE("ui conformance: a probe that drives and reads nothing fails every case that observes something",
          "[ui][conformance]") {
    std::set<std::string> passed;
    for (auto const& testCase : ui::testing::conformanceCases()) {
        InertProbe probe;
        std::optional<std::string> const failure = testCase.run(probe);
        if (!failure.has_value()) {
            passed.emplace(testCase.name);
        } else {
            CHECK_FALSE(failure->empty());
        }
    }
    CHECK(passed ==
          std::set<std::string>{"a drag onto a refusing target delivers nothing", "no setter calls a handler"});
}

// Mutation: make `badgeRaisedTo` check nothing (the narrow badge, or the text inside it, passes).
TEST_CASE("ui conformance: the badge cases flag a badge laid out narrower than its label, or text inside it",
          "[ui][conformance]") {
    std::size_t cases = 0;
    for (auto const& testCase : ui::testing::layoutConformanceCases()) {
        if (!std::string_view{testCase.name}.contains("below its label")) {
            continue;
        }
        ++cases;
        INFO(testCase.name);
        RecordingProbe narrow{Fault::NarrowBadge};
        std::optional<std::string> const tooNarrow = testCase.run(narrow);
        REQUIRE(tooNarrow.has_value());
        CHECK(tooNarrow->contains("against its content width"));
        for (Fault const fault :
             {Fault::NoChildren, Fault::SecondMissing, Fault::HollowNarrow, Fault::HollowNatural}) {
            RecordingProbe broken{fault};
            std::optional<std::string> const failure = testCase.run(broken);
            INFO(static_cast<int>(fault));
            REQUIRE(failure.has_value());
            CHECK(failure->contains("two rows of a badge and a text"));
        }
        RecordingProbe overlapped{Fault::OverlappedText};
        std::optional<std::string> const inside = testCase.run(overlapped);
        REQUIRE(inside.has_value());
        CHECK(inside->contains("inside the badge"));
    }
    CHECK(cases == 2);
}

// Mutation: make the three cases check nothing (a grid, or a key-value, laid out wrongly passes).
TEST_CASE("ui conformance: the grid and key-value width cases flag a wrong size and a missing cell",
          "[ui][conformance]") {
    std::size_t cases = 0;
    for (auto const& testCase : ui::testing::layoutConformanceCases()) {
        std::string_view const name{testCase.name};
        bool const grid =
            name.contains("grid's columns without tracks") || name.contains("content-sized grid without");
        if (!grid && !name.contains("caption width sets")) {
            continue;
        }
        ++cases;
        INFO(testCase.name);
        for (Fault const fault : {Fault::TinyBounds, Fault::HugeBounds, Fault::NoChildren}) {
            RecordingProbe probe{fault};
            CHECK(testCase.run(probe).has_value());
        }
        if (grid) {
            RecordingProbe hollow{Fault::HollowGrid};
            CHECK(testCase.run(hollow).has_value());
        }
    }
    CHECK(cases == 3);
}

// Mutation: make the five least-column-width cases check nothing (a grid that ignores the key, drops no column or
// leaves the text after it overlapping passes).
TEST_CASE("ui conformance: the grid least-column-width cases flag a wrong size, a missing cell and a hollow grid",
          "[ui][conformance]") {
    std::set<std::string_view> const names{"a grid's columns follow its width when it gives a least column width",
                                           "a grid keeps all its columns when its width holds them exactly",
                                           "a grid narrower than one least column shows one column",
                                           "a grid's gap counts in how many least columns fit",
                                           "a grid whose columns drop grows down and pushes what follows it"};
    std::size_t cases = 0;
    for (auto const& testCase : ui::testing::layoutConformanceCases()) {
        if (!names.contains(testCase.name)) {
            continue;
        }
        ++cases;
        INFO(testCase.name);
        for (Fault const fault : {Fault::TinyBounds, Fault::HugeBounds, Fault::NoChildren, Fault::HollowGrid}) {
            RecordingProbe probe{fault};
            INFO(static_cast<int>(fault));
            CHECK(testCase.run(probe).has_value());
        }
    }
    CHECK(cases == 5);
}

// Mutation: make the stretch cases check nothing (a row that shares by content, a row that sums its naturals or a
// button narrowed below its label passes).
TEST_CASE("ui conformance: the stretch cases flag a wrong width, a missing child and a narrowed button",
          "[ui][conformance]") {
    std::set<std::string_view> const names{
        "two stretch siblings of weight 1 share a row equally, whatever their content",
        "stretch siblings share a row in proportion to their weights",
        "a content-sized row of stretch children gives each its natural width",
        "a stretch row nested in a content-sized stretch row keeps its children's naturals",
        "a stretch child held at its minimum leaves the others to share the rest",
        "a content-sized spacer shares a row as a stretch child of weight 1",
        "stretch-height children share a column by weight, whatever their content",
        "a panel's stretch-height children share its height by weight, whatever their content",
        "a dialog's stretch-height children share its height by weight, whatever their content",
        "a content-sized panel is tall enough for each of its stretch-height children's content",
        "a content-sized scroll area, slot and collapsible share their stretch-height children by weight",
        "a drawer's stretch-height children share its height by weight, whatever their content",
        "a tab page's stretch-height children share its height by weight, whatever their content",
        "a horizontal scroll area's stretch-height children share its height by weight",
        "a stretch push button keeps its label's width"};
    std::size_t cases = 0;
    for (auto const& testCase : ui::testing::layoutConformanceCases()) {
        if (!names.contains(testCase.name)) {
            continue;
        }
        ++cases;
        INFO(testCase.name);
        RecordingProbe none{Fault::NoChildren};
        CHECK(testCase.run(none).has_value());
        RecordingProbe tiny{Fault::TinyBounds};
        RecordingProbe huge{Fault::HugeBounds};
        RecordingProbe narrow{Fault::NarrowStretch};
        bool const flagged =
            testCase.run(tiny).has_value() || testCase.run(huge).has_value() || testCase.run(narrow).has_value();
        CHECK(flagged);
    }
    CHECK(cases == names.size());
}

// Mutation: in `FakeSpotlight::setTarget`, keep `_target` empty (every case that rings a target fails).
TEST_CASE("ui conformance: RecordingBackend passes every spotlight case", "[ui][conformance][tours]") {
    auto const cases = ui::testing::spotlightConformanceCases();
    REQUIRE(cases.size() == 5);
    for (auto const& testCase : cases) {
        RecordingProbe probe;
        std::optional<std::string> const failure = testCase.run(probe);
        INFO(std::string{testCase.name} + ": " + failure.value_or("passed"));
        CHECK_FALSE(failure.has_value());
    }
}

// A spotlight case that a probe driving and reading nothing passes measures nothing.
TEST_CASE("ui conformance: a probe that drives and reads nothing fails every spotlight case",
          "[ui][conformance][tours]") {
    for (auto const& testCase : ui::testing::spotlightConformanceCases()) {
        InertProbe probe;
        INFO(testCase.name);
        CHECK(testCase.run(probe).has_value());
    }
}
