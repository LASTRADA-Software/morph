// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lims/org/authorization/declared.hpp"
#include "lims/org/dto/company_dto.hpp"
#include "lims/org/dto/limit_dto.hpp"
#include "lims/org/dto/role_dto.hpp"

/// @file
/// The org module's declared authorization: administering the organisation is a supervisor's.

namespace lims {

/// @brief A new company has no scope yet, so creating one needs a supervisor anywhere. While no supervisor exists
///        anywhere it is refused, which leaves the bootstrap assignment the first administrative act.
template <>
struct Authorization<CreateCompany> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Supervisor;
};

/// @brief A site is created by a supervisor at its company.
template <>
struct Authorization<CreateSite> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Supervisor;

    /// @brief The company the payload names.
    /// @param action The action.
    /// @return Its company.
    [[nodiscard]] static Scope scopeOf(CreateSite const& action) { return Scope{action.companyId}; }
};

/// @brief A role is assigned by a supervisor in the scope it is assigned in. `OrgModel` lets the first assignment
///        through on a lab with no supervisor before it asks.
template <>
struct Authorization<AssignRole> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Supervisor;

    /// @brief The company or site the payload names.
    /// @param action The action.
    /// @return Its scope.
    [[nodiscard]] static Scope scopeOf(AssignRole const& action) {
        return action.company.hasValue() ? Scope{action.company} : Scope{action.site};
    }
};

/// @brief An assignment is revoked by a supervisor in the scope it was made in.
template <>
struct Authorization<RevokeRole> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Supervisor;

    /// @brief The scope of the assignment the payload names.
    /// @param action The action.
    /// @return Its scope; an empty company scope when there is no such assignment.
    [[nodiscard]] static Scope scopeOf(RevokeRole const& action);
};

/// @brief A value limit is set by a supervisor at its company.
template <>
struct Authorization<SetValueLimit> : CompanyRule<SetValueLimit, LimsRole::Supervisor> {};

}  // namespace lims
