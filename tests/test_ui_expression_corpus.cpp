// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <fstream>
#include <map>
#include <morph/ui/expression.hpp>
#include <sstream>
#include <string>
#include <string_view>

namespace ui = morph::ui;

namespace {

// Names from the corpus's `names`, units from its `units`, and the case's locale.
class CorpusContext : public ui::ExpressionContext {
public:
    std::map<std::string, ui::Value, std::less<>> names;
    ui::UnitTable table;
    std::string activeLocale = "en";

    [[nodiscard]] ui::Value name(std::string_view name) const override {
        auto const found = names.find(name);
        return found == names.end() ? ui::Value{} : found->second;
    }
    [[nodiscard]] ui::UnitTable const& units() const override { return table; }
    [[nodiscard]] std::string locale() const override { return activeLocale; }
};

[[nodiscard]] ui::Json const& corpus() {
    static ui::Json const kCorpus = [] {
        std::ifstream file{MORPH_UI_EXPRESSION_CORPUS};
        REQUIRE(file.is_open());
        std::ostringstream text;
        text << file.rdbuf();
        ui::Json dom{};
        REQUIRE_FALSE(glz::read_json(dom, text.str()));
        REQUIRE(dom["cases"].get_array().size() > 2);
        return dom;
    }();
    return kCorpus;
}

[[nodiscard]] ui::Value untyped(ui::Json const& json) {
    std::expected<ui::Value, std::string> value = ui::decodeUntyped(json);
    REQUIRE(value.has_value());
    return *value;
}

[[nodiscard]] CorpusContext context() {
    ui::Json const& data = corpus();
    CorpusContext out;
    for (auto const& [name, value] : data["names"].get_object()) {
        out.names.emplace(name, untyped(value));
    }
    for (ui::Json const& unit : data["units"]["units"].get_array()) {
        out.table.add({.id = unit["id"].get_string(),
                       .display = unit["display"].get_string(),
                       .places = static_cast<std::uint32_t>(*ui::detail::exactInt(unit["places"]))});
    }
    for (ui::Json const& ratio : data["units"]["ratios"].get_array()) {
        out.table.relate({.from = ratio["from"].get_string(),
                          .to = ratio["to"].get_string(),
                          .fromTo = *untyped(ratio["fromTo"]).asDecimal()});
    }
    return out;
}

}  // namespace

// Each case evaluates to its `value`, or is null and reports its `report`, or is refused at parse with its
// `refused`. The corpus is shared: the interpreter and the server's registration read the same file.
// Mutations: any of the library's (each case names the behaviour it pins); report nothing for a runtime error.
TEST_CASE("ui::evaluate: the expression corpus", "[ui][expr]") {
    CorpusContext base = context();
    for (ui::Json const& entry : corpus()["cases"].get_array()) {
        INFO(entry["name"].get_string());
        std::expected<ui::Expr, ui::Problem> tree = ui::parseExpression(entry["expr"], "/expr");
        if (entry.contains("refused")) {
            REQUIRE_FALSE(tree.has_value());
            CHECK(tree.error().reason == entry["refused"].get_string());
            continue;
        }
        REQUIRE(tree.has_value());
        base.activeLocale = entry.contains("locale") ? entry["locale"].get_string() : std::string{"en"};
        ui::Diagnostics diagnostics;
        ui::Value const value = ui::evaluate(*tree, base, diagnostics);
        if (entry.contains("report")) {
            CHECK(value.isNull());
            REQUIRE(diagnostics.reported().size() == 1);
            CHECK(diagnostics.reported().front().reason == entry["report"].get_string());
            continue;
        }
        CHECK(diagnostics.reported().empty());
        CHECK(ui::canonicalJson(value) == ui::canonicalJson(untyped(entry["value"])));
    }
}
