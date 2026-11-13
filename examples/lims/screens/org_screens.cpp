// SPDX-License-Identifier: Apache-2.0

#include "org_screens.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "lims/org/dto/company_dto.hpp"
#include "lims/org/dto/role_dto.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_screens.hpp"
#include "shell_models.hpp"

namespace lims::screens {

namespace ui = morph::ui::author;

namespace {

/// @brief A cell showing one member of its row.
/// @param member The member's wire name.
/// @return The cell.
[[nodiscard]] ui::Node cell(std::string const& member) { return ui::text({.text = ui::row(member)}); }

/// @brief The roles of a user row, one text per role.
/// @return The cell.
[[nodiscard]] ui::Node rolesCell() {
    return ui::forEach(ui::row("roles"), {.key = ui::row(), .axis = std::string{"horizontal"}, .gap = std::int64_t{2}},
                       [](ui::RowScope& /*role*/) { return ui::text({.text = ui::row()}).testId("userRole"); });
}

}  // namespace

ui::Screen companiesScreen() {
    return ui::screen(std::string{kCompanies}, [](ui::Scope& screen) {
        screen.title(ui::t("lims.org.companies.title", "Companies and sites"));
        auto const org = screen.model<OrgModel>("org");
        auto const companies = screen.query<ListCompanies>("companies", org);
        auto const sites = screen.query<ListSites>("sites", org);
        auto const companyRows = companies.value().at(&ListCompaniesResult::companies);
        auto const siteRows = sites.value().at(&ListSitesResult::sites);
        return ui::column(
            {ui::text({.text = ui::coalesce({companies.error(), sites.error()}), .role = std::string{"error"}})
                 .testId("error"),
             ui::panel(
                 {ui::text({.text = ui::format("{0} companies", {ui::count(companyRows)})}).testId("companyCount"),
                  ui::table(
                      companyRows,
                      {.key = ui::row(&CompanyView::id),
                       .columns = {{.name = "name", .label = "Name", .cell = cell("name")},
                                   {.name = "baseCurrency", .label = "Base currency", .cell = cell("baseCurrency")},
                                   {.name = "fiscalYearStart",
                                    .label = "Fiscal year starts",
                                    .cell = cell("fiscalYearStart")}}})
                      .a11y("Companies")
                      .testId("companies"),
                  ui::form<CreateCompany>(org, {.presentation = {.submitLabel = "Create company", .showStatus = true}})
                      .id(ui::element("companyForm"))
                      .testId("companyForm")},
                 {.title = ui::t("lims.org.companies", "Companies"), .padding = 2}),
             ui::panel({ui::text({.text = ui::format("{0} sites", {ui::count(siteRows)})}).testId("siteCount"),
                        ui::table(siteRows,
                                  {.key = ui::row(&SiteView::id),
                                   .columns = {{.name = "name", .label = "Name", .cell = cell("name")},
                                               {.name = "companyId", .label = "Company", .cell = cell("companyId")}}})
                            .a11y("Sites")
                            .testId("sites"),
                        ui::form<CreateSite>(org, {.presentation = {.submitLabel = "Create site", .showStatus = true}})
                            .id(ui::element("siteForm"))
                            .testId("siteForm")},
                       {.title = ui::t("lims.org.sites", "Sites"), .padding = 2})},
            {.gap = 3});
    });
}

ui::Screen accessScreen() {
    return ui::screen(std::string{kAccess}, [](ui::Scope& screen) {
        screen.title(ui::t("lims.org.access.title", "Users and roles"));
        auto const org = screen.model<OrgModel>("org");
        auto const users = screen.query<ListUsers>("users", org);
        auto const assignments = screen.query<ListRoleAssignments>("assignments", org);
        auto const revoke = screen.mutation<RevokeRole>("revoke", org, {.concurrency = ui::Mode::Serial});
        auto const userRows = users.value().at(&ListUsersResult::users);
        auto const assignmentRows = assignments.value().at(&ListRoleAssignmentsResult::assignments);
        return ui::column(
            {ui::text({.text = ui::coalesce({revoke.error(), users.error(), assignments.error()}),
                       .role = std::string{"error"}})
                 .testId("error"),
             ui::panel({ui::text({.text = ui::format("{0} users", {ui::count(userRows)})}).testId("userCount"),
                        ui::table(userRows, {.key = ui::row("id"),
                                             .columns = {{.name = "id", .label = "User", .cell = cell("id")},
                                                         {.name = "roles", .label = "Roles", .cell = rolesCell()}}})
                            .a11y("Users")
                            .testId("users")},
                       {.title = ui::t("lims.org.users", "Users"), .padding = 2}),
             ui::panel({ui::text({.text = ui::format("{0} role assignments", {ui::count(assignmentRows)})})
                            .testId("assignmentCount"),
                        ui::table(assignmentRows, {.key = ui::row(&RoleAssignmentView::id)},
                                  [&](ui::DeclScope& /*row*/) {
                                      return std::vector<ui::TableColumn>{
                                          {.name = "principal", .label = "User", .cell = cell("principal")},
                                          {.name = "role", .label = "Role", .cell = cell("role")},
                                          {.name = "scopeKind", .label = "Scope", .cell = cell("scopeKind")},
                                          {.name = "company", .label = "Company", .cell = cell("company")},
                                          {.name = "site", .label = "Site", .cell = cell("site")},
                                          {.name = "revoke",
                                           .label = "",
                                           .cell = ui::button({.label = "Revoke",
                                                               .onClick = ui::run(
                                                                   revoke, {{&RevokeRole::assignmentId,
                                                                             ui::row(&RoleAssignmentView::id)}})})
                                                       .testId("revoke")},
                                      };
                                  })
                            .a11y("Role assignments")
                            .testId("assignments"),
                        ui::form<AssignRole>(org, {.presentation = {.submitLabel = "Assign role", .showStatus = true}})
                            .id(ui::element("assignForm"))
                            .testId("assignForm")},
                       {.title = ui::t("lims.org.assignments", "Role assignments"), .padding = 2})},
            {.gap = 3});
    });
}

void addOrgScreens(ui::ScreenRegistry& screens, std::vector<ListedScreen>& listed) {
    listed.push_back({.id = std::string{kCompanies},
                      .module = "org",
                      .title = ui::t("lims.org.companies.title", "Companies and sites"),
                      .registered = screens.add(companiesScreen())});
    listed.push_back({.id = std::string{kAccess},
                      .module = "org",
                      .title = ui::t("lims.org.access.title", "Users and roles"),
                      .registered = screens.add(accessScreen())});
}

std::vector<std::string_view> orgScreenIds() { return {kCompanies, kAccess}; }

}  // namespace lims::screens
