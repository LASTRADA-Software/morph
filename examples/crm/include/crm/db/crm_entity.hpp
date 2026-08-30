// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>

/// @file
/// Lightweight ORM entities: the raw DB row shapes, one struct per table,
/// separate from the action DTOs (`dto/*.hpp`) and the model orchestration
/// (`models/*.hpp`) — same three-way split as `examples/lims`.

namespace crm::db {

/// @brief An account — the company/organization a contact belongs to and an
///        opportunity is sold to.
struct AccountRecord {
    static constexpr std::string_view TableName = "crm_accounts";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                              // 1
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"industry"}> industry;                       // 2
    Light::Field<Light::SqlAnsiString<255>, Light::SqlRealName{"website"}> website;                        // 3
    Light::Field<std::int64_t, Light::SqlRealName{"created_at"}> createdAt{0};                             // 4
    Light::Field<std::int32_t, Light::SqlRealName{"version"}> version{1};                                  // 5
};

/// @brief One `(account, principal) -> role` row (README build order §5) —
///        same shape as `kanban::db::ProjectRoleRecord`, keyed on
///        `AccountRecord` instead of a project.
struct AccountRoleRecord {
    static constexpr std::string_view TableName = "crm_account_roles";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&AccountRecord::id, Light::SqlRealName{"account_id"}> account;                        // 1
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"principal"}> principal;                     // 2
    Light::Field<Light::SqlAnsiString<16>, Light::SqlRealName{"role"}> role;                               // 3
};

/// @brief A person, belonging to an account.
struct ContactRecord {
    static constexpr std::string_view TableName = "crm_contacts";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&AccountRecord::id, Light::SqlRealName{"account_id"}> account;                        // 1
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"first_name"}> firstName;                    // 2
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"last_name"}> lastName;                      // 3
    Light::Field<Light::SqlAnsiString<255>, Light::SqlRealName{"email"}> email;                            // 4
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"phone"}> phone;                             // 5
    Light::Field<std::int64_t, Light::SqlRealName{"created_at"}> createdAt{0};                             // 6
    Light::Field<std::int32_t, Light::SqlRealName{"version"}> version{1};                                  // 7
};

/// @brief A prospective sale before qualification/conversion.
///
/// Unlike `ContactRecord`, a lead does not `BelongsTo` an `AccountRecord` —
/// that link is exactly what `ConvertLead` (README build order §3) creates.
/// `companyName`/`contactName`/`email` are the lead's own free-text fields
/// until conversion mints real `AccountRecord`/`ContactRecord` rows from them.
struct LeadRecord {
    static constexpr std::string_view TableName = "crm_leads";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"company_name"}> companyName;               // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"contact_name"}> contactName;               // 2
    Light::Field<Light::SqlAnsiString<255>, Light::SqlRealName{"email"}> email;                            // 3
    Light::Field<int, Light::SqlRealName{"status"}> status{0};                                             // 4
    // Populated only once status == Converted; nullable until then.
    Light::Field<std::optional<std::uint64_t>, Light::SqlRealName{"converted_account_id"}> convertedAccountId;  // 5
    Light::Field<std::optional<std::uint64_t>, Light::SqlRealName{"converted_contact_id"}> convertedContactId;  // 6
    Light::Field<std::optional<std::uint64_t>, Light::SqlRealName{"converted_opportunity_id"}>
        convertedOpportunityId;                                                 // 7
    Light::Field<std::int64_t, Light::SqlRealName{"created_at"}> createdAt{0};  // 8
    Light::Field<std::int32_t, Light::SqlRealName{"version"}> version{1};       // 9
};

/// @brief A sales opportunity in the pipeline, belonging to an account and
///        (optionally) a primary contact.
struct OpportunityRecord {
    static constexpr std::string_view TableName = "crm_opportunities";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;   // 0
    Light::BelongsTo<&AccountRecord::id, Light::SqlRealName{"account_id"}> account;                         // 1
    Light::Field<std::optional<std::uint64_t>, Light::SqlRealName{"primary_contact_id"}> primaryContactId;  // 2
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                               // 3
    Light::Field<int, Light::SqlRealName{"stage"}> stage{0};                                                // 4
    // expectedCloseValue (Money = Quantity<CrmUnit::usd, 2>) is empty-capable
    // (a rep may leave it unentered), so its num/den are nullable — unlike
    // QuoteLineRecord's always-engaged Rational columns.
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"expected_close_value_num"}>
        expectedCloseValueNum;  // 5
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"expected_close_value_den"}>
        expectedCloseValueDen;                                                  // 6
    Light::Field<std::int64_t, Light::SqlRealName{"created_at"}> createdAt{0};  // 7
    Light::Field<std::int32_t, Light::SqlRealName{"version"}> version{1};       // 8
};

/// @brief One row of the `crm_applied_ops` exactly-once ledger — same shape
///        and purpose as `kanban::db::AppliedOpRecord` (LADDER.md strain 5):
///        a client retrying a lost reply frame after a server-side commit
///        must not double-apply `MoveOpportunityStage`. `resultJson` is the
///        full serialized `MoveOpportunityStageResult` the original call
///        produced.
struct AppliedOpRecord {
    static constexpr std::string_view TableName = "crm_applied_ops";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&OpportunityRecord::id, Light::SqlRealName{"opportunity_id"}> opportunity;            // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"op_id"}> opId;                             // 2
    Light::Field<Light::SqlMaxDynamicAnsiString, Light::SqlRealName{"result_json"}> resultJson;            // 3
    Light::Field<std::int64_t, Light::SqlRealName{"created_at"}> createdAt{0};                             // 4
};

/// @brief A quote for an opportunity (README build order §4). Each
///        `morph::math::Rational` field is three plain integer columns
///        (`*_num`/`*_den`/`*_dp`) — the exact pattern
///        `ledger::db::TransactionRecord`/`LedgerAccountRecord` already use
///        for `amount`/`limit`, not a single lossy floating column.
struct QuoteRecord {
    static constexpr std::string_view TableName = "crm_quotes";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&OpportunityRecord::id, Light::SqlRealName{"opportunity_id"}> opportunity;            // 1
    Light::Field<int, Light::SqlRealName{"status"}> status{0};                                             // 2
    Light::Field<std::int64_t, Light::SqlRealName{"tax_rate_num"}> taxRateNum{0};                          // 3
    Light::Field<std::int64_t, Light::SqlRealName{"tax_rate_den"}> taxRateDen{1};                          // 4
    Light::Field<int, Light::SqlRealName{"tax_rate_dp"}> taxRateDp{0};                                     // 5
    Light::Field<std::int64_t, Light::SqlRealName{"grand_total_num"}> grandTotalNum{0};                    // 6
    Light::Field<std::int64_t, Light::SqlRealName{"grand_total_den"}> grandTotalDen{1};                    // 7
    Light::Field<int, Light::SqlRealName{"grand_total_dp"}> grandTotalDp{0};                               // 8
    Light::Field<std::int64_t, Light::SqlRealName{"created_at"}> createdAt{0};                             // 9
    Light::Field<std::int32_t, Light::SqlRealName{"version"}> version{1};                                  // 10
};

/// @brief One priced line of a quote, belonging to a `QuoteRecord`. Stored as
///        its own row per line (a genuine one-to-many table), not a JSON
///        blob column — the relational shape a real "child collection"
///        needs, unlike step 2's flat-list-action contacts-of-an-account
///        (see crm/README.md's written decision for why the two cases
///        differ: a quote's lines are part of the quote's own atomic write,
///        not a separately queried collection).
struct QuoteLineRecord {
    static constexpr std::string_view TableName = "crm_quote_lines";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&QuoteRecord::id, Light::SqlRealName{"quote_id"}> quote;                              // 1
    Light::Field<int, Light::SqlRealName{"line_order"}> lineOrder{0};                                      // 2
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"product_name"}> productName;               // 3
    Light::Field<std::int64_t, Light::SqlRealName{"quantity_num"}> quantityNum{0};                         // 4
    Light::Field<std::int64_t, Light::SqlRealName{"quantity_den"}> quantityDen{1};                         // 5
    Light::Field<int, Light::SqlRealName{"quantity_dp"}> quantityDp{0};                                    // 6
    Light::Field<std::int64_t, Light::SqlRealName{"unit_price_num"}> unitPriceNum{0};                      // 7
    Light::Field<std::int64_t, Light::SqlRealName{"unit_price_den"}> unitPriceDen{1};                      // 8
    Light::Field<int, Light::SqlRealName{"unit_price_dp"}> unitPriceDp{0};                                 // 9
    Light::Field<std::int64_t, Light::SqlRealName{"discount_num"}> discountNum{0};                         // 10
    Light::Field<std::int64_t, Light::SqlRealName{"discount_den"}> discountDen{1};                         // 11
    Light::Field<int, Light::SqlRealName{"discount_dp"}> discountDp{0};                                    // 12
    Light::Field<std::int64_t, Light::SqlRealName{"total_num"}> totalNum{0};                               // 13
    Light::Field<std::int64_t, Light::SqlRealName{"total_den"}> totalDen{1};                               // 14
    Light::Field<int, Light::SqlRealName{"total_dp"}> totalDp{0};                                          // 15
};

/// @brief One conflict flagged by offline replay (README build order §8) —
///        same shape as `lims::db::OfflineConflictRecord`, keyed on
///        `OpportunityRecord` instead of a sample.
struct OpportunityConflictRecord {
    static constexpr std::string_view TableName = "crm_opportunity_conflicts";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&OpportunityRecord::id, Light::SqlRealName{"opportunity_id"}> opportunity;            // 1
    /// @brief The opportunity version the queued update was prepared against.
    Light::Field<std::int32_t, Light::SqlRealName{"base_version"}> baseVersion{0};  // 2
    /// @brief The opportunity version the server actually held at replay time.
    Light::Field<std::int32_t, Light::SqlRealName{"server_version"}> serverVersion{0};  // 3
    /// @brief `ConflictReason` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"reason"}> reason{0};  // 4
    /// @brief `ConflictStatus` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"status"}> status{0};  // 5
    /// @brief The queued payload, verbatim, as the field client serialised it
    ///        — never re-encoded from a decoded struct (see
    ///        `ConflictView::payload`'s own doc comment for why).
    Light::Field<Light::SqlDynamicAnsiString<4096>, Light::SqlRealName{"payload"}> payload;         // 6
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"detected_by"}> detectedBy;           // 7
    Light::Field<std::int64_t, Light::SqlRealName{"detected_at"}> detectedAt{0};                    // 8
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"resolved_by"}> resolvedBy;           // 9
    Light::Field<std::int64_t, Light::SqlRealName{"resolved_at"}> resolvedAt{0};                    // 10
    Light::Field<Light::SqlAnsiString<255>, Light::SqlRealName{"resolution_note"}> resolutionNote;  // 11
};

/// @brief One replayed operation key (README build order §8) — same shape
///        and purpose as `lims::db::ReplayedOpRecord`: at-most-once
///        enforcement for a queue that is documented not to guarantee it
///        itself (`IOfflineQueue::enqueue(payload, idempotencyKey)`'s own
///        doc comment).
struct OpportunityReplayedOpRecord {
    static constexpr std::string_view TableName = "crm_opportunity_replayed_ops";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    /// @brief The queued item's `idempotencyKey`, verbatim.
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"op_key"}> opKey;  // 1
    Light::Field<std::int64_t, Light::SqlRealName{"decided_at"}> decidedAt{0};    // 2
};

/// @brief One custom field definition (README build order §9) — the
///        persisted counterpart of the spike's in-memory
///        `EB_CustomFieldRegistry`. `entity` is `CustomFieldEntity`'s
///        underlying integer; `type` is `CustomFieldType`'s.
struct CustomFieldDefRecord {
    static constexpr std::string_view TableName = "crm_custom_field_defs";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<int, Light::SqlRealName{"entity"}> entity{0};                                             // 1
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"name"}> name;                               // 2
    Light::Field<int, Light::SqlRealName{"type"}> type{0};                                                 // 3
    Light::Field<bool, Light::SqlRealName{"required"}> required{false};                                    // 4
    /// @brief `Role`'s underlying integer (7b: per-field authz). Nullable —
    ///        `AlterTable` has no "required column with a default"
    ///        primitive (`schema.cpp`'s own migration comment); the model
    ///        reads a null back as `Role::Member`.
    Light::Field<std::optional<int>, Light::SqlRealName{"min_role_to_edit"}> minRoleToEdit;  // 5
    /// @brief JSON array of allowed values for a `CustomFieldType::Choice`
    ///        field (7b); absent for every other type. Text, not a native
    ///        array column — the same reason `AccountCustomValueRecord::
    ///        valueJson` and `OpportunityConflictRecord::payload` store
    ///        opaque JSON as text: Lightweight has no array/JSON column
    ///        type. Nullable for the same `AlterTable` reason as
    ///        `minRoleToEdit`; the model reads a null back as `"[]"`.
    Light::Field<std::optional<Light::SqlDynamicAnsiString<1024>>, Light::SqlRealName{"choice_options_json"}>
        choiceOptionsJson;  // 6
};

/// @brief One account's value for one custom field — the persisted
///        counterpart of the spike's `EB_ContactModel::extra` in-memory map.
///        `valueJson` is the field's `CrmCustomValue` (`glz::generic_u64`),
///        serialised — the Lightweight ORM has no native JSON column type
///        (`EXTENSION-BAG-SPIKE.md`'s "What a real 7b would still need"
///        section names exactly this gap), so a text column is what stores
///        it, the same choice `AppliedOpRecord::resultJson`/
///        `OpportunityConflictRecord::payload` already make for other
///        opaque-JSON needs in this rung.
struct AccountCustomValueRecord {
    static constexpr std::string_view TableName = "crm_account_custom_values";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&AccountRecord::id, Light::SqlRealName{"account_id"}> account;                        // 1
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"field_name"}> fieldName;                    // 2
    Light::Field<Light::SqlDynamicAnsiString<1024>, Light::SqlRealName{"value_json"}> valueJson;           // 3
};

/// @brief One saved `ListOpportunities` filter (README build order §10) —
///        `accountId`/`stage` are nullable, mirroring `ListOpportunities`'s
///        own two independent optional filters exactly. `owner` is the
///        principal who created it — a saved view is scoped per-principal
///        (`dto/saved_view_dto.hpp`'s own doc comment), so this column, not
///        an account-role table, is what `SavedViewModel` filters/guards on.
struct SavedViewRecord {
    static constexpr std::string_view TableName = "crm_saved_views";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"owner"}> owner;                             // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                              // 2
    Light::Field<std::optional<std::uint64_t>, Light::SqlRealName{"account_id"}> accountId;                // 3
    Light::Field<std::optional<int>, Light::SqlRealName{"stage"}> stage;                                   // 4
};

}  // namespace crm::db
