// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <string>

#include "crm/core/self_journal.hpp"
#include "crm/dto/contact_dto.hpp"

/// @file
/// `ContactModel` — contact CRUD (README build order §1-2). Unkeyed, same
/// rationale as `AccountModel`.

namespace crm {

class ContactModel {
public:
    CreateContactResult execute(const CreateContact& action);
    UpdateContactResult execute(const UpdateContact& action);
    ContactView execute(const GetContact& action);
    ListContactsResult execute(const ListContacts& action);
    ListContactOptionsResult execute(const ListContactOptions& action);

    void attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey) {
        _journal.attach(std::move(log), std::move(entityKey));
    }

    [[nodiscard]] std::vector<::morph::journal::LogEntry> journalEntries() const { return _journal.entries(); }

private:
    SelfJournal _journal;
};

}  // namespace crm

BRIDGE_REGISTER_MODEL(crm::ContactModel, "ContactModel")
BRIDGE_REGISTER_ACTION(crm::ContactModel, crm::CreateContact, "CreateContact")
BRIDGE_REGISTER_ACTION(crm::ContactModel, crm::UpdateContact, "UpdateContact")
BRIDGE_REGISTER_ACTION(crm::ContactModel, crm::GetContact, "GetContact", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(crm::ContactModel, crm::ListContacts, "ListContacts", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(crm::ContactModel, crm::ListContactOptions, "ListContactOptions", ::morph::model::Loggable::No)
