// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <string>

#include "crm/core/self_journal.hpp"
#include "crm/dto/lead_dto.hpp"

/// @file
/// `LeadModel` — lead capture/edit/list, the terminal `MarkLeadLost`
/// transition (README build order §1), and `ConvertLead` (§3, the
/// multi-model transactional action). `LeadModel` is the "one orchestrating
/// model" `ConvertLead`'s own doc comment (lead_dto.hpp) names — it writes
/// Account/Contact/Opportunity rows via direct ORM calls, never dispatching
/// those models' own `execute()`.

namespace crm {

class LeadModel {
public:
    CreateLeadResult execute(const CreateLead& action);
    UpdateLeadResult execute(const UpdateLead& action);
    MarkLeadLostResult execute(const MarkLeadLost& action);
    LeadView execute(const GetLead& action);
    ListLeadsResult execute(const ListLeads& action);
    ConvertLeadResult execute(const ConvertLead& action);

    void attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey) {
        _journal.attach(std::move(log), std::move(entityKey));
    }

    [[nodiscard]] std::vector<::morph::journal::LogEntry> journalEntries() const { return _journal.entries(); }

private:
    SelfJournal _journal;
};

}  // namespace crm

BRIDGE_REGISTER_MODEL(crm::LeadModel, "LeadModel")
BRIDGE_REGISTER_ACTION(crm::LeadModel, crm::CreateLead, "CreateLead")
BRIDGE_REGISTER_ACTION(crm::LeadModel, crm::UpdateLead, "UpdateLead")
BRIDGE_REGISTER_ACTION(crm::LeadModel, crm::MarkLeadLost, "MarkLeadLost")
BRIDGE_REGISTER_ACTION(crm::LeadModel, crm::GetLead, "GetLead", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(crm::LeadModel, crm::ListLeads, "ListLeads", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(crm::LeadModel, crm::ConvertLead, "ConvertLead")
