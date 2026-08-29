// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <string>

#include "crm/core/self_journal.hpp"
#include "crm/dto/custom_field_dto.hpp"

/// @file
/// `CustomFieldModel` — `AddCustomField`/`ListCustomFields` (README build
/// order §9). Registered plain, unkeyed: a custom field definition is
/// schema-wide, not scoped to one account, same reasoning as
/// `AccountModel`'s own unkeyed registration for the org-wide account list.

namespace crm {

class CustomFieldModel {
public:
    AddCustomFieldResult execute(const AddCustomField& action);
    ListCustomFieldsResult execute(const ListCustomFields& action);

    void attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey) {
        _journal.attach(std::move(log), std::move(entityKey));
    }

    [[nodiscard]] std::vector<::morph::journal::LogEntry> journalEntries() const { return _journal.entries(); }

private:
    SelfJournal _journal;
};

}  // namespace crm

BRIDGE_REGISTER_MODEL(crm::CustomFieldModel, "CustomFieldModel")
BRIDGE_REGISTER_ACTION(crm::CustomFieldModel, crm::AddCustomField, "AddCustomField")
BRIDGE_REGISTER_ACTION(crm::CustomFieldModel, crm::ListCustomFields, "ListCustomFields", ::morph::model::Loggable::No)
