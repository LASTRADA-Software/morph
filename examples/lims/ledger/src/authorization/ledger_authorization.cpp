// SPDX-License-Identifier: Apache-2.0
#include "lims/ledger/authorization/ledger_authorization.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>

#include "lims/ledger/db/ledger_entity.hpp"

namespace lims {

namespace {

/// @brief The company of period @p period, or an empty company.
/// @param period The period.
/// @return The scope.
[[nodiscard]] Scope periodCompany(PeriodId period) {
    if (!period.hasValue()) {
        return Scope{CompanyId{}};
    }
    Lightweight::DataMapper mapper;
    auto const rows =
        mapper.Query<db::PeriodRecord>().Where(::Lightweight::FieldNameOf<&db::PeriodRecord::id>, "=", *period).All();
    return rows.empty() ? Scope{CompanyId{}} : Scope{CompanyId{rows.front().companyId.Value()}};
}

/// @brief The company of entry @p entry, or an empty company.
/// @param entry The entry.
/// @return The scope.
[[nodiscard]] Scope entryCompany(JournalEntryId entry) {
    if (!entry.hasValue()) {
        return Scope{CompanyId{}};
    }
    Lightweight::DataMapper mapper;
    auto const rows =
        mapper.Query<db::EntryRecord>().Where(::Lightweight::FieldNameOf<&db::EntryRecord::id>, "=", *entry).All();
    return rows.empty() ? Scope{CompanyId{}} : Scope{CompanyId{rows.front().companyId.Value()}};
}

}  // namespace

Scope Authorization<BeginClosePeriod>::scopeOf(BeginClosePeriod const& action) {
    return periodCompany(action.periodId);
}

Scope Authorization<PostEntry>::scopeOf(PostEntry const& action) { return entryCompany(action.entryId); }

Scope Authorization<ReverseEntry>::scopeOf(ReverseEntry const& action) { return entryCompany(action.entryId); }

}  // namespace lims
