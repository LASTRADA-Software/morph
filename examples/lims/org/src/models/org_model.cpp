// SPDX-License-Identifier: Apache-2.0
#include "lims/org/models/org_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <cstdint>
#include <map>
#include <morph/session/session.hpp>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "lims/org/authorization/org_authorization.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/core/money.hpp"
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

/// @brief The highest stored value of `LimsRole`.
constexpr int kMaxRole = static_cast<int>(LimsRole::InvoiceApprover);

/// @brief The highest stored value of `ScopeKind`.
constexpr int kMaxScopeKind = static_cast<int>(ScopeKind::Site);

/// @brief The role an assignment row stores.
/// @param row The row.
/// @return The role.
/// @throws ValidationError if the row stores a role outside its enumeration.
[[nodiscard]] LimsRole roleOf(db::RoleAssignmentRecord const& row) {
    int const role = row.role.Value();
    if (role < 0 || role > kMaxRole) {
        throw ValidationError{"role assignment " + std::to_string(row.id.Value()) +
                              " stores a role outside its range"};
    }
    return static_cast<LimsRole>(role);
}

/// @brief The scope kind an assignment row stores.
/// @param row The row.
/// @return The kind.
/// @throws ValidationError if the row stores a scope kind outside its enumeration.
[[nodiscard]] ScopeKind scopeKindOf(db::RoleAssignmentRecord const& row) {
    int const kind = row.scopeKind.Value();
    if (kind < 0 || kind > kMaxScopeKind) {
        throw ValidationError{"role assignment " + std::to_string(row.id.Value()) +
                              " stores a scope kind outside its range"};
    }
    return static_cast<ScopeKind>(kind);
}

/// @brief Maps an assignment row to its view.
/// @param row The row.
/// @return The view.
/// @throws ValidationError if the row stores a role or a scope kind outside its enumeration.
[[nodiscard]] RoleAssignmentView toView(db::RoleAssignmentRecord const& row) {
    auto const kind = scopeKindOf(row);
    return RoleAssignmentView{
        .id = RoleAssignmentId{static_cast<std::int64_t>(row.id.Value())},
        .principal = std::string{row.principal.Value().ToStringView()},
        .role = roleOf(row),
        .scopeKind = kind,
        .company = kind == ScopeKind::Company ? CompanyId{row.scopeId.Value()} : CompanyId{},
        .site = kind == ScopeKind::Site ? SiteId{row.scopeId.Value()} : SiteId{},
        .grantedBy = std::string{row.grantedBy.Value().ToStringView()},
        .grantedAt = timestampFromMillis(row.grantedAt.Value()),
    };
}

/// @brief The kind and id an assignment row stores for @p scope.
/// @param scope A scope holding a value.
/// @return The kind and the id.
[[nodiscard]] std::pair<ScopeKind, std::int64_t> storedScope(Scope const& scope) {
    if (auto const* company = std::get_if<CompanyId>(&scope)) {
        return {ScopeKind::Company, **company};
    }
    return {ScopeKind::Site, *std::get<SiteId>(scope)};
}

/// @brief Whether @p scope names a company or a site that exists.
/// @param mapper The open mapper.
/// @param scope The scope.
/// @return Whether it does.
[[nodiscard]] bool scopeExists(Lightweight::DataMapper& mapper, Scope const& scope) {
    if (auto const* company = std::get_if<CompanyId>(&scope)) {
        return company->hasValue() && !mapper.Query<db::CompanyRecord>()
                                           .Where(::Lightweight::FieldNameOf<&db::CompanyRecord::id>, "=", **company)
                                           .All()
                                           .empty();
    }
    return OrgModel::findSite(std::get<SiteId>(scope)).has_value();
}

/// @brief Every assignment of @p principal, by id.
/// @param mapper The open mapper.
/// @param principal The principal.
/// @return The rows.
[[nodiscard]] std::vector<db::RoleAssignmentRecord> assignmentsOf(Lightweight::DataMapper& mapper,
                                                                  std::string_view principal) {
    auto rows =
        mapper.Query<db::RoleAssignmentRecord>()
            .Where(::Lightweight::FieldNameOf<&db::RoleAssignmentRecord::principal>, "=", std::string{principal})
            .All();
    std::ranges::sort(rows, {}, [](db::RoleAssignmentRecord const& row) { return row.id.Value(); });
    return rows;
}

/// @brief Appends @p role to @p roles unless it is there already.
/// @param roles The roles so far.
/// @param role The role.
void addOnce(std::vector<LimsRole>& roles, LimsRole role) {
    if (std::ranges::find(roles, role) == roles.end()) {
        roles.push_back(role);
    }
}

/// @brief Maps a value-limit row to its view, its limit at @p currency's decimals.
/// @param row The row.
/// @param currency The company's base currency.
/// @return The view.
[[nodiscard]] ValueLimitView toView(db::ValueLimitRecord const& row, CurrencyCode currency) {
    return ValueLimitView{
        .id = ValueLimitId{static_cast<std::int64_t>(row.id.Value())},
        .principal = std::string{row.principal.Value().ToStringView()},
        .company = CompanyId{static_cast<std::int64_t>(row.company.Value())},
        .limit = Amount{::morph::math::Rational{::morph::math::Numerator{row.limitNum.Value()},
                                                ::morph::math::Denominator{row.limitDen.Value()},
                                                ::morph::math::DecimalPlaces{decimalsOf(currency)}}},
        .setBy = std::string{row.setBy.Value().ToStringView()},
        .setAt = timestampFromMillis(row.setAt.Value()),
    };
}

}  // namespace

CompanyView OrgModel::execute(const CreateCompany& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateCompany: a name of at most 128 bytes is required"};
    }
    // Without a supervisor anywhere nobody passes, so the bootstrap assignment stays the first administrative act.
    requireAuthorized(action);
    Lightweight::DataMapper mapper;
    for (auto const& existing : mapper.Query<db::CompanyRecord>().All()) {
        if (existing.name.Value().ToStringView() == action.name) {
            throw Conflict{"CreateCompany: a company named \"" + action.name + "\" already exists"};
        }
    }
    // The company and its first supervisor are one fact: a company with nobody able to administer it could never be
    // given a role again, because roles are not hierarchical and nothing sits above a company.
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    db::CompanyRecord row;
    row.name = Lightweight::SqlAnsiString<128>{action.name};
    row.baseCurrency = static_cast<int>(action.baseCurrency);
    row.fiscalYearStart = static_cast<int>(action.fiscalYearStart);
    mapper.Create(row);
    auto const& principal = ::morph::session::current()->principal;
    db::RoleAssignmentRecord supervisor;
    supervisor.principal = Lightweight::SqlAnsiString<64>{principal};
    supervisor.role = static_cast<int>(LimsRole::Supervisor);
    supervisor.scopeKind = static_cast<int>(ScopeKind::Company);
    supervisor.scopeId = static_cast<std::int64_t>(row.id.Value());
    supervisor.grantedBy = Lightweight::SqlAnsiString<64>{principal};
    supervisor.grantedAt = nowMillis();
    mapper.Create(supervisor);
    sqlTxn.Commit();
    return toView(row);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListCompaniesResult OrgModel::execute(const ListCompanies& action) {
    static_cast<void>(action);
    auto const visible = visibleCompanies(currentPrincipal());
    Lightweight::DataMapper mapper;
    ListCompaniesResult result;
    for (auto const& row : mapper.Query<db::CompanyRecord>().All()) {
        auto view = toView(row);
        if (std::ranges::find(visible, view.id) != visible.end()) {
            result.companies.push_back(std::move(view));
        }
    }
    std::ranges::sort(result.companies, {}, [](CompanyView const& company) { return *company.id; });
    return result;
}

SiteView OrgModel::execute(const CreateSite& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateSite: a company and a name of at most 128 bytes are required"};
    }
    requireRoleSomewhere<CreateSite>();
    Lightweight::DataMapper mapper;
    if (mapper.Query<db::CompanyRecord>()
            .Where(::Lightweight::FieldNameOf<&db::CompanyRecord::id>, "=", *action.companyId)
            .All()
            .empty()) {
        throw NotFound{"CreateSite: company " + std::to_string(*action.companyId) + " does not exist"};
    }
    requireAuthorized(action);
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
    auto const visible = visibleSites(currentPrincipal());
    Lightweight::DataMapper mapper;
    ListSitesResult result;
    for (auto const& row : mapper.Query<db::SiteRecord>().All()) {
        auto view = toView(row);
        if (std::ranges::find(visible, view.id) != visible.end()) {
            result.sites.push_back(std::move(view));
        }
    }
    std::ranges::sort(result.sites, {}, [](SiteView const& site) { return *site.id; });
    return result;
}

RoleAssignmentView OrgModel::execute(const AssignRole& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"AssignRole: a principal and exactly one of a company or a site are required"};
    }
    Scope const scope = Authorization<AssignRole>::scopeOf(action);
    // A lab with no supervisor anywhere cannot appoint its first one any other way. The exception closes as soon as
    // one exists, and the assignment names who made it.
    bool const bootstrap = !supervisorExists();
    if (!bootstrap) {
        requireRoleSomewhere<AssignRole>();
    }
    Lightweight::DataMapper mapper;
    if (!scopeExists(mapper, scope)) {
        throw NotFound{"AssignRole: " + scopeName(scope) + " does not exist"};
    }
    if (!bootstrap) {
        requireAuthorized(action);
    }
    auto const [kind, scopeId] = storedScope(scope);
    for (auto const& row : assignmentsOf(mapper, action.principal)) {
        if (row.role.Value() == static_cast<int>(action.role) && row.scopeKind.Value() == static_cast<int>(kind) &&
            row.scopeId.Value() == scopeId) {
            return toView(row);
        }
    }
    db::RoleAssignmentRecord row;
    row.principal = Lightweight::SqlAnsiString<64>{action.principal};
    row.role = static_cast<int>(action.role);
    row.scopeKind = static_cast<int>(kind);
    row.scopeId = scopeId;
    row.grantedBy = Lightweight::SqlAnsiString<64>{::morph::session::current()->principal};
    row.grantedAt = nowMillis();
    mapper.Create(row);
    return toView(row);
}

RevokeRoleResult OrgModel::execute(const RevokeRole& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"RevokeRole: an assignment is required"};
    }
    requireRoleSomewhere<RevokeRole>();
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::RoleAssignmentRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::RoleAssignmentRecord::id>, "=", *action.assignmentId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"RevokeRole: assignment " + std::to_string(*action.assignmentId) + " does not exist"};
    }
    auto const view = toView(rows.front());
    requireAuthorized(action);
    if (view.role == LimsRole::Supervisor) {
        // Only a company's own supervisors can administer it, so only they are counted: a supervisor at one of its
        // sites cannot assign at the company, and another company's supervisor cannot assign here.
        if (view.scopeKind == ScopeKind::Company &&
            mapper.Query<db::RoleAssignmentRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::RoleAssignmentRecord::role>, "=",
                           static_cast<int>(LimsRole::Supervisor))
                    .Where(::Lightweight::FieldNameOf<&db::RoleAssignmentRecord::scopeKind>, "=",
                           static_cast<int>(ScopeKind::Company))
                    .Where(::Lightweight::FieldNameOf<&db::RoleAssignmentRecord::scopeId>, "=", *view.company)
                    .All()
                    .size() == 1) {
            throw Conflict{"RevokeRole: the last supervisor assignment of company " + std::to_string(*view.company) +
                           " cannot be revoked"};
        }
        // Whatever its scope, the last supervisor assignment of all stays: with none left the bootstrap carve-out
        // would admit whoever acts next.
        if (mapper.Query<db::RoleAssignmentRecord>()
                .Where(::Lightweight::FieldNameOf<&db::RoleAssignmentRecord::role>, "=",
                       static_cast<int>(LimsRole::Supervisor))
                .All()
                .size() == 1) {
            throw Conflict{"RevokeRole: the last supervisor assignment cannot be revoked"};
        }
    }
    mapper.Delete(rows.front());
    return RevokeRoleResult{.assignmentId = action.assignmentId};
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListRoleAssignmentsResult OrgModel::execute(const ListRoleAssignments& action) {
    static_cast<void>(action);
    auto const principal = currentPrincipal();
    auto const sites = visibleSites(principal);
    auto const companies = visibleCompanies(principal);
    Lightweight::DataMapper mapper;
    ListRoleAssignmentsResult result;
    for (auto const& row : mapper.Query<db::RoleAssignmentRecord>().All()) {
        auto view = toView(row);
        bool const seen = view.scopeKind == ScopeKind::Company
                              ? std::ranges::find(companies, view.company) != companies.end()
                              : std::ranges::find(sites, view.site) != sites.end();
        if (seen) {
            result.assignments.push_back(std::move(view));
        }
    }
    std::ranges::sort(result.assignments, {}, [](RoleAssignmentView const& view) { return *view.id; });
    return result;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListUsersResult OrgModel::execute(const ListUsers& action) {
    static_cast<void>(action);
    auto const assignments = execute(ListRoleAssignments{}).assignments;
    std::map<std::string, std::vector<LimsRole>> byPrincipal;
    for (auto const& assignment : assignments) {
        addOnce(byPrincipal[assignment.principal], assignment.role);
    }
    ListUsersResult result;
    for (auto& [principal, roles] : byPrincipal) {
        result.users.push_back(UserView{.id = principal, .roles = std::move(roles)});
    }
    return result;
}

std::vector<SiteId> OrgModel::visibleSites(std::string_view principal) {
    Lightweight::DataMapper mapper;
    auto const assignments = assignmentsOf(mapper, principal);
    std::vector<SiteId> visible;
    for (auto const& site : mapper.Query<db::SiteRecord>().All()) {
        auto const siteId = static_cast<std::int64_t>(site.id.Value());
        auto const companyId = static_cast<std::int64_t>(site.company.Value());
        bool const seen = std::ranges::any_of(assignments, [siteId, companyId](db::RoleAssignmentRecord const& row) {
            auto const kind = scopeKindOf(row);
            return (kind == ScopeKind::Site && row.scopeId.Value() == siteId) ||
                   (kind == ScopeKind::Company && row.scopeId.Value() == companyId);
        });
        if (seen) {
            visible.emplace_back(siteId);
        }
    }
    std::ranges::sort(visible, {}, [](SiteId const& site) { return *site; });
    return visible;
}

std::vector<CompanyId> OrgModel::visibleCompanies(std::string_view principal) {
    Lightweight::DataMapper mapper;
    std::vector<CompanyId> visible;
    for (auto const& row : assignmentsOf(mapper, principal)) {
        if (scopeKindOf(row) == ScopeKind::Company) {
            visible.emplace_back(row.scopeId.Value());
        }
    }
    for (SiteId const site : visibleSites(principal)) {
        if (auto const found = findSite(site)) {
            visible.push_back(found->companyId);
        }
    }
    std::ranges::sort(visible, {}, [](CompanyId const& company) { return *company; });
    auto const duplicates = std::ranges::unique(visible);
    visible.erase(duplicates.begin(), duplicates.end());
    return visible;
}

std::vector<LimsRole> OrgModel::rolesIn(std::string_view principal, Scope const& scope) {
    std::optional<std::int64_t> company;
    std::optional<std::int64_t> site;
    if (auto const* asCompany = std::get_if<CompanyId>(&scope)) {
        company = asCompany->value;
    } else if (auto const found = findSite(std::get<SiteId>(scope))) {
        site = *found->id;
        company = *found->companyId;
    }
    Lightweight::DataMapper mapper;
    std::vector<LimsRole> roles;
    for (auto const& row : assignmentsOf(mapper, principal)) {
        auto const kind = scopeKindOf(row);
        bool const covers = (kind == ScopeKind::Company && company == row.scopeId.Value()) ||
                            (kind == ScopeKind::Site && site == row.scopeId.Value());
        if (covers) {
            addOnce(roles, roleOf(row));
        }
    }
    return roles;
}

std::vector<LimsRole> OrgModel::rolesAnywhere(std::string_view principal) {
    Lightweight::DataMapper mapper;
    std::vector<LimsRole> roles;
    for (auto const& row : assignmentsOf(mapper, principal)) {
        addOnce(roles, roleOf(row));
    }
    return roles;
}

bool OrgModel::supervisorExists() {
    Lightweight::DataMapper mapper;
    return !mapper.Query<db::RoleAssignmentRecord>()
                .Where(::Lightweight::FieldNameOf<&db::RoleAssignmentRecord::role>, "=",
                       static_cast<int>(LimsRole::Supervisor))
                .All()
                .empty();
}

CompanyId OrgModel::seededCompany() { return CompanyId{db::kSeededCompanyId}; }

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

std::optional<CompanyView> OrgModel::findCompany(CompanyId company) {
    if (!company.hasValue()) {
        return std::nullopt;
    }
    Lightweight::DataMapper mapper;
    auto const rows = mapper.Query<db::CompanyRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::CompanyRecord::id>, "=", *company)
                          .All();
    if (rows.empty()) {
        return std::nullopt;
    }
    return toView(rows.front());
}

ValueLimitView OrgModel::execute(const SetValueLimit& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"SetValueLimit: a principal, a company and a limit are required"};
    }
    requireRoleSomewhere<SetValueLimit>();
    auto const company = findCompany(action.company);
    if (!company.has_value()) {
        throw NotFound{"SetValueLimit: company " + std::to_string(*action.company) + " does not exist"};
    }
    requireAuthorized(action);
    auto const& limit = *action.limit;
    if (limit.numerator < 0 || !fitsCurrency(limit, company->baseCurrency)) {
        throw ValidationError{"SetValueLimit: a limit is a non-negative amount in " +
                              std::string{currencyName(company->baseCurrency)}};
    }
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto rows = mapper.Query<db::ValueLimitRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::ValueLimitRecord::principal>, "=", action.principal)
                    .Where(::Lightweight::FieldNameOf<&db::ValueLimitRecord::company>, "=", *action.company)
                    .All();
    db::ValueLimitRecord row = rows.empty() ? db::ValueLimitRecord{} : rows.front();
    row.principal = Lightweight::SqlAnsiString<64>{action.principal};
    row.company = static_cast<std::uint64_t>(*action.company);
    row.limitNum = limit.numerator;
    row.limitDen = limit.denominator;
    row.setBy = Lightweight::SqlAnsiString<64>{::morph::session::current()->principal};
    row.setAt = nowMillis();
    if (rows.empty()) {
        mapper.Create(row);
    } else {
        mapper.Update(row);
    }
    sqlTxn.Commit();
    return toView(row, company->baseCurrency);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListValueLimitsResult OrgModel::execute(const ListValueLimits& action) {
    static_cast<void>(action);
    auto const visible = visibleCompanies(currentPrincipal());
    Lightweight::DataMapper mapper;
    ListValueLimitsResult result;
    for (auto const& row : mapper.Query<db::ValueLimitRecord>().All()) {
        CompanyId const company{static_cast<std::int64_t>(row.company.Value())};
        if (std::ranges::find(visible, company) == visible.end()) {
            continue;
        }
        auto const view = findCompany(company);
        if (!view.has_value()) {
            continue;
        }
        result.limits.push_back(toView(row, view->baseCurrency));
    }
    std::ranges::sort(result.limits, {}, [](ValueLimitView const& limit) { return *limit.id; });
    return result;
}

std::optional<::morph::math::Rational> OrgModel::valueLimit(std::string_view principal, CompanyId company) {
    auto const view = findCompany(company);
    if (!view.has_value()) {
        return std::nullopt;
    }
    Lightweight::DataMapper mapper;
    auto const rows =
        mapper.Query<db::ValueLimitRecord>()
            .Where(::Lightweight::FieldNameOf<&db::ValueLimitRecord::principal>, "=", std::string{principal})
            .Where(::Lightweight::FieldNameOf<&db::ValueLimitRecord::company>, "=", *company)
            .All();
    if (rows.empty()) {
        return std::nullopt;
    }
    return *toView(rows.front(), view->baseCurrency).limit;
}

}  // namespace lims
