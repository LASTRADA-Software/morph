// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "lims/billing/core/types.hpp"
#include "lims/org/authorization/declared_guard.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/core/references.hpp"
#include "lims/org/core/types.hpp"
#include "lims/posting/posting.hpp"

/// @file
/// A company's billing settings and the clients it bills, as the wire sees them.

namespace lims {

/// @brief Sets how a company bills: the accounts its invoices post to, its tax rate and its approval and run timing.
///
/// The account codes are names in the company's chart, kept as text: billing does not read the ledger's tables, and
/// the ledger refuses a posting that names a code its chart lacks. Setting twice replaces the earlier settings.
struct SetBillingSettings {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The company.
    CompanyId company;
    /// @brief The account an invoice's total is owed on.
    std::string receivableAccount;
    /// @brief The account an invoice's net amount is earned on.
    std::string revenueAccount;
    /// @brief The account an invoice's tax is owed on.
    std::string taxAccount;
    /// @brief The account a rounding difference is posted to.
    std::string roundingAccount;
    /// @brief The company's one flat tax rate, from 0 to 1 (0.19 for 19 %), at most six decimals.
    Rate taxRate;
    /// @brief How many hours an approval may wait before it escalates, 1 to 720.
    Count approvalHours;
    /// @brief The day of the month the billing run drafts invoices, 1 to 28 so that every month has it.
    Count billingDay;

    /// @brief Whether the settings are well-formed.
    /// @return `true` when a company, four account codes and the three numbers are named. The ranges of the numbers
    /// are the model's to judge.
    [[nodiscard]] bool validate() const noexcept {
        return company.hasValue() && isAccountCode(receivableAccount) && isAccountCode(revenueAccount) &&
               isAccountCode(taxAccount) && isAccountCode(roundingAccount) && taxRate.hasValue() &&
               approvalHours.hasValue() && billingDay.hasValue();
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief A company's billing settings.
struct BillingSettingsView {
    /// @brief The company; the settings' key.
    CompanyId company;
    /// @brief The account an invoice's total is owed on.
    std::string receivableAccount;
    /// @brief The account an invoice's net amount is earned on.
    std::string revenueAccount;
    /// @brief The account an invoice's tax is owed on.
    std::string taxAccount;
    /// @brief The account a rounding difference is posted to.
    std::string roundingAccount;
    /// @brief The flat tax rate.
    Rate taxRate;
    /// @brief How many hours an approval may wait.
    Count approvalHours;
    /// @brief The day of the month the billing run drafts invoices.
    Count billingDay;
};

/// @brief Reads a company's billing settings.
struct GetBillingSettings {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief Registers a lab client for billing at a company, in a currency.
struct RegisterBillingClient {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The lab's client.
    ClientId client;
    /// @brief The company that bills it.
    CompanyId company;
    /// @brief The name on its invoices.
    std::string name;
    /// @brief The currency it is billed in at the company.
    CurrencyCode currency = CurrencyCode::EUR;

    /// @brief Whether the registration is well-formed.
    /// @return `true` when a client above zero, a company and a name of 1 to 128 bytes are named.
    [[nodiscard]] bool validate() const noexcept {
        return client.hasValue() && *client > 0 && company.hasValue() && !name.empty() && name.size() <= 128;
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief How a client is billed at a company.
struct BillingClientView {
    /// @brief Its id.
    BillingClientId id;
    /// @brief The lab's client.
    ClientId client;
    /// @brief The company.
    CompanyId company;
    /// @brief The name on its invoices.
    std::string name;
    /// @brief The currency it is billed in.
    CurrencyCode currency = CurrencyCode::EUR;
};

/// @brief Lists the billing clients of every company the caller can see.
struct ListBillingClients {};

/// @brief The billing clients, by company then client.
struct ListBillingClientsResult {
    /// @brief The clients.
    std::vector<BillingClientView> clients;
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::BillingSettingsView, "Billing_Settings",
                       ::morph::record::key<&lims::BillingSettingsView::company>)
BRIDGE_REGISTER_RECORD(lims::BillingClientView, "Billing_Client")
