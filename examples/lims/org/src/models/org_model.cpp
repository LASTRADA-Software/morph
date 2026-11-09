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

/// @brief The highest stored value of `LimsRole`.
constexpr int kMaxRole = static_cast<int>(LimsRole::Supervisor);

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

/// @brief The scope an assignment action names.
/// @param action A validated assignment.
/// @return Its company or its site.
[[nodiscard]] Scope scopeOf(AssignRole const& action) {
    return action.company.hasValue() ? Scope{action.company} : Scope{action.site};
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

/// @brief Throws `Forbidden` unless the authenticated principal is a supervisor in @p scope.
/// @param scope The scope.
void requireSupervisorIn(Scope const& scope) {
    auto const& principal = ::morph::session::current()->principal;
    auto const held = OrgModel::rolesIn(principal, scope);
    if (std::ranges::find(held, LimsRole::Supervisor) == held.end()) {
        throw Forbidden{"'" + principal + "' does not hold the supervisor role at " + scopeName(scope)};
    }
}

/// @brief Throws `Forbidden` unless the authenticated principal is a supervisor in some scope.
///
/// Asked before a scope or an assignment is looked up, so a caller with no supervisor role anywhere learns nothing
/// about which ids exist.
void requireSupervisorAnywhere() {
    auto const& principal = ::morph::session::current()->principal;
    auto const held = OrgModel::rolesAnywhere(principal);
    if (std::ranges::find(held, LimsRole::Supervisor) == held.end()) {
        throw Forbidden{"'" + principal + "' does not hold the supervisor role"};
    }
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

RoleAssignmentView OrgModel::execute(const AssignRole& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"AssignRole: a principal and exactly one of a company or a site are required"};
    }
    Scope const scope = scopeOf(action);
    // A lab with no supervisor anywhere cannot appoint its first one any other way. The exception closes as soon as
    // one exists, and the assignment names who made it.
    bool const bootstrap = !supervisorExists();
    if (!bootstrap) {
        requireSupervisorAnywhere();
    }
    Lightweight::DataMapper mapper;
    if (!scopeExists(mapper, scope)) {
        throw NotFound{"AssignRole: " + scopeName(scope) + " does not exist"};
    }
    if (!bootstrap) {
        requireSupervisorIn(scope);
    }
    auto const [kind, id] = storedScope(scope);
    for (auto const& row : assignmentsOf(mapper, action.principal)) {
        if (row.role.Value() == static_cast<int>(action.role) && row.scopeKind.Value() == static_cast<int>(kind) &&
            row.scopeId.Value() == id) {
            return toView(row);
        }
    }
    db::RoleAssignmentRecord row;
    row.principal = Lightweight::SqlAnsiString<64>{action.principal};
    row.role = static_cast<int>(action.role);
    row.scopeKind = static_cast<int>(kind);
    row.scopeId = id;
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
    requireSupervisorAnywhere();
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::RoleAssignmentRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::RoleAssignmentRecord::id>, "=", *action.assignmentId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"RevokeRole: assignment " + std::to_string(*action.assignmentId) + " does not exist"};
    }
    auto const view = toView(rows.front());
    requireSupervisorIn(view.scopeKind == ScopeKind::Company ? Scope{view.company} : Scope{view.site});
    // Only a company's own supervisors can administer it, so only they are counted: a supervisor at one of its sites
    // cannot assign at the company, and another company's supervisor cannot assign here.
    if (view.role == LimsRole::Supervisor && view.scopeKind == ScopeKind::Company) {
        auto const supervisorsHere = std::ranges::count_if(
            mapper.Query<db::RoleAssignmentRecord>()
                .Where(::Lightweight::FieldNameOf<&db::RoleAssignmentRecord::role>, "=",
                       static_cast<int>(LimsRole::Supervisor))
                .All(),
            [&](db::RoleAssignmentRecord const& row) {
                return scopeKindOf(row) == ScopeKind::Company && row.scopeId.Value() == *view.company;
            });
        if (supervisorsHere == 1) {
            throw Conflict{"RevokeRole: the last supervisor assignment of company " + std::to_string(*view.company) +
                           " cannot be revoked"};
        }
    }
    mapper.Delete(rows.front());
    return RevokeRoleResult{.assignmentId = action.assignmentId};
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListRoleAssignmentsResult OrgModel::execute(const ListRoleAssignments& action) {
    static_cast<void>(action);
    Lightweight::DataMapper mapper;
    ListRoleAssignmentsResult result;
    for (auto const& row : mapper.Query<db::RoleAssignmentRecord>().All()) {
        result.assignments.push_back(toView(row));
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
