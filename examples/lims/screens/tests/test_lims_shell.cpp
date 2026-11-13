// SPDX-License-Identifier: Apache-2.0
//
// The application shell on the screen harness: the scope switcher hands every screen its context, and the shell's
// parts are drawn for the people lims really has, the ones who see all of it and the ones who do not.

#include <algorithm>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <memory>
#include <morph/session/session_auth.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <string_view>
#include <testkit/db_fixture.hpp>
#include <testkit/screen_harness.hpp>
#include <utility>
#include <vector>

#include "kit/kit.hpp"
#include "lims/app/lims_authorizer.hpp"
#include "lims/billing/dto/invoice_dto.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/inventory/dto/stock_dto.hpp"
#include "lims/inventory/models/procurement_model.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims/lab/dto/sample_dto.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/ledger/dto/account_dto.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "lims/org/dto/auth_dto.hpp"
#include "lims/org/models/auth_model.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_screens.hpp"
#include "lims_shell_parts.hpp"
#include "lims_test_support.hpp"
#include "scenario/org_scenario.hpp"
#include "scenario/scenario_client.hpp"
#include "screen_test_support.hpp"
#include "shell_models.hpp"

namespace {

namespace ui = morph::ui::author;
namespace kit = lims::screens::kit;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;

constexpr std::string_view kScoped = "test.scoped";
constexpr std::string_view kProbeLogin = "test.login";
constexpr std::string_view kProbeHome = "test.home";

/// @brief Adds a registered screen to @p served as an entry of @p module.
/// @param served The bundle under construction.
/// @param registered The screen as the registry holds it.
/// @param id The screen's id.
/// @param module The navigator's module.
/// @param isPublic Whether it opens before sign-in.
/// @return Nothing, or the refusal.
std::expected<void, morph::ui::Refusal> list(morph::ui::UiRegistry& served, ui::RegisteredScreen const& registered,
                                             std::string_view id, std::string module, bool isPublic = false) {
    morph::ui::CatalogEntry entry;
    entry.id = std::string{id};
    entry.title = std::string{id};
    entry.module = std::move(module);
    entry.isPublic = isPublic;
    return served.addScreen(std::move(entry), registered.text);
}

/// @brief The bundle's tail every probe shares: the application document, lims's two catalogues and the shell's parts.
/// @param served The bundle under construction.
/// @param available What the shell may declare parts for.
/// @return The bundle, or the refusal.
std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> finish(
    morph::ui::UiRegistry& served, lims::screens::ShellAvailability const& available) {
    if (auto added = lims::screens::addApplicationDocument(served); !added) {
        return std::unexpected(std::move(added.error()));
    }
    if (auto added = lims::screens::addCatalogues(served); !added) {
        return std::unexpected(std::move(added.error()));
    }
    if (auto set = served.setShellParts(lims::screens::shellParts(available)); !set) {
        return std::unexpected(std::move(set.error()));
    }
    return served.build();
}

/// @brief One screen of the ledger module that lists the accounts of the shell's context.
/// @param contextBound Whether the query reads the context; `false` reads the seeded company whatever is chosen.
/// @return The screen.
ui::Screen accountsProbe(bool contextBound) {
    return ui::screen(std::string{kScoped}, [contextBound](ui::Scope& screen) {
        kit::pageHead(screen, "ledger", ui::AnyHandle{"Accounts"});
        auto const ledger = screen.model<lims::LedgerModel>("ledger");
        auto const company = contextBound ? ui::env("context") : ui::AnyHandle{1};
        auto const accounts =
            screen.query<lims::ListAccounts>("accounts", ledger, {{&lims::ListAccounts::company, company}});
        return ui::column(
            {ui::text({.text = ui::format("{0} accounts",
                                          {ui::count(accounts.value().at(&lims::ListAccountsResult::accounts))})})
                 .testId("accountCount"),
             ui::text({.text = ui::coalesce({ui::env("context"), ui::AnyHandle{"none"}})}).testId("context")});
    });
}

/// @brief A bundle of the one probe screen, with the shell declared over the ledger.
/// @param contextBound Whether the probe reads the context.
/// @return The bundle, or the refusal.
std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> scopedBundle(bool contextBound) {
    ui::ScreenRegistry registry;
    if (auto facts = lims::screens::registerShellFacts(registry); !facts) {
        return std::unexpected(facts.error());
    }
    auto registered = registry.add(accountsProbe(contextBound));
    if (!registered) {
        return std::unexpected(registered.error());
    }
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = "lims-shell-test", .version = "1.0"},
                                 registry.catalog()};
    if (auto added = list(served, **registered, kScoped, "ledger"); !added) {
        return std::unexpected(std::move(added.error()));
    }
    return finish(served, lims::screens::ShellAvailability{.modules = {"ledger"}, .approvals = false});
}

/// @brief Seeds the companies the shell switches between: the seeded one with one account, and "Osaka branch" with
/// two.
void seedAccounts() {
    lims::test::grant("acc", lims::LimsRole::Accountant);
    lims::test::grant("sam", lims::LimsRole::Supervisor);
    lims::CompanyId osaka;
    {
        lims::test::ScopedPrincipal const sam{"sam"};
        osaka = lims::OrgModel{}
                    .execute(lims::CreateCompany{.name = "Osaka branch",
                                                 .baseCurrency = lims::CurrencyCode::JPY,
                                                 .fiscalYearStart = lims::Month::April})
                    .id;
        lims::OrgModel{}.execute(
            lims::AssignRole{.principal = "acc", .role = lims::LimsRole::Accountant, .company = osaka, .site = {}});
    }
    lims::test::ScopedPrincipal const acc{"acc"};
    lims::LedgerModel ledger;
    ledger.execute(lims::CreateAccount{.company = lims::test::seededCompany(),
                                       .code = "1200",
                                       .name = "Receivables",
                                       .type = lims::AccountType::Asset});
    for (std::string const code : {"9000", "9100"}) {
        ledger.execute(lims::CreateAccount{
            .company = osaka, .code = code, .name = "Account " + code, .type = lims::AccountType::Asset});
    }
}

/// @brief Whether the widget of @p testId shows @p expected.
/// @param harness The harness.
/// @param testId The widget.
/// @param expected The text.
/// @return Whether it does within the wait budget.
bool shows(ScreenHarness& harness, std::string_view testId, std::string_view expected) {
    return harness.waitForText(testId, expected);
}

/// @brief The sign-in a probe bundle opens with: any login, then the home probe.
/// @return The screen.
ui::Screen loginProbe() {
    return ui::screen(std::string{kProbeLogin}, [](ui::Scope& screen) {
        auto const auth = screen.model<lims::LimsAuthModel>("auth", {.instance = ui::Instance::Private});
        auto const username = ui::element("username");
        auto const login = screen.mutation<lims::LimsLogin>(
            "login", auth,
            {.body = {{&lims::LimsLogin::username, username.value()}},
             .onSuccess = {ui::signIn(ui::MutationRef{"login"}), ui::navigate(std::string{kProbeHome})}});
        return ui::column({ui::textInput({.label = "Username"}).id(username).testId("username"),
                           ui::button({.label = "Sign in", .onClick = ui::run(login)}).testId("signIn")});
    });
}

/// @brief A screen of a module that reads one model at the shell's context and offers one action that needs the
///        module's own role, as the module's real screens will: a person without the role does not see it.
/// @tparam ReadModel The model that answers the read.
/// @tparam Read The read; it takes the company.
/// @tparam WriteModel The model that runs the action.
/// @tparam Write The action that needs a role.
/// @param id The screen's id.
/// @return The screen.
template <typename ReadModel, typename Read, typename WriteModel, typename Write>
ui::Screen moduleProbe(std::string_view id) {
    return ui::screen(std::string{id}, [](ui::Scope& screen) {
        auto const reads = screen.model<ReadModel>("reads");
        auto const writes = screen.model<WriteModel>("writes");
        static_cast<void>(screen.query<Read>("read", reads, {{&Read::company, ui::env("context")}}));
        static_cast<void>(screen.mutation<Write>("write", writes));
        return ui::text({.text = "module"}).testId("module");
    });
}

/// @brief A screen that needs no role: the first a signed-in person lands on.
/// @param id The screen's id.
/// @return The screen.
ui::Screen homeProbe(std::string_view id) {
    return ui::screen(std::string{id}, [](ui::Scope& screen) {
        auto const org = screen.model<lims::OrgModel>("org");
        static_cast<void>(screen.query<lims::ListMyScopes>("scopes", org));
        return ui::text({.text = "home"}).testId("home");
    });
}

/// @brief The bundle of one screen per module, a sign-in, and the shell declared over all of it.
/// @return The bundle, or the refusal.
std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> modulesBundle() {
    ui::ScreenRegistry registry;
    if (auto facts = lims::screens::registerShellFacts(registry); !facts) {
        return std::unexpected(facts.error());
    }
    struct Entry {
        ui::Screen screen;
        std::string_view id;
        std::string module;
        bool isPublic;
    };
    std::vector<Entry> entries;
    entries.push_back({loginProbe(), kProbeLogin, "", true});
    entries.push_back({homeProbe(kProbeHome), kProbeHome, "shell", false});
    entries.push_back(
        {moduleProbe<lims::SampleDirectoryModel, lims::CountSamplesToVerify, lims::SampleModel, lims::RegisterSample>(
             "test.lab"),
         "test.lab", "lab", false});
    entries.push_back(
        {moduleProbe<lims::InvoiceModel, lims::CountOpenInvoiceWork, lims::InvoiceModel, lims::CreateInvoice>(
             "test.billing"),
         "test.billing", "billing", false});
    entries.push_back(
        {moduleProbe<lims::LedgerModel, lims::CountClosingPeriods, lims::LedgerModel, lims::CreateAccount>(
             "test.ledger"),
         "test.ledger", "ledger", false});
    entries.push_back(
        {moduleProbe<lims::StockModel, lims::ListItems, lims::StockModel, lims::CreateItem>("test.inventory"),
         "test.inventory", "inventory", false});
    entries.push_back(
        {moduleProbe<lims::InvoiceModel, lims::CountPendingApprovals, lims::InvoiceModel, lims::ApproveInvoice>(
             "lims.approvals"),
         "lims.approvals", "shell", false});
    std::vector<ui::RegisteredScreen const*> registered;
    for (Entry const& entry : entries) {
        auto added = registry.add(entry.screen);
        if (!added) {
            return std::unexpected(added.error());
        }
        registered.push_back(*added);
    }
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = "lims-shell-test", .version = "1.0"},
                                 registry.catalog()};
    for (std::size_t index = 0; index < entries.size(); ++index) {
        Entry const& entry = entries.at(index);
        if (auto added = list(served, *registered.at(index), entry.id, entry.module, entry.isPublic); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return finish(served, lims::screens::ShellAvailability{.modules = {"lab", "billing", "ledger", "inventory"},
                                                           .approvals = true});
}

/// @brief The lab as the scenario has it: anna supervises the seeded company, and the scenario's people hold their
/// roles.
void seedPeople() {
    {
        lims::screens::ScenarioClient anna{"anna"};
        static_cast<void>(anna.call<lims::OrgModel>(lims::AssignRole{.principal = "anna",
                                                                     .role = lims::LimsRole::Supervisor,
                                                                     .company = lims::OrgModel::seededCompany(),
                                                                     .site = {}}));
    }
    lims::screens::seedOrgScenario();
}

/// @brief What the shell reported to a signed-in person.
struct Shown {
    /// @brief The problems the client reported before the person signed in.
    std::vector<std::string> beforeSignIn;
    /// @brief The problems it reported from the sign-in on.
    std::vector<std::string> afterSignIn;
};

/// @brief Signs @p login in against a server that authorizes as lims does, lets the shell's parts settle, searches,
/// and
///        returns what the shell reported.
/// @param login The person's login.
/// @return What it reported.
Shown signedIn(std::string const& login) {
    morph::client::LocalApp app{.name = "lims-shell", .bundle = &modulesBundle, .setUp = {}};
    app.setUp = [](morph::client::LocalSetup const& setup) { lims::screens::prepareLocal(setup.user); };
    app.serverAuthorizer = [] {
        return std::make_shared<lims::auth::LimsAuthorizer>(
            "lims-local-development", morph::session::hmacSha256,
            [](std::string_view principal) { return lims::OrgModel::rolesAnywhere(principal); });
    };
    ScreenHarness harness{app, std::string{kProbeLogin}, RigMode::Simulated, {}, morph::client::LocalSetup{}};
    Shown shown;
    static_cast<void>(harness.waitFor([] { return false; }, std::chrono::milliseconds{500}));
    shown.beforeSignIn = harness.problems();
    harness.edit("username", login);
    harness.click("signIn");
    static_cast<void>(
        harness.waitFor([&harness] { return !harness.mounted().empty() && harness.mounted().back() == kProbeHome; }));
    static_cast<void>(harness.waitFor(
        [&harness] { return harness.backend().find("Menu", "testId", "shell_principal_menu").has_value(); }));
    // Let the counts and the inbox answer, then search, so every part has been read once.
    static_cast<void>(harness.waitFor([] { return false; }, std::chrono::milliseconds{1500}));
    if (auto const search = harness.backend().find("TextInput", "testId", "shell_search")) {
        harness.backend().edit(*search, "a");
        harness.advance(std::chrono::milliseconds{300});
        static_cast<void>(harness.waitFor([] { return false; }, std::chrono::milliseconds{1500}));
    }
    shown.afterSignIn.assign(harness.problems().begin() + static_cast<std::ptrdiff_t>(shown.beforeSignIn.size()),
                             harness.problems().end());
    return shown;
}

}  // namespace

// Mutations: read the seeded company in the probe's query whatever is chosen (the list stays at "1 accounts"; the
// next case is that probe); name the seeded company in the page head's eyebrow whatever is chosen (it stays
// "Ledger · Main laboratory").
TEST_CASE("lims shell: choosing a company in the switcher changes a screen's list to that company's records",
          "[lims-screens][shell]") {
    morph::ladder::testkit::DbFixture fixture;
    seedAccounts();
    ScreenHarness harness{
        morph::client::LocalApp{.name = "lims-shell", .bundle = [] { return scopedBundle(true); }, .setUp = {}},
        std::string{kScoped},
        RigMode::Local,
        {},
        morph::client::LocalSetup{.db = {}, .user = "acc"}};
    CHECK(shows(harness, "accountCount", "1 accounts"));
    CHECK(shows(harness, "context", "1"));
    CHECK(shows(harness, "head_eyebrow", "Ledger · Main laboratory"));
    auto const menu = harness.backend().find("Menu", "testId", "shell_context");
    REQUIRE(menu.has_value());
    CHECK(harness.backend().prop(*menu, "label").contains("Main laboratory"));
    harness.backend().chooseMenuEntry(*menu, {1});
    CHECK(shows(harness, "accountCount", "2 accounts"));
    CHECK(shows(harness, "context", "2"));
    CHECK(shows(harness, "head_eyebrow", "Ledger · Osaka branch"));
    lims::screens::test::reportProblems(harness);
    CHECK(harness.problems().empty());
}

// Mutation: the same probe with its query reading the seeded company, which the switcher cannot change.
TEST_CASE("lims shell: a screen that does not read the context stays where it was", "[lims-screens][shell]") {
    morph::ladder::testkit::DbFixture fixture;
    seedAccounts();
    ScreenHarness harness{
        morph::client::LocalApp{.name = "lims-shell", .bundle = [] { return scopedBundle(false); }, .setUp = {}},
        std::string{kScoped},
        RigMode::Local,
        {},
        morph::client::LocalSetup{.db = {}, .user = "acc"}};
    REQUIRE(shows(harness, "accountCount", "1 accounts"));
    auto const menu = harness.backend().find("Menu", "testId", "shell_context");
    REQUIRE(menu.has_value());
    harness.backend().chooseMenuEntry(*menu, {1});
    CHECK(shows(harness, "context", "2"));
    CHECK(harness.text("accountCount") == "1 accounts");
}

// The people lims has who hold a role see every module's screens, and the shell reports nothing to them once they
// are signed in. Mutation: declare the inbox's screen without registering it (the client reports it as unlisted).
TEST_CASE("lims shell: the parts for the scenario's analyst and accountant report no problem",
          "[lims-screens][shell][auth]") {
    morph::ladder::testkit::DbFixture fixture;
    seedPeople();
    for (std::string const login : {"julia", "manpreet"}) {
        auto const shown = signedIn(login);
        for (std::string const& problem : shown.afterSignIn) {
            UNSCOPED_INFO(login << " (after sign-in): " << problem);
        }
        CHECK(shown.afterSignIn.empty());
    }
}

// A person who holds no role is refused the billing and ledger models, so the catalog does not list those modules
// to them; the client reports the declaration's search sources, counts and inbox against that catalog as unlisted,
// though the person is simply not permitted them. The case passes while the client reports it and fails once it does
// not, at which point the tag comes off.
TEST_CASE("lims shell: a person with no role is reported modules the catalog hides from them",
          "[lims-screens][shell][auth][!shouldfail]") {
    morph::ladder::testkit::DbFixture fixture;
    seedPeople();
    auto const shown = signedIn("tobias");
    for (std::string const& problem : shown.afterSignIn) {
        UNSCOPED_INFO("tobias (after sign-in): " << problem);
    }
    CHECK(shown.afterSignIn.empty());
}
