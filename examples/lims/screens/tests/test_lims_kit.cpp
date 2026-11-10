// SPDX-License-Identifier: Apache-2.0
//
// The screen kit: money, balances, days and states in words, in English and German, and catalogues that agree.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <fstream>
#include <glaze/glaze.hpp>
#include <map>
#include <memory>
#include <morph/ui/author.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/catalogue.hpp>
#include <morph/ui/message.hpp>
#include <morph/ui/service.hpp>
#include <morph/util/rational.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <testkit/db_fixture.hpp>
#include <testkit/screen_harness.hpp>

#include "kit/kit.hpp"
#include "lims/billing/core/types.hpp"
#include "lims/org/models/org_model.hpp"
#include "scenario/org_scenario.hpp"
#include "scenario/scenario_client.hpp"
#include "screen_test_support.hpp"

namespace {

namespace ui = morph::ui::author;
namespace kit = lims::screens::kit;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;

constexpr std::string_view kProbe = "test.kit";

/// @brief An exact decimal.
/// @param numerator The digits without the point.
/// @param places The digits after the point.
/// @return The decimal.
ui::AnyHandle decimalOf(std::int64_t numerator, std::int64_t denominator, std::int64_t places) {
    return ui::AnyHandle{morph::ui::Decimal{
        morph::math::Rational{morph::math::Numerator{numerator}, morph::math::Denominator{denominator},
                              morph::math::DecimalPlaces{static_cast<std::uint32_t>(places)}}}};
}

/// @brief One screen showing the kit's five formats and one catalogue message with an argument over literals, and a
///        button that switches to German.
/// @return The bundle, with lims's two catalogues, or the refusal.
std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> kitBundle() {
    ui::ScreenRegistry screens;
    auto registered = screens.add(ui::screen(std::string{kProbe}, [](ui::Scope& screen) {
        static_cast<void>(screen);
        return ui::column({
            ui::text({.text = kit::money(decimalOf(1319640, 100, 2), "EUR")}).testId("eur"),
            ui::text({.text = kit::money(decimalOf(1234567, 1, 0), "JPY")}).testId("jpy"),
            ui::text({.text = kit::balance(decimalOf(-1319640, 100, 2), "EUR")}).testId("credit"),
            ui::text({.text = kit::day(ui::AnyHandle{"2026-11-09T09:00:00Z"})}).testId("day"),
            ui::text({.text = kit::statusWord<lims::BillingRunState>(ui::AnyHandle{"FinishedWithFailures"})})
                .testId("state"),
            ui::text({.text = ui::t("lims.billing.draftFor", "Draft for {$client}",
                                    {{"client", ui::AnyHandle{"Stadtwerke Köln"}}})})
                .testId("argument"),
            ui::button({.label = "Deutsch", .onClick = ui::setLocale("de")}).testId("german"),
        });
    }));
    if (!registered) {
        return std::unexpected(registered.error());
    }
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = "lims-kit-test", .version = "1.0"},
                                 screens.catalog()};
    morph::ui::CatalogEntry entry;
    entry.id = std::string{kProbe};
    entry.title = "Kit";
    if (auto added = served.addScreen(std::move(entry), (*registered)->text); !added) {
        return std::unexpected(std::move(added.error()));
    }
    if (auto added = lims::screens::addApplicationDocument(served); !added) {
        return std::unexpected(std::move(added.error()));
    }
    if (auto added = lims::screens::addCatalogues(served); !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

/// @brief A screen showing people's names from a users list: one who is named, one who is not, and one who is not in
///        the list at all.
/// @return The bundle.
std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> namesBundle() {
    ui::ScreenRegistry screens;
    auto registered = screens.add(ui::screen("test.names", [](ui::Scope& screen) {
        auto const org = screen.model<lims::OrgModel>("org");
        auto const users = screen.query<lims::ListUsers>("users", org);
        auto const rows = users.value().at(&lims::ListUsersResult::users);
        return ui::column({
            ui::text({.text = kit::personName(rows, "anna")}).testId("named"),
            ui::text({.text = kit::personName(rows, "nameless")}).testId("unnamed"),
            ui::text({.text = kit::personName(rows, "nobody")}).testId("absent"),
        });
    }));
    if (!registered) {
        return std::unexpected(registered.error());
    }
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = "lims-names-test", .version = "1.0"},
                                 screens.catalog()};
    morph::ui::CatalogEntry entry;
    entry.id = "test.names";
    entry.title = "Names";
    if (auto added = served.addScreen(std::move(entry), (*registered)->text); !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

/// @brief The modules whose catalogue files the screens keep, one file per module and locale.
constexpr std::array<std::string_view, 7> kCatalogueModules{"kit",    "org",       "lab",  "billing",
                                                            "ledger", "inventory", "shell"};

/// @brief One module's catalogue in one locale, read from the source tree.
/// @param locale `en` or `de`.
/// @param module One of `kCatalogueModules`.
/// @return Its keys and messages.
std::map<std::string, std::string> catalogue(std::string_view locale, std::string_view module) {
    std::ifstream file{std::string{LIMS_I18N_DIR} + "/" + std::string{locale} + "/" + std::string{module} + ".json"};
    REQUIRE(file.is_open());
    std::stringstream bytes;
    bytes << file.rdbuf();
    std::map<std::string, std::string> messages;
    REQUIRE_FALSE(glz::read_json(messages, bytes.str()));
    return messages;
}

/// @brief The merged catalogues of lims's bundle, read back as a client reads them.
/// @return The catalogues, one per locale.
morph::ui::MessageCatalogues servedCatalogues() {
    auto const bundle = kitBundle();
    REQUIRE(bundle);
    auto manifest = morph::ui::parseManifest((*bundle)->manifest);
    REQUIRE(manifest);
    std::map<std::string, std::string, std::less<>> files;
    for (auto const& [hash, file] : (*bundle)->files) {
        files.emplace(hash, file.bytes);
    }
    auto catalogues = morph::ui::loadCatalogues(*manifest, files);
    REQUIRE(catalogues.problems().empty());
    return catalogues;
}

/// @brief A catalogue message formatted with named arguments.
/// @param catalogues The catalogues.
/// @param key The message's key.
/// @param locale `en` or `de`.
/// @param args The arguments.
/// @return The text, or empty when the locale holds no such key.
std::string formatted(morph::ui::MessageCatalogues const& catalogues, std::string_view key, std::string_view locale,
                      morph::ui::ValueRecord const& args = {}) {
    auto const pattern = catalogues.lookup(key, locale);
    if (!pattern) {
        return {};
    }
    auto const parsed = morph::ui::parseMessage(*pattern);
    REQUIRE(parsed);
    return morph::ui::formatMessage(*parsed, args, morph::ui::MessageLocale{}).text;
}

}  // namespace

// What the shell shows today: no client installs the bundle's catalogues or a locale's number formats, so a screen
// shows the evaluator's own money form (the amount at the currency's places followed by its code) and every `t` shows
// its fallback. The day follows the application document's date pattern. The catalogue case below measures the
// messages themselves.
// Mutations: make statusWord show the enumerator's name (the state reads "FinishedWithFailures"); leave
// addApplicationDocument out of kitBundle (the day reads "2026-11-09").
TEST_CASE("lims kit: money, balances, days and states as the shell shows them", "[lims-screens][kit]") {
    morph::ladder::testkit::DbFixture fixture;
    ScreenHarness harness{morph::client::LocalApp{.name = "lims-kit", .bundle = &kitBundle, .setUp = {}},
                          std::string{kProbe},
                          RigMode::Local,
                          {},
                          morph::client::LocalSetup{.db = {}, .user = "tech"}};
    lims::screens::test::reportProblems(harness);
    CHECK(harness.waitForText("eur", "13196.40 EUR"));
    CHECK(harness.waitForText("jpy", "1234567 JPY"));
    CHECK(harness.waitForText("credit", "13196.40 EUR Cr"));
    CHECK(harness.waitForText("day", "09-Nov-2026"));
    CHECK(harness.waitForText("state", "Finished with failures"));
    CHECK(harness.waitForText("argument", "Draft for Stadtwerke Köln"));
}

// Mutations: write `lims.billing.draftFor`'s pattern as `Draft for {client}` (MessageFormat 2 reads it as literal
// text, and the argument case reads "Draft for {client}"); rename a state's key in the catalogue (the lookup is
// empty).
TEST_CASE("lims kit: the merged catalogues hold the words, in English and German", "[lims-screens][kit]") {
    auto const catalogues = servedCatalogues();
    CHECK(formatted(catalogues, "lims.status.InvoiceState.Submitted", "en") == "Waiting for approval");
    CHECK(formatted(catalogues, "lims.status.InvoiceState.Submitted", "de") == "Wartet auf Freigabe");
    CHECK(formatted(catalogues, "lims.credit", "en") == "Cr");
    CHECK(formatted(catalogues, "lims.credit", "de") == "H");
    morph::ui::ValueRecord const client{{"client", morph::ui::Value{"Stadtwerke Köln"}}};
    CHECK(formatted(catalogues, "lims.billing.draftFor", "en", client) == "Draft for Stadtwerke Köln");
    CHECK(formatted(catalogues, "lims.billing.draftFor", "de", client) == "Entwurf für Stadtwerke Köln");
}

// Mutation: build statusWord's key from another name than the enumeration's (the key is not in the catalogue).
TEST_CASE("lims kit: every state a screen can show has its words in both locales", "[lims-screens][kit]") {
    auto const catalogues = servedCatalogues();
    for (std::string_view const name : {"Draft", "Submitted", "Approved", "Posted"}) {
        std::string const key = "lims.status.InvoiceState." + std::string{name};
        INFO(key);
        CHECK_FALSE(formatted(catalogues, key, "en").empty());
        CHECK_FALSE(formatted(catalogues, key, "de").empty());
        // The words the screen asks for are the catalogue's key, not only a key the catalogue happens to hold.
        auto const word = kit::statusWord<lims::InvoiceState>(ui::AnyHandle{std::string{name}});
        std::string text;
        REQUIRE_FALSE(glz::write_json(word.json(), text));
        CHECK(text.find("\"" + key + "\"") != std::string::npos);
    }
}

// Mutations: delete one key from de/org.json (the case names it); copy one key of kit.json into org.json (the
// duplicate check names it).
TEST_CASE("lims kit: each module's catalogues hold the same keys in both locales, and no key is in two modules",
          "[lims-screens][kit]") {
    std::map<std::string, std::string> owner;
    for (std::string_view const module : kCatalogueModules) {
        auto const english = catalogue("en", module);
        auto const german = catalogue("de", module);
        for (auto const& [key, message] : english) {
            static_cast<void>(message);
            INFO("missing from de/" << module << ".json: " << key);
            CHECK(german.contains(key));
            INFO(key << " is in both " << owner[key] << ".json and " << module << ".json");
            CHECK((owner[key].empty() || owner[key] == module));
            owner[key] = std::string{module};
        }
        for (auto const& [key, message] : german) {
            static_cast<void>(message);
            INFO("missing from en/" << module << ".json: " << key);
            CHECK(english.contains(key));
        }
    }
}

// Mutations: make personName answer the principal always ("named" reads "anna"); drop the non-empty-name condition
// from its filter (a person with no name reads as the empty string, not the login).
TEST_CASE("lims kit: a person is shown by their display name, and by their login where they have none",
          "[lims-screens][kit]") {
    morph::ladder::testkit::DbFixture fixture;
    {
        // The first supervisor is appointed by the bootstrap assignment, as local mode does before the scenario.
        lims::screens::ScenarioClient anna{"anna"};
        static_cast<void>(anna.call<lims::OrgModel>(lims::AssignRole{.principal = "anna",
                                                                     .role = lims::LimsRole::Supervisor,
                                                                     .company = lims::OrgModel::seededCompany(),
                                                                     .site = {}}));
    }
    lims::screens::seedOrgScenario();
    {
        // A person who holds a role and was never named: the scenario names everyone it creates.
        lims::screens::ScenarioClient julia{"julia"};
        auto const companies = julia.call<lims::OrgModel>(lims::ListCompanies{}).companies;
        auto const analytik =
            std::ranges::find(companies, std::string{"Rheinlab Analytik GmbH"}, &lims::CompanyView::name);
        REQUIRE(analytik != companies.end());
        static_cast<void>(julia.call<lims::OrgModel>(lims::AssignRole{
            .principal = "nameless", .role = lims::LimsRole::Analyst, .company = analytik->id, .site = {}}));
    }
    ScreenHarness harness{morph::client::LocalApp{.name = "lims-names", .bundle = &namesBundle, .setUp = {}},
                          "test.names",
                          RigMode::Local,
                          {},
                          morph::client::LocalSetup{.db = {}, .user = "julia"}};
    lims::screens::test::reportProblems(harness);
    CHECK(harness.waitForText("named", "Dr. Anna Meyer"));
    CHECK(harness.waitForText("unnamed", "nameless"));
    CHECK(harness.waitForText("absent", "nobody"));
}
