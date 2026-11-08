// SPDX-License-Identifier: Apache-2.0

// The edges of the interpreter's commands and reads: names that hold no value, writes to names that hold no state,
// inline files that cannot be read, saved or sent, and the helpers that format and decode them.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;
using ui::testing::readJson;

namespace {

constexpr char const* kEdges = R"({
  "vocab": "ui/1", "id": "edges", "params": { "blob": "string?", "bad": "string?" },
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "n": { "init": 4 }, "picked": { "init": "/data/readings.csv" } },
  "mutations": { "attach": { "model": "sample", "action": "Lims_Attach", "body": { "obj": { "kind": "csv" } } } },
  "view": { "kind": "column", "children": [
    { "kind": "textInput", "id": "note", "testId": "note" },
    { "kind": "button", "testId": "padded", "label": "c",
      "onClick": [ { "download": "AQI=", "from": "bytes", "save": "dialog", "name": "two.bin" } ] },
    { "kind": "button", "testId": "notBase64", "label": "d",
      "onClick": [ { "download": { "ref": "bad" }, "from": "bytes", "save": "dialog", "name": "bad.bin" } ] },
    { "kind": "button", "testId": "badByte", "label": "e",
      "onClick": [ { "download": { "lit": [ 1, 300 ] }, "from": "bytes", "save": "dialog", "name": "bad.bin" } ] },
    { "kind": "button", "testId": "notBytes", "label": "f",
      "onClick": [ { "download": 5, "from": "bytes", "save": "dialog", "name": "bad.bin" } ] },
    { "kind": "button", "testId": "big", "label": "g",
      "onClick": [ { "download": { "ref": "blob" }, "from": "bytes", "save": "dialog", "name": "big.bin" } ] },
    { "kind": "button", "testId": "send", "label": "h",
      "onClick": [ { "upload": { "ref": "picked" }, "to": "attach", "into": "bytes", "as": "content",
                     "typeAs": "mime", "id": "up" } ] }
  ] } })";

}  // namespace

// Mutation: answer `null` for a name of the category the switch has no case for (return the name's value).
TEST_CASE("interpreter edges: a read of a name that holds no value is null, and of state is its value",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(kEdges);
    CHECK(readJson(*screen, R"({"ref": "n"})") == "4");
    CHECK(readJson(*screen, R"({"ref": "zzz"})") == "null");
    CHECK(readJson(*screen, R"({"ref": "sample"})") == "null");
    CHECK(readJson(*screen, R"({"ref": "attach"})") == "null");
    CHECK(readJson(*screen, R"({"ref": "note"})") == "null");
    CHECK(readJson(*screen, R"({"ref": "up"})") == "null");
    CHECK(readJson(*screen, R"({"ref": "app"})") == "null");
    CHECK(readJson(*screen, R"({"ref": "app.banner"})") == "null");
}

// Mutation: leave the padding in the size (the padded text decodes to three bytes, or is refused).
TEST_CASE("interpreter edges: a download from padded base64 saves the decoded bytes", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(kEdges);
    h.backend.click(h.byTestId("Button", "padded"));
    h.settle();
    REQUIRE(h.host.saved.size() == 1);
    CHECK(h.host.saved.front().bytes == std::string{"\x01\x02", 2});
    CHECK(h.diagnostics.empty());
}

// Mutation: save inline bytes the decoder refused (the bad text reaches the host), or drop a reason.
TEST_CASE("interpreter edges: inline bytes that do not decode never reach the host, and say why",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(kEdges, ui::ValueRecord{{"bad", ui::Value{"not base64 !"}}});
    for (std::string_view const button : {"notBase64", "badByte", "notBytes"}) {
        h.backend.click(h.byTestId("Button", button));
        h.settle();
    }
    CHECK(h.host.saved.empty());
    REQUIRE(h.diagnostics.size() == 3);
    CHECK(h.diagnostics.at(0).reason == "download: the text is not base64");
    CHECK(h.diagnostics.at(1).reason == "download: a byte list holds integers 0 to 255");
    CHECK(h.diagnostics.at(2).reason == "download: inline bytes are base64 text or a list of bytes");
}

// Mutation: round the size to whole mebibytes (the report says "2 MiB"), or stop at the first decimal.
TEST_CASE("interpreter edges: an oversize download reports its size with one decimal", "[ui][interpreter]") {
    Harness h;
    std::string const blob(2'097'152, 'A');
    auto const screen = h.mount(kEdges, ui::ValueRecord{{"blob", ui::Value{blob}}});
    h.backend.click(h.byTestId("Button", "big"));
    h.settle();
    CHECK(h.host.saved.empty());
    REQUIRE(h.diagnostics.size() == 1);
    CHECK(h.diagnostics.front().reason ==
          "download: 1.5 MiB is over the 1 MiB inline cap; serve it through the file store");
}

// Mutation: ignore saveFile's refusal (no report, and `done` is never called with false).
TEST_CASE("interpreter edges: a host that cannot save inline bytes is reported", "[ui][interpreter]") {
    Harness h;
    h.host.refuseTransfers = true;
    auto const screen = h.mount(kEdges);
    h.backend.click(h.byTestId("Button", "padded"));
    h.settle();
    CHECK(h.host.saved.empty());
    REQUIRE(h.diagnostics.size() == 1);
    CHECK(h.diagnostics.front().reason == "`download` is not supported by the host");
}

// Mutation: run the mutation after a failed read (a call goes out), or leave the error unset.
TEST_CASE("interpreter edges: an upload into bytes the host could not read fails with its reason",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(kEdges);
    h.backend.click(h.byTestId("Button", "send"));
    h.settle();
    h.host.readDone(std::unexpected(std::string{"disk gone"}));
    h.settle();
    CHECK(h.dispatcher.count("Lims_Attach") == 0);
    CHECK(readJson(*screen, R"({"ref": "up.error"})") == R"("disk gone")");
    CHECK(readJson(*screen, R"({"ref": "up.pending"})") == "false");
}

// Mutation: drop the `typeAs` member from the body.
TEST_CASE("interpreter edges: an upload into bytes names the media type under typeAs", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(kEdges);
    h.backend.click(h.byTestId("Button", "send"));
    h.settle();
    h.host.readDone(ui::InlineFile{.name = "r.csv", .mediaType = "text/csv", .bytes = "a"});
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_Attach") == 1);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_Attach")).body ==
          R"({"content":"YQ==","kind":"csv","mime":"text/csv"})");
}

// Mutation: print `bytes` as KiB for a count under 1024, or drop the decimal for a non-whole count.
TEST_CASE("interpreter edges: byte counts read as bytes, KiB or MiB with one decimal when not whole",
          "[ui][interpreter]") {
    CHECK(ui::detail::sizeText(0) == "0 bytes");
    CHECK(ui::detail::sizeText(1023) == "1023 bytes");
    CHECK(ui::detail::sizeText(1024) == "1 KiB");
    CHECK(ui::detail::sizeText(1536) == "1.5 KiB");
    CHECK(ui::detail::sizeText(std::size_t{1024} * 1024) == "1 MiB");
    CHECK(ui::detail::sizeText(std::size_t{1024} * 1024 * 3 / 2) == "1.5 MiB");
}

// Mutation: count the padding characters in the size.
TEST_CASE("interpreter edges: a base64 text's size is read from its length less its padding", "[ui][interpreter]") {
    CHECK(ui::detail::base64Size("AQID") == 3);
    CHECK(ui::detail::base64Size("AQI=") == 2);
    CHECK(ui::detail::base64Size("AQ==") == 1);
    CHECK(ui::detail::base64Size("") == 0);
}

// Mutation: return the parsed fraction, or an error value, for text that is not a value.
TEST_CASE("interpreter edges: JSON text that is no value reads as null", "[ui][interpreter]") {
    CHECK(ui::detail::jsonTextValue("not json").isNull());
    CHECK(ui::detail::jsonTextValue("1.5").isNull());
    CHECK(ui::detail::jsonTextValue("[1,2]") ==
          ui::Value{ui::ValueList{ui::Value{std::int64_t{1}}, ui::Value{std::int64_t{2}}}});
}

// Mutation: key an instance by its JSON for a number or text key (or by the empty text for a list key).
TEST_CASE("interpreter edges: an instance key's text is its number, its text or its JSON", "[ui][interpreter]") {
    CHECK(ui::detail::instanceKeyText(ui::Value{}).empty());
    CHECK(ui::detail::instanceKeyText(ui::Value{std::int64_t{7}}) == "7");
    CHECK(ui::detail::instanceKeyText(ui::Value{"abc"}) == "abc");
    // Parsed rather than constructed, so the optimizer cannot fold the variant and warn about the text branch.
    ui::Value const listKey = ui::detail::jsonTextValue("[1]");
    REQUIRE(listKey.asList() != nullptr);
    CHECK(ui::detail::instanceKeyText(listKey) == "[1]");
}

namespace {

/// Ten rows in a table that virtualizes from five rows on, with an overscan of two.
constexpr std::string_view kVirtual = R"({
  "vocab": "ui/1", "id": "virtual",
  "state": { "items": { "init": { "lit": [
    { "id": 0, "n": "row-0" }, { "id": 1, "n": "row-1" }, { "id": 2, "n": "row-2" }, { "id": 3, "n": "row-3" },
    { "id": 4, "n": "row-4" }, { "id": 5, "n": "row-5" }, { "id": 6, "n": "row-6" }, { "id": 7, "n": "row-7" },
    { "id": 8, "n": "row-8" }, { "id": 9, "n": "row-9" } ] } } },
  "view": { "kind": "table", "rows": { "ref": "items" }, "virtualizeThreshold": 5, "virtualizeOverscan": 2,
            "columns": [ { "id": "n", "label": "Name" } ] } })";

}  // namespace

// Mutation: ignore the threshold or the overscan props (the table realizes every row, or the default window).
TEST_CASE("interpreter edges: a table's virtualization threshold and overscan reach the table", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(kVirtual);
    int const table = h.backend.all("Table").front();
    CHECK(h.backend.prop(table, "realizedWindow") == "0+2/10");
    h.backend.scrollTable(table, 5, 2);
    h.settle();
    CHECK(h.backend.prop(table, "realizedWindow") == "3+6/10");
}

namespace {

/// One element of each kind whose properties the screen reads.
constexpr std::string_view kElements = R"({
  "vocab": "ui/1", "id": "elements",
  "state": { "items": { "init": { "lit": [ { "id": 1 } ] } } },
  "view": { "kind": "column", "children": [
    { "kind": "table", "id": "grid", "rows": { "ref": "items" }, "columns": [ { "id": "id", "label": "Id" } ] },
    { "kind": "tabs", "id": "pages", "tabs": [
      { "id": "one", "label": "One", "children": [ { "kind": "text", "text": "1" } ] },
      { "id": "two", "label": "Two", "children": [ { "kind": "text", "text": "2" } ] } ] },
    { "kind": "dialog", "id": "dlg", "title": "T", "children": [ { "kind": "text", "text": "x" } ] } ] } })";

}  // namespace

// Mutation: answer a tabs property other than `selected` with the shown tab, or a table's pending with `true`.
TEST_CASE("interpreter edges: a table, tabs and a dialog answer only the properties they have", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(kElements);
    CHECK(readJson(*screen, R"({"ref": "grid.pending"})") == "false");
    CHECK(readJson(*screen, R"({"ref": "grid.error"})") == "null");
    CHECK(readJson(*screen, R"({"ref": "grid.nothing"})") == "null");
    CHECK(readJson(*screen, R"({"ref": "grid.sourceCount"})") == "1");
    CHECK(readJson(*screen, R"({"ref": "pages.selected"})") == R"("one")");
    CHECK(readJson(*screen, R"({"ref": "pages.nothing"})") == "null");
    CHECK(readJson(*screen, R"({"ref": "dlg.open"})") == "false");
    CHECK(readJson(*screen, R"({"ref": "dlg.submitted"})") == "false");
    CHECK(readJson(*screen, R"({"ref": "dlg.nothing"})") == "null");
}
