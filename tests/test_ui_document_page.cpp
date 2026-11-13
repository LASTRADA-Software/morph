// SPDX-License-Identifier: Apache-2.0

// A document page, its regions and a locale scope (docs/spec/ui/document_page.md): the sheet's scale and its regions'
// rectangles over the recording backend, the terminal's cell conversion, the refusals of the document form and of the
// mount, and the formatting of a subtree in another locale. Each case names the mutation that must fail it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <morph/reactive/runtime.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/page_layout.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/theme.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::Mm;
using ui::testing::Harness;
using ui::testing::readJson;

namespace {

// A4 with margins of 10 mm above, 20 mm right, 15 mm below and 25 mm left, in micrometres.
constexpr char const* kSheet = R"({
  "vocab": "ui/1", "id": "sheet",
  "view": { "kind": "page", "testId": "sheet", "width": 210000, "height": 297000,
    "margins": { "lit": { "top": 10000, "right": 20000, "bottom": 15000, "left": 25000 } },
    "a11y": { "name": "Invoice" },
    "children": [
      { "kind": "pageRegion", "testId": "head", "anchor": "top", "children": [{ "kind": "text", "text": "Letterhead" }] },
      { "kind": "pageRegion", "testId": "window", "anchor": "offset", "x": 20000, "y": 45000, "width": 85000,
        "height": 45000, "children": [{ "kind": "text", "text": "Address" }] },
      { "kind": "pageRegion", "testId": "foot", "anchor": "bottom", "children": [{ "kind": "text", "text": "Footer" }] } ] } })";

}  // namespace

// Mutations: compute the scale without min(1, ...) (the 300 mm case scales up to 1.4286); scale only the width (the
// displayed height keeps 297).
TEST_CASE("document page: a page is scaled down to the width it is given, never up", "[ui][page]") {
    Harness h;
    auto const screen = h.mount(kSheet);
    int const sheet = h.byTestId("Page", "sheet");
    CHECK(h.backend.prop(sheet, "size") == "210x297");
    CHECK(h.backend.prop(sheet, "margins") == "10,20,15,25");
    CHECK(h.backend.prop(sheet, "scale") == "1");
    CHECK(h.backend.prop(sheet, "displayed") == "210x297");
    h.backend.clearLog();
    h.backend.setPageAvailableWidth(Mm{105});
    CHECK(h.backend.prop(sheet, "scale") == "0.5");
    CHECK(h.backend.prop(sheet, "displayed") == "105x148.5");
    CHECK(h.backend.log().front() == "set Page#" + std::to_string(sheet) + " scale=0.5");
    h.backend.setPageAvailableWidth(Mm{300});
    CHECK(h.backend.prop(sheet, "scale") == "1");
    CHECK(h.backend.prop(sheet, "displayed") == "210x297");
    h.backend.setPageAvailableWidth(std::nullopt);
    CHECK(h.backend.prop(sheet, "scale") == "1");
}

// Mutations: place a Bottom region from the top of the margin box (y = 10); ignore the bottom margin (y = 285).
TEST_CASE("document page: a footer lies on the bottom margin whatever its height", "[ui][page]") {
    Harness h;
    auto const screen = h.mount(kSheet);
    int const foot = h.byTestId("PageRegion", "foot");
    int const head = h.byTestId("PageRegion", "head");
    CHECK(h.backend.prop(foot, "anchor") == "Bottom");
    h.backend.setRegionContentHeight(foot, Mm{12});
    CHECK(h.backend.prop(foot, "rect") == "25,270,165,12");
    h.backend.setRegionContentHeight(foot, Mm{30});
    CHECK(h.backend.prop(foot, "rect") == "25,252,165,30");
    // A Top region lies on the top margin.
    h.backend.setRegionContentHeight(head, Mm{20});
    CHECK(h.backend.prop(head, "rect") == "25,10,165,20");
}

// Mutation: measure the offset from the margin box (the window reads 45,55,...).
TEST_CASE("document page: an address window lies at its offset from the sheet's edge", "[ui][page]") {
    Harness h;
    auto const screen = h.mount(kSheet);
    int const window = h.byTestId("PageRegion", "window");
    CHECK(h.backend.prop(window, "rect") == "20,45,85,45");
    CHECK(h.backend.prop(window, "overflow") == "false");
}

// Mutation: leave the overflow out of the layout (a window whose content is taller records overflow=false).
TEST_CASE("document page: a region whose content does not fit is marked", "[ui][page]") {
    Harness h;
    auto const screen = h.mount(kSheet);
    int const window = h.byTestId("PageRegion", "window");
    h.backend.setRegionContentHeight(window, Mm{60});
    CHECK(h.backend.prop(window, "overflow") == "true");
    h.backend.setRegionContentHeight(window, Mm{40});
    CHECK(h.backend.prop(window, "overflow") == "false");
    // A content-sized footer taller than the sheet runs off its top edge.
    int const foot = h.byTestId("PageRegion", "foot");
    h.backend.setRegionContentHeight(foot, Mm{400});
    CHECK(h.backend.prop(foot, "overflow") == "true");
}

TEST_CASE("document page: the page is a document on the paper surface, its regions a plain order", "[ui][page]") {
    Harness h;
    auto const screen = h.mount(kSheet);
    int const sheet = h.byTestId("Page", "sheet");
    CHECK(h.backend.prop(sheet, "a11yRole") == "document");
    CHECK(h.backend.prop(sheet, "surface") == "paper");
    CHECK(h.backend.prop(sheet, "a11yName") == "Invoice");
    // The regions are the page's children in the document's order.
    auto const children = h.backend.children(sheet);
    REQUIRE(children.size() == 3);
    CHECK(children[0] == h.byTestId("PageRegion", "head"));
    CHECK(children[1] == h.byTestId("PageRegion", "window"));
    CHECK(children[2] == h.byTestId("PageRegion", "foot"));
}

// Mutation: round sizes instead of edges in toCells (the abutting regions drift apart by a cell).
TEST_CASE("page layout: the terminal converts millimetres to cells by their edges", "[ui][page]") {
    ui::CellPitch const pitch{};
    CHECK(ui::columnsOf(Mm{210}, pitch) == 83);
    CHECK(ui::rowsOf(Mm{297}, pitch) == 70);
    // The window of DIN 5008: 85 mm by 45 mm at (20, 45).
    ui::CellRect const window = ui::toCells(ui::PageRect{Mm{20}, Mm{45}, Mm{85}, Mm{45}}, pitch);
    CHECK(window == ui::CellRect{8, 11, 33, 10});
    // Two regions that touch in millimetres touch in cells.
    ui::CellRect const upper = ui::toCells(ui::PageRect{Mm{0}, Mm{0}, Mm{10}, Mm{10}}, pitch);
    ui::CellRect const lower = ui::toCells(ui::PageRect{Mm{0}, Mm{10}, Mm{10}, Mm{10}}, pitch);
    CHECK(upper.row + upper.height == lower.row);
    // A footer of three rows is three rows, lying on the rounded bottom edge.
    ui::CellRect const footer =
        ui::toCells(ui::PageRect{Mm{25}, Mm{270}, Mm{165}, Mm{12}}, ui::PageAnchor::Bottom, pitch, 3);
    CHECK(footer.height == 3);
    CHECK(footer.row + footer.height == ui::rowsOf(Mm{282}, pitch));
}

TEST_CASE("page layout: lengths are exact, and a geometry is refused where it makes no sheet", "[ui][page]") {
    CHECK(Mm{98.5}.micrometres() == 98'500);
    CHECK(Mm{98.5}.text() == "98.5");
    CHECK(Mm{210}.text() == "210");
    CHECK(Mm{0.001}.text() == "0.001");
    CHECK_THROWS_AS(Mm{std::numeric_limits<double>::infinity()}, std::invalid_argument);
    CHECK_THROWS_AS(Mm{std::nan("")}, std::invalid_argument);
    ui::PageGeometry const a4{.width = Mm{210}, .height = Mm{297}, .margins = ui::PageMargins{Mm{20}}};
    CHECK_FALSE(a4.problem().has_value());
    CHECK(ui::PageGeometry{.width = Mm{5}, .height = Mm{297}}.problem().has_value());
    CHECK(ui::PageGeometry{.width = Mm{210}, .height = Mm{297}, .margins = ui::PageMargins{Mm{105}}}.problem());
    ui::RegionGeometry window{.anchor = ui::PageAnchor::Offset, .x = Mm{20}, .y = Mm{45}, .width = Mm{85}};
    CHECK_FALSE(window.problem(a4).has_value());
    window.width = Mm{200};
    CHECK(window.problem(a4).has_value());
    CHECK(ui::RegionGeometry{.anchor = ui::PageAnchor::Top, .x = Mm{1}}.problem(a4).has_value());
    // An offset region with no stated width spans to the margin box's right edge (190 mm here), so it needs room.
    ui::RegionGeometry open{.anchor = ui::PageAnchor::Offset, .x = Mm{189}, .y = Mm{45}};
    CHECK_FALSE(open.problem(a4).has_value());
    open.x = Mm{190};
    CHECK(open.problem(a4).has_value());
}

// Mutation: add the micrometre counts in 32 bits (the sum wraps to a negative length).
TEST_CASE("page layout: adding or subtracting lengths holds at the largest and smallest length", "[ui][page]") {
    auto const highest = Mm::fromMicrometres(std::numeric_limits<std::int32_t>::max());
    auto const lowest = Mm::fromMicrometres(std::numeric_limits<std::int32_t>::lowest());
    auto const one = Mm::fromMicrometres(1);
    CHECK(highest + one == highest);
    CHECK(lowest - one == lowest);
    CHECK(Mm{210} - Mm{20} == Mm{190});
}

// Mutation: leave the orphaning out of the page's destructor (the region then releases itself from a page that is
// gone; run under AddressSanitizer).
TEST_CASE("page region: a region that outlives its page leaves nothing behind", "[ui][page]") {
    ui::testing::RecordingBackend backend;
    auto page = backend.createPage(nullptr, ui::PageGeometry{.width = Mm{210}, .height = Mm{297}});
    auto region = backend.createPageRegion(page.get(), ui::RegionGeometry{});
    page.reset();
    region.reset();
    SUCCEED();
}

namespace {

// A refusal of the document form, with the pointer it names.
void refusedAt(std::string const& view, std::string const& pointer) {
    auto const loaded = ui::loadDocumentText(R"({"vocab": "ui/1", "id": "r", "view": )" + view + "}");
    INFO(view);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == pointer);
}

}  // namespace

// Mutations: drop each check; the case that names it then loads.
TEST_CASE("document page: the document form refuses what a sheet cannot be", "[ui][page]") {
    auto const page = [](std::string const& extra, std::string const& children = "[]") {
        return R"({"kind": "page", "width": 210000, "height": 297000, )" + extra +
               R"("a11y": {"name": "x"}, "children": )" + children + "}";
    };
    refusedAt(page("", R"([{ "kind": "text", "text": "x" }])"), "/view/children/0");
    refusedAt(R"({"kind": "pageRegion", "anchor": "top"})", "/view");
    refusedAt(R"({"kind": "column", "children": [{"kind": "pageRegion", "anchor": "top"}]})", "/view/children/0");
    refusedAt(page(R"("margins": 110000, )"), "/view");
    refusedAt(page(R"("sizing": "content", )"), "/view/sizing");
    refusedAt(page(R"("minWidth": 5, )"), "/view/minWidth");
    refusedAt(page("", R"([{ "kind": "pageRegion", "anchor": "top", "x": 5000 }])"), "/view/children/0");
    refusedAt(page("", R"([{ "kind": "pageRegion", "anchor": "top", "sizing": "content" }])"),
              "/view/children/0/sizing");
    refusedAt(page("", R"([{ "kind": "pageRegion", "anchor": "offset", "x": 100000, "width": 200000 }])"),
              "/view/children/0");
    refusedAt(page("", R"([{ "kind": "pageRegion", "anchor": "middle" }])"), "/view/children/0/anchor");
    refusedAt(R"({"kind": "page", "width": 210.5, "height": 297000})", "/view/width");
    refusedAt(R"({"kind": "localeScope", "locale": "de", "visible": true, "children": []})", "/view/visible");
    refusedAt(R"({"kind": "localeScope", "children": []})", "/view");
    CHECK(ui::loadDocumentText(R"({"vocab": "ui/1", "id": "r", "view": )" +
                               page("", R"([{ "kind": "pageRegion", "anchor": "bottom" }])") + "}")
              .has_value());
}

TEST_CASE("document page: the mount refuses a view tree that is not a sheet", "[ui][page]") {
    morph::testing::StepExecutor owner;
    morph::reactive::Runtime runtime{owner};
    ui::testing::RecordingBackend backend;
    ui::PageGeometry const a4{.width = Mm{210}, .height = Mm{297}, .margins = ui::PageMargins{Mm{10}}};
    auto const region = [](ui::RegionGeometry geometry) {
        return ui::pageRegion(ui::PageRegion{.geometry = geometry});
    };
    auto const mountThrows = [&](ui::Node node) {
        CHECK_THROWS_AS((ui::Mounted{runtime, backend, std::move(node)}), std::logic_error);
    };
    // A page holds regions and nothing else.
    mountThrows(ui::page(ui::Page{.geometry = a4, .children = {ui::text({.text = "x"})}}));
    // A region belongs in a page.
    mountThrows(region({}));
    mountThrows(ui::column({.children = {region({})}}));
    // No offset on a pinned region, and no region leaving the sheet.
    mountThrows(ui::page(ui::Page{.geometry = a4, .children = {region({.anchor = ui::PageAnchor::Top, .x = Mm{5}})}}));
    mountThrows(ui::page(ui::Page{
        .geometry = a4, .children = {region({.anchor = ui::PageAnchor::Offset, .x = Mm{150}, .width = Mm{100}})}}));
    // An empty margin box.
    mountThrows(
        ui::page(ui::Page{.geometry = {.width = Mm{210}, .height = Mm{297}, .margins = ui::PageMargins{Mm{105}}}}));
    // A page sizes itself; a region is sized by its geometry.
    mountThrows(ui::page(ui::Page{.geometry = a4, .common = {.layout = {.width = ui::Sizing::fixed(10)}}}));
    mountThrows(ui::page(ui::Page{.geometry = a4,
                                  .children = {ui::pageRegion(ui::PageRegion{
                                      .geometry = {}, .common = {.layout = {.height = ui::Sizing::stretch()}}})}}));
    // A sheet that is valid mounts.
    CHECK_NOTHROW((ui::Mounted{runtime, backend, ui::page(ui::Page{.geometry = a4, .children = {region({})}})}));
}

namespace {

constexpr char const* kScope = R"({
  "vocab": "ui/1", "id": "scope",
  "state": { "tag": { "type": "string?", "init": "de" }, "amount": { "type": "decimal", "init": { "lit": { "num": 15, "den": 10, "dp": 1 } } } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "outside",
      "text": { "fn": "money", "args": [{ "lit": { "num": 123450, "den": 100, "dp": 2 } }, "EUR"] } },
    { "kind": "localeScope", "locale": { "ref": "tag" }, "children": [
      { "kind": "text", "testId": "inside",
        "text": { "fn": "money", "args": [{ "lit": { "num": 123450, "den": 100, "dp": 2 } }, "EUR"] } },
      { "kind": "text", "testId": "bare", "text": { "ref": "amount" } },
      { "kind": "text", "testId": "literal", "text": { "fn": "number", "args": [{ "lit": { "num": 25, "den": 10, "dp": 1 } }] } },
      { "kind": "text", "testId": "message", "text": { "t": "greeting", "fallback": "Hello {$n :number}", "args": { "n": { "ref": "amount" } } } },
      { "kind": "text", "testId": "day", "text": { "fn": "date", "args": ["2026-10-24T09:00:00Z"] } },
      { "kind": "text", "testId": "parsed", "text": { "fn": "coalesce", "args": [{ "fn": "parseDecimal", "args": ["1.5"] }, "none"] } },
      { "kind": "text", "testId": "env", "text": { "env": "locale" } },
      { "kind": "keyValue", "testId": "kv", "items": { "list": [ { "obj": { "label": "Total", "value": { "ref": "amount" } } } ] } },
      { "kind": "text", "testId": "plain", "text": { "lit": { "num": 25, "den": 10, "dp": 1 } } },
      { "kind": "localeScope", "locale": "en", "children": [
        { "kind": "text", "testId": "nested",
          "text": { "fn": "money", "args": [{ "lit": { "num": 123450, "den": 100, "dp": 2 } }, "EUR"] } } ] },
      { "kind": "localeScope", "locale": "", "children": [
        { "kind": "text", "testId": "emptied",
          "text": { "fn": "money", "args": [{ "lit": { "num": 123450, "den": 100, "dp": 2 } }, "EUR"] } } ] } ] },
    { "kind": "text", "testId": "after", "text": { "fn": "number", "args": [{ "lit": { "num": 25, "den": 10, "dp": 1 } }] } } ] } })";

}  // namespace

// Mutations: install no context in the scope (both read €1,234.50); let the scope replace the session's (both read
// 1.234,50 €).
TEST_CASE("locale scope: money inside a scope is written in the scope's locale, the session's stays", "[ui][page]") {
    Harness h;
    h.session.setLocale("en");
    auto const screen = h.mount(kScope);
    CHECK(h.text("outside") == "€1,234.50");
    CHECK(h.text("inside") == "1.234,50\u00a0€");
    CHECK(h.text("after") == "2.5");
    CHECK(h.text("literal") == "2,5");
}

// Mutation: make bindProp read the session's facts (a bare decimal bound to a text shows 1.5 inside the scope).
TEST_CASE("locale scope: a bare number a node shows, a message and a key-value item follow the scope", "[ui][page]") {
    Harness h;
    h.session.setLocale("en");
    auto const screen = h.mount(kScope);
    CHECK(h.text("bare") == "1,5");
    CHECK(h.text("message") == "Hello 1,5");
}

// Mutation: make keyValueItemOf read the session's facts (the value shows 1.5).
TEST_CASE("locale scope: a key-value item's value follows the scope", "[ui][page]") {
    Harness h;
    h.session.setLocale("en");
    auto const screen = h.mount(kScope);
    int const kv = h.byTestId("KeyValue", "kv");
    INFO(h.backend.prop(kv, "items"));
    CHECK(h.backend.prop(kv, "items").contains(R"(1\,5)"));
}

// Mutation: make the literal branch of bindProp read the session's facts (the text shows 2.5).
TEST_CASE("locale scope: a literal number a node shows follows the scope", "[ui][page]") {
    Harness h;
    h.session.setLocale("en");
    auto const screen = h.mount(kScope);
    CHECK(h.text("plain") == "2,5");
}

// Mutation: let the row's pattern win over the application's (the declared case reads 24.10.2026).
TEST_CASE("locale scope: the application's declared date pattern wins, else the scope's row", "[ui][page]") {
    Harness h;
    h.session.setLocale("en");
    auto const screen = h.mount(kScope);
    CHECK(h.text("day") == "24.10.2026");
    h.session.setDatePatterns("dd-MMM-yyyy", "dd-MMM-yyyy HH:mm");
    h.settle();
    CHECK(h.text("day") == "24-Oct-2026");
}

// Mutations: build the inner scope from the enclosing one (nested reads 1.234,50 €); read the session's locale for an
// empty tag from the scope (emptied reads 1.234,50 €).
TEST_CASE("locale scope: scopes nest, and an inner one inherits nothing from the outer", "[ui][page]") {
    Harness h;
    h.session.setLocale("en");
    auto const screen = h.mount(kScope);
    CHECK(h.text("nested") == "€1,234.50");
    CHECK(h.text("emptied") == "€1,234.50");
}

// Mutation: read the scope's locale for env.locale (it reads de).
TEST_CASE("locale scope: env locale stays the session's inside a scope", "[ui][page]") {
    Harness h;
    h.session.setLocale("en");
    auto const screen = h.mount(kScope);
    CHECK(h.text("env") == "en");
}

// Mutation: read the scope's facts in parseDecimal (it reads 15 for "1.5", `.` being de's group separator).
TEST_CASE("locale scope: typed text is read in the session's locale", "[ui][page]") {
    Harness h;
    h.session.setLocale("en");
    auto const screen = h.mount(kScope);
    // The decimal is shown in the scope's locale, so it is the parsed 1.5 under en's reading.
    CHECK(h.text("parsed") == "1,5");
}

// Mutations: report on a null tag; report on every read.
TEST_CASE("locale scope: a missing tag is the session's locale and silent, an unlisted one is reported once",
          "[ui][page]") {
    Harness h;
    h.session.setLocale("en");
    auto const screen = h.mount(kScope);
    CHECK(h.diagnostics.empty());
    h.session.setLocale("en");
    // An unlisted tag writes with en's facts and is reported once, however often it is read.
    auto const polish = h.mount(R"({
      "vocab": "ui/1", "id": "pl",
      "view": { "kind": "column", "children": [
        { "kind": "localeScope", "locale": "pl", "children": [
          { "kind": "text", "testId": "a", "text": { "fn": "number", "args": [{ "lit": { "num": 25, "den": 10, "dp": 1 } }] } } ] },
        { "kind": "localeScope", "locale": "pl", "children": [
          { "kind": "text", "testId": "b", "text": { "fn": "number", "args": [{ "lit": { "num": 25, "den": 10, "dp": 1 } }] } } ] } ] } })");
    CHECK(h.text("a") == "2.5");
    // A tag that is null (a query still loading) is the session's locale and reports nothing.
    auto const loading = h.mount(R"({
      "vocab": "ui/1", "id": "loading",
      "view": { "kind": "localeScope", "locale": null, "children": [{ "kind": "text", "testId": "c", "text": "x" }] } })");
    for (ui::Refusal const& refusal : h.diagnostics) {
        CHECK(refusal.path != "/view/locale");
    }
    h.session.setDatePatterns("dd.MM.yyyy", "dd.MM.yyyy HH:mm");
    h.settle();
    int reports = 0;
    for (ui::Refusal const& refusal : h.diagnostics) {
        if (refusal.reason.find("no formatting facts for locale `pl`") != std::string::npos) {
            ++reports;
        }
    }
    CHECK(reports == 1);
}

// Mutation: a number as the tag is silently taken for none.
TEST_CASE("locale scope: a tag that is not a string is reported at the locale path", "[ui][page]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "bad",
      "view": { "kind": "localeScope", "locale": 5, "children": [{ "kind": "text", "text": "x" }] } })");
    REQUIRE_FALSE(h.diagnostics.empty());
    CHECK(h.diagnostics.front().path == "/view/locale");
}

// Mutation: the scope reads a stale context (the date stays 24.10.2026 after the application declares a pattern);
// the session's language alone moves nothing inside a scope that has its own tag.
TEST_CASE("locale scope: a change of the session's facts reaches the scope, a change of its language does not",
          "[ui][page]") {
    Harness h;
    h.session.setLocale("en");
    auto const screen = h.mount(kScope);
    CHECK(h.text("inside") == "1.234,50\u00a0€");
    h.session.setLocale("fr");
    h.settle();
    CHECK(h.text("inside") == "1.234,50\u00a0€");
    h.session.setDatePatterns("dd-MMM-yyyy", "dd-MMM-yyyy HH:mm");
    h.settle();
    CHECK(h.text("day") == "24-Oct-2026");
}

namespace {

// One of each stateful kind inside a scope whose tag is `de`, with the same number bound outside it.
constexpr char const* kStateful = R"({
  "vocab": "ui/1", "id": "stateful",
  "state": { "tag": { "type": "string?", "init": "de" },
             "amount": { "type": "decimal", "init": { "lit": { "num": 15, "den": 10, "dp": 1 } } },
             "unit": { "type": "string?", "init": null }, "agreed": { "type": "bool", "init": false },
             "when": { "type": "any", "init": null }, "typed": { "type": "string", "init": "" },
             "rows": { "init": { "lit": [ { "id": 1, "total": "1234.5" }, { "id": 2, "total": "2" } ] } } },
  "view": { "kind": "column", "children": [
    { "kind": "localeScope", "locale": { "ref": "tag" }, "children": [
      { "kind": "textInput", "id": "amountInput", "testId": "amount", "value": { "ref": "amount" },
        "onChange": [ { "set": "amount", "value": { "fn": "parseDecimal", "args": [ { "ref": "event" } ] } },
                      { "set": "typed", "value": { "ref": "event" } } ] },
      { "kind": "text", "testId": "shown", "text": { "ref": "amount" } },
      { "kind": "select", "id": "unitSelect", "testId": "unit", "options": { "lit": [ "g", "kg" ] }, "value": { "ref": "unit" },
        "onChange": [ { "set": "unit", "value": { "ref": "event" } } ] },
      { "kind": "checkbox", "id": "agree", "testId": "agree", "label": "agree", "checked": { "ref": "agreed" },
        "onChange": [ { "set": "agreed", "value": { "ref": "event" } } ] },
      { "kind": "dateTimeInput", "id": "whenInput", "testId": "when", "mode": "date", "value": { "ref": "when" },
        "onChange": [ { "set": "when", "value": { "ref": "event" } } ] },
      { "kind": "text", "testId": "whenShown", "text": { "fn": "coalesce", "args": [ { "ref": "when" }, "unset" ] } },
      { "kind": "table", "id": "lines", "testId": "lines", "rows": { "ref": "rows" }, "selectionMode": "single",
        "columns": [ { "id": "total", "label": "Total" } ] },
      { "kind": "text", "testId": "selected", "text": { "fn": "format", "args": [ "{0}", { "ref": "lines.selectedCount" } ] } },
      { "kind": "dialog", "id": "dlg", "testId": "dlg", "title": "T", "open": true, "children": [
        { "kind": "text", "testId": "dlgAmount", "text": { "ref": "amount" } },
        { "kind": "textInput", "id": "inner", "testId": "inner" },
        { "kind": "text", "testId": "dlgTyped", "text": { "fn": "coalesce", "args": [ { "ref": "inner.value" }, "none" ] } } ] } ] },
    { "kind": "text", "testId": "outer", "text": { "ref": "amount" } } ] } })";

}  // namespace

// Mutation: let `elementFor` stop at the scope that carries the locale (every stateful node below dereferences a
// null element state while it is lowered).
TEST_CASE("locale scope: a stateful node inside a scope mounts and keeps its state", "[ui][page]") {
    Harness h;
    h.session.setLocale("en");
    auto const screen = h.mount(kStateful);
    REQUIRE(h.diagnostics.empty());
    // A number input shows the bound 1.5 as typed in the session's locale, and commits a typed 2.25 as 2.25; what the
    // scope displays is written in the scope's.
    int const amount = h.byTestId("TextInput", "amount");
    CHECK(h.backend.prop(amount, "text") == "1.5");
    CHECK(h.text("shown") == "1,5");
    h.backend.edit(amount, "2.25");
    h.settle();
    CHECK(h.text("shown") == "2,25");
    CHECK(h.text("outer") == "2.25");
    CHECK(h.backend.prop(h.byTestId("TextInput", "amount"), "text") == "2.25");
    // The other stateful kinds are driven through their state.
    h.backend.choose(h.byTestId("Select", "unit"), ui::Key{std::string{"kg"}});
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "unit"})") == R"("kg")");
    h.backend.toggle(h.byTestId("Checkbox", "agree"));
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "agreed"})") == "true");
    using namespace std::chrono;
    morph::time::DateTime const instant{year{2026}, month{10}, day{24}, hours{9}, minutes{0}, seconds{0}};
    h.backend.setDateTime(h.byTestId("DateTimeInput", "when"), morph::time::Timestamp{instant});
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "when"})") != "null");
    // A table keeps its selection and its default cells are written in the scope's locale.
    int const lines = h.byTestId("Table", "lines");
    h.backend.selectRows(lines, {ui::Key{std::int64_t{1}}});
    h.settle();
    CHECK(h.text("selected") == "1");
    // A dialog's body reads the scope too, and keeps an input of its own.
    CHECK(h.text("dlgAmount") == "2,25");
    h.backend.edit(h.byTestId("TextInput", "inner"), "x");
    h.settle();
    CHECK(h.text("dlgTyped") == "x");
}

namespace {

// A scope whose tag is set after the mount, around a bound number, a translated text and a table with translated
// column labels.
constexpr char const* kTagged = R"({
  "vocab": "ui/1", "id": "tagged",
  "state": { "tag": { "type": "string?", "init": null },
             "amount": { "type": "decimal", "init": { "lit": { "num": 15, "den": 10, "dp": 1 } } },
             "rows": { "init": { "lit": [ { "id": 1, "total": "1234.5" } ] } } },
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "toDe", "label": "de", "onClick": { "set": "tag", "value": "de" } },
    { "kind": "button", "testId": "toEn", "label": "en", "onClick": { "set": "tag", "value": "en" } },
    { "kind": "localeScope", "locale": { "ref": "tag" }, "children": [
      { "kind": "text", "testId": "bound", "text": { "ref": "amount" } },
      { "kind": "text", "testId": "plain", "text": { "lit": { "num": 25, "den": 10, "dp": 1 } } },
      { "kind": "text", "testId": "word", "text": { "t": "word", "fallback": "Word" } },
      { "kind": "table", "id": "lines", "testId": "lines", "rows": { "ref": "rows" },
        "columns": [ { "id": "total", "label": { "t": "total", "fallback": "Total" } } ] } ] } ] } })";

void translate(Harness& h) {
    h.session.setLocale("fr");
    ui::FormatContext format = h.session.formatting();
    format.translations = [](std::string_view key, std::string_view locale) -> std::optional<std::string> {
        if (locale == "de") {
            return key == "word" ? "Wort" : "Summe";
        }
        if (locale == "en") {
            return key == "word" ? "Word!" : "Sum";
        }
        return std::nullopt;
    };
    h.session.setFormatting(format);
}

}  // namespace

// Mutations: compute `localeFormat` without reading the `locale` expression (nothing moves when the tag arrives);
// leave the selector of the scope's slot constant (the table's labels, read once at lowering, stay as they were).
TEST_CASE(
    "locale scope: a tag that arrives or changes after the mount moves everything inside, once-read values "
    "included",
    "[ui][page]") {
    Harness h;
    translate(h);
    auto const screen = h.mount(kTagged);
    REQUIRE(h.diagnostics.empty());
    // Not known yet: the session's locale.
    CHECK(h.text("word") == "Word");
    CHECK(h.text("bound") == "1.5");
    CHECK(h.backend.prop(h.byTestId("Table", "lines"), "columns") == "[Total:content]");
    h.backend.click(h.byTestId("Button", "toDe"));
    h.settle();
    CHECK(h.text("word") == "Wort");
    CHECK(h.text("bound") == "1,5");
    CHECK(h.text("plain") == "2,5");
    CHECK(h.backend.prop(h.byTestId("Table", "lines"), "columns") == "[Summe:content]");
    h.backend.click(h.byTestId("Button", "toEn"));
    h.settle();
    CHECK(h.text("word") == "Word!");
    CHECK(h.text("bound") == "1.5");
    CHECK(h.text("plain") == "2.5");
    CHECK(h.backend.prop(h.byTestId("Table", "lines"), "columns") == "[Sum:content]");
}

// The invoice's shape: the customer's locale comes from a query that loads after the mount, and the line table's
// labels are translated. Mutation: as above.
TEST_CASE("locale scope: a table's translated labels follow a tag a query delivers", "[ui][page]") {
    Harness h;
    translate(h);
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "invoice",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "queries": { "customer": { "model": "sample", "action": "Lims_ListResults", "body": { "obj": { "id": 1 } } } },
      "view": { "kind": "localeScope", "locale": { "query": "customer" }, "children": [
        { "kind": "table", "id": "lines", "testId": "lines", "rows": { "lit": [ { "total": "1" } ] },
          "columns": [ { "id": "total", "label": { "t": "total", "fallback": "Total" } } ] } ] } })");
    CHECK(h.backend.prop(h.byTestId("Table", "lines"), "columns") == "[Total:content]");
    h.reply(0, "\"de\"");
    CHECK(h.backend.prop(h.byTestId("Table", "lines"), "columns") == "[Summe:content]");
}

// Mutation: leave `formatSource` out of the component's scope (the fallback's text shows 1.5).
TEST_CASE("locale scope: a custom component's fallback formats in the scope of its use site", "[ui][page]") {
    Harness h;
    h.session.setLocale("en");
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "component",
      "components": { "Gauge": { "host": "Gauge", "version": 1, "props": { "level": "number" },
                                 "fallback": { "kind": "text", "testId": "fallback", "text": { "ref": "props.level" } } } },
      "view": { "kind": "localeScope", "locale": "de", "children": [
        { "kind": "custom", "name": "Gauge", "props": { "level": { "lit": { "num": 15, "den": 10, "dp": 1 } } } } ] } })");
    CHECK(h.text("fallback") == "1,5");
}

// Mutation: build the page's slot out of the scope (the page leaves the mount).
TEST_CASE("locale scope: a scope around a page formats the regions in its locale", "[ui][page]") {
    Harness h;
    h.session.setLocale("en");
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "around",
      "view": { "kind": "localeScope", "locale": "de", "children": [
        { "kind": "page", "width": 210000, "height": 297000, "a11y": { "name": "Sheet" }, "children": [
          { "kind": "pageRegion", "anchor": "top", "children": [
            { "kind": "text", "testId": "total", "text": { "fn": "money", "args": [ { "lit": { "num": 123450, "den": 100, "dp": 2 } }, "EUR" ] } } ] } ] } ] } })");
    CHECK(h.text("total") == "1.234,50 €");
}

TEST_CASE("locale scope: it lowers to a slot over a holder column", "[ui][page]") {
    Harness h;
    auto const screen = h.mount(kScope);
    // One slot for each of the three scopes.
    CHECK(h.backend.all("Slot").size() == 3);
}

// Mutation: the author API writes the lengths as millimetres (the document refuses them).
TEST_CASE("document page: the author API writes a sheet the loader reads back", "[ui][page][author]") {
    namespace author = ui::author;
    author::Node const view = author::page(
        {author::pageRegion(author::text({.text = "Letterhead"}), {.anchor = ui::PageAnchor::Top}),
         author::pageRegion(
             author::text({.text = "Address"}),
             {.anchor = ui::PageAnchor::Offset, .x = Mm{20}, .y = Mm{45}, .width = Mm{85}, .height = Mm{45}}),
         author::pageRegion(author::text({.text = "Footer"}), {.anchor = ui::PageAnchor::Bottom})},
        {.width = Mm{210}, .height = Mm{297}, .margins = {Mm{10}, Mm{20}, Mm{15}, Mm{25}}});
    CHECK_THROWS_AS(author::page({}, {.width = Mm{5}, .height = Mm{297}}), std::invalid_argument);
    CHECK_FALSE(view.json().empty());
}

namespace {

// The shipped paper surface: the roles `themeContrastProblems` reads, taken from `paperScheme` in Theme.qml itself.
// A QML colour with eight digits is `#AARRGGBB`.
ui::ThemeValues shippedPaper() {
    std::ifstream file{MORPH_PAPER_THEME_QML};
    REQUIRE(file.is_open());
    std::string const qml{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    auto const begin = qml.find("paperScheme: MorphScheme {");
    REQUIRE(begin != std::string::npos);
    auto const end = qml.find('}', begin);
    std::istringstream block{qml.substr(begin, end - begin)};
    ui::ThemeValues paper = ui::themeValues();
    std::set<std::string> found;
    std::string line;
    while (std::getline(block, line)) {
        std::smatch match;
        if (!std::regex_search(line, match, std::regex{R"re(^\s*(\w+): "#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})")re"})) {
            continue;
        }
        std::string const digits = match[2].str();
        auto const byte = [&digits](std::size_t at) {
            return static_cast<std::uint8_t>(std::stoul(digits.substr(at, 2), nullptr, 16));
        };
        ui::Rgba const color = digits.size() == 8 ? ui::Rgba{byte(2), byte(4), byte(6), byte(0)}
                                                  : ui::Rgba{byte(0), byte(2), byte(4), 255};
        paper.colors.insert_or_assign("color." + match[1].str(), color);
        found.insert(match[1].str());
    }
    for (char const* role :
         {"surface", "text", "muted", "selection", "selectionText", "track", "hover", "controlSurface"}) {
        INFO(role);
        REQUIRE(found.contains(role));
    }
    return paper;
}

}  // namespace

// Mutation: change `selection` in Theme.qml's `paperScheme` to a dark colour (its text is then unreadable on it).
TEST_CASE("document page: the paper surface's roles hold their contrast on white", "[ui][page][theme]") {
    ui::ThemeValues paper = shippedPaper();
    CHECK(paper.colors.at("color.surface").hex() == "#ffffff");
    CHECK(ui::themeContrastProblems(paper).empty());
    // The check reads what it is given: the same values with a dark selection are refused.
    paper.colors.insert_or_assign("color.selection", ui::Rgba{0x20, 0x20, 0x20, 255});
    CHECK_FALSE(ui::themeContrastProblems(paper).empty());
}

// Mutation: drop `page` from the accessibility rule (an unnamed page registers).
TEST_CASE("document page: an unnamed page breaks the accessibility rules", "[ui][page][author]") {
    auto const check = [](std::string const& view) {
        auto const loaded = ui::loadDocumentText(R"({"vocab": "ui/1", "id": "a", "view": )" + view + "}");
        REQUIRE(loaded.has_value());
        return ui::author::checkAccessibility(*loaded);
    };
    CHECK(check(R"({"kind": "page", "width": 210000, "height": 297000})").has_value());
    CHECK_FALSE(
        check(R"({"kind": "page", "width": 210000, "height": 297000, "a11y": {"name": "Invoice"}})").has_value());
}
