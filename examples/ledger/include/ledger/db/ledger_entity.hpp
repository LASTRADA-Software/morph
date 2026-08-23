// SPDX-License-Identifier: Apache-2.0
#pragma once

// NOTE: the brief's original #include <Lightweight/DataMapper/BelongsTo.hpp>
// + <Lightweight/DataMapper/Field.hpp> pair does not transitively provide
// Light::SqlAnsiString/SqlRealName/PrimaryKey (confirmed by a real compile
// failure once the machine-local compiler-cache launcher's stale-object bug
// -- see examples/ledger/CMakeLists.txt's existing comment about
// src/db/schema.cpp.obj -- was worked around for this target too). Every
// real precedent entity (bank::db::AccountRecord via the umbrella
// <Lightweight/Lightweight.hpp>; bookmarks::db::ImportedOpRecord via
// <Lightweight/DataMapper/DataMapper.hpp>, which transitively pulls in
// SqlRealName.hpp/BelongsTo.hpp/Field.hpp plus SqlDataBinder.hpp's string
// binders) instead includes something broader. This file follows
// bookmarks::db::ImportedOpRecord's exact include.
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ledger::db {

struct LedgerRecord {
    static constexpr std::string_view TableName = "ledgers";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                              // 1
};

// Declared here, ahead of AccountRecord, rather than in its previous
// position further down this file: AccountRecord's new nullable `category`
// BelongsTo (Task 10) forms a pointer-to-member of &CategoryRecord::id as a
// template argument, which requires CategoryRecord to be a *complete* type
// at that point (a forward declaration is not enough for BelongsTo's own
// template parameter). CategoryRecord itself depends only on LedgerRecord
// (already declared above), so moving it here introduces no cycle.
struct CategoryRecord {
    static constexpr std::string_view TableName = "categories";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&LedgerRecord::id, Light::SqlRealName{"ledger_id"}> ledger;                           // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                              // 2
};

struct AccountRecord {
    static constexpr std::string_view TableName = "accounts";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&LedgerRecord::id, Light::SqlRealName{"ledger_id"}> ledger;                           // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                              // 2
    Light::Field<int, Light::SqlRealName{"kind"}> kind;                                                    // 3
    Light::Field<Light::SqlAnsiString<3>, Light::SqlRealName{"currency_code"}> currencyCode;               // 4
    // Nullable: an account need not belong to a category (Task 10's schema
    // addition -- design spec §3's budget-report join target). Added via an
    // ALTER TABLE migration (schema.cpp's 20260819000012), the first such
    // migration in this codebase.
    Light::BelongsTo<&CategoryRecord::id, Light::SqlRealName{"category_id"}, Light::SqlNullable::Null> category;  // 5
};

struct TransactionJournalRecord {
    static constexpr std::string_view TableName = "transaction_journals";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&LedgerRecord::id, Light::SqlRealName{"ledger_id"}> ledger;                           // 1
    Light::Field<Light::SqlAnsiString<256>, Light::SqlRealName{"description"}> description;                // 2
    Light::Field<std::int64_t, Light::SqlRealName{"date"}> date{0};  // 3 -- epoch millis
    Light::Field<std::optional<Light::SqlAnsiString<64>>, Light::SqlRealName{"causal_parent_id"}>
        causalParentId;  // 4 -- nullable, per design spec §5's causalParentId shape
};

struct TransactionLegRecord {
    static constexpr std::string_view TableName = "transaction_legs";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&TransactionJournalRecord::id, Light::SqlRealName{"journal_id"}> journal;             // 1
    Light::BelongsTo<&AccountRecord::id, Light::SqlRealName{"account_id"}> account;                        // 2
    Light::Field<std::int64_t, Light::SqlRealName{"amount_num"}> amountNum{0};                             // 3
    Light::Field<std::int64_t, Light::SqlRealName{"amount_den"}> amountDen{1};                             // 4
    Light::Field<int, Light::SqlRealName{"amount_dp"}> amountDp{0};                                        // 5
    Light::Field<Light::SqlAnsiString<3>, Light::SqlRealName{"currency_code"}> currencyCode;               // 6
    // Nullable foreign-amount triple -- present only on a foreign-amount-pair
    // leg (design spec §1 step 3). Plain std::optional<T> field, not a
    // BelongsTo: confirmed real via Lightweight/DataBinder/StdOptional.hpp's
    // SqlDataBinder<std::optional<T>> specialization.
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"foreign_amount_num"}> foreignAmountNum;  // 7
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"foreign_amount_den"}> foreignAmountDen;  // 8
    Light::Field<std::optional<int>, Light::SqlRealName{"foreign_amount_dp"}> foreignAmountDp;             // 9
    Light::Field<std::optional<Light::SqlAnsiString<3>>, Light::SqlRealName{"foreign_currency_code"}>
        foreignCurrencyCode;  // 10
};

struct BudgetRecord {
    static constexpr std::string_view TableName = "budgets";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&LedgerRecord::id, Light::SqlRealName{"ledger_id"}> ledger;                           // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                              // 2
    Light::BelongsTo<&CategoryRecord::id, Light::SqlRealName{"category_id"}> category;                     // 3
};

struct BudgetLimitRecord {
    static constexpr std::string_view TableName = "budget_limits";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&BudgetRecord::id, Light::SqlRealName{"budget_id"}> budget;                           // 1
    Light::Field<Light::SqlAnsiString<7>, Light::SqlRealName{"month"}> month;                 // 2 -- "YYYY-MM"
    Light::Field<std::int64_t, Light::SqlRealName{"limit_num"}> limitNum{0};                  // 3
    Light::Field<std::int64_t, Light::SqlRealName{"limit_den"}> limitDen{1};                  // 4
    Light::Field<int, Light::SqlRealName{"limit_dp"}> limitDp{0};                             // 5
    Light::Field<Light::SqlAnsiString<3>, Light::SqlRealName{"currency_code"}> currencyCode;  // 6
};

struct RuleRecord {
    static constexpr std::string_view TableName = "rules";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&LedgerRecord::id, Light::SqlRealName{"ledger_id"}> ledger;                           // 1
    Light::Field<int, Light::SqlRealName{"trigger"}> trigger{0};                                           // 2
    Light::Field<Light::SqlAnsiString<256>, Light::SqlRealName{"match_text"}> matchText;                   // 3
    Light::Field<int, Light::SqlRealName{"action"}> action{0};                                             // 4
    Light::Field<Light::SqlAnsiString<256>, Light::SqlRealName{"action_value"}> actionValue;               // 5
    Light::Field<int, Light::SqlRealName{"version"}> version{1};                                           // 6
};

/// @brief Exactly-once ledger for `StoreTransaction` (Task 11b, this
///        rung's own re-test of the pattern kanban's `execute
///        (MoveTaskPosition)` establishes at rung 4): one row per
///        `(ledger_id, op_id)`, storing the full serialized
///        `GetLedgerResult` the original call produced. A lookup hit
///        means the call has already been applied -- the stored result is
///        returned verbatim, with no re-journaling and no re-mutation.
///        Distinct from `ImportedOpRecord` below (Task 15's own chunk-
///        retry dedup, keyed by `(owner_principal, op_id)` instead --
///        different scope, different key, same pattern occurring for the
///        second time in this rung).
struct AppliedOpRecord {
    static constexpr std::string_view TableName = "ledger_applied_ops";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&LedgerRecord::id, Light::SqlRealName{"ledger_id"}> ledger;                           // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"op_id"}> opId;                             // 2
    Light::Field<Light::SqlMaxDynamicAnsiString, Light::SqlRealName{"result_json"}> resultJson;            // 3
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs{0};                        // 4
};

/// @brief Mirrors `bookmarks::db::ImportedOpRecord`'s exact shape (design
///        spec §8): op-id ledger for chunk-retry dedup, keyed by
///        `(owner_principal, op_id)`.
struct ImportedOpRecord {
    static constexpr std::string_view TableName = "ledger_imported_ops";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"owner_principal"}> ownerPrincipal;          // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"op_id"}> opId;                             // 2
    Light::Field<std::int64_t, Light::SqlRealName{"applied_at_ms"}> appliedAtMs{0};                        // 3
};

/// @brief Cross-import duplicate detection (design spec §8) -- distinct
///        from `ImportedOpRecord`; see that struct's own doc comment for
///        the difference.
struct ImportedTxnHashRecord {
    static constexpr std::string_view TableName = "ledger_imported_txn_hashes";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&LedgerRecord::id, Light::SqlRealName{"ledger_id"}> ledger;                           // 1
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"hash"}> hash;                               // 2
};

struct ReportJobRecord {
    static constexpr std::string_view TableName = "ledger_report_jobs";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&LedgerRecord::id, Light::SqlRealName{"ledger_id"}> ledger;                           // 1
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"job_id"}> jobId;                            // 2
    Light::Field<int, Light::SqlRealName{"kind"}> kind{0};                                                 // 3
    Light::Field<int, Light::SqlRealName{"status"}> status{0};                                             // 4
    // Nullable AND unbounded -- a combination no existing rung's entity
    // needs yet (polls::db::VoteHistoryRecord::previousVotesJson is
    // unbounded but always-populated, never nullable). Field<std::optional
    // <T>, ...>'s wrapping is confirmed generic (StdOptional.hpp specializes
    // SqlDataBinder<std::optional<T>> for any T with its own binder), so
    // wrapping the same Light::SqlMaxDynamicAnsiString type
    // polls::db::VoteHistoryRecord::previousVotesJson already uses in
    // std::optional<> is the correct composition, not a new guess -- but
    // this exact composition has no precedent in the codebase to copy
    // verbatim, so build+test this field specifically before trusting it.
    Light::Field<std::optional<Light::SqlMaxDynamicAnsiString>, Light::SqlRealName{"result_json"}>
        resultJson;  // 5 -- nullable, unbounded (NVarchar(0) at the DDL layer); absent until the job completes
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs{0};  // 6
    // `SubmitReport::params` verbatim, so the job row records what it was
    // asked to compute and not merely that it was asked. Load-bearing since
    // the aggregation moved out of SubmitReport's own call frame and into
    // RunReportJob (morph#160): the runner that eventually settles this job
    // may be in a different process from the one that accepted it, and has
    // nothing but this row to reconstruct the request from.
    //
    // Nullable, unlike the other required columns, purely because it was
    // added by ALTER TABLE to a table that may already hold rows (schema.cpp's
    // 20260819000014) -- SQLite cannot add a NOT NULL column without a
    // default. Every row this rung writes populates it, including with the
    // empty-params "{}" an all-time report submits; std::nullopt means only
    // "written before this column existed", which decodeMonthlyParams already
    // handles as the all-time fallback.
    Light::Field<std::optional<Light::SqlMaxDynamicAnsiString>, Light::SqlRealName{"params_json"}>
        paramsJson;  // 7 -- nullable, unbounded
};

}  // namespace ledger::db
