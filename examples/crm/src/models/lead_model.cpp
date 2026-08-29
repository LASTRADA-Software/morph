// SPDX-License-Identifier: Apache-2.0
#include "crm/models/lead_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlTransaction.hpp>

#include "crm/core/errors.hpp"
#include "crm/core/model_support.hpp"
#include "crm/db/crm_entity.hpp"

namespace crm {

namespace {

LeadView toView(const db::LeadRecord& row) {
    LeadView view{
        .id = LeadId{static_cast<std::int64_t>(row.id.Value())},
        .companyName = std::string{row.companyName.Value().ToStringView()},
        .contactName = std::string{row.contactName.Value().ToStringView()},
        .email = std::string{row.email.Value().ToStringView()},
        .status = static_cast<LeadStatus>(row.status.Value()),
        .version = row.version.Value(),
    };
    if (row.convertedAccountId.Value().has_value()) {
        view.convertedAccountId = AccountId{static_cast<std::int64_t>(*row.convertedAccountId.Value())};
    }
    if (row.convertedContactId.Value().has_value()) {
        view.convertedContactId = ContactId{static_cast<std::int64_t>(*row.convertedContactId.Value())};
    }
    if (row.convertedOpportunityId.Value().has_value()) {
        view.convertedOpportunityId = OpportunityId{static_cast<std::int64_t>(*row.convertedOpportunityId.Value())};
    }
    return view;
}

/// @brief Throws `IllegalTransition` unless @p row is still editable
///        (`New`/`Working` — not `Converted`/`Lost`).
void requireEditable(const db::LeadRecord& row) {
    const auto status = static_cast<LeadStatus>(row.status.Value());
    if (status == LeadStatus::Converted || status == LeadStatus::Lost) {
        throw IllegalTransition{"lead is terminal (Converted or Lost) and can no longer be edited"};
    }
}

}  // namespace

CreateLeadResult LeadModel::execute(const CreateLead& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateLead: companyName and contactName are required"};
    }

    Lightweight::DataMapper mapper;
    db::LeadRecord row;
    row.companyName = Lightweight::SqlAnsiString<128>{action.companyName};
    row.contactName = Lightweight::SqlAnsiString<128>{action.contactName};
    row.email = Lightweight::SqlAnsiString<255>{action.email};
    row.status = static_cast<int>(LeadStatus::New);
    row.createdAt = nowMillis();
    row.version = 1;
    mapper.Create(row);

    CreateLeadResult result{.leadId = LeadId{static_cast<std::int64_t>(row.id.Value())}};
    _journal.recordSuccess<LeadModel>(action, result, nowMillis());
    return result;
}

UpdateLeadResult LeadModel::execute(const UpdateLead& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"UpdateLead: leadId, companyName and contactName are required"};
    }

    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::LeadRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::LeadRecord::id>, "=", *action.leadId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"UpdateLead: no such lead"};
    }
    auto& row = rows.front();
    if (row.version.Value() != action.expectedVersion) {
        throw Conflict{"UpdateLead: version mismatch — record was edited concurrently"};
    }
    requireEditable(row);

    row.companyName = Lightweight::SqlAnsiString<128>{action.companyName};
    row.contactName = Lightweight::SqlAnsiString<128>{action.contactName};
    row.email = Lightweight::SqlAnsiString<255>{action.email};
    row.version = row.version.Value() + 1;
    mapper.Update(row);

    UpdateLeadResult result{.lead = toView(row)};
    _journal.recordSuccess<LeadModel>(action, result, nowMillis());
    return result;
}

MarkLeadLostResult LeadModel::execute(const MarkLeadLost& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"MarkLeadLost: leadId is required"};
    }

    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::LeadRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::LeadRecord::id>, "=", *action.leadId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"MarkLeadLost: no such lead"};
    }
    auto& row = rows.front();
    requireEditable(row);

    row.status = static_cast<int>(LeadStatus::Lost);
    row.version = row.version.Value() + 1;
    mapper.Update(row);

    MarkLeadLostResult result{.lead = toView(row)};
    _journal.recordSuccess<LeadModel>(action, result, nowMillis());
    return result;
}

LeadView LeadModel::execute(const GetLead& action) {
    if (!action.validate()) {
        throw ValidationError{"GetLead: leadId is required"};
    }
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::LeadRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::LeadRecord::id>, "=", *action.leadId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"GetLead: no such lead"};
    }
    return toView(rows.front());
}

ListLeadsResult LeadModel::execute(const ListLeads& action) {
    (void)action;
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::LeadRecord>().All();
    ListLeadsResult result;
    result.leads.reserve(rows.size());
    for (const auto& row : rows) {
        result.leads.push_back(toView(row));
    }
    return result;
}

ConvertLeadResult LeadModel::execute(const ConvertLead& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"ConvertLead: leadId and opportunityName are required"};
    }

    Lightweight::DataMapper mapper;
    auto leadRows = mapper.Query<db::LeadRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::LeadRecord::id>, "=", *action.leadId)
                        .All();
    if (leadRows.empty()) {
        throw NotFound{"ConvertLead: no such lead"};
    }
    auto& leadRow = leadRows.front();
    requireEditable(leadRow);  // Converted/Lost cannot be converted again

    // The whole conversion — three inserts across three tables this model
    // does not otherwise own — is one Lightweight::SqlTransaction (RAII:
    // auto-rollback on any exception before Commit(), matching
    // OpportunityModel::execute(MoveOpportunityStage)'s identical use). A
    // crash mid-conversion therefore leaves zero new rows, never one or two
    // — see ConvertLead's own doc comment (lead_dto.hpp) for why this,
    // rather than dispatching AccountModel/ContactModel/OpportunityModel's
    // own execute(), is the chosen idiom.
    Lightweight::SqlTransaction transaction{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};

    db::AccountRecord accountRow;
    accountRow.name = Lightweight::SqlAnsiString<128>{std::string{leadRow.companyName.Value().ToStringView()}};
    accountRow.createdAt = nowMillis();
    accountRow.version = 1;
    mapper.Create(accountRow);

    db::ContactRecord contactRow;
    contactRow.account = accountRow;
    // The lead's single free-text contactName splits into first/last on the
    // first space — a lossy but reversible-enough heuristic for a name with
    // no separate given/family fields to begin with; a lead captured with
    // only "Company" and no personal name at all still converts (both halves
    // empty is legal on ContactRecord, unlike CreateContact's own validate(),
    // which this path does not go through).
    const std::string& contactName = std::string{leadRow.contactName.Value().ToStringView()};
    const auto spacePos = contactName.find(' ');
    contactRow.firstName =
        Lightweight::SqlAnsiString<64>{spacePos == std::string::npos ? contactName : contactName.substr(0, spacePos)};
    contactRow.lastName = Lightweight::SqlAnsiString<64>{
        spacePos == std::string::npos ? std::string{} : contactName.substr(spacePos + 1)};
    contactRow.email = Lightweight::SqlAnsiString<255>{std::string{leadRow.email.Value().ToStringView()}};
    contactRow.createdAt = nowMillis();
    contactRow.version = 1;
    mapper.Create(contactRow);

    db::OpportunityRecord opportunityRow;
    opportunityRow.account = accountRow;
    opportunityRow.primaryContactId = contactRow.id.Value();
    opportunityRow.name = Lightweight::SqlAnsiString<128>{action.opportunityName};
    opportunityRow.stage = static_cast<int>(OpportunityStage::Prospecting);
    opportunityRow.createdAt = nowMillis();
    opportunityRow.version = 1;
    mapper.Create(opportunityRow);

    leadRow.status = static_cast<int>(LeadStatus::Converted);
    leadRow.convertedAccountId = accountRow.id.Value();
    leadRow.convertedContactId = contactRow.id.Value();
    leadRow.convertedOpportunityId = opportunityRow.id.Value();
    leadRow.version = leadRow.version.Value() + 1;
    mapper.Update(leadRow);

    transaction.Commit();

    ConvertLeadResult result{
        .accountId = AccountId{static_cast<std::int64_t>(accountRow.id.Value())},
        .contactId = ContactId{static_cast<std::int64_t>(contactRow.id.Value())},
        .opportunityId = OpportunityId{static_cast<std::int64_t>(opportunityRow.id.Value())},
    };
    // One journal entry for the whole conversion (LeadModel's own log) — see
    // ConvertLead's doc comment for why this resolves, rather than
    // relocates, the "three per-model entries with no causal link" limit.
    _journal.recordSuccess<LeadModel>(action, result, nowMillis());
    return result;
}

}  // namespace crm
