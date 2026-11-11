// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <vector>

#include "lims/org/authorization/declared_guard.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/core/strong_id.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// Value limits: the amount, in a company's base currency, up to which a principal may approve there.

namespace lims {

LIMS_DEFINE_STRONG_ID(ValueLimitId);

/// @brief Sets a principal's value limit at a company, replacing any they had there.
struct SetValueLimit {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief Whose limit.
    std::string principal;
    /// @brief The company.
    CompanyId company;
    /// @brief The limit, in the company's base currency.
    Amount limit;

    /// @brief Whether the request is well-formed.
    /// @return `true` when a principal of at most 64 bytes, a company and a limit are named.
    [[nodiscard]] bool validate() const noexcept {
        return !principal.empty() && principal.size() <= 64 && company.hasValue() && limit.hasValue();
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief One principal's limit at one company.
struct ValueLimitView {
    /// @brief Its id.
    ValueLimitId id;
    /// @brief Whose limit.
    std::string principal;
    /// @brief The company.
    CompanyId company;
    /// @brief The limit, in the company's base currency.
    Amount limit;
    /// @brief Who set it.
    std::string setBy;
    /// @brief When.
    ::morph::time::Timestamp setAt;
};

/// @brief Lists the value limits the caller may see: those at companies they supervise, and their own.
struct ListValueLimits {
    /// @brief Always ready.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief The limits, by id.
struct ListValueLimitsResult {
    /// @brief The limits.
    std::vector<ValueLimitView> limits;
};

}  // namespace lims

LIMS_DEFINE_STRONG_ID_WIRE(ValueLimitId);
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(ValueLimitId, "lims.valueLimitId");

BRIDGE_REGISTER_RECORD(lims::ValueLimitView, "Org_ValueLimit")
