// SPDX-License-Identifier: Apache-2.0
#include "shell_probe_bundles.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kit/kit.hpp"
#include "lims/billing/dto/invoice_dto.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/inventory/dto/stock_dto.hpp"
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
#include "shell_screens.hpp"

namespace lims::shelltest {

namespace ui = morph::ui::author;
namespace kit = lims::screens::kit;

std::expected<void, morph::ui::Refusal> list(morph::ui::UiRegistry& served, ui::RegisteredScreen const& registered,
                                             std::string_view id, std::string module, bool isPublic) {
    morph::ui::CatalogEntry entry;
    entry.id = std::string{id};
    entry.title = std::string{id};
    entry.module = std::move(module);
    entry.isPublic = isPublic;
    return served.addScreen(std::move(entry), registered.text);
}

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

namespace {

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
}  // namespace

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
}  // namespace lims::shelltest
