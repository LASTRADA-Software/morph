// SPDX-License-Identifier: Apache-2.0
#include "ledger/core/errors.hpp"
#include "ledger/core/units.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/ledger_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlTransaction.hpp>

#include <map>
#include <optional>
#include <string>

namespace ledger {

namespace {

/// @brief Sums every leg posted against @p accountId into a single
///        `Rational`, tagged at @p decimalPlaces. In-model summation via
///        `Rational::operator+`, never a raw SQL `SUM()` -- design spec
///        §3's rule against combining differently-denominated rows in SQL
///        applies here exactly as it does for budgets: each
///        `TransactionLegRecord` row can carry its own `amount_den`
///        (design spec §1), so only `Rational`'s own reduction logic can
///        combine them correctly.
/// @param mapper The data mapper to query legs through.
/// @param accountId The account whose legs to sum.
/// @param decimalPlaces The account's own currency precision, used to seed
///        the running total's zero starting value.
/// @return The account's real balance -- the sum of all its legs.
[[nodiscard]] morph::math::Rational sumAccountLegs(Lightweight::DataMapper& mapper, std::uint64_t accountId,
                                                    morph::math::DecimalPlaces decimalPlaces) {
    auto legRows = mapper.Query<db::TransactionLegRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::TransactionLegRecord::account>, "=", accountId)
                        .All();
    auto total = morph::math::Rational::zero(decimalPlaces);
    for (const auto& legRow : legRows) {
        const auto legAmount = morph::math::Rational{morph::math::Numerator{legRow.amountNum.Value()},
                                                       morph::math::Denominator{legRow.amountDen.Value()},
                                                       morph::math::DecimalPlaces{
                                                           static_cast<std::uint32_t>(legRow.amountDp.Value())}};
        total = total + legAmount;
    }
    return total;
}

}  // namespace

AccountInfo LedgerModel::execute(const OpenAccount& action) {
    if (!action.validate()) {
        throw ValidationError{"OpenAccount: ledgerId and name are required"};
    }
    Lightweight::DataMapper mapper;
    // The ledger row must already exist -- this rung's scope has no
    // CreateLedger action (see the design note in the task brief); load it
    // by primary key rather than fabricating a stub LedgerRecord, since
    // BelongsTo assignment needs the real persisted parent (per
    // polls::db::OptionRecord's own `opt.poll = poll;` usage, where `poll`
    // is a row that has actually round-tripped through Create/Query).
    auto ledgerRows =
        mapper.Query<db::LedgerRecord>().Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *action.ledgerId).All();
    if (ledgerRows.empty()) {
        throw NotFound{"OpenAccount: no such ledger"};
    }
    db::AccountRecord accountRow;
    accountRow.ledger = ledgerRows.front();
    accountRow.name = action.name;
    accountRow.kind = static_cast<int>(action.kind);
    accountRow.currencyCode = currencyToCode(action.currency);
    mapper.Create(accountRow);
    // Returns the freshly created account's info, not void -- see
    // ledger_model.hpp's doc comment on this method for why a void
    // execute() cannot be registered via BRIDGE_REGISTER_ACTION.
    return AccountInfo{
        .id = AccountId{static_cast<std::int64_t>(accountRow.id.Value())},
        .name = action.name,
        .kind = action.kind,
        .currency = action.currency,
        .balance = morph::math::Rational{
            morph::math::Numerator{0}, morph::math::Denominator{1},
            morph::math::DecimalPlaces{UnitTraits<Currency>::meta(action.currency).defaultDecimals}},  // no
                                                                            // legs exist yet at this task's
                                                                            // scope -- Task 8 computes a real
                                                                            // balance -- but the placeholder
                                                                            // zero is still tagged at the
                                                                            // account's actual currency
                                                                            // precision (0 for JPY/KRW, 2 for
                                                                            // USD/EUR), not a hardcoded 2
    };
}

GetLedgerResult LedgerModel::execute(const GetLedger& action) {
    if (!action.validate()) {
        throw ValidationError{"GetLedger: ledgerId is required"};
    }
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::AccountRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::AccountRecord::ledger>, "=", *action.ledgerId)
                    .All();
    GetLedgerResult result;
    result.accounts.reserve(rows.size());
    for (const auto& row : rows) {
        const auto currency = codeToCurrency(row.currencyCode.Value().ToStringView());
        const auto decimalPlaces = morph::math::DecimalPlaces{UnitTraits<Currency>::meta(currency).defaultDecimals};
        result.accounts.push_back(AccountInfo{
            .id = AccountId{static_cast<std::int64_t>(row.id.Value())},
            .name = std::string{row.name.Value().ToStringView()},
            .kind = static_cast<AccountKind>(row.kind.Value()),
            .currency = currency,
            // Real balance: the sum of every leg posted against this
            // account, computed in-model via Rational::operator+ (never a
            // raw SQL SUM() -- see sumAccountLegs's own doc comment).
            .balance = sumAccountLegs(mapper, row.id.Value(), decimalPlaces),
        });
    }
    return result;
}

GetLedgerResult LedgerModel::execute(const StoreTransaction& action) {
    if (!action.validate()) {
        throw ValidationError{"StoreTransaction: description and at least two legs with engaged accountIds are required"};
    }
    Lightweight::DataMapper mapper;

    // Partition legs by the account's OWN currency, never a client-supplied
    // field (design spec §1) -- look up every referenced account first.
    std::map<std::string, morph::math::Rational> sumsByCurrency;
    std::vector<db::AccountRecord> legAccounts;
    legAccounts.reserve(action.legs.size());
    for (const auto& leg : action.legs) {
        auto rows = mapper.Query<db::AccountRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", *leg.accountId)
                        .All();
        if (rows.empty()) {
            throw NotFound{"StoreTransaction: no such account"};
        }
        legAccounts.push_back(rows.front());
        const std::string currency{legAccounts.back().currencyCode.Value().ToStringView()};
        auto it = sumsByCurrency.find(currency);
        if (it == sumsByCurrency.end()) {
            sumsByCurrency.emplace(currency, leg.amount);
        } else {
            it->second = it->second + leg.amount;
        }
    }
    for (const auto& [currency, sum] : sumsByCurrency) {
        if (sum.numerator != 0) {
            throw ZeroSumViolation{currency, "legs did not sum to zero"};
        }
    }

    // Constructor/commit shape copied verbatim from
    // bank::LoanModel::execute(const dto::TakeLoan&) (examples/bank/src/
    // models/loan_model.cpp:77-80) -- the exact multi-row-commit pattern
    // this rung's own StoreTransaction (journal + N legs) needs.
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    db::TransactionJournalRecord journalRow;
    journalRow.description = action.description;
    // DateTime->epoch-millis conversion copied verbatim from
    // bookmarks::db's own nowMs()/fromEpochMs() helpers
    // (bookmark_model.cpp:61-63): Timestamp::value is
    // std::optional<DateTime>, DateTime::value is
    // std::chrono::sys_time<milliseconds> -- .time_since_epoch().count()
    // gives the raw millisecond integer this entity column stores.
    // action.date is a client-supplied "when did this happen" field
    // (design spec §1) -- not a server audit stamp, so this does NOT go
    // through morph::ladder::now() (see this rung's own note on that
    // convention, which binds server-stamped timestamps like
    // ImportedOpRecord::appliedAtMs/ReportJobRecord::createdAtMs in later
    // tasks, not a client-supplied journal date).
    journalRow.date = action.date.value.has_value() ? (*action.date.value).value.time_since_epoch().count() : 0;
    auto ledgerRows = mapper.Query<db::LedgerRecord>()
                           .Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *action.ledgerId)
                           .All();
    if (ledgerRows.empty()) {
        throw NotFound{"StoreTransaction: no such ledger"};
    }
    journalRow.ledger = ledgerRows.front();
    mapper.Create(journalRow);

    for (std::size_t i = 0; i < action.legs.size(); ++i) {
        db::TransactionLegRecord legRow;
        legRow.journal = journalRow;
        legRow.account = legAccounts[i];
        legRow.amountNum = action.legs[i].amount.numerator;
        legRow.amountDen = action.legs[i].amount.denominator;
        legRow.amountDp = static_cast<int>(action.legs[i].amount.decimalPlaces.value);
        legRow.currencyCode = legAccounts[i].currencyCode.Value();
        // Foreign-amount triple: display/audit metadata only, never read by
        // the zero-sum partitioning loop above (design spec §1 step 3).
        // Assigned unconditionally -- std::optional's own empty state
        // already expresses "no foreign amount" through the nullable
        // column, so this never branches on whether the leg has one.
        const auto& foreignAmount = action.legs[i].foreignAmount;
        legRow.foreignAmountNum = foreignAmount ? std::optional{foreignAmount->numerator} : std::nullopt;
        legRow.foreignAmountDen = foreignAmount ? std::optional{foreignAmount->denominator} : std::nullopt;
        legRow.foreignAmountDp =
            foreignAmount ? std::optional{static_cast<int>(foreignAmount->decimalPlaces.value)} : std::nullopt;
        legRow.foreignCurrencyCode =
            action.legs[i].foreignCurrency
                ? std::optional{Lightweight::SqlAnsiString<3>{currencyToCode(*action.legs[i].foreignCurrency)}}
                : std::nullopt;
        mapper.Create(legRow);
    }
    sqlTxn.Commit();

    return execute(GetLedger{.ledgerId = action.ledgerId});
}

}  // namespace ledger
