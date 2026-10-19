// SPDX-License-Identifier: Apache-2.0

// Documents checked against action effects (spec 8 §10): a query must read, and two warnings point at mutations
// nothing refreshes after and at watches that may feed another client's changes back.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <morph/ui/document.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ui = morph::ui;
using ui::ActionEffect;
using ui::ActionInfo;
using ui::ActionRecordInfo;
using ui::LoadWarning;
using ui::LoadWarningKind;

namespace {

/// One model, `T_Model`. Its reads hold `T_Row` (`T_List`) or `T_Other` (`T_ListOther`); its writes declare
/// `updates T_Row` (`T_Edit`), `changes T_Row` (`T_Rename`), `changesInstance` (`T_Reset`), `none` (`T_Open`), or
/// nothing (`T_Write`).
ui::Catalog testCatalog() {
    ui::Catalog catalog;
    catalog.model = [](std::string_view type) -> std::optional<ui::ModelInfo> {
        if (type != "T_Model") {
            return std::nullopt;
        }
        return ui::ModelInfo{};
    };
    catalog.action = [](std::string_view action) -> std::optional<ActionInfo> {
        ActionInfo info;
        info.modelType = "T_Model";
        auto const effect = [](std::string kind, std::optional<std::string> record) {
            return std::vector<ActionEffect>{ActionEffect{.kind = std::move(kind), .record = std::move(record)}};
        };
        if (action == "T_List") {
            info.records = {ActionRecordInfo{.record = "T_Row", .keyField = "id", .paths = {""}}};
        } else if (action == "T_ListOther") {
            info.records = {ActionRecordInfo{.record = "T_Other", .keyField = "id", .paths = {""}}};
        } else if (action == "T_Edit") {
            info.effects = effect("updates", "T_Row");
            info.records = {ActionRecordInfo{.record = "T_Row", .keyField = "id", .paths = {""}}};
        } else if (action == "T_Rename") {
            info.effects = effect("changes", "T_Row");
        } else if (action == "T_Reset") {
            info.effects = effect("changesInstance", std::nullopt);
        } else if (action == "T_Open") {
            info.effects = effect("none", std::nullopt);
        } else if (action != "T_Write") {
            return std::nullopt;
        }
        return info;
    };
    return catalog;
}

/// A document with one model alias `m`, the given queries, mutations and watches, and a text view.
std::string document(std::string_view queries, std::string_view mutations, std::string_view watches = "[]") {
    return R"({"vocab":"ui/1","id":"t","models":{"m":{"type":"T_Model"}},"queries":)" + std::string{queries} +
           R"(,"mutations":)" + std::string{mutations} + R"(,"watch":)" + std::string{watches} +
           R"(,"view":{"kind":"text","text":"x"}})";
}

ui::Document load(std::string const& text) {
    auto loaded = ui::loadDocumentText(text, ui::LoadOptions{.catalog = testCatalog()});
    if (!loaded) {
        FAIL(loaded.error().message());
    }
    return std::move(*loaded);
}

std::vector<LoadWarning> warningsOf(std::string const& text, LoadWarningKind kind) {
    std::vector<LoadWarning> out;
    for (LoadWarning const& warning : load(text).warnings) {
        if (warning.kind == kind) {
            out.push_back(warning);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("read_only_document_loads_without_warnings", "[push][documents]") {
    auto const doc =
        load(document(R"({"rows":{"model":"m","action":"T_List"}})",
                      R"({"edit":{"model":"m","action":"T_Edit"},"open":{"model":"m","action":"T_Open"}})"));
    CHECK(doc.warnings.empty());
}

TEST_CASE("query_of_action_with_effects_refused", "[push][documents]") {
    for (std::string_view const action : {"T_Edit", "T_Rename", "T_Reset", "T_Open"}) {
        CAPTURE(action);
        auto const loaded =
            ui::loadDocumentText(document(R"({"rows":{"model":"m","action":")" + std::string{action} + R"("}})", "{}"),
                                 ui::LoadOptions{.catalog = testCatalog()});
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().path == "/queries/rows");
        CHECK(loaded.error().reason.contains("a query reads"));
    }
    // An evaluation runs no handler, so it changes nothing whatever the action declares.
    auto const evaluated = ui::loadDocumentText(document(R"({"preview":{"model":"m","evaluate":"T_Edit"}})", "{}"),
                                                ui::LoadOptions{.catalog = testCatalog()});
    CHECK(evaluated.has_value());
    // Without the catalog's actions nothing is known about effects.
    CHECK(ui::loadDocumentText(document(R"({"rows":{"model":"m","action":"T_Edit"}})", "{}")).has_value());
}

TEST_CASE("uncovered_mutation_warns", "[push][documents]") {
    auto const uncovered = warningsOf(
        document(R"({"rows":{"model":"m","action":"T_List"}})", R"({"write":{"model":"m","action":"T_Write"}})"),
        LoadWarningKind::Uncovered);
    REQUIRE(uncovered.size() == 1);
    CHECK(uncovered.front().path == "/mutations/write");
    CHECK(uncovered.front().text.contains("`T_Write`"));

    // A `refetch` after a success is a refresh the document declares itself.
    CHECK(warningsOf(document(R"({"rows":{"model":"m","action":"T_List"}})",
                              R"({"write":{"model":"m","action":"T_Write","onSuccess":[{"refetch":"rows"}]}})"),
                     LoadWarningKind::Uncovered)
              .empty());
    // Without the catalog's actions nothing is known about effects.
    auto const blind = ui::loadDocumentText(document("{}", R"({"write":{"model":"m","action":"T_Write"}})"));
    REQUIRE(blind.has_value());
    CHECK(blind->warnings.empty());
}

TEST_CASE("effects_none_silences_uncovered", "[push][documents]") {
    auto const doc = load(document("{}", R"({"open":{"model":"m","action":"T_Open"}})"));
    CHECK(doc.warnings.empty());
}

TEST_CASE("watch_feedback_warns", "[push][documents]") {
    auto const fed = warningsOf(
        document(R"({"rows":{"model":"m","action":"T_List"}})", R"({"rename":{"model":"m","action":"T_Rename"}})",
                 R"([{"on":{"query":"rows"},"do":[{"run":"rename"}]}])"),
        LoadWarningKind::WatchFeedback);
    REQUIRE(fed.size() == 1);
    CHECK(fed.front().path == "/watch/0");
    CHECK(fed.front().text.contains("\"source\": \"user\""));

    // Read through a `let`, and run from a nested command.
    auto const nested = warningsOf(
        R"({"vocab":"ui/1","id":"t","models":{"m":{"type":"T_Model"}},
            "queries":{"rows":{"model":"m","action":"T_List"}},
            "let":{"count":{"fn":"length","args":[{"query":"rows"}]}},
            "mutations":{"reset":{"model":"m","action":"T_Reset"}},
            "watch":[{"on":{"ref":"count"},"do":[{"if":[true,[{"run":"reset"}],[]]}]}],
            "view":{"kind":"text","text":"x"}})",
        LoadWarningKind::WatchFeedback);
    CHECK(nested.size() == 1);

    // A watch of state, or one running a mutation that changes nothing, cannot feed a change back.
    CHECK(warningsOf(R"({"vocab":"ui/1","id":"t","models":{"m":{"type":"T_Model"}},"state":{"s":{"init":0}},
                         "queries":{"rows":{"model":"m","action":"T_List"}},
                         "mutations":{"rename":{"model":"m","action":"T_Rename"},"open":{"model":"m","action":"T_Open"}},
                         "watch":[{"on":{"ref":"s"},"do":[{"run":"rename"}]},
                                  {"on":{"query":"rows"},"do":[{"run":"open"}]}],
                         "view":{"kind":"text","text":"x"}})",
                     LoadWarningKind::WatchFeedback)
              .empty());
}

TEST_CASE("watch_source_user_does_not_warn", "[push][documents]") {
    auto const doc =
        load(document(R"({"rows":{"model":"m","action":"T_List"}})", R"({"rename":{"model":"m","action":"T_Rename"}})",
                      R"([{"on":{"query":"rows"},"source":"user","do":[{"run":"rename"}]}])"));
    CHECK(doc.warnings.empty());
}
