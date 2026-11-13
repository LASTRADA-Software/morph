// SPDX-License-Identifier: Apache-2.0

#include "lims_shell_parts.hpp"

#include <algorithm>
#include <array>
#include <morph/ui/author.hpp>
#include <utility>

#include "lims/billing/dto/invoice_dto.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/inventory/dto/requisition_dto.hpp"
#include "lims/inventory/dto/stock_dto.hpp"
#include "lims/inventory/models/procurement_model.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims/lab/dto/sample_dto.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/ledger/dto/account_dto.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "lims/org/dto/role_dto.hpp"
#include "lims/org/dto/shell_dto.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims::screens {

namespace {

namespace ui = morph::ui;

/// @brief One module's declaration: its count and its search, over actions of its own model.
struct ModuleDeclaration {
    /// @brief The module's id, as the catalog's entries name it.
    std::string_view module;
    /// @brief The model that counts the module's open work.
    std::string_view countModel;
    /// @brief The action that counts the module's open work.
    std::string_view count;
    /// @brief The model that searches the module's records.
    std::string_view searchModel;
    /// @brief The action that searches the module's records.
    std::string_view search;
    /// @brief The catalogue key of the heading over the module's hits.
    std::string_view searchTitle;
};

constexpr std::string_view kCompany = "company";

/// @brief The four modules that have a count and a search, in the order the shell draws them.
constexpr std::array<ModuleDeclaration, 4> kModules{{
    {"lab", "SampleDirectoryModel", "CountSamplesToVerify", "SampleDirectoryModel", "SearchSamples",
     "lims.shell.search.samples"},
    {"billing", "InvoiceModel", "CountOpenInvoiceWork", "InvoiceModel", "SearchInvoices",
     "lims.shell.search.invoices"},
    {"ledger", "LedgerModel", "CountClosingPeriods", "LedgerModel", "SearchAccounts", "lims.shell.search.accounts"},
    {"inventory", "ProcurementModel", "CountPendingRequisitions", "StockModel", "SearchStock",
     "lims.shell.search.stock"},
}};

/// @brief A count read at the chosen company.
/// @param module The module's id; empty for an inbox source.
/// @param model The model.
/// @param action The count action.
/// @return The declaration.
[[nodiscard]] ui::ModuleCount companyCount(std::string_view module, std::string_view model, std::string_view action) {
    return ui::ModuleCount{.module = std::string{module},
                           .model = std::string{model},
                           .action = std::string{action},
                           .countField = "count",
                           .contextField = std::string{kCompany},
                           .localeField = {}};
}

}  // namespace

ui::ShellParts shellParts(ShellAvailability const& available) {
    ui::ShellParts parts;
    parts.context = ui::ContextSwitch{.model = "OrgModel",
                                      .action = "ListMyScopes",
                                      .localeField = {},
                                      .optionsField = "scopes",
                                      .labelField = "label",
                                      .valueField = "company",
                                      .detailField = "detail",
                                      .groupField = {}};
    for (ModuleDeclaration const& module : kModules) {
        if (std::ranges::find(available.modules, module.module) == available.modules.end()) {
            continue;
        }
        parts.modules.push_back(companyCount(module.module, module.countModel, module.count));
        parts.search.push_back(ui::SearchSource{.module = std::string{module.module},
                                                .title = std::string{module.searchTitle},
                                                .model = std::string{module.searchModel},
                                                .action = std::string{module.search},
                                                .queryField = "query",
                                                .resultsField = "hits",
                                                .contextField = std::string{kCompany},
                                                .localeField = {}});
    }
    parts.searchHint = "lims.shell.searchHint";
    if (available.approvals) {
        parts.inbox = ui::InboxEntry{.screen = std::string{kApprovals},
                                     .sources = {companyCount({}, "InvoiceModel", "CountPendingApprovals"),
                                                 companyCount({}, "ProcurementModel", "CountPendingRequisitions")}};
    }
    parts.principal = ui::PrincipalMenu{.model = "OrgModel",
                                        .action = "GetSignedInUser",
                                        .contextField = {},
                                        .localeField = {},
                                        .nameField = "displayName",
                                        .detailField = {},
                                        .subtitleField = {},
                                        .rolesField = "roles",
                                        .locales = {"en", "de"},
                                        .formats = {"en", "de"}};
    return parts;
}

morph::ui::Catalog shellReads() {
    morph::ui::author::CatalogFacts facts;
    facts.action<OrgModel, ListMyScopes>();
    facts.action<OrgModel, GetSignedInUser>();
    facts.action<SampleDirectoryModel, CountSamplesToVerify>();
    facts.action<SampleDirectoryModel, SearchSamples>();
    facts.action<InvoiceModel, CountOpenInvoiceWork>();
    facts.action<InvoiceModel, CountPendingApprovals>();
    facts.action<InvoiceModel, SearchInvoices>();
    facts.action<LedgerModel, CountClosingPeriods>();
    facts.action<LedgerModel, SearchAccounts>();
    facts.action<ProcurementModel, CountPendingRequisitions>();
    facts.action<StockModel, SearchStock>();
    return facts.catalog();
}

}  // namespace lims::screens
