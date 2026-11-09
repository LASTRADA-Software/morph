// SPDX-License-Identifier: Apache-2.0
#include "lims/org/models/org_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <algorithm>
#include <cstdint>
#include <morph/session/session.hpp>
#include <string>
#include <vector>

#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/db/org_entity.hpp"

namespace lims {

namespace {

/// @brief The highest stored value of `CurrencyCode`.
constexpr int kMaxCurrencyCode = static_cast<int>(CurrencyCode::JPY);

/// @brief The highest stored value of `Month`.
constexpr int kMaxMonth = static_cast<int>(Month::December);

/// @brief Maps a company row to its view.
/// @param row The row.
/// @return The view.
/// @throws ValidationError if the row stores a currency or a month outside its enumeration.
[[nodiscard]] CompanyView toView(db::CompanyRecord const& row) {
    int const currency = row.baseCurrency.Value();
    int const month = row.fiscalYearStart.Value();
    if (currency < 0 || currency > kMaxCurrencyCode || month < 0 || month > kMaxMonth) {
        throw ValidationError{"company " + std::to_string(row.id.Value()) +
                              " stores a base currency or fiscal-year start outside its range"};
    }
    return CompanyView{
        .id = CompanyId{static_cast<std::int64_t>(row.id.Value())},
        .name = std::string{row.name.Value().ToStringView()},
        .baseCurrency = static_cast<CurrencyCode>(currency),
        .fiscalYearStart = static_cast<Month>(month),
    };
}

/// @brief Maps a site row to its view.
/// @param row The row.
/// @return The view.
[[nodiscard]] SiteView toView(db::SiteRecord const& row) {
    return SiteView{
        .id = SiteId{static_cast<std::int64_t>(row.id.Value())},
        .companyId = CompanyId{static_cast<std::int64_t>(row.company.Value())},
        .name = std::string{row.name.Value().ToStringView()},
    };
}

}  // namespace

CompanyView OrgModel::execute(const CreateCompany& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateCompany: a name of at most 128 bytes is required"};
    }
    Lightweight::DataMapper mapper;
    for (auto const& existing : mapper.Query<db::CompanyRecord>().All()) {
        if (existing.name.Value().ToStringView() == action.name) {
            throw Conflict{"CreateCompany: a company named \"" + action.name + "\" already exists"};
        }
    }
    db::CompanyRecord row;
    row.name = Lightweight::SqlAnsiString<128>{action.name};
    row.baseCurrency = static_cast<int>(action.baseCurrency);
    row.fiscalYearStart = static_cast<int>(action.fiscalYearStart);
    mapper.Create(row);
    return toView(row);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListCompaniesResult OrgModel::execute(const ListCompanies& action) {
    static_cast<void>(action);
    Lightweight::DataMapper mapper;
    ListCompaniesResult result;
    for (auto const& row : mapper.Query<db::CompanyRecord>().All()) {
        result.companies.push_back(toView(row));
    }
    std::ranges::sort(result.companies, {}, [](CompanyView const& company) { return *company.id; });
    return result;
}

SiteView OrgModel::execute(const CreateSite& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateSite: a company and a name of at most 128 bytes are required"};
    }
    Lightweight::DataMapper mapper;
    if (mapper.Query<db::CompanyRecord>()
            .Where(::Lightweight::FieldNameOf<&db::CompanyRecord::id>, "=", *action.companyId)
            .All()
            .empty()) {
        throw NotFound{"CreateSite: company " + std::to_string(*action.companyId) + " does not exist"};
    }
    for (auto const& existing : mapper.Query<db::SiteRecord>().All()) {
        if (static_cast<std::int64_t>(existing.company.Value()) == *action.companyId &&
            existing.name.Value().ToStringView() == action.name) {
            throw Conflict{"CreateSite: company " + std::to_string(*action.companyId) +
                           " already has a site named \"" + action.name + "\""};
        }
    }
    db::SiteRecord row;
    row.company = static_cast<std::uint64_t>(*action.companyId);
    row.name = Lightweight::SqlAnsiString<128>{action.name};
    mapper.Create(row);
    return toView(row);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListSitesResult OrgModel::execute(const ListSites& action) {
    static_cast<void>(action);
    Lightweight::DataMapper mapper;
    ListSitesResult result;
    for (auto const& row : mapper.Query<db::SiteRecord>().All()) {
        result.sites.push_back(toView(row));
    }
    std::ranges::sort(result.sites, {}, [](SiteView const& site) { return *site.id; });
    return result;
}

std::optional<SiteView> OrgModel::findSite(SiteId site) {
    if (!site.hasValue()) {
        return std::nullopt;
    }
    Lightweight::DataMapper mapper;
    auto const rows =
        mapper.Query<db::SiteRecord>().Where(::Lightweight::FieldNameOf<&db::SiteRecord::id>, "=", *site).All();
    if (rows.empty()) {
        return std::nullopt;
    }
    return toView(rows.front());
}

}  // namespace lims
