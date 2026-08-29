// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <string>

#include "crm/core/self_journal.hpp"
#include "crm/dto/account_dto.hpp"

/// @file
/// `AccountModel` — account CRUD (README build order §1). Registered plain,
/// unkeyed: crm's org-wide account list is one shared record set, like
/// `lims::AnalysisCatalogModel`'s lab-wide catalogue, not a per-instance
/// keyed model like `kanban::BoardModel` — there is no natural per-caller
/// sharding boundary for accounts at this step.

namespace crm {

class AccountModel {
public:
    CreateAccountResult execute(const CreateAccount& action);
    UpdateAccountResult execute(const UpdateAccount& action);
    AccountView execute(const GetAccount& action);
    ListAccountsResult execute(const ListAccounts& action);
    ListAccountOptionsResult execute(const ListAccountOptions& action);
    SetAccountRoleResult execute(const SetAccountRole& action);
    GetAccountRolesResult execute(const GetAccountRoles& action);
    GetAccountHistoryResult execute(const GetAccountHistory& action);
    UndoLastAccountChangeResult execute(const UndoLastAccountChange& action);

    /// @brief Attaches a durable action log (see `SelfJournal`'s doc comment
    ///        for why a plain-constructed model needs this called by hand).
    void attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey) {
        _journal.attach(std::move(log), std::move(entityKey));
    }

    /// @brief Every entry this instance has recorded, for the field-level
    ///        audit view (README build order §6).
    [[nodiscard]] std::vector<::morph::journal::LogEntry> journalEntries() const { return _journal.entries(); }

private:
    SelfJournal _journal;
};

}  // namespace crm

BRIDGE_REGISTER_MODEL(crm::AccountModel, "AccountModel")
BRIDGE_REGISTER_ACTION(crm::AccountModel, crm::CreateAccount, "CreateAccount")
BRIDGE_REGISTER_ACTION(crm::AccountModel, crm::UpdateAccount, "UpdateAccount")
BRIDGE_REGISTER_ACTION(crm::AccountModel, crm::GetAccount, "GetAccount", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(crm::AccountModel, crm::ListAccounts, "ListAccounts", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(crm::AccountModel, crm::ListAccountOptions, "ListAccountOptions", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(crm::AccountModel, crm::SetAccountRole, "SetAccountRole")
BRIDGE_REGISTER_ACTION(crm::AccountModel, crm::GetAccountRoles, "GetAccountRoles", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(crm::AccountModel, crm::GetAccountHistory, "GetAccountHistory", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(crm::AccountModel, crm::UndoLastAccountChange, "UndoLastAccountChange")
