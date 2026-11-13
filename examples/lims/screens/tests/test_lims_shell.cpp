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
#include <morph/ui/shell_parts.hpp>
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
#include "shell_screens.hpp"

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
    if (auto set = served.setShellParts(lims::screens::shellParts(available), lims::screens::shellReads()); !set) {
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
             "lims.ledger.accounts"),
         "lims.ledger.accounts", "ledger", false});
    entries.push_back(
        {moduleProbe<lims::StockModel, lims::ListItems, lims::StockModel, lims::CreateItem>("test.inventory"),
         "test.inventory", "inventory", false});
    entries.push_back(
        {moduleProbe<lims::InvoiceModel, lims::CountPendingApprovals, lims::InvoiceModel, lims::ApproveInvoice>(
             "lims.approvals"),
         "lims.approvals", "shell", false});
    entries.push_back({lims::screens::aboutScreen(), lims::screens::kAbout, "shell", false});
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
    return finish(served, lims::screens::ShellAvailability{
                              .modules = {"lab", "billing", "ledger", "inventory"}, .approvals = true, .about = true});
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
    // One account in the first company julia sees, so a search for "a" finds a hit the ledger's source answers.
    lims::screens::ScenarioClient julia{"julia"};
    auto const scopes = julia.call<lims::OrgModel>(lims::ListMyScopes{});
    REQUIRE_FALSE(scopes.scopes.empty());
    static_cast<void>(julia.call<lims::LedgerModel>(lims::CreateAccount{.company = scopes.scopes.front().company,
                                                                        .code = "1200",
                                                                        .name = "Accounts receivable",
                                                                        .type = lims::AccountType::Asset}));
}

/// @brief What the shell reported to a signed-in person, and what it drew for them.
struct Shown {
    /// @brief The problems the client reported before the person signed in.
    std::vector<std::string> beforeSignIn;
    /// @brief The problems it reported from the sign-in on.
    std::vector<std::string> afterSignIn;
    /// @brief Whether the scope switcher was drawn once the scopes were read.
    bool switcherDrawn = false;
    /// @brief The switcher's face: the chosen scope's name.
    std::string scope;
    /// @brief The switcher's entries, one per line.
    std::string scopes;
    /// @brief Whether the person's menu was drawn once the person was read.
    bool personDrawn = false;
    /// @brief The person's menu face: their name.
    std::string person;
    /// @brief The person's menu entries, one per line.
    std::string personEntries;
    /// @brief The source headings the search drew for the text "a", in order.
    std::vector<std::string> searchHeadings;
    /// @brief Whether the search drew its "nothing found" row.
    bool searchEmpty = false;
    /// @brief The modules the navigator draws a group for, in the order tried.
    std::vector<std::string> modules;
    /// @brief The product mark's title.
    std::string brand;
    /// @brief The version in the sidebar's foot.
    std::string version;
    /// @brief The connection state in the sidebar's foot.
    std::string connection;
    /// @brief Whether the help button was drawn.
    bool helpDrawn = false;
    /// @brief The initials in the person's trigger.
    std::string avatar;
    /// @brief The screens mounted after the help button was pressed.
    std::vector<std::string> afterHelp;
};

/// @brief Whether a widget of @p kind with @p testId exists.
/// @param harness The harness.
/// @param kind The widget's kind.
/// @param testId The widget's test id.
/// @return Whether it does.
bool exists(ScreenHarness& harness, std::string_view kind, std::string_view testId) {
    return harness.backend().find(kind, "testId", testId).has_value();
}

/// @brief Whether the widget of @p kind with @p testId is drawn.
/// @param harness The harness.
/// @param kind The widget's kind.
/// @param testId The widget's test id.
/// @return Whether it exists and is visible.
bool drawn(ScreenHarness& harness, std::string_view kind, std::string_view testId) {
    auto const found = harness.backend().find(kind, "testId", testId);
    return found.has_value() && harness.backend().prop(*found, "visible") == "true";
}

/// @brief The search headings the shell draws, in order.
/// @param harness The harness.
/// @return Their texts.
std::vector<std::string> searchHeadings(ScreenHarness& harness) {
    std::vector<std::string> headings;
    // A source with no hits draws no heading, so the places are not contiguous.
    for (std::size_t source = 0; source < 8; ++source) {
        if (auto const found = harness.text("shell_search_source_" + std::to_string(source))) {
            headings.push_back(*found);
        }
    }
    return headings;
}

/// @brief Signs @p login in against a server that authorizes as lims does, waits until the shell has drawn the
///        switcher, the person's menu and the search's answer (each a fact that exists only once its read was
///        answered), and returns what the shell reported and drew.
/// @param login The person's login.
/// @param expectHits Whether the search should find a hit for "a"; otherwise it waits for the empty row.
/// @return What it reported and drew.
Shown signedIn(std::string const& login, bool expectHits, bool german = false) {
    morph::client::LocalApp app{.name = "lims-shell", .bundle = &modulesBundle, .setUp = {}};
    app.setUp = [](morph::client::LocalSetup const& setup) { lims::screens::prepareLocal(setup.user); };
    app.serverAuthorizer = [] {
        return std::make_shared<lims::auth::LimsAuthorizer>(
            "lims-local-development", morph::session::hmacSha256,
            [](std::string_view principal) { return lims::OrgModel::rolesAnywhere(principal); });
    };
    // The harness returns once the first screen is mounted, which the shell does when the switcher has answered or
    // been refused: the signed-out reads are answered by then.
    ScreenHarness harness{app, std::string{kProbeLogin}, RigMode::Simulated, {}, morph::client::LocalSetup{}};
    Shown shown;
    shown.beforeSignIn = harness.problems();
    harness.edit("username", login);
    harness.click("signIn");
    static_cast<void>(
        harness.waitFor([&harness] { return !harness.mounted().empty() && harness.mounted().back() == kProbeHome; }));
    auto const faceOf = [&harness](std::string_view testId) {
        auto const found = harness.backend().find("Menu", "testId", testId);
        return found ? harness.backend().prop(*found, "label") : std::string{};
    };
    auto const entriesOf = [&harness](std::string_view testId) {
        auto const found = harness.backend().find("Menu", "testId", testId);
        return found ? harness.backend().prop(*found, "items") : std::string{};
    };
    // The switcher is drawn only once the scopes have been read; the person's face is empty until the read's answer is
    // drawn, so that wait is on the face, not on the menu's existence.
    shown.switcherDrawn = harness.waitFor([&] { return drawn(harness, "Menu", "shell_context"); });
    shown.personDrawn = harness.waitFor(
        [&] { return drawn(harness, "Menu", "shell_principal_menu") && !faceOf("shell_principal_menu").empty(); });
    if (german) {
        // The language entry sits among the menu's rows after the roles; find it by its label, not its place.
        auto const menu = harness.backend().find("Menu", "testId", "shell_principal_menu");
        REQUIRE(menu.has_value());
        std::string const items = entriesOf("shell_principal_menu");
        std::size_t index = 0;
        std::size_t at = 1;
        for (; at < items.size(); ++index) {
            std::size_t end = at;
            while (end < items.size() && items.at(end) != ',' && items.at(end) != ']') {
                end += items.at(end) == '\\' ? 2 : 1;
            }
            if (items.compare(at, 6, "German") == 0) {
                break;
            }
            at = end + 1;
        }
        REQUIRE(at < items.size());
        harness.backend().chooseMenuEntry(*menu, {index});
        static_cast<void>(harness.waitFor([&] { return entriesOf("shell_context").contains("Standort"); }));
    }
    shown.scope = faceOf("shell_context");
    shown.scopes = entriesOf("shell_context");
    shown.person = faceOf("shell_principal_menu");
    shown.personEntries = entriesOf("shell_principal_menu");
    // Search for "a" and wait until the sources have answered (the busy row is gone and a heading is drawn): every
    // part has then been read once.
    if (auto const search = harness.backend().find("TextInput", "testId", "shell_search")) {
        harness.backend().edit(*search, "a");
        // The shell shows its busy row once it has taken the text and armed the pause; only then does time pass.
        static_cast<void>(harness.waitFor([&] { return harness.text("shell_search_busy").has_value(); }));
        harness.advance(std::chrono::milliseconds{morph::ui::kSearchPause});
        static_cast<void>(harness.waitFor([&] {
            return expectHits ? !searchHeadings(harness).empty() : harness.text("shell_search_empty").has_value();
        }));
        shown.searchHeadings = searchHeadings(harness);
        shown.searchEmpty = harness.text("shell_search_empty").has_value();
    }
    for (std::string const module : {"lab", "billing", "ledger", "inventory"}) {
        if (exists(harness, "Row", "nav_module_row_" + module)) {
            shown.modules.push_back(module);
        }
    }
    shown.brand = harness.text("shell_brand_title").value_or("");
    shown.version = harness.text("shell_version").value_or("");
    shown.connection = harness.text("shell_connection").value_or("");
    shown.helpDrawn = drawn(harness, "Button", "shell_help");
    if (auto const person = harness.backend().find("Menu", "testId", "shell_principal_menu")) {
        shown.avatar = harness.backend().prop(*person, "avatar");
    }
    if (auto const help = harness.backend().find("Button", "testId", "shell_help")) {
        harness.backend().click(*help);
        static_cast<void>(harness.waitFor(
            [&harness] { return !harness.mounted().empty() && harness.mounted().back() == lims::screens::kAbout; }));
        shown.afterHelp = harness.mounted();
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

// The people lims has who hold a role see every module's screens: once signed in, the shell draws the switcher with
// the companies they can see, the person's menu with their name and roles, a group per module and the ledger's hit for
// a search, and reports nothing. Mutations: the shell draws no parts (the waits for the switcher and the person's menu
// fail); ListMyScopes names the sites in a company's detail instead of its currency and their number (the analyst's
// switcher line is not "EUR · 2 sites").
TEST_CASE("lims shell: the scenario's analyst and accountant are drawn their scopes, their name and the search",
          "[lims-screens][shell][auth]") {
    morph::ladder::testkit::DbFixture fixture;
    seedPeople();

    auto const julia = signedIn("julia", true);
    for (std::string const& problem : julia.afterSignIn) {
        UNSCOPED_INFO("julia (after sign-in): " << problem);
    }
    CHECK(julia.afterSignIn.empty());
    CHECK(julia.beforeSignIn.empty());
    CHECK(julia.switcherDrawn);
    CHECK(julia.scope == "Rheinlab Analytik GmbH");
    CHECK(julia.scopes ==
          "[Rheinlab Analytik GmbH;checked=true;description=EUR · 2 sites,"
          "Rheinlab Japan K.K.;checked=false;description=JPY · 1 site]");
    CHECK(julia.personDrawn);
    CHECK(julia.person == "Julia Schneider");
    CHECK(julia.avatar == "JS");
    CHECK(julia.brand == "lims");
    CHECK(julia.version == "v1.0");
    CHECK(julia.connection == "Connected");
    CHECK(julia.helpDrawn);
    REQUIRE_FALSE(julia.afterHelp.empty());
    CHECK(julia.afterHelp.back() == std::string{lims::screens::kAbout});
    CHECK(julia.personEntries.contains("Supervisor"));
    CHECK(julia.personEntries.contains("Accountant"));
    CHECK(julia.personEntries.contains("InvoiceApprover"));
    CHECK(julia.searchHeadings == std::vector<std::string>{"Accounts"});
    CHECK(julia.modules == std::vector<std::string>{"lab", "billing", "ledger", "inventory"});

    auto const manpreet = signedIn("manpreet", true);
    for (std::string const& problem : manpreet.afterSignIn) {
        UNSCOPED_INFO("manpreet (after sign-in): " << problem);
    }
    CHECK(manpreet.afterSignIn.empty());
    CHECK(manpreet.beforeSignIn.empty());
    CHECK(manpreet.switcherDrawn);
    CHECK(manpreet.scope == "Rheinlab Analytik GmbH");
    CHECK(manpreet.scopes == "[Rheinlab Analytik GmbH;checked=true;description=EUR · 1 site]");
    CHECK(manpreet.personDrawn);
    CHECK(manpreet.person == "Manpreet Kaur");
    CHECK(manpreet.avatar == "MK");
    CHECK(manpreet.personEntries.contains("Analyst"));
    CHECK_FALSE(manpreet.personEntries.contains("Accountant"));
    CHECK(manpreet.searchHeadings == std::vector<std::string>{"Accounts"});
    CHECK(manpreet.modules == std::vector<std::string>{"lab", "billing", "ledger", "inventory"});
}

// The switcher's detail line is worded from the catalogue in the session's language: a person who chooses German reads
// the German word for a site. Mutations: the model sends the English words as plain text (the line stays "EUR · 2
// sites"); the German catalogue lacks the keys (the line is the English fallback).
TEST_CASE("lims shell: a German session reads the switcher's second line in German", "[lims-screens][shell][auth]") {
    morph::ladder::testkit::DbFixture fixture;
    seedPeople();

    auto const julia = signedIn("julia", true, true);
    CHECK(julia.afterSignIn.empty());
    CHECK(julia.scopes ==
          "[Rheinlab Analytik GmbH;checked=true;description=EUR · 2 Standorte,"
          "Rheinlab Japan K.K.;checked=false;description=JPY · 1 Standort]");
}

// A person who holds no role is refused the billing and ledger models, so the catalog does not list those modules to
// them; the client leaves out the search sources, counts and inbox that name them, and reports nothing, because the
// authoriser is working. They still get a switcher (with no company to offer) and their menu. Mutations: the shell
// draws no parts (the switcher and menu waits fail); the model reports the parts naming a module the catalog hides
// (the problems are not empty).
TEST_CASE("lims shell: a person with no role is drawn the shell without the modules the catalog hides from them",
          "[lims-screens][shell][auth]") {
    morph::ladder::testkit::DbFixture fixture;
    seedPeople();
    auto const shown = signedIn("tobias", false);
    for (std::string const& problem : shown.afterSignIn) {
        UNSCOPED_INFO("tobias (after sign-in): " << problem);
    }
    CHECK(shown.afterSignIn.empty());
    CHECK(shown.beforeSignIn.empty());
    CHECK(shown.switcherDrawn);
    CHECK(shown.scopes == "[]");
    CHECK(shown.personDrawn);
    CHECK(shown.person == "Tobias Krüger");
    CHECK(shown.avatar == "TK");
    CHECK(shown.personEntries.contains("Sign out"));
    CHECK(shown.searchEmpty);
    CHECK(shown.searchHeadings.empty());
    CHECK(shown.modules == std::vector<std::string>{"lab", "inventory"});
}
