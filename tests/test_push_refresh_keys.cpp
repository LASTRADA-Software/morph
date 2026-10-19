// SPDX-License-Identifier: Apache-2.0

// The refresh keys (spec 8 §10): actions declare their effects, so a document has no `invalidates`, `patch` or
// `refreshOn`. It keeps `optimistic`, `refreshEvery` and the `refetch` command.

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <morph/ui/author.hpp>
#include <morph/ui/document.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace ui = morph::ui;
namespace author = morph::ui::author;

namespace {

/// One model, `T_Model`: `T_List` reads `T_Row`, `T_Edit` declares no effects.
ui::Catalog testCatalog() {
    ui::Catalog catalog;
    catalog.model = [](std::string_view type) -> std::optional<ui::ModelInfo> {
        if (type != "T_Model") {
            return std::nullopt;
        }
        return ui::ModelInfo{};
    };
    catalog.action = [](std::string_view action) -> std::optional<ui::ActionInfo> {
        if (action != "T_List" && action != "T_Edit") {
            return std::nullopt;
        }
        ui::ActionInfo info;
        info.modelType = "T_Model";
        if (action == "T_List") {
            info.records = {ui::ActionRecordInfo{.record = "T_Row", .keyField = "id", .paths = {""}}};
        }
        return info;
    };
    return catalog;
}

/// A document of @p vocab with one model alias `m`, the given queries and mutations, and a text view.
std::string document(std::string_view vocab, std::string_view queries, std::string_view mutations) {
    return R"({"vocab":")" + std::string{vocab} + R"(","id":"t","models":{"m":{"type":"T_Model"}},"queries":)" +
           std::string{queries} + R"(,"mutations":)" + std::string{mutations} +
           R"(,"view":{"kind":"text","text":"x"}})";
}

std::expected<ui::Document, ui::Refusal> load(std::string const& text) {
    return ui::loadDocumentText(text, ui::LoadOptions{.catalog = testCatalog()});
}

constexpr std::string_view kRows = R"({"rows":{"model":"m","action":"T_List"}})";

}  // namespace

TEST_CASE("refuses_invalidates", "[push][refresh]") {
    auto const loaded =
        load(document(ui::kVocabulary, kRows, R"({"edit":{"model":"m","action":"T_Edit","invalidates":["rows"]}})"));
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "/mutations/edit/invalidates");
    CHECK(loaded.error().reason.contains("does not declare"));
    CHECK(loaded.error().reason.contains("effects"));
}

TEST_CASE("refuses_refresh_on", "[push][refresh]") {
    auto const loaded =
        load(document(ui::kVocabulary, R"({"rows":{"model":"m","action":"T_List","refreshOn":["T_Edit"]}})", "{}"));
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "/queries/rows/refreshOn");
    CHECK(loaded.error().reason.contains("does not declare"));
    CHECK(loaded.error().reason.contains("effects"));
}

TEST_CASE("refuses_patch", "[push][refresh]") {
    auto const loaded = load(document(
        ui::kVocabulary, kRows,
        R"({"edit":{"model":"m","action":"T_Edit","patch":{"query":"rows","key":{"ref":"result.id"},"with":{"ref":"result"}}}})"));
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "/mutations/edit/patch");
    CHECK(loaded.error().reason.contains("does not declare"));
    CHECK(loaded.error().reason.contains("effects"));
}

TEST_CASE("keeps_optimistic_refresh_every_and_refetch", "[push][refresh]") {
    auto const loaded =
        load(document(ui::kVocabulary, R"({"rows":{"model":"m","action":"T_List","refreshEvery":1000}})",
                      R"({"edit":{"model":"m","action":"T_Edit",
                    "optimistic":{"query":"rows","key":{"ref":"body.id"},"with":{"ref":"body"}},
                    "onSuccess":[{"refetch":"rows"}]}})"));
    if (!loaded) {
        FAIL(loaded.error().message());
    } else {
        CHECK(loaded->vocab == "ui/1");
        REQUIRE(loaded->scope.queries.size() == 1);
        CHECK(loaded->scope.queries.front().refreshEvery == 1000);
        REQUIRE(loaded->scope.mutations.size() == 1);
        ui::MutationDecl const& edit = loaded->scope.mutations.front();
        REQUIRE(edit.optimistic.has_value());
        CHECK(edit.optimistic->query == "rows");
        CHECK(edit.optimistic->key != nullptr);
        CHECK(edit.optimistic->with != nullptr);
        REQUIRE(edit.onSuccess.size() == 1);
        CHECK(edit.onSuccess.front().kind == ui::CommandKind::Refetch);
    }
    // An optimistic write still names a query of the document.
    auto const unknown = load(document(
        ui::kVocabulary, kRows,
        R"({"edit":{"model":"m","action":"T_Edit","optimistic":{"query":"none","key":1,"with":{"obj":{}}}}})"));
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().path == "/mutations/edit/optimistic/query");
}

// Mutation: accept another vocabulary alongside ui/1 — a `ui/2` document loads.
TEST_CASE("one_vocabulary_is_read", "[push][refresh]") {
    auto const other = load(document("ui/2", kRows, "{}"));
    REQUIRE_FALSE(other.has_value());
    CHECK(other.error().path == "/vocab");
}

TEST_CASE("author_emits_ui1", "[push][refresh]") {
    auto const emitted = author::screen("author.refresh", [](author::Scope&) { return author::spacer(); }).write();
    if (!emitted) {
        FAIL(emitted.error().message());
    } else {
        CHECK(emitted->text.starts_with(R"({"vocab":"ui/1","id":"author.refresh")"));
        auto const reloaded = ui::loadDocumentText(emitted->text);
        REQUIRE(reloaded.has_value());
        CHECK(reloaded->vocab == "ui/1");
    }
}
