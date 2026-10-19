// SPDX-License-Identifier: Apache-2.0

// The `ui/2` vocabulary (spec 8 §10): actions declare their effects, so a `ui/2` document has no `invalidates`,
// `patch` or `refreshOn`. It keeps `optimistic`, `refreshEvery` and the `refetch` command. A `ui/1` document still
// loads with its refresh keys, the authoring layer emits `ui/2`, and a bundle needs the vocabularies its documents
// declare.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <expected>
#include <morph/ui/author.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/service.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui_service_support.hpp"

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

TEST_CASE("ui2_refuses_invalidates", "[push][ui2]") {
    auto const loaded =
        load(document(ui::kVocabulary2, kRows, R"({"edit":{"model":"m","action":"T_Edit","invalidates":["rows"]}})"));
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "/mutations/edit/invalidates");
    CHECK(loaded.error().reason.contains("ui/2"));
    CHECK(loaded.error().reason.contains("effects"));
}

TEST_CASE("ui2_refuses_refresh_on", "[push][ui2]") {
    auto const loaded =
        load(document(ui::kVocabulary2, R"({"rows":{"model":"m","action":"T_List","refreshOn":["T_Edit"]}})", "{}"));
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "/queries/rows/refreshOn");
    CHECK(loaded.error().reason.contains("ui/2"));
    CHECK(loaded.error().reason.contains("effects"));
}

TEST_CASE("ui2_refuses_patch", "[push][ui2]") {
    auto const loaded = load(document(
        ui::kVocabulary2, kRows,
        R"({"edit":{"model":"m","action":"T_Edit","patch":{"query":"rows","key":{"ref":"result.id"},"with":{"ref":"result"}}}})"));
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().path == "/mutations/edit/patch");
    CHECK(loaded.error().reason.contains("ui/2"));
    CHECK(loaded.error().reason.contains("effects"));
}

TEST_CASE("ui2_keeps_optimistic", "[push][ui2]") {
    auto const loaded =
        load(document(ui::kVocabulary2, R"({"rows":{"model":"m","action":"T_List","refreshEvery":1000}})",
                      R"({"edit":{"model":"m","action":"T_Edit",
                    "optimistic":{"query":"rows","key":{"ref":"body.id"},"with":{"ref":"body"}},
                    "onSuccess":[{"refetch":"rows"}]}})"));
    if (!loaded) {
        FAIL(loaded.error().message());
    } else {
        CHECK(loaded->vocab == "ui/2");
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
        ui::kVocabulary2, kRows,
        R"({"edit":{"model":"m","action":"T_Edit","optimistic":{"query":"none","key":1,"with":{"obj":{}}}}})"));
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().path == "/mutations/edit/optimistic/query");
}

TEST_CASE("ui1_document_still_loads_with_refresh_on", "[push][ui2]") {
    auto const loaded =
        load(document(ui::kVocabulary, R"({"rows":{"model":"m","action":"T_List","refreshOn":["T_Edit"]}})",
                      R"({"edit":{"model":"m","action":"T_Edit","invalidates":["rows"],
                    "patch":{"query":"rows","key":{"ref":"result.id"},"with":{"ref":"result"}}}})"));
    if (!loaded) {
        FAIL(loaded.error().message());
    } else {
        CHECK(loaded->vocab == "ui/1");
        REQUIRE(loaded->scope.queries.size() == 1);
        CHECK(loaded->scope.queries.front().refreshOn == std::vector<std::string>{"T_Edit"});
        REQUIRE(loaded->scope.mutations.size() == 1);
        CHECK(loaded->scope.mutations.front().invalidates == std::vector<std::string>{"rows"});
        CHECK(loaded->scope.mutations.front().patch.has_value());
        // `T_Edit` declares no effects, so the document's own refreshes are what refreshes after it.
        CHECK(loaded->warnings.empty());
    }
    auto const other = load(document("ui/3", kRows, "{}"));
    REQUIRE_FALSE(other.has_value());
    CHECK(other.error().path == "/vocab");
}

TEST_CASE("author_emits_ui2", "[push][ui2]") {
    CHECK(ui::kVocabulary == "ui/1");
    CHECK(ui::kVocabulary2 == "ui/2");
    auto const emitted = author::screen("author.ui2", [](author::Scope&) { return author::spacer(); }).write();
    if (!emitted) {
        FAIL(emitted.error().message());
    } else {
        CHECK(emitted->text.starts_with(R"({"vocab":"ui/2","id":"author.ui2")"));
        auto const reloaded = ui::loadDocumentText(emitted->text);
        REQUIRE(reloaded.has_value());
        CHECK(reloaded->vocab == "ui/2");
    }
}

TEST_CASE("bundle_needs_the_vocabularies_its_documents_declare", "[push][ui2]") {
    auto const screenText = [](std::string_view vocab, std::string_view id) {
        return R"({"vocab":")" + std::string{vocab} + R"(","id":")" + std::string{id} +
               R"(","view":{"kind":"text","text":"x"}})";
    };
    auto const built = [&](std::vector<std::string_view> const& vocabs) {
        ui::UiRegistry registry{ui::ApplicationInfo{.id = "app", .version = "1"}};
        for (std::size_t index = 0; index < vocabs.size(); ++index) {
            std::string const id = "s" + std::to_string(index);
            REQUIRE(registry.addScreen(uiservice::entry(id), screenText(vocabs.at(index), id)).has_value());
        }
        auto bundle = registry.build();
        REQUIRE(bundle.has_value());
        return (*bundle)->vocabularies;
    };
    // A bundle of `ui/1` documents is served to a client that speaks only `ui/1`.
    CHECK(built({"ui/1"}) == std::vector<std::string>{"expr/1", "ui/1"});
    CHECK(built({"ui/2"}) == std::vector<std::string>{"expr/1", "ui/2"});
    CHECK(built({"ui/1", "ui/2"}) == std::vector<std::string>{"expr/1", "ui/1", "ui/2"});
    // With no document, the bundle needs the first vocabulary.
    CHECK(built({}) == std::vector<std::string>{"expr/1", "ui/1"});
}
