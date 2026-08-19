// SPDX-License-Identifier: Apache-2.0
#include "ledger/core/errors.hpp"
#include "ledger/core/units.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/ledger_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>

namespace ledger {

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
        result.accounts.push_back(AccountInfo{
            .id = AccountId{static_cast<std::int64_t>(row.id.Value())},
            .name = std::string{row.name.Value().ToStringView()},
            .kind = static_cast<AccountKind>(row.kind.Value()),
            .currency = currency,
            .balance = morph::math::Rational{
                morph::math::Numerator{0}, morph::math::Denominator{1},
                morph::math::DecimalPlaces{UnitTraits<Currency>::meta(currency).defaultDecimals}},  // no legs
                                                                                // exist yet at this task's
                                                                                // scope -- Task 8 computes a
                                                                                // real balance -- but the
                                                                                // placeholder zero is still
                                                                                // tagged at the account's
                                                                                // actual currency precision
                                                                                // (0 for JPY/KRW, 2 for
                                                                                // USD/EUR), not a hardcoded 2
        });
    }
    return result;
}

}  // namespace ledger
