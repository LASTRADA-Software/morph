// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/forms/forms.hpp>
#include <optional>
#include <string>
#include <vector>

#include "crm/core/types.hpp"

/// @file
/// Opportunity CRUD action/result DTOs. Step 1 scope only: plain CRUD with a
/// `stage` field a caller may set directly on create/update. The *guarded,
/// journaled* stage transition (`MoveOpportunityStage`, modelled on
/// `kanban::BoardModel::execute(MoveTaskPosition)`) is README build order
/// §3's job, not step 1's — this file grows a pipeline DTO file alongside it
/// when that step starts.

namespace crm {

/// @brief `forms::Choice` over an account id — same shape as
///        `AccountChoice` (contact_dto.hpp), reused here because morph's
///        forms `Choice` is a type alias per field, not a shared named type
///        callers reference structurally.
using OpportunityAccountChoice = ::morph::forms::Choice<std::string, "ListAccountOptions", "id", "name">;

/// @brief `forms::Choice` over a contact id, for the opportunity's primary
///        contact — backed by `ListContactOptions` (contact_dto.hpp), which
///        already accepts an `accountId` filter for the cascading-picklist
///        shape ("account combo" then "contact combo scoped to it").
using PrimaryContactChoice = ::morph::forms::Choice<std::string, "ListContactOptions", "id", "name">;

struct OpportunityView {
    OpportunityId id;
    AccountId accountId;
    std::optional<ContactId> primaryContactId;
    std::string name;
    OpportunityStage stage = OpportunityStage::Prospecting;
    Money expectedCloseValue;
    std::int32_t version = 0;
};

/// @brief README build order §7 ("Dynamic logic... conditional required
///        encoded in the served schema"), using the shipped `requiredWhen`
///        rule (morph#78) rather than a hand-written check: once a rep
///        enters an expected close value for the deal, the schema itself
///        demands a primary contact before the record can be saved — a
///        deal with real money behind it needs someone to actually talk to.
///        `expectedCloseValue` (a `Money = Quantity<CrmUnit::usd, 2>`) is
///        the field this rule conditions on; `OpportunityStage` (a plain
///        `enum class`) and `primaryContact` (a `Choice`) both fail the
///        framework's `EmptyCapableField`/`ComparableField` constraints
///        (confirmed by investigation, not assumed — `Choice` has
///        `operator==` but no `operator<=>`; a raw `enum class` has no
///        `hasValue()` at all), so neither could be the condition or the
///        comparison operand a `greater`/`less`-based rule would need.
///        `requiredWhen`'s own condition (`engaged`) needs only
///        `EmptyCapableField`, which `Money` (via `Quantity`) satisfies.
#define CRM_OPPORTUNITY_FORM_RULES(ActionType)                                               \
    static constexpr auto formRules = ::morph::forms::ruleList(::morph::forms::requiredWhen( \
        &ActionType::primaryContact, ::morph::forms::engaged(&ActionType::expectedCloseValue)))

struct CreateOpportunity {
    OpportunityAccountChoice account;
    PrimaryContactChoice primaryContact;
    std::string name;
    Money expectedCloseValue;

    CRM_OPPORTUNITY_FORM_RULES(CreateOpportunity);

    [[nodiscard]] bool validate() const noexcept {
        return account.hasValue() && !name.empty() && ::morph::forms::allRulesSatisfied(*this);
    }
};

struct CreateOpportunityResult {
    OpportunityId opportunityId;
};

struct UpdateOpportunity {
    OpportunityId opportunityId;
    OpportunityAccountChoice account;
    PrimaryContactChoice primaryContact;
    std::string name;
    Money expectedCloseValue;
    std::int32_t expectedVersion = 0;

    CRM_OPPORTUNITY_FORM_RULES(UpdateOpportunity);

    [[nodiscard]] bool validate() const noexcept {
        return opportunityId.hasValue() && account.hasValue() && !name.empty() &&
               ::morph::forms::allRulesSatisfied(*this);
    }
};

#undef CRM_OPPORTUNITY_FORM_RULES

struct UpdateOpportunityResult {
    OpportunityView opportunity;
};

struct GetOpportunity {
    OpportunityId opportunityId;

    [[nodiscard]] bool validate() const noexcept { return opportunityId.hasValue(); }
};

/// @brief Lists opportunities, optionally filtered by account and/or stage.
///
/// Both filters are independent and combine with AND when both are set —
/// same "each engaged filter narrows further" shape `ListContacts`'s single
/// `accountId` filter already uses, extended to a second dimension. `stage`
/// is the filter README build order §10's saved views run against
/// (`SavedView`, `dto/saved_view_dto.hpp`).
struct ListOpportunities {
    std::optional<AccountId> accountId;
    std::optional<OpportunityStage> stage;

    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct ListOpportunitiesResult {
    std::vector<OpportunityView> opportunities;
};

}  // namespace crm
