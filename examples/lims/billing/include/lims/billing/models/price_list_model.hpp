// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>
#include <morph/util/datetime.hpp>
#include <optional>

#include "lims/billing/dto/price_dto.hpp"
#include "lims/billing/dto/settings_dto.hpp"

/// @file
/// `PriceListModel`: a company's billing settings, the clients it bills and its price lists.

namespace lims {

/// @brief What each company bills and at what price.
///
/// Unkeyed, holding no connection: each `execute()` opens a `Lightweight::DataMapper` for its own duration. A client
/// is billed in the currency it is registered with at a company. The price of an analysis on a date comes from the
/// company's list in that currency with the latest start not after the date, the list in force; within it the
/// client's own price wins over the list's. A list in force with no price for an analysis prices nothing, so an older
/// list's price never shows through a newer list. A list stays open to new prices, including one already in force; a
/// price is never changed (a second one for the same analysis and client is a `Conflict`), and a correction is a new
/// list with a later effective date.
class PriceListModel {
public:
    /// @brief Sets a company's billing settings, replacing any it had.
    /// @param action The company, the four account codes, the tax rate, the approval hours and the billing day.
    /// @return The settings.
    /// @throws ValidationError if the request is malformed, the tax rate is outside 0 to 1 or has more than six
    ///         decimals, the approval hours are not a whole number from 1 to 720, or the billing day is not a whole
    ///         number from 1 to 28.
    /// @throws NotFound if the company does not exist.
    /// @throws Forbidden if the caller is not a supervisor at the company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    BillingSettingsView execute(const SetBillingSettings& action);

    /// @brief Reads a company's billing settings, when the caller can see the company.
    /// @param action The company.
    /// @return The settings.
    /// @throws ValidationError if no company is named.
    /// @throws NotFound if the company has no settings or is not visible to the caller.
    BillingSettingsView execute(const GetBillingSettings& action);

    /// @brief Registers a client for billing at a company, in a currency.
    /// @param action The client, company, name and currency.
    /// @return The registration.
    /// @throws ValidationError if the request is malformed.
    /// @throws NotFound if the company does not exist.
    /// @throws Conflict if the client is already registered at the company.
    /// @throws Forbidden if the caller is not a billing clerk at the company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    BillingClientView execute(const RegisterBillingClient& action);

    /// @brief Lists the billing clients of every company the caller can see.
    /// @param action Nothing.
    /// @return The clients by company then client; none of a company the caller cannot see.
    /// @throws ValidationError if a row holds a currency outside its enumeration.
    ListBillingClientsResult execute(const ListBillingClients& action);

    /// @brief Creates a price list.
    /// @param action The company, name, currency and start.
    /// @return The list, with no prices.
    /// @throws ValidationError if the request is malformed.
    /// @throws NotFound if the company does not exist.
    /// @throws Conflict if the company already has a list in the currency from that instant.
    /// @throws Forbidden if the caller is not a billing clerk at the company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    PriceListView execute(const CreatePriceList& action);

    /// @brief Sets the price of an analysis in a list: the list's own, or one client's.
    /// @param action The list, analysis, client (empty for the list's own) and amount.
    /// @return The price.
    /// @throws ValidationError if the request is malformed, the amount is negative, or it is not a whole number of
    ///         the list's currency's minor units.
    /// @throws NotFound if the price list does not exist.
    /// @throws Conflict if the list already has a price for the analysis and client.
    /// @throws Forbidden if the caller is not a billing clerk at the list's company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    PriceView execute(const SetPrice& action);

    /// @brief Lists a company's price lists with their prices, when the caller can see the company.
    /// @param action The company.
    /// @return Its lists by start then id; none for a company the caller cannot see.
    /// @throws ValidationError if no company is named, or a row holds a currency outside its enumeration.
    ListPriceListsResult execute(const ListPriceLists& action);

    /// @brief The price of an analysis for a client on a date, when the caller can see the company.
    /// @param action The company, client, analysis and date.
    /// @return The price.
    /// @throws ValidationError if the question is malformed.
    /// @throws NotFound if the company is not visible to the caller, or `priceOn` finds no price.
    PriceQuoteView execute(const QuotePrice& action);

    /// @brief The company's billing settings.
    /// @param company The company.
    /// @return Them, or nothing when the company has none.
    [[nodiscard]] static std::optional<BillingSettingsView> settingsOf(CompanyId company);

    /// @brief How @p client is billed at @p company.
    /// @param client The lab's client.
    /// @param company The company.
    /// @return The registration, or nothing when the client is not registered there.
    [[nodiscard]] static std::optional<BillingClientView> billingClient(ClientId client, CompanyId company);

    /// @brief The price of @p analysis for @p client at @p company on @p date: from the list in force in the client's
    ///        currency, the client's own price first.
    /// @param company The company.
    /// @param client The client.
    /// @param analysis The analysis definition.
    /// @param date The date.
    /// @return The price, or nothing when the client is not registered, no list is in force, or it has no price for
    ///         the analysis.
    [[nodiscard]] static std::optional<PriceQuoteView> priceOn(CompanyId company, ClientId client, AnalysisId analysis,
                                                               ::morph::time::Timestamp const& date);

    /// @brief Stores a company's settings, replacing any it had.
    ///
    /// The seam under `SetBillingSettings` after its checks. Two first calls for one company race on the unique
    /// index: the loser's insert fails, and it then finds the winner's row and replaces it, as if it had come after.
    /// @param action The settings, already validated and authorized.
    /// @param betweenLookupAndWrite Called once the lookup found no settings and before the insert; a test stores
    ///        the winner's row here. Empty in production.
    /// @return The stored settings.
    /// @throws Lightweight::SqlException if the write fails for another reason.
    [[nodiscard]] static BillingSettingsView storeSettings(SetBillingSettings const& action,
                                                           std::function<void()> const& betweenLookupAndWrite);

    /// @brief Stores a registration, answering a client another call registered first as a Conflict.
    /// @param action The registration, already validated and authorized.
    /// @param betweenLookupAndInsert Called once the lookup found no registration and before the insert; a test
    ///        registers the winner here. Empty in production.
    /// @return The registration.
    /// @throws Conflict if the client is already registered at the company.
    /// @throws Lightweight::SqlException if the insert fails for another reason.
    [[nodiscard]] static BillingClientView storeClient(RegisterBillingClient const& action,
                                                       std::function<void()> const& betweenLookupAndInsert);

    /// @brief Stores a price list, answering a start another call created first as a Conflict.
    /// @param action The list, already validated and authorized.
    /// @param betweenLookupAndInsert Called once the lookup found no such list and before the insert; a test
    ///        creates the winner here. Empty in production.
    /// @return The list, with no prices.
    /// @throws Conflict if the company already has a list in the currency from that instant.
    /// @throws Lightweight::SqlException if the insert fails for another reason.
    [[nodiscard]] static PriceListView storePriceList(CreatePriceList const& action,
                                                      std::function<void()> const& betweenLookupAndInsert);

    /// @brief Stores a price, answering one another call set first as a Conflict.
    /// @param action The price, already validated and authorized.
    /// @param betweenLookupAndInsert Called once the lookup found no such price and before the insert; a test sets
    ///        the winner here. Empty in production.
    /// @return The price.
    /// @throws Conflict if the list already has a price for the analysis and client.
    /// @throws Lightweight::SqlException if the insert fails for another reason.
    [[nodiscard]] static PriceView storePrice(SetPrice const& action,
                                              std::function<void()> const& betweenLookupAndInsert);
};

}  // namespace lims

// The registration macros declare a non-const status object each.
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_MODEL(lims::PriceListModel, "PriceListModel")
BRIDGE_REGISTER_ACTION(lims::PriceListModel, lims::SetBillingSettings, "SetBillingSettings")
BRIDGE_REGISTER_EFFECTS(lims::SetBillingSettings, ::morph::effects::changes<lims::BillingSettingsView>)
BRIDGE_REGISTER_ACTION(lims::PriceListModel, lims::GetBillingSettings, "GetBillingSettings",
                       ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::PriceListModel, lims::RegisterBillingClient, "RegisterBillingClient")
BRIDGE_REGISTER_EFFECTS(lims::RegisterBillingClient, ::morph::effects::changes<lims::BillingClientView>)
BRIDGE_REGISTER_ACTION(lims::PriceListModel, lims::ListBillingClients, "ListBillingClients",
                       ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::PriceListModel, lims::CreatePriceList, "CreatePriceList")
BRIDGE_REGISTER_EFFECTS(lims::CreatePriceList, ::morph::effects::changes<lims::PriceListView>)
BRIDGE_REGISTER_ACTION(lims::PriceListModel, lims::SetPrice, "SetPrice")
BRIDGE_REGISTER_EFFECTS(lims::SetPrice, ::morph::effects::changes<lims::PriceView>,
                        ::morph::effects::changes<lims::PriceListView>)
BRIDGE_REGISTER_ACTION(lims::PriceListModel, lims::ListPriceLists, "ListPriceLists", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::PriceListModel, lims::QuotePrice, "QuotePrice", ::morph::model::Loggable::No)
// NOLINTEND(misc-const-correctness)
