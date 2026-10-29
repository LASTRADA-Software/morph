// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <morph/ui/author.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ui_interpreter_support.hpp"

/// @file
/// @brief The server-side builders of `morph::ui::author`: expression handles, declarations, nodes, commands,
///        registration with its catalog and accessibility rules, and the authoring goldens.

namespace author_test {

struct ResultInputs {
    std::int64_t mass = 0;
    std::int64_t volume = 0;
};

struct ResultView {
    std::int64_t id = 0;
    ResultInputs inputs;
    std::optional<std::int64_t> concentration;
    bool outsideLimits = false;
};

struct Evaluated {
    std::optional<std::int64_t> concentration;
};

struct ListResults {
    std::int64_t sampleId = 0;
};

struct EvaluateResult {
    std::int64_t resultId = 0;
    ResultInputs inputs;
};

struct CaptureConcentration {
    std::int64_t resultId = 0;
    ResultInputs inputs;
};

struct LoadSample {
    std::int64_t id = 0;
};

struct HeaderSection {
    std::string sampleNumber;
    std::int64_t project = 0;
};

struct Header {
    std::string sampleNumber;
};

struct GetHeader {};

struct ListProjects {
    std::int64_t page = 0;
};

struct UploadFile {
    std::string name;
};

struct ListSamples {
    std::int64_t page = 0;
};

/// A list action's record result: the rows in one member, beside a scalar.
struct SampleList {
    std::vector<ResultView> samples;
    std::int64_t total = 0;
};

struct SampleModel {
    using PrimaryKey = std::int64_t;
    std::vector<ResultView> execute(ListResults const& action);
    Evaluated execute(EvaluateResult const& action);
    ResultView execute(CaptureConcentration const& action);
    bool execute(LoadSample const& action);
    bool execute(HeaderSection const& action);
    Header execute(GetHeader const& action);
    std::vector<Header> execute(ListProjects const& action);
    bool execute(UploadFile const& action);
    SampleList execute(ListSamples const& action);
};

struct SignIn {
    std::string user;
    std::string password;
};

struct Token {
    std::string token;
};

struct SessionModel {
    Token execute(SignIn const& action);
};

/// A struct whose JSON leaves a member out, so its member pointer has no name.
struct Partial {
    std::int64_t shown = 0;
    std::int64_t hidden = 0;
};

}  // namespace author_test

template <>
struct glz::meta<author_test::Partial> {
    using T = author_test::Partial;
    static constexpr auto value = glz::object("shown", &T::shown);
};

template <>
struct morph::model::ModelTraits<author_test::SampleModel> {
    static constexpr std::string_view typeId() noexcept { return "Lims_SampleModel"; }
};

template <>
struct morph::model::ModelTraits<author_test::SessionModel> {
    static constexpr std::string_view typeId() noexcept { return "Auth_SessionModel"; }
};

// NOLINTBEGIN(cppcoreguidelines-macro-usage): one traits specialisation per test action.
#define AUTHOR_TEST_ACTION(A, RESULT, NAME)                                  \
    template <>                                                              \
    struct morph::model::ActionTraits<author_test::A> {                      \
        using Result = RESULT;                                               \
        static constexpr std::string_view typeId() noexcept { return NAME; } \
    };
// NOLINTEND(cppcoreguidelines-macro-usage)

AUTHOR_TEST_ACTION(ListResults, std::vector<author_test::ResultView>, "Lims_ListResults")
AUTHOR_TEST_ACTION(EvaluateResult, author_test::Evaluated, "Lims_EvaluateResult")
AUTHOR_TEST_ACTION(CaptureConcentration, author_test::ResultView, "Lims_CaptureConcentration")
AUTHOR_TEST_ACTION(LoadSample, bool, "Lims_LoadSample")
AUTHOR_TEST_ACTION(HeaderSection, bool, "Lims_HeaderSection")
AUTHOR_TEST_ACTION(GetHeader, author_test::Header, "Lims_GetHeader")
AUTHOR_TEST_ACTION(ListProjects, std::vector<author_test::Header>, "Lims_ListProjects")
AUTHOR_TEST_ACTION(UploadFile, bool, "Lims_UploadFile")
AUTHOR_TEST_ACTION(SignIn, author_test::Token, "Auth_SignIn")
AUTHOR_TEST_ACTION(ListSamples, author_test::SampleList, "Lims_ListSamples")

template <>
struct morph::ui::author::ActionFacts<author_test::ListResults> {
    static constexpr bool credential = false;
    static std::vector<std::string> calculatedFields() { return {"concentration"}; }
    static std::vector<std::string> secretFields() { return {}; }
};

template <>
struct morph::ui::author::ActionFacts<author_test::SignIn> {
    static constexpr bool credential = true;
    static std::vector<std::string> calculatedFields() { return {}; }
    static std::vector<std::string> secretFields() { return {"token"}; }
};

template <>
struct morph::ui::author::ModelFacts<author_test::SessionModel> {
    static constexpr bool stateless = true;
};

namespace {

namespace ui = morph::ui::author;
using author_test::CaptureConcentration;
using author_test::Evaluated;
using author_test::EvaluateResult;
using author_test::GetHeader;
using author_test::Header;
using author_test::HeaderSection;
using author_test::ListProjects;
using author_test::ListResults;
using author_test::ListSamples;
using author_test::LoadSample;
using author_test::ResultInputs;
using author_test::ResultView;
using author_test::SampleModel;
using author_test::SessionModel;
using author_test::SignIn;
using author_test::UploadFile;
using morph::ui::Json;
using morph::ui::Refusal;
using namespace std::chrono_literals;

std::string text(Json const& json) { return glz::write_json(json).value_or(std::string{}); }

std::string text(ui::AnyHandle const& handle) { return text(handle.json()); }

std::string text(ui::Command const& command) { return text(command.json()); }

std::string text(ui::Node const& node) { return text(node.json()); }

std::string pretty(Json const& json) {
    std::string out;
    REQUIRE_FALSE(glz::write<glz::opts{.prettify = true}>(json, out));
    return out + "\n";
}

std::string readFile(std::string const& path) {
    std::ifstream const file{path};
    std::stringstream content;
    content << file.rdbuf();
    return content.str();
}

/// Compares a document with its checked-in golden. With MORPH_UPDATE_GOLDENS set, rewrites the golden instead.
void checkGolden(std::string const& name, Json const& json) {
    std::string const path = std::string{MORPH_UI_GOLDEN_DIR} + "/" + name + ".json";
    std::string const actual = pretty(json);
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, on the test's own thread.
    if (std::getenv("MORPH_UPDATE_GOLDENS") != nullptr) {
        std::ofstream{path} << actual;
    }
    CHECK(readFile(path) == actual);
}

/// Registers a screen in a fresh registry and returns the refusal, or nothing.
std::optional<Refusal> refusalOf(ui::Screen const& screen) {
    ui::ScreenRegistry registry;
    auto added = registry.add(screen);
    if (added) {
        return std::nullopt;
    }
    return added.error();
}

/// Loads a hand-written document and runs the accessibility rules on it.
std::optional<Refusal> accessibility(std::string_view document) {
    auto loaded = morph::ui::loadDocumentText(document);
    REQUIRE(loaded.has_value());
    return ui::checkAccessibility(*loaded);
}

/// The reference screen of spec 5 §11, written with the builders.
ui::Screen resultsScreen() {
    return ui::screen<"lims.results">([](ui::Scope& screen) {
        auto sampleId = screen.param<std::int64_t>("sampleId");
        auto sample = screen.model<SampleModel>("sample", {.instance = sampleId});
        auto results = screen.query<ListResults>("results", sample, {{&ListResults::sampleId, sampleId}});
        return ui::table(
                   results.value(), {.key = ui::row(&ResultView::id)},
                   [&sample](ui::RowScope& rowScope) {
                       auto draft = rowScope.state("draft", ui::row(&ResultView::inputs));
                       auto calc = rowScope.query<EvaluateResult>(
                           "calc", sample,
                           {{&EvaluateResult::resultId, ui::row(&ResultView::id)}, {&EvaluateResult::inputs, draft}},
                           {.debounce = 300ms});
                       return ui::cells(
                           {ui::textInput({.value = draft.at(&ResultInputs::mass),
                                           .label = ui::t("lims.mass", "Mass"),
                                           .commitOn = ui::Commit::Change}),
                            ui::text({.text = ui::quantity(calc.value()
                                                               .at(&Evaluated::concentration)
                                                               .orElse(ui::row(&ResultView::concentration))),
                                      .stale = calc.pending()})});
                   })
            .a11y(ui::t("lims.results", "Results"));
    });
}

}  // namespace

TEST_CASE("handles build expr/1 trees from operators", "[ui][author]") {
    auto const count = ui::ref<std::int64_t>("count");
    auto const limit = ui::ref<std::int64_t>("limit");
    auto const flag = ui::ref<bool>("flag");

    CHECK(text(count + 1) == R"({"fn":"add","args":[{"ref":"count"},1]})");
    CHECK(text(2 - count) == R"({"fn":"sub","args":[2,{"ref":"count"}]})");
    CHECK(text(count * limit) == R"({"fn":"mul","args":[{"ref":"count"},{"ref":"limit"}]})");
    CHECK(text(count / 2) == R"({"fn":"div","args":[{"ref":"count"},2]})");
    CHECK(text(count < limit) == R"({"fn":"lt","args":[{"ref":"count"},{"ref":"limit"}]})");
    CHECK(text(count <= 3) == R"({"fn":"le","args":[{"ref":"count"},3]})");
    CHECK(text(count > 3) == R"({"fn":"gt","args":[{"ref":"count"},3]})");
    CHECK(text(count >= 3) == R"({"fn":"ge","args":[{"ref":"count"},3]})");
    CHECK(text(flag && count < 3) == R"({"fn":"and","args":[{"ref":"flag"},{"fn":"lt","args":[{"ref":"count"},3]}]})");
    CHECK(text(flag || false) == R"({"fn":"or","args":[{"ref":"flag"},false]})");
    CHECK(text(!flag) == R"({"fn":"not","args":[{"ref":"flag"}]})");
    CHECK(text(-count) == R"({"fn":"neg","args":[{"ref":"count"}]})");
    CHECK(text(ui::eq(count, 1)) == R"({"fn":"eq","args":[{"ref":"count"},1]})");
    CHECK(text(ui::ne("a", ui::ref<std::string>("name"))) == R"({"fn":"ne","args":["a",{"ref":"name"}]})");
    CHECK(text(count.orElse(0)) == R"({"fn":"coalesce","args":[{"ref":"count"},0]})");
}

TEST_CASE("handles write constants exactly", "[ui][author]") {
    CHECK(text(ui::AnyHandle{true}) == "true");
    CHECK(text(ui::AnyHandle{std::int64_t{-7}}) == "-7");
    CHECK(text(ui::AnyHandle{"text"}) == R"("text")");
    CHECK(text(ui::AnyHandle{std::string_view{"view"}}) == R"("view")");
    CHECK(text(ui::AnyHandle{nullptr}) == "null");
    CHECK(text(ui::AnyHandle{morph::math::Rational{morph::math::Numerator{15}, morph::math::Denominator{10},
                                                   morph::math::DecimalPlaces{2}}}) ==
          R"({"lit":{"num":3,"den":2,"dp":2}})");
    CHECK(text(ui::AnyHandle{std::vector<std::string>{"a", "b"}}) == R"({"lit":["a","b"]})");
    CHECK(text(ui::AnyHandle{morph::ui::Value{std::int64_t{4}}}) == "4");
    CHECK_FALSE(ui::AnyHandle{}.present());
    CHECK_THROWS_AS(ui::AnyHandle{}.json(), ui::AuthoringError);
}

TEST_CASE("handles read fields by member pointer below a name", "[ui][author]") {
    auto const view = ui::ref<ResultView>("view");
    CHECK(text(view.at(&ResultView::inputs).at(&ResultInputs::mass)) == R"({"ref":"view.inputs.mass"})");
    CHECK(text(ui::row(&ResultView::id)) == R"({"row":"id"})");
    CHECK(text(ui::row<ResultView>().at(&ResultView::id)) == R"({"row":"id"})");
    CHECK(text(ui::row("inputs").at("mass")) == R"({"row":"inputs.mass"})");
    CHECK(text(ui::ref("x").at("y")) == R"({"ref":"x.y"})");
    // A member glaze does not reflect has no JSON name.
    CHECK_THROWS_AS(ui::row(&author_test::Partial::hidden), ui::AuthoringError);
    CHECK(text(ui::row(&author_test::Partial::shown)) == R"({"row":"shown"})");
}

// Mutation: leave the scroll at its content height in pinned (the scroll loses its sizing).
TEST_CASE("pinned writes a stretching column around a stretching scroll, with its bars outside it", "[ui][author]") {
    ui::Node const node = ui::pinned(ui::text({.text = "body"}),
                                     {.top = ui::text({.text = "head"}), .bottom = ui::text({.text = "bar"})});
    CHECK(text(node) ==
          R"({"kind":"column","children":[{"kind":"text","text":"head"},)"
          R"({"kind":"scroll","children":[{"kind":"text","text":"body"}],)"
          R"("sizing":{"lit":{"width":{"stretch":1},"height":{"stretch":1}}}},)"
          R"({"kind":"text","text":"bar"}],"sizing":{"lit":{"width":{"stretch":1},"height":{"stretch":1}}}})");
}

// Mutation: drop the focus from a blocked step's commands in stepper (the blocking field is not focused).
TEST_CASE("stepper writes a wizard over screen state; Continue moves one step, or focuses a blocked step's field",
          "[ui][author]") {
    ui::ScreenRegistry screens;
    auto const added = screens.add(ui::screen("author.stepper", [](ui::Scope& screen) {
        auto const step = screen.state("step", std::int64_t{0});
        auto const ready = screen.state("ready", false);
        auto const attempted = screen.state("attempted", false);
        auto const pulls = ui::element("pulls");
        return ui::stepper({.step = step,
                            .steps = {{.label = "One", .children = ui::text({.text = "one"}).testId("one")},
                                      {.label = "Two",
                                       .children = ui::textInput({.label = "Pulls"}).id(pulls).testId("pulls"),
                                       .canContinue = ready,
                                       .blocking = pulls,
                                       .onBlocked = ui::set(attempted, true)}},
                            .onFinish = ui::notify("done"),
                            .testIdPrefix = "w-"});
    }));
    INFO((added ? std::string{} : added.error().message()));
    REQUIRE(added.has_value());
    std::string const& written = (*added)->text;
    CHECK(written.contains(R"({"kind":"steps","items":)"));
    CHECK(written.contains(R"("kind":"switch","on":{"ref":"step"})"));
    morph::ui::testing::Harness h;
    auto const mounted = h.mount(written);
    REQUIRE(mounted != nullptr);
    CHECK(h.backend.find("Text", "testId", "one").has_value());
    h.backend.click(h.byTestId("Button", "w-continue"));
    h.settle();
    // One step on, not two: the step's own `set` must not satisfy the next step's test.
    REQUIRE(h.backend.find("TextInput", "testId", "pulls").has_value());
    CHECK(h.backend.prop(h.byTestId("Button", "w-continue"), "label") == "Finish");
    h.backend.click(h.byTestId("Button", "w-continue"));
    h.settle();
    CHECK(h.backend.focused() == h.byTestId("TextInput", "pulls"));
    CHECK(morph::ui::testing::readJson(*mounted, R"({"ref":"attempted"})") == "true");
    // A step that names its blocking field keeps Continue enabled, so the click can show the field.
    CHECK(h.backend.prop(h.byTestId("Button", "w-continue"), "enabled") != "false");
}

TEST_CASE("the library and reading forms have builders", "[ui][author]") {
    auto const items = ui::ref<std::vector<ResultView>>("items");
    CHECK(text(ui::engaged(ui::ref("x"))) == R"({"fn":"engaged","args":[{"ref":"x"}]})");
    CHECK(text(ui::coalesce({ui::ref("x"), 1})) == R"({"fn":"coalesce","args":[{"ref":"x"},1]})");
    CHECK(text(ui::concat({"a", ui::ref("b")})) == R"({"fn":"concat","args":["a",{"ref":"b"}]})");
    CHECK(text(ui::concatLists({ui::ref("a"), ui::ref("b")})) ==
          R"({"fn":"concatLists","args":[{"ref":"a"},{"ref":"b"}]})");
    ui::Handle<std::vector<std::int64_t>> const typed =
        ui::concatLists(std::vector{ui::ref<std::vector<std::int64_t>>("a"), ui::ref<std::vector<std::int64_t>>("b")});
    CHECK(text(typed) == R"({"fn":"concatLists","args":[{"ref":"a"},{"ref":"b"}]})");
    CHECK(text(ui::slice(ui::ref("a"), 0, 5)) == R"({"fn":"slice","args":[{"ref":"a"},0,5]})");
    CHECK(text(ui::slice(ui::ref("a"), 5)) == R"({"fn":"slice","args":[{"ref":"a"},5]})");
    CHECK(text(ui::sorted(ui::ref("a"), "x", [](ui::AnyHandle const& item) { return item.at("due"); })) ==
          R"({"sort":{"ref":"a"},"as":"x","by":{"ref":"x.due"}})");
    CHECK(text(ui::sorted(
              ui::ref("a"), "x", [](ui::AnyHandle const& item) { return item; }, "desc")) ==
          R"({"sort":{"ref":"a"},"as":"x","by":{"ref":"x"},"order":"desc"})");
    CHECK(text(ui::sortKey("due", ui::SortOrder::Descending)) == R"({"lit":{"column":"due","direction":"desc"}})");
    CHECK(text(ui::runs(ui::ref("a"), "w", [](ui::AnyHandle const& well) { return well.at("empty"); })) ==
          R"({"runs":{"ref":"a"},"as":"w","by":{"ref":"w.empty"}})");
    CHECK(text(ui::format("{0} of {1}", {1, 2})) == R"({"fn":"format","args":["{0} of {1}",1,2]})");
    CHECK(text(ui::count(items)) == R"({"fn":"count","args":[{"ref":"items"}]})");
    CHECK(text(ui::sum(items)) == R"({"fn":"sum","args":[{"ref":"items"}]})");
    CHECK(text(ui::includes(items, 3)) == R"({"fn":"includes","args":[{"ref":"items"},3]})");
    CHECK(text(ui::isEmpty(items)) == R"({"fn":"isEmpty","args":[{"ref":"items"}]})");
    CHECK(text(ui::round(ui::ref("x"), 2)) == R"({"fn":"round","args":[{"ref":"x"},2]})");
    // The default mode is written as no mode, so documents written before it are unchanged.
    CHECK(text(ui::round(ui::ref("x"), 2, ui::Rounding::HalfEven)) == R"({"fn":"round","args":[{"ref":"x"},2]})");
    CHECK(text(ui::round(ui::ref("x"), 2, ui::Rounding::HalfUp)) ==
          R"({"fn":"round","args":[{"ref":"x"},2,"halfUp"]})");
    CHECK(text(ui::decimal(ui::ref("x"), 1)) == R"({"fn":"decimal","args":[{"ref":"x"},1]})");
    CHECK(text(ui::decimal(ui::ref("x"), 1, ui::Rounding::HalfUp)) ==
          R"({"fn":"decimal","args":[{"ref":"x"},1,"halfUp"]})");
    CHECK(text(ui::abs(ui::ref("x"))) == R"({"fn":"abs","args":[{"ref":"x"}]})");
    CHECK(text(ui::dateAdd(ui::ref("start"), 3, ui::DateUnit::Months)) ==
          R"({"fn":"dateAdd","args":[{"ref":"start"},3,"months"]})");
    CHECK(text(ui::dateAdd(ui::ref("start"), ui::ref<std::int64_t>("n"), ui::DateUnit::Minutes)) ==
          R"({"fn":"dateAdd","args":[{"ref":"start"},{"ref":"n"},"minutes"]})");
    CHECK(text(ui::sqrt(ui::ref("x"), 4)) == R"({"fn":"sqrt","args":[{"ref":"x"},4]})");
    CHECK(text(ui::stdev(items, 3)) == R"({"fn":"stdev","args":[{"ref":"items"},3]})");
    CHECK(text(ui::rsd(items, 2, ui::Rounding::HalfUp)) == R"({"fn":"rsd","args":[{"ref":"items"},2,"halfUp"]})");
    CHECK(text(ui::quantity(ui::ref("q"))) == R"({"fn":"quantity","args":[{"ref":"q"}]})");
    CHECK(text(ui::quantity(ui::ref("q"), "mg/L")) == R"({"fn":"quantity","args":[{"ref":"q"},"mg/L"]})");
    CHECK(text(ui::call("mean", {items})) == R"({"fn":"mean","args":[{"ref":"items"}]})");
    CHECK(text(ui::t("k", "Fallback")) == R"({"t":"k","fallback":"Fallback"})");
    CHECK(text(ui::t("k", "{$n} rows", {{"n", ui::count(items)}})) ==
          R"({"t":"k","fallback":"{$n} rows","args":{"n":{"fn":"count","args":[{"ref":"items"}]}}})");
    CHECK(text(ui::cond<std::int64_t>(ui::ref<bool>("c"), 1, 2)) == R"({"if":[{"ref":"c"},1,2]})");
    CHECK(text(ui::obj({{"a", 1}, {"b", ui::ref("b")}})) == R"({"obj":{"a":1,"b":{"ref":"b"}}})");
    CHECK(text(ui::list({1, "x"})) == R"({"list":[1,"x"]})");
    CHECK(text(ui::map(items, "it", [](ui::AnyHandle const& item) { return item.at("id"); })) ==
          R"({"map":{"ref":"items"},"as":"it","to":{"ref":"it.id"}})");
    CHECK(text(ui::filter(items, &ResultView::outsideLimits)) ==
          R"({"filter":{"ref":"items"},"as":"item","where":{"ref":"item.outsideLimits"}})");
    CHECK(text(ui::env("widthClass")) == R"({"env":"widthClass"})");
    CHECK(text(ui::session("displayName")) == R"({"session":"displayName"})");
    CHECK(text(ui::hostValue("build")) == R"({"host":"build"})");
    CHECK(text(ui::prop("series")) == R"({"ref":"props.series"})");
    CHECK(text(ui::event()) == R"({"ref":"event"})");
    auto const mass = ui::element("mass");
    CHECK(text(mass.value()) == R"({"ref":"mass.value"})");
    CHECK(text(mass.draft()) == R"({"ref":"mass.draft"})");
    CHECK(text(mass.checked()) == R"({"ref":"mass.checked"})");
    CHECK(text(mass.selected()) == R"({"ref":"mass.selected"})");
    CHECK(text(mass.selection()) == R"({"ref":"mass.selection"})");
    CHECK(text(mass.open()) == R"({"ref":"mass.open"})");
    CHECK(text(mass.valid()) == R"({"ref":"mass.valid"})");
    CHECK(text(mass.ready()) == R"({"ref":"mass.ready"})");
    CHECK(text(mass.get("field.value")) == R"({"ref":"mass.field.value"})");
    CHECK(text(ui::Point{"density"}.pref()) == R"({"pref":"density"})");
    CHECK(text(ui::Point{"filter"}.pref("text")) == R"({"pref":"filter.text"})");
}

TEST_CASE("an arrange point names the column whose children it orders, and the screen validates", "[ui][author]") {
    ui::ScreenRegistry screens;
    auto const added = screens.add(ui::screen("author.arrange", [](ui::Scope& screen) {
        auto const board = ui::element("board");
        static_cast<void>(screen.arrangePoint("tiles", board));
        return ui::column({ui::text({.text = "sales"}).id(ui::element("sales")),
                           ui::text({.text = "stock"}).id(ui::element("stock"))})
            .id(board);
    }));
    INFO((added ? std::string{} : added.error().message()));
    REQUIRE(added.has_value());
    CHECK((*added)->text.contains(R"("tiles":{"kind":"arrange","of":"board"})"));
}

// Mutation: drop the separator branch of menuItem (the separator is then written as an empty entry).
TEST_CASE("a menu button writes its style and trigger, and a separator writes nothing else", "[ui][author]") {
    CHECK(text(ui::menuButton({.label = "More", .icon = "more", .emphasis = morph::ui::ButtonEmphasis::Ghost},
                              {{.label = "Print"},
                               ui::menuSeparator(),
                               {.label = "Retire", .description = "needs reason", .visible = false}})
                   .json()) ==
          R"({"kind":"menu","items":[{"label":"Print"},{"separator":true},)"
          R"({"label":"Retire","description":"needs reason","visible":false}],"style":"button","label":"More",)"
          R"("icon":"more","emphasis":"ghost"})");
}

// Mutation: drop `onRemove` from the badge's writer.
TEST_CASE("a removable badge writes its remove commands and label", "[ui][author]") {
    CHECK(text(ui::badge({.text = "Result", .onRemove = ui::notify("gone"), .removeLabel = "Drop Result"}).json())
              .contains(R"("onRemove":)"));
    CHECK(text(ui::badge({.text = "Result", .onRemove = ui::notify("gone"), .removeLabel = "Drop Result"}).json())
              .contains(R"("removeLabel":"Drop Result")"));
}

// Mutation: drop the tracks branch of grid (no `tracks` is written).
TEST_CASE("a grid's tracks are written as parts, each key only when given", "[ui][author]") {
    CHECK(text(ui::grid(ui::Nodes{}, {.tracks = {{.width = ui::fixed(3)},
                                                 {.width = ui::fixed(6), .align = morph::ui::Align::Center},
                                                 {}}})
                   .json()) ==
          R"({"kind":"grid","tracks":[{"width":{"lit":{"fixed":3}}},{"width":{"lit":{"fixed":6}},"align":"center"},)"
          R"({}]})");
}

// Mutation: drop the align branch of stackNode (the column's `align` is not written).
TEST_CASE("align, justify and alignSelf are written by name, and a screen using them validates", "[ui][author]") {
    CHECK(text(ui::column({ui::text({.text = "a"}).alignSelf(morph::ui::Align::End)},
                          {.align = morph::ui::Align::Center, .justify = morph::ui::Justify::Between})
                   .json()) ==
          R"({"kind":"column","children":[{"kind":"text","text":"a","alignSelf":"end"}],"align":"center",)"
          R"("justify":"between"})");
    CHECK(text(ui::row(ui::Nodes{}, {.align = morph::ui::Align::Stretch, .justify = morph::ui::Justify::Start})
                   .json()) == R"({"kind":"row","align":"stretch","justify":"start"})");
    CHECK(text(ui::grid(ui::Nodes{}, {.align = morph::ui::Align::Start}).json()) ==
          R"({"kind":"grid","align":"start"})");
    CHECK(text(ui::row(ui::Nodes{}).json()) == R"({"kind":"row"})");
    ui::ScreenRegistry screens;
    auto const added = screens.add(ui::screen("author.aligned", [](ui::Scope&) {
        return ui::column({ui::row(ui::Nodes{ui::text({.text = "x"}).alignSelf(morph::ui::Align::Start)},
                                   {.align = morph::ui::Align::End})},
                          {.align = morph::ui::Align::Center, .justify = morph::ui::Justify::Center});
    }));
    INFO((added ? std::string{} : added.error().message()));
    CHECK(added.has_value());
    auto const refused = screens.add(ui::screen(
        "author.alignedRoot", [](ui::Scope&) { return ui::column(ui::Nodes{}).alignSelf(morph::ui::Align::Center); }));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message().contains("`alignSelf` places a node in its row, column or grid"));
}

// Mutation: leave `align` out of tableNode's column parts (the column is written without it).
TEST_CASE("a table column's align and valign and the table's valign are written by name", "[ui][author]") {
    CHECK(text(ui::table(
                   ui::ref("rows"),
                   {.columns = {{.name = "area", .align = morph::ui::Align::End, .valign = morph::ui::Align::Start},
                                {.name = "id"}},
                    .valign = morph::ui::Align::End})
                   .json()) ==
          R"({"kind":"table","rows":{"ref":"rows"},"columns":[{"id":"area","align":"end","valign":"start"},)"
          R"({"id":"id"}],"valign":"end"})");
}

// Mutation: leave `style` out of button() (the item is written as a push button).
TEST_CASE("a button's style and selection are written, and the loader takes them", "[ui][author]") {
    auto const current = ui::ref<std::string>("current");
    CHECK(text(ui::button({.label = "A", .style = morph::ui::ButtonStyle::Item, .selected = ui::eq(current, "a")})
                   .json()) ==
          R"({"kind":"button","style":"item","label":"A","selected":{"fn":"eq","args":[{"ref":"current"},"a"]}})");
    CHECK(text(ui::button({.label = "P", .style = morph::ui::ButtonStyle::Push}).json()) ==
          R"({"kind":"button","style":"push","label":"P"})");
    CHECK(text(ui::button({.label = "P"}).json()) == R"({"kind":"button","label":"P"})");
}

template <typename M>
concept ResultBinds = requires(ui::Scope& screen, M mutation) { screen.let("r", mutation.result()); };

static_assert(ResultBinds<ui::MutationHandle<CaptureConcentration>>, "a mutation's result binds with let");

TEST_CASE("query and mutation handles read their state", "[ui][author]") {
    ui::QueryHandle<ListResults> const results{"results"};
    CHECK(text(results.pending()) == R"({"pending":"results"})");
    CHECK(text(results.error()) == R"({"error":"results"})");
    CHECK(text(results.errorKind()) == R"({"errorKind":"results"})");
    CHECK(text(results.can()) == R"({"can":"results"})");
    ui::QueryHandle<GetHeader> const header{"header"};
    CHECK(text(header.at(&Header::sampleNumber)) == R"({"ref":"header.sampleNumber"})");
    ui::MutationHandle<CaptureConcentration> const capture{"capture"};
    CHECK(text(capture.result()) == R"({"result":"capture"})");
    CHECK(text(capture.result().at(&ResultView::id)) == R"({"get":{"result":"capture"},"path":"id"})");
    CHECK(text(capture.pending()) == R"({"pending":"capture"})");
    CHECK(text(capture.error()) == R"({"error":"capture"})");
    CHECK(text(capture.errorKind()) == R"({"errorKind":"capture"})");
    CHECK(text(capture.can()) == R"({"can":"capture"})");
    CHECK(ui::MutationRef{capture}.name() == "capture");
}

TEST_CASE("the reference screen of spec 5 section 11 matches its golden", "[ui][author][golden]") {
    auto emitted = resultsScreen().write();
    REQUIRE(emitted.has_value());
    checkGolden("results", emitted->json);
    ui::ScreenRegistry registry;
    auto added = registry.add(resultsScreen());
    if (!added) {
        FAIL(added.error().message());
    } else {
        CHECK((*added)->id == "lims.results");
        CHECK(registry.find("lims.results") == *added);
        CHECK(registry.find("lims.other") == nullptr);
        CHECK(registry.size() == 1);
    }
}

namespace {

/// Every declaration, lifecycle key, component and customization point.
ui::Screen declarationsScreen() {
    return ui::screen("lims.declarations", [](ui::Scope& screen) {
        auto sampleId = screen.param<std::int64_t>("sampleId");
        auto copyOf = screen.param<std::optional<std::int64_t>>("copyOf");
        auto mode = screen.param<std::string>("mode");
        static_cast<void>(screen.param<bool>("readOnly"));
        static_cast<void>(screen.param<morph::math::Rational>("dilution"));
        static_cast<void>(screen.param<morph::ui::Quantity>("mass"));
        static_cast<void>(screen.param<morph::time::DateTime>("since"));
        static_cast<void>(screen.param<std::vector<std::string>>("tags"));
        static_cast<void>(screen.param<Header>("headerRecord"));

        auto sample = screen.model<SampleModel>("sample", {.instance = sampleId});
        auto working = screen.model<SampleModel>("working", {.instance = ui::Instance::Private});
        static_cast<void>(screen.model<SessionModel>("shared", {.instance = ui::Instance::Shared}));
        static_cast<void>(screen.model<SampleModel>("borrowed", {.instance = ui::Borrow{"sample"}}));
        auto session = screen.model<SessionModel>("session");

        auto unsaved = screen.state("unsaved", false);
        auto note = screen.state<std::string>("note");
        auto picked = screen.state<std::int64_t>("picked", ui::ref<std::int64_t>("sampleId"));
        auto optional = screen.state("optional", std::vector<std::string>{"turbidity"});
        auto anything = screen.state<ui::Any>("anything", ui::AnyHandle{1});
        auto total = screen.let("total", picked + 1);

        auto results = screen.query<ListResults>(
            "results", sample, {{&ListResults::sampleId, sampleId}},
            {.when = ui::engaged(sampleId), .debounce = 250ms, .refreshEvery = 30s, .staleTime = 5s});
        auto header = screen.query<GetHeader>("header", working);
        auto preview = screen.evaluate<EvaluateResult>("preview", working, {{&EvaluateResult::resultId, picked}});
        // An evaluation's value is the action body, its computed fields filled, so the handle reads the action.
        static_assert(std::same_as<decltype(preview)::Result, EvaluateResult>);
        CHECK(text(preview.at(&EvaluateResult::resultId)) == R"({"ref":"preview.resultId"})");

        static_cast<void>(screen.mutation<CaptureConcentration>(
            "capture", sample,
            {.body = {{&CaptureConcentration::resultId, picked}},
             .concurrency = ui::Mode::Serial,
             .serialKey = picked,
             .optimistic = ui::Optimistic{.query = results,
                                          .key = ui::ref("body.resultId"),
                                          .with = ui::obj({{"inputs", ui::ref("body.inputs")}})},
             .onSuccess = {ui::set(unsaved, true)},
             .onError = ui::notify(ui::t("lims.captureFailed", "Capture failed"), ui::Role::Error),
             .offlineQueue = true}));
        auto load = screen.mutation<LoadSample>("load", working, {.concurrency = ui::Mode::Latest});
        static_cast<void>(screen.mutation<HeaderSection>("section", working, {.concurrency = ui::Mode::Parallel}));
        auto signIn = screen.mutation<SignIn>("signIn", session);

        screen.watch(picked, ui::set(note, ""));
        screen.watch(ui::element("noteInput").value(), {ui::set(unsaved, true)}, ui::Source::User);
        screen.check({.name = "pickedPositive",
                      .assertion = picked > 0,
                      .message = ui::t("lims.picked", "Pick a sample"),
                      .targets = {ui::element("noteInput")},
                      .blocking = true});
        screen.check({.name = "anythingSet", .assertion = ui::engaged(anything)});

        screen.title(ui::t("lims.declarations", "Declarations"));
        screen.identity(sampleId);
        screen.dirty(unsaved);
        screen.onMount(ui::ifThen(ui::engaged(copyOf), ui::run(load, {{&LoadSample::id, copyOf}}), ui::run(load)));
        screen.onUnmount(ui::set(note, ""));
        screen.onClose(ui::ifThen(unsaved, ui::cancelClose()));
        screen.onSave(ui::run(load));
        screen.onBackendChange(ui::refetch(header));
        screen.key("Ctrl+S", ui::run(load));
        screen.showErrors(ui::ShowErrors::Touched);
        screen.prefetch(results);

        auto chart = screen.component("Chart", {.qml = "sha256:" + std::string(64, 'a'),
                                                .props = {{"title", ui::PropType::String},
                                                          {"series", ui::PropType::Json},
                                                          {"limit", ui::PropType::Exact},
                                                          {"count", ui::PropType::Int},
                                                          {"scale", ui::PropType::Number},
                                                          {"live", ui::PropType::Bool}},
                                                .events = {{"pointSelected", "key"}},
                                                .fallback = ui::text({.text = ui::prop("title")})});
        auto grid = screen.component("Grid", {.host = "ResultGrid", .version = 2, .fallback = ui::spacer()});
        auto columns = screen.columnsPoint("resultColumns", ui::element("resultsTable"));
        auto density = screen.choicePoint("density", {"comfortable", "compact"}, "comfortable");
        static_cast<void>(screen.choicePoint("theme", {"light", "dark"}));
        auto filter = screen.valuePoint<std::optional<std::string>>("resultFilter");
        static_cast<void>(screen.point("panels", "layout"));
        static_cast<void>(screen.point("filters", "saved"));

        return ui::column(
            {ui::textInput({.value = note, .label = ui::t("lims.note", "Note")}).id(ui::element("noteInput")),
             ui::text({.text = ui::format("{0}", {total})}),
             ui::text({.text = ui::concat({mode, density.pref(), filter.pref()})}),
             ui::text({.text = ui::cond<ui::Any>(ui::includes(optional, "turbidity"), "yes", "no")})
                 .visible(preview.pending()),
             ui::table(results.value(), {.key = ui::row(&ResultView::id)})
                 .id(ui::element("resultsTable"))
                 .a11y("Results"),
             ui::customize(columns),
             ui::custom(chart, {.props = {{"title", "Trend"}, {"series", results.value()}},
                                .on = {{"pointSelected", ui::set(picked, ui::event())}}})
                 .a11y("Trend"),
             ui::custom(grid).a11y("Grid"), ui::button({.label = "Sign in", .onClick = ui::signIn(signIn)})});
    });
}

// Mutation: write `x` from the `xLabel` reader in `ui::chart` (the series' `x` then reads the run).
TEST_CASE("author: a chart writes its series per point from the row, and its lines", "[ui][author]") {
    ui::Node const node =
        ui::chart({.summary = ui::ref("trendSummary"),
                   .series = {{.label = "25 °C",
                               .points = ui::ref("assay25"),
                               .y = [](ui::AnyHandle const& point) { return point.at("value"); },
                               .x = [](ui::AnyHandle const& point) { return point.at("month"); },
                               .xLabel = [](ui::AnyHandle const& point) { return point.at("run"); },
                               .joined = false}},
                   .lines = {{.value = 95, .label = "LSL 95.0", .tone = "err", .style = ui::LineStyle::Dashed}},
                   .yPlaces = 1,
                   .yLabel = "Assay, % LC"});
    CHECK(text(node) == R"({"kind":"chart","summary":{"ref":"trendSummary"},"series":[{"label":"25 °C",)"
                        R"("points":{"ref":"assay25"},"y":{"row":"value"},"x":{"row":"month"},"xLabel":{"row":"run"},)"
                        R"("joined":false}],"lines":[{"value":95,"label":"LSL 95.0","tone":"err","style":"dashed"}],)"
                        R"("yPlaces":1,"yLabel":"Assay, % LC"})");
}

/// Every node kind of the palette, and the properties every node takes.
ui::Screen paletteScreen() {
    return ui::screen("lims.palette", [](ui::Scope& screen) {
        auto sample = screen.model<SampleModel>("sample", {.instance = ui::Instance::Private});
        auto header = screen.query<GetHeader>("header", sample);
        auto results = screen.query<ListResults>("results", sample);
        auto pick = screen.mutation<LoadSample>("pick", sample);
        auto choice = screen.state<std::string>("choice");
        auto checked = screen.state("checked", true);
        auto confirm = ui::element("confirm");
        auto side = ui::element("side");
        auto density = screen.choicePoint("density", {"comfortable", "compact"});

        auto inputs = ui::column({ui::text({.text = "Plain", .role = "heading", .stale = header.pending()}),
                                  ui::textInput({.value = choice,
                                                 .label = "Choice",
                                                 .placeholder = "Type",
                                                 .mode = "multiline",
                                                 .commitOn = ui::Commit::Enter,
                                                 .onChange = ui::set(choice, ui::event()),
                                                 .onCommit = ui::set(checked, false),
                                                 .onSubmit = ui::open(confirm)})
                                      .readonly(false)
                                      .required(true)
                                      .errors(ui::list({}))
                                      .stale(false),
                                  ui::textInput({.value = choice, .label = "Blur", .commitOn = ui::Commit::Blur}),
                                  ui::textInput({.label = "Change", .commitOn = ui::Commit::Change}),
                                  ui::checkbox({.label = "Checked",
                                                .checked = checked,
                                                .onToggle = ui::set(checked, ui::event()),
                                                .onChange = ui::set(checked, true)}),
                                  ui::select({.options = ui::list({ui::obj({{"key", "a"}, {"label", "A"}})}),
                                              .selected = choice,
                                              .label = "Pick",
                                              .style = "radio",
                                              .onStale = ui::OnStale::Clear,
                                              .onChoose = ui::set(choice, ui::event()),
                                              .onChange = ui::set(choice, "x")}),
                                  ui::select({.label = "Keep", .onStale = ui::OnStale::Keep}),
                                  ui::dateTimeInput(ui::ref("choice"), "When", ui::set(choice, "")),
                                  ui::slider({.value = 1,
                                              .label = "Level",
                                              .min = 0,
                                              .max = 10,
                                              .step = 1,
                                              .onChange = ui::set(checked, true),
                                              .onCommit = ui::set(checked, false)}),
                                  ui::filePicker({.label = "File",
                                                  .accept = ".csv",
                                                  .multiple = false,
                                                  .mode = "open",
                                                  .onPick = ui::set(choice, "picked")})},
                                 {.gap = 8});

        auto layout = ui::row(
            {ui::grid({ui::spacer().span(2)}, {.gap = 4, .columns = 3}),
             ui::panel({ui::text({.text = "In a panel"})},
                       {.title = "Panel", .padding = 8, .collapsible = true, .collapsed = false}),
             ui::scroll({ui::text({.text = "Scrolled"})}, "vertical"), ui::scroll({ui::spacer()}),
             ui::switchOn(choice,
                          {ui::Case{"a", ui::text({.text = "A"})},
                           ui::Case{"b",
                                    [](ui::DeclScope& caseScope) {
                                        auto local = caseScope.state("local", 1);
                                        return ui::text({.text = ui::format("{0}", {local})});
                                    }}},
                          ui::text({.text = "Neither"})),
             ui::switchOn(choice, {ui::Case{"c", ui::Nodes{}}}),
             ui::tabs({{.label = "One", .children = ui::text({.text = "First"})},
                       {.label = "Two", .children = ui::text({.text = "Second"})}}),
             ui::splitter({ui::spacer(), ui::spacer()}, "horizontal", ui::list({1, 2})), ui::splitter({ui::spacer()})},
            {.gap = 4});

        auto menus =
            ui::menu({{.label = "File",
                       .items = {{.label = "New", .icon = "plus", .keys = "Ctrl+N", .onClick = ui::open(side)},
                                 {.label = "Wrap", .checked = checked, .enabled = true}}}});

        auto overlays =
            ui::column({ui::dialog(confirm,
                                   {.title = "Confirm",
                                    .open = false,
                                    .showErrors = ui::ShowErrors::Submit,
                                    .defaultAction = ui::close(ui::self),
                                    .cancel = ui::close(confirm),
                                    .onDismiss = ui::set(checked, false)},
                                   [](ui::DeclScope& dialogScope) {
                                       auto reason = dialogScope.state<std::string>("reason");
                                       return ui::Nodes{ui::textInput({.value = reason, .label = "Reason"})};
                                   }),
                        ui::dialog(ui::element("plain"), {ui::text({.text = "Plain dialog"})}, {.title = "Plain"}),
                        ui::drawer(side, {ui::text({.text = "Side"})}, {.side = "end"}).a11y("Side panel"),
                        ui::busy(header.pending(), "Loading"), ui::busy(false)});

        auto lists = ui::column(
            {ui::forEach(results.value(), {.key = ui::row(&ResultView::id), .axis = "vertical", .gap = 2},
                         [](ui::RowScope& rowScope) {
                             auto rowSample =
                                 rowScope.model<SampleModel>("rowSample", {.instance = ui::row(&ResultView::id)});
                             auto rowHeader = rowScope.query<GetHeader>("rowHeader", rowSample);
                             return ui::text({.text = rowHeader.at(&Header::sampleNumber)});
                         }),
             ui::table(results.value(), {.key = ui::row(&ResultView::id),
                                         .columns = {{.name = "id",
                                                      .label = "Id",
                                                      .width = 80,
                                                      .cell = ui::text({.text = ui::row("id")}),
                                                      .sortable = true,
                                                      .sortBy = ui::row("id")},
                                                     {.name = "flag"}},
                                         .selectionMode = ui::Selection::Multiple,
                                         .sort = ui::list({ui::sortKey("id", ui::SortOrder::Descending)}),
                                         .onSortChange = ui::set(choice, "sorted"),
                                         .rowTone = "neutral",
                                         .onActivate = ui::set(choice, ui::row("id"))})
                 .a11y("Results"),
             ui::dataTable<ListProjects>(sample, {{&ListProjects::page, 0}},
                                         {.selectionMode = ui::Selection::Single,
                                          .mode = ui::TableMode::Server,
                                          .sort = ui::list({}),
                                          .filters = ui::list({}),
                                          .highlightKey = ui::ref("choice"),
                                          .onActivate = ui::run(pick, {{&LoadSample::id, ui::row("id")}})})
                 .a11y("Projects"),
             ui::dataTable<ListProjects>(sample, {.selectionMode = ui::Selection::None, .mode = ui::TableMode::Client})
                 .a11y("All projects")});

        auto feedback = ui::column(
            {ui::banner({.tone = "warning",
                         .text = "Careful",
                         .dismissible = true,
                         .action = ui::ActionPart{.label = "Undo", .onClick = ui::set(checked, false)},
                         .onDismiss = ui::set(checked, true)}),
             ui::banner({.text = "Bare", .action = ui::ActionPart{}}), ui::badge("New", "info", "star"),
             ui::badge("Plain"), ui::progress(ui::AnyHandle{nullptr}, "Working"), ui::progress(1),
             ui::steps(ui::list({}), 0), ui::steps(ui::list({})), ui::keyValue(ui::list({})),
             ui::chart(
                 {.summary = "Runs within limits",
                  .series = {{.points = ui::list({}), .y = [](ui::AnyHandle const& run) { return run.at("value"); }}},
                  .lines = {{.value = 100, .label = "mean"}}}),
             ui::emptyState({.title = "Nothing",
                             .text = "No rows",
                             .icon = "empty",
                             .action = ui::ActionPart{.label = "Add", .onClick = ui::set(checked, true)}}),
             ui::emptyState({.title = "Bare"}),
             ui::collapsible({ui::text({.text = "Inside"})}, {.title = "More",
                                                              .open = checked,
                                                              .header = ui::badge("3"),
                                                              .onToggle = ui::set(checked, ui::event())}),
             ui::collapsible({ui::spacer()}),
             ui::dropZone({ui::text({.text = "Drop here"})},
                          {.accept = ".csv", .multiple = true, .onDrop = ui::set(choice, "dropped")}),
             ui::dropZone({ui::spacer()}), ui::customize(density),
             ui::boundary({ui::text({.text = header.at(&Header::sampleNumber)})}, ui::text({.text = "Loading"}),
                          {ui::text({.text = ui::ref("boundary.error")}),
                           ui::button({.label = "Retry", .onClick = ui::retry()})}),
             ui::boundary({ui::spacer()})});

        auto forms = ui::column(
            {ui::form<HeaderSection>(
                 sample,
                 {.prefill = header.value(),
                  .submit = "explicit",
                  .showErrors = ui::ShowErrors::Always,
                  .fields = {{&HeaderSection::sampleNumber,
                              {.hidden = false, .label = ui::t("lims.number", "Number"), .widget = "text"}},
                             {&HeaderSection::project,
                              {.omit = false, .unit = "mg", .decimals = 2, .blankAs = ui::AnyHandle{nullptr}}}},
                  .onSubmit = ui::set(checked, true),
                  .onSuccess = ui::set(checked, false),
                  .onError = ui::notify("Failed", ui::Role::Warning)})
                 .id(ui::element("headerForm")),
             ui::form<HeaderSection>(sample), ui::collection<ListProjects>(sample),
             ui::wizard<HeaderSection>(sample)});

        auto common = ui::button({.label = "Common", .icon = "gear", .onActivate = ui::focus(ui::element("common"))})
                          .id(ui::element("common"))
                          .visible(checked)
                          .enabled(!checked)
                          .tooltip("Tip")
                          .sizing("content")
                          .autofocus(true)
                          .testId("common-button")
                          .surface("accent")
                          .a11y("Common", "button")
                          .drag(ui::ref("choice"))
                          .drop(ui::AnyHandle{"text"}, ui::set(choice, ui::event()))
                          .drop({}, ui::set(choice, "any"))
                          .key("Ctrl+Shift+C", ui::set(checked, true))
                          .key("Alt+X", ui::set(checked, false));

        return ui::column({inputs, layout, menus, overlays, lists, feedback, forms, common,
                           ui::button({.icon = "x"}).a11y(ui::t("lims.close", "Close"), {})});
    });
}

/// Every command of ui/1.
ui::Screen commandsScreen() {
    return ui::screen("lims.commands", [](ui::Scope& screen) {
        auto sample = screen.model<SampleModel>("sample", {.instance = ui::Instance::Private});
        auto session = screen.model<SessionModel>("session");
        auto header = screen.query<GetHeader>("header", sample);
        auto load = screen.mutation<LoadSample>("load", sample);
        auto upload = screen.mutation<UploadFile>("upload", sample);
        auto signIn = screen.mutation<SignIn>("signIn", session);
        auto note = screen.state<std::string>("note");
        auto url = screen.state<std::string>("url");
        auto ids = screen.state("ids", std::vector<std::int64_t>{1, 2});
        auto density = screen.choicePoint("density", {"comfortable", "compact"});
        auto dialog = ui::element("details");
        screen.onClose(ui::seq({ui::cancelClose(), ui::signOut()}));

        auto commands = std::vector<ui::Command>{
            ui::set(note, "text"),
            ui::reset(note),
            ui::run(load),
            ui::run(load, {{&LoadSample::id, 7}}, {.as = "loaded"}),
            ui::run(load, {{&LoadSample::id, ui::ref<std::int64_t>("each")}}, {.each = ids, .as = "each"}),
            ui::refetch(header),
            ui::open(dialog),
            ui::close(dialog),
            ui::navigate("lims.sample"),
            ui::navigate("lims.sample", {.params = ui::obj({{"sampleId", 1}}),
                                         .as = ui::OpenAs::Dialog,
                                         .onResult = ui::set(note, ui::event()),
                                         .onCancel = ui::set(note, "cancelled")}),
            ui::navigate("lims.tab", {.as = ui::OpenAs::Tab}),
            ui::navigate("lims.drawer", {.as = ui::OpenAs::Drawer}),
            ui::navigateRoute(ui::concat({"/samples/", note}), {.as = ui::OpenAs::Window}),
            ui::returnWith(note),
            ui::focus(ui::element("noteInput")),
            ui::host("print"),
            ui::host("export", ui::obj({{"format", "csv"}})),
            ui::switchBackend("local"),
            ui::resetPrefs(density),
            ui::resetAllPrefs(),
            ui::setLocale("de"),
            ui::notify("Saved"),
            ui::notify("Done", ui::Role::Success),
            ui::seq({ui::run(load, {}, {.as = "first"}), ui::set(note, ui::ref("first"))}),
            ui::all({ui::refetch(header), ui::run(load)}),
            ui::ifThen(ui::isEmpty(note), ui::set(note, "empty")),
            ui::ifThen(ui::isEmpty(note), ui::set(note, "empty"), {ui::reset(note), ui::refetch(header)}),
            ui::signIn(signIn),
            ui::upload(ui::event(), upload),
            ui::upload(ui::event(), upload, {.as = "uploaded", .transfer = "transfer"}),
            ui::downloadToFile(ui::ref("url")),
            ui::downloadToFile(ui::ref("url"), "report.csv"),
            ui::downloadToPreview(ui::ref("url"), url),
        };
        std::vector<ui::Node> buttons;
        buttons.reserve(commands.size() + 2);
        std::int64_t index = 0;
        for (ui::Command const& command : commands) {
            buttons.push_back(ui::button({.label = ui::format("{0}", {index++}), .onClick = command}));
        }
        buttons.push_back(ui::textInput({.value = note, .label = "Note"}).id(ui::element("noteInput")));
        buttons.push_back(ui::dialog(dialog, {ui::button({.label = "Close", .onClick = ui::close(ui::self)})},
                                     {.title = "Details"}));
        return ui::column(ui::Nodes{buttons});
    });
}

}  // namespace

TEST_CASE("declarations, lifecycle keys, components and points match their golden", "[ui][author][golden]") {
    auto emitted = declarationsScreen().write();
    REQUIRE(emitted.has_value());
    checkGolden("declarations", emitted->json);
    auto const refusal = refusalOf(declarationsScreen());
    if (refusal) {
        FAIL(refusal->message());
    }
}

TEST_CASE("every node kind matches its golden", "[ui][author][golden]") {
    auto emitted = paletteScreen().write();
    REQUIRE(emitted.has_value());
    checkGolden("palette", emitted->json);
    auto const refusal = refusalOf(paletteScreen());
    if (refusal) {
        FAIL(refusal->message());
    }
}

TEST_CASE("every command matches its golden", "[ui][author][golden]") {
    auto emitted = commandsScreen().write();
    REQUIRE(emitted.has_value());
    checkGolden("commands", emitted->json);
    auto const refusal = refusalOf(commandsScreen());
    if (refusal) {
        FAIL(refusal->message());
    }
}

TEST_CASE("a builder error refuses the screen at its root", "[ui][author][registration]") {
    // A field of a computed int is written as a `get`, which the loader refuses where it is read.
    auto const belowComputed = refusalOf(ui::screen("author.computed", [](ui::Scope& screen) {
        auto value = screen.state("value", std::int64_t{1});
        return ui::text({.text = (value + 1).at("x")});
    }));
    REQUIRE(belowComputed.has_value());
    CHECK(belowComputed->document == "author.computed");
    CHECK(belowComputed->path == "/view/text");
    CHECK(belowComputed->reason.contains("`get` reads a path out of a record or a list, not int"));

    auto const setComputed = refusalOf(ui::screen("author.setComputed", [](ui::Scope& screen) {
        auto value = screen.state("value", std::int64_t{1});
        return ui::button({.label = "Set", .onClick = ui::set(value + 1, 2)});
    }));
    REQUIRE(setComputed.has_value());
    CHECK(setComputed->reason == "`set` names a state, an element or a declared value");

    auto const optimisticWithoutQuery = refusalOf(ui::screen("author.optimistic", [](ui::Scope& screen) {
        auto sample = screen.model<SampleModel>("sample", {.instance = ui::Instance::Private});
        static_cast<void>(screen.mutation<LoadSample>("load", sample, {.optimistic = ui::Optimistic{}}));
        return ui::spacer();
    }));
    REQUIRE(optimisticWithoutQuery.has_value());
    CHECK(optimisticWithoutQuery->reason == "an `optimistic` write names the query whose row it writes");

    auto const thrown = refusalOf(ui::screen("author.thrown", [](ui::Scope&) -> ui::Node { throw 42; }));
    REQUIRE(thrown.has_value());
    CHECK(thrown->reason == "the screen's builder threw an exception that is not a std::exception");
}

TEST_CASE("registration runs the load-time validation with the builders' catalog", "[ui][author][registration]") {
    SECTION("an unresolved reference") {
        auto const refusal = refusalOf(ui::screen("author.unresolved", [](ui::Scope&) {
            return ui::button({.label = "Set", .onClick = ui::set(ui::ref("missing"), 1)});
        }));
        REQUIRE(refusal.has_value());
        CHECK(refusal->path == "/view/onClick/0");
    }
    SECTION("an instance key on a model that is not keyed") {
        auto const refusal = refusalOf(ui::screen("author.keyed", [](ui::Scope& screen) {
            auto user = screen.param<std::string>("user");
            static_cast<void>(screen.model<SessionModel>("session", {.instance = user}));
            return ui::spacer();
        }));
        REQUIRE(refusal.has_value());
        CHECK(refusal->path == "/models/session/instance");
    }
    SECTION("an optimistic write to a field the server calculates") {
        auto const refusal = refusalOf(ui::screen("author.optimistic", [](ui::Scope& screen) {
            auto sample = screen.model<SampleModel>("sample", {.instance = ui::Instance::Private});
            auto results = screen.query<ListResults>("results", sample);
            static_cast<void>(screen.mutation<CaptureConcentration>(
                "capture", sample,
                {.optimistic = ui::Optimistic{
                     .query = results, .key = ui::ref("body.resultId"), .with = ui::obj({{"concentration", 1}})}}));
            return ui::spacer();
        }));
        REQUIRE(refusal.has_value());
        CHECK(refusal->path == "/mutations/capture/optimistic/with/obj/concentration");
    }
    SECTION("an optimistic write to a query whose action answers a record, without `at`") {
        auto const refusal = refusalOf(ui::screen("author.patchAt", [](ui::Scope& screen) {
            auto const sample = screen.model<SampleModel>("sample", {.instance = ui::Instance::Private});
            auto samples = screen.query<ListSamples>("samples", sample);
            static_cast<void>(screen.mutation<CaptureConcentration>(
                "capture", sample,
                {
                    .optimistic =
                        ui::Optimistic{.query = samples, .key = ui::ref("body.resultId"), .with = ui::ref("body")},
                }));
            return ui::spacer();
        }));
        REQUIRE(refusal.has_value());
        CHECK(refusal->path == "/mutations/capture/optimistic");
    }
    SECTION("an `at` that is not one of the result's lists") {
        auto const refusal = refusalOf(ui::screen("author.patchAt", [](ui::Scope& screen) {
            auto const sample = screen.model<SampleModel>("sample", {.instance = ui::Instance::Private});
            auto samples = screen.query<ListSamples>("samples", sample);
            static_cast<void>(screen.mutation<CaptureConcentration>("capture", sample,
                                                                    {
                                                                        .optimistic =
                                                                            ui::Optimistic{
                                                                                .query = samples,
                                                                                .at = "total",
                                                                                .key = ui::ref("body.resultId"),
                                                                                .with = ui::ref("body"),
                                                                            },
                                                                    }));
            return ui::spacer();
        }));
        REQUIRE(refusal.has_value());
        CHECK(refusal->path == "/mutations/capture/optimistic/at");
    }
    SECTION("a credential-bearing mutation queued offline") {
        auto const refusal = refusalOf(ui::screen("author.offline", [](ui::Scope& screen) {
            auto session = screen.model<SessionModel>("session");
            static_cast<void>(screen.mutation<SignIn>("signIn", session, {.offlineQueue = true}));
            return ui::spacer();
        }));
        REQUIRE(refusal.has_value());
        CHECK(refusal->path == "/mutations/signIn/offline");
    }
    SECTION("an accessibility rule") {
        auto const refusal = refusalOf(ui::screen(
            "author.icon", [](ui::Scope&) { return ui::column({ui::spacer(), ui::button({.icon = "x"})}); }));
        REQUIRE(refusal.has_value());
        CHECK(refusal->path == "/view/children/1");
        CHECK(refusal->reason ==
              "accessibility: a button without text has an `a11y.name`; an icon alone does not name it");
    }
}

TEST_CASE("the registry keeps one screen per id and answers the catalog", "[ui][author][registration]") {
    ui::ScreenRegistry registry;
    REQUIRE(registry.add(resultsScreen()).has_value());
    auto const duplicate = registry.add(resultsScreen());
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error().path == "/id");
    REQUIRE(registry.add(commandsScreen()).has_value());
    CHECK(registry.size() == 2);
    CHECK(registry.find("lims.commands")->text.starts_with(R"({"vocab":"ui/1","id":"lims.commands")"));

    morph::ui::Catalog const catalog = registry.catalog();
    auto const sample = catalog.model("Lims_SampleModel");
    REQUIRE(sample.has_value());
    CHECK(sample->keyed);
    CHECK_FALSE(sample->stateless);
    auto const session = catalog.model("Auth_SessionModel");
    REQUIRE(session.has_value());
    CHECK(session->stateless);
    CHECK_FALSE(session->keyed);
    CHECK_FALSE(catalog.model("Unknown").has_value());
    auto const evaluate = catalog.action("Lims_EvaluateResult");
    REQUIRE(evaluate.has_value());
    CHECK(evaluate->modelType == "Lims_SampleModel");
    auto const signIn = catalog.action("Auth_SignIn");
    REQUIRE(signIn.has_value());
    CHECK(signIn->credential);
    CHECK(signIn->secretFields == std::vector<std::string>{"token"});
    CHECK_FALSE(catalog.action("Unknown").has_value());
}

TEST_CASE("an optimistic write names the list inside a record result with `at`, and the catalog knows the lists",
          "[ui][author][registration]") {
    ui::Screen const screen{
        "author.patchAt", [](ui::Scope& scope) {
            auto const sample = scope.model<SampleModel>("sample", {.instance = ui::Instance::Private});
            auto samples = scope.query<ListSamples>("samples", sample);
            static_cast<void>(scope.mutation<CaptureConcentration>("capture", sample,
                                                                   {
                                                                       .optimistic =
                                                                           ui::Optimistic{
                                                                               .query = samples,
                                                                               .at = "samples",
                                                                               .key = ui::ref("body.resultId"),
                                                                               .with = ui::ref("body"),
                                                                           },
                                                                   }));
            return ui::spacer();
        }};
    ui::ScreenRegistry registry;
    auto const added = registry.add(screen);
    if (!added) {
        FAIL(added.error().message());
    }
    CHECK(registry.find("author.patchAt")
              ->text.contains(R"("optimistic":{"query":"samples","at":"samples","key":{"ref":"body.resultId"})"));

    morph::ui::Catalog const catalog = registry.catalog();
    CHECK(catalog.action("Lims_ListSamples")->listFields == std::vector<std::string>{"samples"});
    CHECK(catalog.action("Lims_CaptureConcentration")->listFields == std::vector<std::string>{});
    ui::CatalogFacts facts;
    facts.action<SampleModel, ListResults>();
    facts.action<SampleModel, LoadSample>();
    // A bare list and a scalar are not records: the catalog does not say, so a patch is checked when it runs.
    CHECK_FALSE(facts.actions.at("Lims_ListResults").listFields.has_value());
    CHECK_FALSE(facts.actions.at("Lims_LoadSample").listFields.has_value());
}

TEST_CASE("catalog facts keep the first fact recorded", "[ui][author]") {
    ui::CatalogFacts first;
    first.actions.emplace("A", morph::ui::ActionInfo{.modelType = "M1"});
    first.models.emplace("M1", morph::ui::ModelInfo{.stateless = true});
    ui::CatalogFacts second;
    second.actions.emplace("A", morph::ui::ActionInfo{.modelType = "M2"});
    second.actions.emplace("B", morph::ui::ActionInfo{.modelType = "M2"});
    second.models.emplace("M1", morph::ui::ModelInfo{});
    first.merge(second);
    CHECK(first.actions.at("A").modelType == "M1");
    CHECK(first.actions.at("B").modelType == "M2");
    CHECK(first.models.at("M1").stateless);
}

TEST_CASE("nodes and commands accept JSON no builder spells", "[ui][author]") {
    Json json;
    json["kind"] = "spacer";
    ui::Node const node{json};
    CHECK(text(node.withText("testId", "raw")) == R"({"kind":"spacer","testId":"raw"})");
    CHECK(text(node.with("visible", {})) == R"({"kind":"spacer"})");
    CHECK(text(ui::Command{ui::AnyHandle{"x"}.json()}) == R"("x")");
    CHECK(text(ui::AnyHandle::raw(json)) == R"({"kind":"spacer"})");
    CHECK(ui::QueryRef{ui::QueryHandle<GetHeader>{"header"}}.name() == "header");
}

TEST_CASE("accessibility: inputs have a label or an a11y name", "[ui][author][a11y]") {
    for (std::string const kind : {"textInput", "dateTimeInput", "slider", "select", "checkbox", "filePicker"}) {
        CAPTURE(kind);
        auto const bare = accessibility(R"({"vocab": "ui/1", "id": "a", "view": {"kind": ")" + kind + R"("}})");
        REQUIRE(bare.has_value());
        CHECK(bare->document == "a");
        CHECK(bare->path == "/view");
        CHECK(bare->reason == "accessibility: a `" + kind + "` has a `label` or an `a11y.name`");
        CHECK_FALSE(
            accessibility(R"({"vocab": "ui/1", "id": "a", "view": {"kind": ")" + kind + R"(", "label": "Mass"}})")
                .has_value());
        CHECK_FALSE(accessibility(R"({"vocab": "ui/1", "id": "a", "view": {"kind": ")" + kind +
                                  R"(", "a11y": {"name": "Mass"}}})")
                        .has_value());
    }
    // A label written as an empty constant, or null, names nothing.
    CHECK(accessibility(R"({"vocab": "ui/1", "id": "a", "view": {"kind": "textInput", "label": ""}})").has_value());
    CHECK(accessibility(R"({"vocab": "ui/1", "id": "a", "view": {"kind": "textInput", "label": null}})").has_value());
    CHECK(accessibility(R"({"vocab": "ui/1", "id": "a", "view": {"kind": "textInput", "label": true}})") ==
          std::nullopt);
    CHECK_FALSE(accessibility(R"({"vocab": "ui/1", "id": "a",
        "view": {"kind": "textInput", "label": {"t": "k", "fallback": "Mass"}}})")
                    .has_value());
    CHECK(accessibility(R"({"vocab": "ui/1", "id": "a",
        "view": {"kind": "textInput", "a11y": {"role": "textbox"}}})")
              .has_value());
    CHECK(accessibility(R"({"vocab": "ui/1", "id": "a",
        "view": {"kind": "textInput", "a11y": {"name": ""}}})")
              .has_value());
}

TEST_CASE("accessibility: buttons, dialogs, tables and custom nodes are named", "[ui][author][a11y]") {
    auto const icon = accessibility(R"({"vocab": "ui/1", "id": "a", "view": {"kind": "button", "icon": "x"}})");
    REQUIRE(icon.has_value());
    CHECK(icon->reason == "accessibility: a button without text has an `a11y.name`; an icon alone does not name it");
    CHECK_FALSE(accessibility(R"({"vocab": "ui/1", "id": "a", "view": {"kind": "button", "label": "Go"}})"));
    CHECK_FALSE(accessibility(R"({"vocab": "ui/1", "id": "a",
        "view": {"kind": "button", "icon": "x", "a11y": {"name": "Close"}}})"));

    for (std::string const kind : {"dialog", "drawer"}) {
        CAPTURE(kind);
        auto const bare = accessibility(R"({"vocab": "ui/1", "id": "a", "view": {"kind": "column", "children": [
            {"kind": "spacer"}, {"kind": ")" +
                                        kind + R"(", "id": "d"}]}})");
        REQUIRE(bare.has_value());
        CHECK(bare->path == "/view/children/1");
        CHECK(bare->reason == "accessibility: a `" + kind + "` has a `title` or an `a11y.name`");
        CHECK_FALSE(accessibility(R"({"vocab": "ui/1", "id": "a", "view": {"kind": ")" + kind +
                                  R"(", "id": "d", "title": "Details"}})"));
        CHECK_FALSE(accessibility(R"({"vocab": "ui/1", "id": "a", "view": {"kind": ")" + kind +
                                  R"(", "id": "d", "a11y": {"name": "Details"}}})"));
    }

    auto const table =
        accessibility(R"({"vocab": "ui/1", "id": "a", "view": {"kind": "table", "rows": {"lit": []}}})");
    REQUIRE(table.has_value());
    CHECK(table->reason == "accessibility: a `table` has an `a11y.name`");
    CHECK_FALSE(accessibility(R"({"vocab": "ui/1", "id": "a",
        "view": {"kind": "table", "rows": {"lit": []}, "a11y": {"name": "Rows"}}})"));

    std::string const component =
        R"("components": {"Chart": {"host": "Chart", "version": 1, "fallback": {"kind": "spacer"}}})";
    auto const custom = accessibility(R"({"vocab": "ui/1", "id": "a", )" + component +
                                      R"(, "view": {"kind": "custom", "name": "Chart"}})");
    REQUIRE(custom.has_value());
    CHECK(custom->reason == "accessibility: a `custom` has an `a11y.name`");
    CHECK_FALSE(accessibility(R"({"vocab": "ui/1", "id": "a", )" + component +
                              R"(, "view": {"kind": "custom", "name": "Chart", "a11y": {"name": "Chart"}}})"));

    // A form field takes its field's label.
    CHECK_FALSE(accessibility(R"({"vocab": "ui/1", "id": "a", "models": {"m": {"type": "M"}},
        "view": {"kind": "form", "model": "m", "action": "A"}})"));
}

TEST_CASE("accessibility: every node of the view and of each fallback is walked", "[ui][author][a11y]") {
    auto const inTab = accessibility(R"({"vocab": "ui/1", "id": "a", "view": {"kind": "tabs",
        "tabs": [{"label": "One", "children": [{"kind": "spacer"}]}, {"label": "Two", "children": [{"kind": "checkbox"}]}]}})");
    REQUIRE(inTab.has_value());
    CHECK(inTab->path == "/view/tabs/1/children/0");

    auto const inCell = accessibility(R"({"vocab": "ui/1", "id": "a", "view": {"kind": "table", "rows": {"lit": []},
        "a11y": {"name": "Rows"}, "columns": [{"id": "c", "cell": {"kind": "slider"}}]}})");
    REQUIRE(inCell.has_value());
    CHECK(inCell->path == "/view/columns/0/cell");

    auto const inCase = accessibility(R"({"vocab": "ui/1", "id": "a", "view": {"kind": "switch", "on": 1,
        "cases": [{"when": 1, "children": [{"kind": "select"}]}]}})");
    REQUIRE(inCase.has_value());
    CHECK(inCase->path == "/view/cases/0/children/0");

    auto const inBoundary = accessibility(R"({"vocab": "ui/1", "id": "a", "view": {"kind": "boundary",
        "children": [{"kind": "spacer"}], "error": [{"kind": "button", "icon": "retry"}]}})");
    REQUIRE(inBoundary.has_value());
    CHECK(inBoundary->path == "/view/error/0");

    auto const inFallback = accessibility(R"({"vocab": "ui/1", "id": "a",
        "components": {"Chart": {"host": "Chart", "version": 1, "fallback": {"kind": "filePicker"}}},
        "view": {"kind": "spacer"}})");
    REQUIRE(inFallback.has_value());
    CHECK(inFallback->path == "/components/Chart/fallback");

    CHECK_FALSE(accessibility(R"({"vocab": "ui/1", "id": "a",
        "components": {"Chart": {"host": "Chart", "version": 1, "fallback": {"kind": "spacer"}}},
        "view": {"kind": "column", "children": [{"kind": "text", "text": "x"}],
                 "a11y": {"name": "unused"}}})"));
    CHECK_FALSE(accessibility(R"({"vocab": "ui/1", "id": "app"})"));
}

namespace {

ui::Screen registeredScreen() {
    return ui::screen("author.registered", [](ui::Scope&) { return ui::spacer(); });
}

ui::Screen refusedScreen() {
    return ui::screen("author.refused", [](ui::Scope&) { return ui::button({}); });
}

}  // namespace

// enrollScreen is noexcept; clang-tidy cannot see that through the macro's template call.
// NOLINTBEGIN(bugprone-throwing-static-initialization,cert-err58-cpp)
MORPH_REGISTER_SCREEN(registeredScreen())
MORPH_REGISTER_SCREEN(refusedScreen())
// NOLINTEND(bugprone-throwing-static-initialization,cert-err58-cpp)

TEST_CASE("MORPH_REGISTER_SCREEN enrols at static initialisation and keeps refusals", "[ui][author][registration]") {
    ui::ScreenRegistry const& registry = ui::screenRegistry();
    CHECK(registry.find("author.registered") != nullptr);
    CHECK(registry.find("author.refused") == nullptr);
    CHECK(registry.lost() == 0);
    REQUIRE(registry.refusals().size() == 1);
    CHECK(registry.refusals().front().document == "author.refused");
    CHECK(registry.refusals().front().path == "/view");
}

// Mutation: let fieldOf throw below a value that is not a named read, as it once did; every `get` case fails.
TEST_CASE("a field of a computed value is read with get", "[ui][author]") {
    auto const xs = ui::ref("xs");
    auto const first = ui::call("first", {xs});
    CHECK(text(first.at("date")) == R"({"get":{"fn":"first","args":[{"ref":"xs"}]},"path":"date"})");
    // A field below a `get` extends its path: one node, not a `get` of a `get`.
    CHECK(text(first.at("a").at("b")) == R"({"get":{"fn":"first","args":[{"ref":"xs"}]},"path":"a.b"})");
    auto const view = ui::ref<ResultView>("view");
    CHECK(text(view.orElse(view).at(&ResultView::id)) ==
          R"({"get":{"fn":"coalesce","args":[{"ref":"view"},{"ref":"view"}]},"path":"id"})");
    CHECK(text(ui::list({1}).at("0")) == R"({"get":{"list":[1]},"path":"0"})");
    // A `t` carries companion keys, so it is not a `get` to extend.
    CHECK(text(ui::t("k", "Fallback").at("x")) == R"({"get":{"t":"k","fallback":"Fallback"},"path":"x"})");
}

TEST_CASE("only a ref or row read names a target", "[ui][author]") {
    // A row field is read, but never written.
    CHECK_THROWS_AS(ui::set(ui::row("x"), 1), ui::AuthoringError);
    CHECK_THROWS_AS(ui::reset(ui::env("locale")), ui::AuthoringError);
}

TEST_CASE("options left unset write no key", "[ui][author]") {
    CHECK(text(ui::spacer().a11y({}, "separator")) == R"({"kind":"spacer","a11y":{"role":"separator"}})");
    CHECK(text(ui::select({.label = "Pick"})) == R"({"kind":"select","label":"Pick"})");
    ui::ModelHandle<SampleModel> const sample{"sample"};
    CHECK(text(ui::dataTable<ListProjects>(sample)) ==
          R"({"kind":"table","model":"sample","action":"Lims_ListProjects"})");
}

TEST_CASE("dateTimeInput's options overload exposes mode, unlike the positional one", "[ui][author]") {
    CHECK(text(ui::dateTimeInput(ui::ref("choice"), "When")) ==
          R"({"kind":"dateTimeInput","value":{"ref":"choice"},"label":"When"})");
    CHECK(text(ui::dateTimeInput({.value = ui::ref("choice"), .label = "When", .mode = "date"})) ==
          R"({"kind":"dateTimeInput","value":{"ref":"choice"},"label":"When","mode":"date"})");
}

// ui::button() (not ui::ActionButtonOptions's actionButton()) has its own, separate emphasis-to-string mapping; a
// ghost button must write "ghost" from this site too, not just from the actionButton()/screen.action() path.
TEST_CASE("a button's own emphasis is written, including ghost", "[ui][author]") {
    CHECK(text(ui::button({.label = "Plain"})) == R"({"kind":"button","label":"Plain"})");
    CHECK(text(ui::button({.label = "Go", .emphasis = morph::ui::ButtonEmphasis::Primary})) ==
          R"({"kind":"button","emphasis":"primary","label":"Go"})");
    CHECK(text(ui::button({.label = "Drop", .emphasis = morph::ui::ButtonEmphasis::Danger})) ==
          R"({"kind":"button","emphasis":"danger","label":"Drop"})");
    CHECK(text(ui::button({.label = "Remove", .emphasis = morph::ui::ButtonEmphasis::Ghost})) ==
          R"({"kind":"button","emphasis":"ghost","label":"Remove"})");
}

TEST_CASE("a scope that declares one kind of thing writes it", "[ui][author]") {
    auto const emitted = ui::screen("author.scopes", [](ui::Scope& screen) {
                             auto sample = screen.model<SampleModel>("sample", {.instance = ui::Instance::Private});
                             auto withLet = [](ui::DeclScope& scope) {
                                 static_cast<void>(scope.let("doubled", ui::AnyHandle{2} * 2));
                                 return ui::spacer();
                             };
                             auto withQuery = [&sample](ui::DeclScope& scope) {
                                 static_cast<void>(scope.query<GetHeader>("inner", sample));
                                 return ui::spacer();
                             };
                             auto withMutation = [&sample](ui::DeclScope& scope) {
                                 static_cast<void>(scope.mutation<LoadSample>("innerLoad", sample));
                                 return ui::spacer();
                             };
                             auto withCheck = [](ui::DeclScope& scope) {
                                 scope.check({.name = "always", .assertion = true});
                                 return ui::spacer();
                             };
                             return ui::column({ui::dialog(ui::element("a"), {.title = "A"}, withLet),
                                                ui::dialog(ui::element("b"), {.title = "B"}, withQuery),
                                                ui::dialog(ui::element("c"), {.title = "C"}, withMutation),
                                                ui::dialog(ui::element("d"), {.title = "D"}, withCheck)});
                         }).write();
    REQUIRE(emitted.has_value());
    CHECK(emitted->text.contains(R"("scope":{"let":{"doubled")"));
    CHECK(emitted->text.contains(R"("scope":{"queries":{"inner")"));
    CHECK(emitted->text.contains(R"("scope":{"mutations":{"innerLoad")"));
    CHECK(emitted->text.contains(R"("scope":{"checks":[{"id":"always")"));
}

TEST_CASE("a component without a fallback is refused when the screen loads", "[ui][author][registration]") {
    auto const refusal = refusalOf(ui::screen("author.noFallback", [](ui::Scope& screen) {
        auto chart = screen.component("Chart", {.host = "Chart", .version = 1});
        return ui::custom(chart).a11y("Chart");
    }));
    REQUIRE(refusal.has_value());
    CHECK(refusal->path == "/components/Chart");
    CHECK(refusal->reason == "a component names a `fallback` of built-in nodes");
}

TEST_CASE("accessibility skips what a hand-built document leaves out", "[ui][author][a11y]") {
    morph::ui::Document document;
    document.id = "built";
    document.components.emplace("Chart", morph::ui::Component{});
    CHECK_FALSE(ui::checkAccessibility(document).has_value());
}

// Mutation: let enroll(Screen) drop the screen, or let a throwing maker escape enroll (it is noexcept: terminate).
TEST_CASE("a local registry enrols a screen directly, and counts a maker that throws outside the builder as lost",
          "[ui][author][registration]") {
    ui::ScreenRegistry registry;
    registry.enroll(registeredScreen());
    CHECK(registry.find("author.registered") != nullptr);
    registry.enroll([]() -> ui::Screen { throw std::runtime_error{"no memory"}; });
    CHECK(registry.lost() == 1);
    CHECK(registry.refusals().empty());
    registry.enroll(registeredScreen());  // a duplicate id is a refusal, not a loss
    CHECK(registry.refusals().size() == 1);
    CHECK(registry.lost() == 1);
}

// Mutation: spell parseDecimal's call with its arguments swapped.
TEST_CASE("parseDecimal builds the library call over the text and its places", "[ui][author]") {
    CHECK(text(ui::parseDecimal(ui::ref("typed"), 2)) == R"({"fn":"parseDecimal","args":[{"ref":"typed"},2]})");
}

// Mutation: leave `actions` out of the head (the document carries no action, and its run has nothing to name).
TEST_CASE("a screen's head writes its subtitle, status badges and actions beside the title", "[ui][author]") {
    auto const emitted = ui::screen("author.head", [](ui::Scope& screen) {
                             auto stage = screen.state<std::string>("stage", ui::AnyHandle{"Draft"});
                             screen.title(ui::AnyHandle{"Sample"});
                             screen.subtitle(stage);
                             screen.status({.text = ui::AnyHandle{"Draft"}, .tone = ui::AnyHandle{"warn"}});
                             screen.action({.id = "save",
                                            .label = ui::AnyHandle{"Save"},
                                            .emphasis = morph::ui::ButtonEmphasis::Primary,
                                            .onClick = {ui::set(stage, ui::AnyHandle{"Saved"})}});
                             return ui::spacer();
                         }).write();
    REQUIRE(emitted.has_value());
    CHECK(emitted->text.contains(R"("subtitle":{"ref":"stage"})"));
    CHECK(emitted->text.contains(R"("status":[{"tone":"warn","text":"Draft"}])"));
    CHECK(emitted->text.contains(
        R"("actions":[{"id":"save","label":"Save","emphasis":"primary","onClick":[{"set":"stage","value":"Saved"}]}])"));
}

// Mutation: leave the id out of a menu item's JSON (the head menu entry has nothing to be chosen by).
TEST_CASE("a screen's head writes its own menu, each entry with its id, and the menu's label", "[ui][author]") {
    auto const emitted =
        ui::screen("author.head.menu", [](ui::Scope& screen) {
            auto stage = screen.state<std::string>("stage", ui::AnyHandle{"Draft"});
            screen.title(ui::AnyHandle{"Instrument"});
            screen.menuLabel(ui::AnyHandle{"More"});
            screen.menu({.id = "calibrate",
                         .label = ui::AnyHandle{"Calibrate"},
                         .onClick = {ui::set(stage, ui::AnyHandle{"Calibrated"})}});
            screen.menu(ui::menuSeparator());
            screen.menu(
                {.id = "retire", .label = ui::AnyHandle{"Retire"}, .description = ui::AnyHandle{"needs reason"}});
            return ui::spacer();
        }).write();
    REQUIRE(emitted.has_value());
    CHECK(emitted->text.contains(R"("menuLabel":"More")"));
    CHECK(emitted->text.contains(
        R"("menu":[{"id":"calibrate","label":"Calibrate","onClick":[{"set":"stage","value":"Calibrated"}]},)"
        R"({"separator":true},{"id":"retire","label":"Retire","description":"needs reason"}])"));
}

// Mutation: write a head select's kind as `search`, or write a width of 0 (a key the default leaves out).
TEST_CASE("a screen's head writes its search and select tools in order", "[ui][author]") {
    auto const emitted = ui::screen("author.head.tools", [](ui::Scope& screen) {
                             auto query = screen.state<std::string>("query", ui::AnyHandle{""});
                             screen.title(ui::AnyHandle{"Samples"});
                             screen.tool(ui::HeadSearchOptions{.id = "q",
                                                               .label = ui::AnyHandle{"Search"},
                                                               .value = query,
                                                               .onChange = {ui::set(query, ui::event())},
                                                               .width = 24});
                             screen.tool(ui::HeadSelectOptions{
                                 .id = "lab", .label = ui::AnyHandle{"Lab"}, .options = ui::AnyHandle{"North"}});
                             return ui::spacer();
                         }).write();
    REQUIRE(emitted.has_value());
    CHECK(emitted->text.contains(
        R"("tools":[{"id":"q","kind":"search","label":"Search","width":24,"value":{"ref":"query"},)"
        R"("onChange":[{"set":"query","value":{"ref":"event"}}]},{"id":"lab","kind":"select","label":"Lab","options":"North"}])"));
}

// Mutation: leave the options' actions out of the overlay (the dialog carries no footer buttons).
TEST_CASE("a dialog's actions are written as its footer buttons, in its scope", "[ui][author]") {
    auto const emitted = ui::screen("author.dialog.actions", [](ui::Scope& screen) {
                             auto saved = screen.state<std::string>("saved", ui::AnyHandle{"no"});
                             return ui::dialog(ui::element("dlg"), {ui::text({.text = saved})},
                                               {.title = ui::AnyHandle{"Confirm"},
                                                .actions = {{.id = "ok",
                                                             .label = ui::AnyHandle{"OK"},
                                                             .emphasis = morph::ui::ButtonEmphasis::Primary,
                                                             .onClick = {ui::set(saved, ui::AnyHandle{"yes"})}}}});
                         }).write();
    REQUIRE(emitted.has_value());
    CHECK(emitted->text.contains(
        R"("actions":[{"id":"ok","label":"OK","emphasis":"primary","onClick":[{"set":"saved","value":"yes"}]}])"));
}

// Mutation: leave `spans` out of text() (the node is written without its runs).
TEST_CASE("a text's spans are written as runs that the loader takes", "[ui][author]") {
    auto const lot = ui::ref<std::string>("lot");
    CHECK(text(ui::text({.text = "Lot",
                         .spans = ui::spans({"Lot ", ui::mono(lot), ui::strong("!"),
                                             ui::styled("x", {.strong = true, .mono = true})}),
                         .role = "muted"})
                   .json()) ==
          R"({"kind":"text","text":"Lot","spans":{"list":["Lot ",{"obj":{"text":{"ref":"lot"},"mono":true}},)"
          R"({"obj":{"text":"!","strong":true}},{"obj":{"text":"x","strong":true,"mono":true}}]},"role":"muted"})");
    ui::ScreenRegistry screens;
    auto const added = screens.add(ui::screen("author.spans", [](ui::Scope&) {
        return ui::banner({.spans = ui::spans({ui::strong("Refused."), " Try again."})});
    }));
    INFO((added ? std::string{} : added.error().message()));
    CHECK(added.has_value());
}

// Mutation: leave `description` out of the dialog writer (the key is not written).
TEST_CASE("a dialog's description is written beside its title", "[ui][author]") {
    auto const sign = ui::element("sign");
    auto const json =
        text(ui::dialog(sign, {ui::text({.text = "x"})}, {.title = "Sign", .description = "Stored"}).json());
    CHECK(json.contains(R"("title":"Sign","description":"Stored")"));
}

// Mutation: drop the group rule from brokenRule (the unnamed group passes).
TEST_CASE("accessibility: a group has a label or an a11y.name", "[ui][author]") {
    auto const unnamed = accessibility(R"({"vocab": "ui/1", "id": "g", "view": {"kind": "group", "children": []}})");
    REQUIRE(unnamed.has_value());
    CHECK(unnamed->reason == "accessibility: a `group` has a `label` or an `a11y.name`");
    CHECK_FALSE(accessibility(R"({"vocab": "ui/1", "id": "g", "view": {"kind": "group", "label": "Conditions",
        "children": []}})")
                    .has_value());
    CHECK_FALSE(accessibility(R"({"vocab": "ui/1", "id": "g", "view": {"kind": "group",
        "a11y": {"name": "Conditions"}, "children": []}})")
                    .has_value());
}
