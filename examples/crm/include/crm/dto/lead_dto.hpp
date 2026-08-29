// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "crm/core/types.hpp"

/// @file
/// Lead CRUD action/result DTOs. `ConvertLead` (README build order §3, the
/// multi-model transactional action) is not here — this file covers only the
/// pre-conversion lifecycle: capture, edit, list, and mark-lost.

namespace crm {

struct LeadView {
    LeadId id;
    std::string companyName;
    std::string contactName;
    std::string email;
    LeadStatus status = LeadStatus::New;
    std::optional<AccountId> convertedAccountId;
    std::optional<ContactId> convertedContactId;
    std::optional<OpportunityId> convertedOpportunityId;
    std::int32_t version = 0;
};

struct CreateLead {
    std::string companyName;
    std::string contactName;
    std::string email;

    [[nodiscard]] bool validate() const noexcept { return !companyName.empty() && !contactName.empty(); }
};

struct CreateLeadResult {
    LeadId leadId;
};

/// @brief Updates a lead's editable fields. Only legal while the lead is
///        still `New`/`Working` — a `Converted`/`Lost` lead is terminal
///        (README build order §3's pipeline-as-guarded-state-machine
///        pattern, same rationale as `IllegalTransition` on an opportunity
///        stage).
struct UpdateLead {
    LeadId leadId;
    std::string companyName;
    std::string contactName;
    std::string email;
    std::int32_t expectedVersion = 0;

    [[nodiscard]] bool validate() const noexcept {
        return leadId.hasValue() && !companyName.empty() && !contactName.empty();
    }
};

struct UpdateLeadResult {
    LeadView lead;
};

/// @brief Marks a lead disqualified without conversion. Terminal, like
///        `Converted` — a `Lost` lead cannot be revived by this rung
///        (README's stretch/later buckets do not name lead reactivation).
struct MarkLeadLost {
    LeadId leadId;

    [[nodiscard]] bool validate() const noexcept { return leadId.hasValue(); }
};

struct MarkLeadLostResult {
    LeadView lead;
};

struct GetLead {
    LeadId leadId;

    [[nodiscard]] bool validate() const noexcept { return leadId.hasValue(); }
};

struct ListLeads {
    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct ListLeadsResult {
    std::vector<LeadView> leads;
};

/// @brief Converts a lead into an Account + Contact + Opportunity,
///        atomically (README build order §3, the multi-model transactional
///        action).
///
/// Design decision, in writing (LADDER.md's discipline rule): `LeadModel`
/// itself is the "one orchestrating model owning the whole conversion"
/// crm/README.md recommends — its `execute(const ConvertLead&)` writes the
/// three `crm_accounts`/`crm_contacts`/`crm_opportunities` rows via direct
/// Lightweight ORM calls, wrapped in one `Lightweight::SqlTransaction`,
/// **never dispatching `AccountModel`/`ContactModel`/`OpportunityModel`'s own
/// `execute()`**. This is why: `Completion<T>` has no blocking `wait()`/
/// `get()` at all (`docs/spec/core/completion.md`) — the only way an
/// orchestrator could "wait" for three nested dispatched actions is a
/// hand-rolled block, and `ThreadPoolExecutor` is a single fixed-size pool
/// shared by every model's strand (`include/morph/core/executor.hpp`), so N
/// concurrent conversions blocking on nested completions can starve the pool
/// outright — no thread is ever free to run the nested `execute()` that
/// would unblock them (see `test_convert_lead.cpp`'s pool-starvation test,
/// which demonstrates the naive alternative deadlocking a small pool, and
/// `docs/findings/` for why this is genuinely new ground: no sanctioned
/// internal-client seam exists yet, and the only same-model cascade
/// precedent, `kanban::BoardModel::evaluateRules`, does not extend
/// mechanically to three different models' tables).
///
/// This also resolves — not just relocates — the "three per-model journal
/// entries carry no causal link" limit `LADDER.md`'s Journal honesty section
/// and this README both name: there are no separate per-model entries to
/// link, because there is exactly one `execute()` call and one journaled
/// entry (`LeadModel`'s own). It does **not** by itself solve crash-atomicity
/// — that is what wrapping the three writes in a real `SqlTransaction`
/// (RAII commit/rollback, `Lightweight/SqlTransaction.hpp`) does: a crash
/// mid-conversion leaves zero new rows, not one or two, and — because
/// `SelfJournal::recordSuccess` only runs after `execute()` returns — no
/// journal entry either, which is the same "nothing happened" state the DB
/// itself is left in. See `test_convert_lead.cpp`'s crash-between-legs test.
struct ConvertLead {
    LeadId leadId;
    std::string opportunityName;

    [[nodiscard]] bool validate() const noexcept { return leadId.hasValue() && !opportunityName.empty(); }
};

struct ConvertLeadResult {
    AccountId accountId;
    ContactId contactId;
    OpportunityId opportunityId;
};

}  // namespace crm
