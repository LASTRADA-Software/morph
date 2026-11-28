// SPDX-License-Identifier: Apache-2.0

// `download` of bytes a document holds: which text decodes, which list is a byte list, what the cap refuses, and
// what reaches the host. Each case is the `download` command's value, and the file the host saved or the report.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;

namespace {

std::string documentOf(std::string_view download) {
    return R"({"vocab":"ui/1","id":"dl","view":{"kind":"button","testId":"go","label":"Go","onClick":[)" +
           std::string{download} + "]}}";
}

bool reported(ui::testing::Harness const& h, std::string_view text) {
    return std::ranges::any_of(h.diagnostics,
                               [text](ui::Refusal const& refusal) { return refusal.reason.contains(text); });
}

}  // namespace

TEST_CASE("cov h download: inline bytes are base64 text or a list of bytes, and nothing else",
          "[covh][ui][interpreter]") {
    struct Case {
        char const* what;
        std::string value;   // the command's value, as JSON
        char const* saved;   // the bytes the host saved; empty when none
        char const* report;  // empty when nothing is reported
    };
    std::string const text = R"("YWJj")";
    for (Case const& item : std::vector<Case>{
             {"base64 text", text, "abc", ""},
             {"base64 with padding", R"("YQ==")", "a", ""},
             {"a url-safe dash", R"("Pz8-")", "", "the text is not base64"},
             {"the plus and slash digits", R"("+/+/")", "\xFB\xFF\xBF", ""},
             {"a digit", R"("MDEy")", "012", ""},
             {"a character past the lowercase letters", R"("YW{j")", "", "the text is not base64"},
             {"text of a stray length", R"("Y")", "", "the text is not base64"},
             {"text with a character outside the alphabet", R"("YW*j")", "", "the text is not base64"},
             {"a byte list", R"({"lit":[104,105]})", "hi", ""},
             {"a list holding a negative", R"({"lit":[1,-1]})", "", "a byte list holds integers 0 to 255"},
             {"a list holding more than a byte", R"({"lit":[256]})", "", "a byte list holds integers 0 to 255"},
             {"a list holding a string", R"({"lit":["a"]})", "", "a byte list holds integers 0 to 255"},
             {"a number", "5", "", "inline bytes are base64 text or a list of bytes"},
         }) {
        INFO(item.what);
        ui::testing::Harness h;
        auto const screen =
            h.mount(documentOf(R"({"download":)" + item.value + R"(,"from":"bytes","save":"dialog","name":"a.bin"})"));
        h.backend.click(h.byTestId("Button", "go"));
        h.settle();
        if (std::string_view{item.saved}.empty()) {
            CHECK(h.host.saved.empty());
        } else {
            REQUIRE(h.host.saved.size() == 1);
            CHECK(h.host.saved.front().bytes == item.saved);
            CHECK(h.host.saved.front().name == "a.bin");
        }
        if (std::string_view{item.report}.empty()) {
            CHECK(h.diagnostics.empty());
        } else {
            CHECK(reported(h, item.report));
        }
    }
}

TEST_CASE("cov h download: bytes over the inline cap are refused before the host is asked",
          "[covh][ui][interpreter]") {
    std::string const huge(1'500'000, 'A');
    ui::testing::Harness text;
    auto const first =
        text.mount(documentOf(R"({"download":")" + huge + R"(","from":"bytes","save":"dialog","name":"a.bin"})"));
    text.backend.click(text.byTestId("Button", "go"));
    text.settle();
    CHECK(text.host.saved.empty());
    CHECK(reported(text, "is over the 1 MiB inline cap"));
    CHECK(reported(text, "1.1 MiB"));

    std::string bytes = "[";
    for (std::size_t at = 0; at < 1'048'577; ++at) {
        bytes += at == 0 ? "1" : ",1";
    }
    bytes += "]";
    ui::testing::Harness list;
    auto const second = list.mount(
        documentOf(R"({"download":{"lit":)" + bytes + R"(},"from":"bytes","save":"dialog","name":"a.bin"})"));
    list.backend.click(list.byTestId("Button", "go"));
    list.settle();
    CHECK(list.host.saved.empty());
    CHECK(reported(list, "is over the 1 MiB inline cap"));
    CHECK(reported(list, "1 MiB"));
}

TEST_CASE("cov h download: a host that refuses inline files reports the command as unsupported",
          "[covh][ui][interpreter]") {
    ui::testing::Harness h;
    h.host.refuseTransfers = true;
    auto const screen = h.mount(documentOf(R"({"download":"YWJj","from":"bytes","save":"dialog","name":"a.bin"})"));
    h.backend.click(h.byTestId("Button", "go"));
    h.settle();
    CHECK(h.host.saved.empty());
    CHECK(reported(h, "`download` is not supported by the host"));
}
