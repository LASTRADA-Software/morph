// SPDX-License-Identifier: Apache-2.0
//
// Message catalogues (spec 6 §19 and §12 "Settings and messages"): a bundle's catalogues become the provider every
// `t` resolves through; `setLocale` re-resolves them; a missing catalogue or key shows the fallback; a malformed
// catalogue is refused at registration and left out at load; and a title is found in every locale the bundle has.

#include <catch2/catch_test_macros.hpp>
#include <map>
#include <morph/ui/bundle.hpp>
#include <morph/ui/catalogue.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <utility>
#include <vector>

#include "ui_interpreter_support.hpp"

using morph::ui::Manifest;
using morph::ui::ManifestFile;
using morph::ui::MessageCatalogues;

namespace {

using Files = std::map<std::string, std::string, std::less<>>;

constexpr char const* kDe = R"({"lims.sample.title": "Probenerfassung", "hello": "Hallo"})";
constexpr char const* kPl = R"({"lims.sample.title": "Rejestracja próbek",
  "samples": ".input {$n :number}\n.match $n\none {{{$n} próbka}}\nfew {{{$n} próbki}}\n* {{{$n} próbek}}"})";

constexpr char const* kScreen = R"({
  "vocab": "ui/1", "id": "msgs",
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "title", "text": { "t": "lims.sample.title", "fallback": "Sample entry" } },
    { "kind": "text", "testId": "count",
      "text": { "t": "samples", "fallback": "{$n} samples", "args": { "n": 3 } } },
    { "kind": "button", "testId": "de", "label": "de", "onClick": { "setLocale": "de" } },
    { "kind": "button", "testId": "deAT", "label": "de-AT", "onClick": { "setLocale": "de-AT" } },
    { "kind": "button", "testId": "pl", "label": "pl", "onClick": { "setLocale": "pl" } },
    { "kind": "button", "testId": "fr", "label": "fr", "onClick": { "setLocale": "fr" } } ] } })";

/// A bundle built by the UI service's registry with a catalogue per locale, read back as a client reads it.
std::pair<Manifest, Files> served(std::vector<std::pair<std::string, std::string>> const& catalogues) {
    morph::ui::UiRegistry registry{morph::ui::ApplicationInfo{.id = "lims", .version = "1"}};
    for (auto const& [id, bytes] : catalogues) {
        auto const added =
            registry.addFile(morph::ui::BundleFile{.kind = "messages", .id = id, .bytes = bytes, .isPublic = false});
        REQUIRE(added);
    }
    auto const built = registry.build();
    REQUIRE(built);
    auto manifest = morph::ui::parseManifest((*built)->manifest);
    REQUIRE(manifest);
    Files files;
    for (auto const& [hash, file] : (*built)->files) {
        files.emplace(hash, file.bytes);
    }
    return {std::move(*manifest), std::move(files)};
}

MessageCatalogues loaded() {
    auto const [manifest, files] = served({{"i18n/de.json", kDe}, {"i18n/pl.json", kPl}});
    MessageCatalogues catalogues = morph::ui::loadCatalogues(manifest, files);
    REQUIRE(catalogues.problems().empty());
    return catalogues;
}

}  // namespace

TEST_CASE("catalogues: setLocale re-resolves every t from the bundle's catalogues", "[ui][catalogue]") {
    morph::ui::testing::Harness h;
    h.session.setTranslations(loaded().provider());
    auto const screen = h.mount(kScreen);
    CHECK(h.text("title") == "Sample entry");
    CHECK(h.text("count") == "3 samples");

    h.backend.click(h.byTestId("Button", "pl"));
    h.settle();
    CHECK(h.text("title") == "Rejestracja próbek");
    CHECK(h.text("count") == "3 próbki");
    CHECK(h.host.log.back() == "setLocale pl");

    h.backend.click(h.byTestId("Button", "de"));
    h.settle();
    CHECK(h.text("title") == "Probenerfassung");
    // The German catalogue has no `samples`: that key shows its fallback.
    CHECK(h.text("count") == "3 samples");
}

TEST_CASE("catalogues: a missing catalogue shows the fallback; a region falls back to its language",
          "[ui][catalogue]") {
    morph::ui::testing::Harness h;
    h.session.setTranslations(loaded().provider());
    auto const screen = h.mount(kScreen);
    h.backend.click(h.byTestId("Button", "fr"));
    h.settle();
    CHECK(h.text("title") == "Sample entry");
    h.backend.click(h.byTestId("Button", "deAT"));
    h.settle();
    CHECK(h.text("title") == "Probenerfassung");
}

TEST_CASE("catalogues: a malformed catalogue is refused at registration", "[ui][catalogue]") {
    morph::ui::UiRegistry registry{morph::ui::ApplicationInfo{.id = "lims", .version = "1"}};
    auto const add = [&registry](std::string id, std::string bytes) {
        return registry.addFile(morph::ui::BundleFile{
            .kind = "messages", .id = std::move(id), .bytes = std::move(bytes), .isPublic = false});
    };
    CHECK_FALSE(add("de", kDe));
    CHECK_FALSE(add("i18n/.json", kDe));
    CHECK_FALSE(add("i18n/d e.json", kDe));
    CHECK_FALSE(add("i18n/de.json", "{not json"));
    CHECK_FALSE(add("i18n/de.json", R"(["hello"])"));
    CHECK_FALSE(add("i18n/de.json", R"({"hello": 3})"));
    CHECK_FALSE(add("i18n/de.json", R"({"!plurals": "x"})"));
    CHECK_FALSE(add("i18n/de.json", R"({"": "x"})"));
    auto const outside = add("i18n/de.json", R"({"hello": "{$n :datetime}"})");
    REQUIRE_FALSE(outside);
    CHECK(outside.error().message().contains("outside the ui/1 subset"));
    CHECK(add("i18n/de.json", kDe));
}

TEST_CASE("catalogues: at load a malformed catalogue is left out and a pattern outside the subset is dropped",
          "[ui][catalogue]") {
    std::string const good = R"({"hello": "Hallo", "bad": "{$n :datetime}"})";
    std::string const broken = R"({"hello": 3})";
    Manifest manifest;
    manifest.files = {
        ManifestFile{.id = "i18n/de.json", .kind = "messages", .hash = morph::ui::contentHash(good), .size = 0},
        ManifestFile{.id = "i18n/fr.json", .kind = "messages", .hash = morph::ui::contentHash(broken), .size = 0},
        ManifestFile{.id = "fr", .kind = "messages", .hash = morph::ui::contentHash(good), .size = 0},
        ManifestFile{.id = "i18n/it.json", .kind = "messages", .hash = morph::ui::contentHash("{}"), .size = 0}};
    Files const files = {{morph::ui::contentHash(good), good}, {morph::ui::contentHash(broken), broken}};
    MessageCatalogues const catalogues = morph::ui::loadCatalogues(manifest, files);
    CHECK(catalogues.locales() == std::vector<std::string>{"de"});
    CHECK(catalogues.lookup("hello", "de") == "Hallo");
    CHECK_FALSE(catalogues.lookup("bad", "de").has_value());
    CHECK_FALSE(catalogues.lookup("hello", "fr").has_value());
    CHECK(catalogues.problems().size() == 4);
}

TEST_CASE("catalogues: search matches a title in every locale the bundle has", "[ui][catalogue]") {
    MessageCatalogues const catalogues = loaded();
    CHECK(morph::ui::textInEveryLocale(catalogues, "lims.sample.title", "Sample entry") ==
          std::vector<std::string>{"Probenerfassung", "Rejestracja próbek"});
    CHECK(morph::ui::matchesInAnyLocale(catalogues, "lims.sample.title", "Sample entry", "proben"));
    CHECK(morph::ui::matchesInAnyLocale(catalogues, "lims.sample.title", "Sample entry", "REJESTRACJA"));
    CHECK(morph::ui::matchesInAnyLocale(catalogues, "lims.sample.title", "Sample entry", "sample"));
    CHECK_FALSE(morph::ui::matchesInAnyLocale(catalogues, "lims.sample.title", "Sample entry", "audit"));
    // A key no catalogue has is found by its fallback alone.
    CHECK(morph::ui::matchesInAnyLocale(catalogues, "lims.audit.title", "Audit trail", "audit"));
}
