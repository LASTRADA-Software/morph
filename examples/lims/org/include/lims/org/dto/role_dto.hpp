// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/core/records.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "lims/org/authorization/declared_guard.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// Roles and role assignments: who may do what, and where.

namespace lims {

/// @brief What a principal is permitted to do.
///
/// A closed set, so an `enum class` rather than free-form strings
/// (`examples/IMPLEMENTATION.md` rule 3). Roles are not hierarchical: a
/// supervisor who must also verify is granted `Verifier` explicitly, because
/// an implicit hierarchy is the kind of thing that quietly grants somebody
/// the second pair of eyes on their own work.
enum class LimsRole : std::uint8_t {
    Analyst,          ///< May capture results.
    Verifier,         ///< May verify somebody else's results.
    Supervisor,       ///< May grant and revoke roles, and set value limits.
    Accountant,       ///< May keep a company's ledger: accounts, periods, entries and rates.
    BillingClerk,     ///< May keep a company's billing: prices, clients, invoices and billing runs.
    InvoiceApprover,  ///< May approve somebody else's invoice up to their value limit.
    Service,      ///< May run one module's reaction to another's, or a scheduled job: held only by the server's own
                  ///< `system:` principals.
    StockKeeper,  ///< May keep a company's stock: items, locations, lots, recipes and receipts.
    Purchaser,    ///< May buy for a company: suppliers and purchase orders.
    PurchaseApprover,  ///< May approve somebody else's requisition up to their value limit for it.
};

/// @brief The stable ascii name of @p role, as it appears in a token claim and
///        in the journal.
/// @param role The role to name.
/// @return Its wire name.
[[nodiscard]] constexpr std::string_view roleName(LimsRole role) noexcept {
    switch (role) {
        case LimsRole::Analyst:
            return "analyst";
        case LimsRole::Verifier:
            return "verifier";
        case LimsRole::Supervisor:
            return "supervisor";
        case LimsRole::Accountant:
            return "accountant";
        case LimsRole::BillingClerk:
            return "billing-clerk";
        case LimsRole::InvoiceApprover:
            return "invoice-approver";
        case LimsRole::Service:
            return "service";
        case LimsRole::StockKeeper:
            return "stock-keeper";
        case LimsRole::Purchaser:
            return "purchaser";
        case LimsRole::PurchaseApprover:
            return "purchase-approver";
        default:
            return "unknown";
    }
}

/// @brief Assigns a role to a principal in one scope: a company, or one site.
///
/// Requires `Supervisor` in that scope, except on a lab with no supervisor anywhere, where the first assignment is
/// allowed so the lab can be set up at all; that exception closes as soon as any supervisor exists.
struct AssignRole {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief Who gets the role.
    std::string principal;
    /// @brief The role.
    LimsRole role = LimsRole::Analyst;
    /// @brief The company scope; empty when the scope is a site.
    CompanyId company;
    /// @brief The site scope; empty when the scope is a company.
    SiteId site;

    /// @brief Neither scope is required on its own; the rule below requires exactly one.
    static constexpr std::array optionalFields{std::string_view{"company"}, std::string_view{"site"}};

    /// @brief The scope is a company or a site, never both and never neither.
    static constexpr auto formRules =
        ::morph::forms::ruleList(::morph::forms::exactlyOneOf(&AssignRole::company, &AssignRole::site));

    /// @brief Whether the assignment is well-formed.
    /// @return `true` when a principal of at most 64 bytes and exactly one scope are named.
    [[nodiscard]] bool validate() const noexcept {
        return !principal.empty() && principal.size() <= 64 && ::morph::forms::allRulesSatisfied(*this);
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief One role assignment.
struct RoleAssignmentView {
    /// @brief Its id.
    RoleAssignmentId id;
    /// @brief Who holds the role.
    std::string principal;
    /// @brief The role.
    LimsRole role = LimsRole::Analyst;
    /// @brief Whether the scope is a company or a site.
    ScopeKind scopeKind = ScopeKind::Company;
    /// @brief The company, when the scope is one.
    CompanyId company;
    /// @brief The site, when the scope is one.
    SiteId site;
    /// @brief Who made the assignment.
    std::string grantedBy;
    /// @brief When.
    ::morph::time::Timestamp grantedAt;
};

/// @brief Revokes one role assignment. Requires `Supervisor` in its scope; the last supervisor's is never revoked.
struct RevokeRole {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The assignment.
    RoleAssignmentId assignmentId;

    /// @brief Whether an assignment is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return assignmentId.hasValue(); }
};

/// @brief Which assignment a revoke removed.
struct RevokeRoleResult {
    /// @brief The revoked assignment.
    RoleAssignmentId assignmentId;
};

/// @brief Lists the role assignments.
struct ListRoleAssignments {
    /// @brief Always ready.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief The role assignments, by id.
struct ListRoleAssignmentsResult {
    /// @brief The assignments.
    std::vector<RoleAssignmentView> assignments;
};

/// @brief One user: a principal holding at least one role assignment. lims keeps no user store of its own; a
///        principal is whoever signs in.
struct UserView {
    /// @brief The principal.
    std::string id;
    /// @brief Their display name; empty when none is set, and a screen then shows the login.
    std::string displayName;
    /// @brief Every role they hold somewhere, in the order first assigned.
    std::vector<LimsRole> roles;
};

/// @brief Lists the users.
struct ListUsers {
    /// @brief Always ready.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief The users, by principal.
struct ListUsersResult {
    /// @brief The users.
    std::vector<UserView> users;
};

/// @brief Names a person: sets the display name screens show for a principal.
///
/// Allowed for the person themselves, and for a supervisor of a company in which the person holds a role. The rule
/// relates two principals, which the declared role-and-scope table cannot state, so the model checks it.
struct SetDisplayName {
    /// @brief Whose name.
    std::string principal;
    /// @brief The name to show.
    std::string displayName;

    /// @brief Whether the request is well-formed.
    /// @return `true` when a principal of 1 to 64 bytes and a display name of 1 to 128 bytes are named.
    [[nodiscard]] bool validate() const noexcept {
        return !principal.empty() && principal.size() <= 64 && !displayName.empty() && displayName.size() <= 128;
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief The caller's own user view: their roles and their display name.
struct GetSignedInUser {
    /// @brief Always ready.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

}  // namespace lims

/// @brief Reflects `LimsRole` as its enumerator name.
template <>
struct glz::meta<lims::LimsRole> {
    using enum lims::LimsRole;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Analyst, Verifier, Supervisor, Accountant, BillingClerk,
                                                 InvoiceApprover, Service, StockKeeper, Purchaser, PurchaseApprover);
};

BRIDGE_REGISTER_RECORD(lims::RoleAssignmentView, "Org_RoleAssignment")
BRIDGE_REGISTER_RECORD(lims::UserView, "Org_User")
