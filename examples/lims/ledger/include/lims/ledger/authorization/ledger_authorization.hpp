// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lims/ledger/dto/account_dto.hpp"
#include "lims/ledger/dto/entry_dto.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "lims/org/authorization/declared.hpp"

/// @file
/// The ledger module's declared authorization: keeping a company's books is an accountant's, at that company.

namespace lims {

/// @brief An account is added by an accountant at its company.
template <>
struct Authorization<CreateAccount> : CompanyRule<CreateAccount, LimsRole::Accountant> {};

/// @brief A month is opened by an accountant at its company.
template <>
struct Authorization<OpenPeriod> : CompanyRule<OpenPeriod, LimsRole::Accountant> {};

/// @brief A month's close is begun by an accountant at the period's company.
template <>
struct Authorization<BeginClosePeriod> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Accountant;

    /// @brief The company of the period the payload names.
    /// @param action The action.
    /// @return It; an empty company when there is no such period.
    [[nodiscard]] static Scope scopeOf(BeginClosePeriod const& action);
};

/// @brief A period is closed by an accountant at its company.
template <>
struct Authorization<ClosePeriod> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Accountant;

    /// @brief The company of the period the payload names.
    /// @param action The action.
    /// @return It; an empty company when there is no such period.
    [[nodiscard]] static Scope scopeOf(ClosePeriod const& action);
};

/// @brief A draft is made by an accountant at its company.
template <>
struct Authorization<CreateDraftEntry> : CompanyRule<CreateDraftEntry, LimsRole::Accountant> {};

/// @brief A draft is posted by an accountant at the entry's company.
template <>
struct Authorization<PostEntry> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Accountant;

    /// @brief The company of the entry the payload names.
    /// @param action The action.
    /// @return It; an empty company when there is no such entry.
    [[nodiscard]] static Scope scopeOf(PostEntry const& action);
};

/// @brief An entry is reversed by an accountant at its company.
template <>
struct Authorization<ReverseEntry> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Accountant;

    /// @brief The company of the entry the payload names.
    /// @param action The action.
    /// @return It; an empty company when there is no such entry.
    [[nodiscard]] static Scope scopeOf(ReverseEntry const& action);
};

}  // namespace lims
