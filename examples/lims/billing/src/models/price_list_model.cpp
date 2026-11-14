// SPDX-License-Identifier: Apache-2.0
#include "lims/billing/models/price_list_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "lims/billing/authorization/billing_authorization.hpp"
#include "lims/billing/db/billing_entity.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims {

namespace {

using ::morph::math::DecimalPlaces;
using ::morph::math::Denominator;
using ::morph::math::Numerator;
using ::morph::math::Rational;

constexpr int kMaxCurrencyCode = static_cast<int>(CurrencyCode::JPY);
constexpr std::int64_t kRateScale = 1'000'000;  // a Rate holds six decimals
constexpr std::int64_t kMinApprovalHours = 1;
constexpr std::int64_t kMaxApprovalHours = 720;
constexpr std::int64_t kMinBillingDay = 1;
constexpr std::int64_t kMaxBillingDay = 28;

/// @brief The currency a row stores.
/// @param value The stored integer.
/// @param what The row, for the message.
/// @return The currency.
/// @throws ValidationError if the integer is outside the enumeration.
[[nodiscard]] CurrencyCode currencyOf(int value, std::string const& what) {
    if (value < 0 || value > kMaxCurrencyCode) {
        throw ValidationError{what + " stores a currency outside its range"};
    }
    return static_cast<CurrencyCode>(value);
}

/// @brief A stored amount, at the currency's decimals.
/// @param numerator The numerator.
/// @param denominator The denominator.
/// @param currency The currency.
/// @return The amount.
[[nodiscard]] Amount amountOf(std::int64_t numerator, std::int64_t denominator, CurrencyCode currency) {
    return Amount{Rational{Numerator{numerator}, Denominator{denominator}, DecimalPlaces{decimalsOf(currency)}}};
}

/// @brief The whole number @p count holds.
/// @param count The count.
/// @param what The field, for the message.
/// @return Its value.
/// @throws ValidationError if it is not a whole number.
[[nodiscard]] std::int64_t wholeOf(Count const& count, std::string_view what) {
    if (!(*count).isInteger()) {
        throw ValidationError{"SetBillingSettings: " + std::string{what} + " must be a whole number"};
    }
    return (*count).numerator;
}

[[nodiscard]] BillingSettingsView toView(db::BillingSettingsRecord const& row) {
    return BillingSettingsView{
        .company = CompanyId{row.companyId.Value()},
        .receivableAccount = std::string{row.receivableAccount.Value().ToStringView()},
        .revenueAccount = std::string{row.revenueAccount.Value().ToStringView()},
        .taxAccount = std::string{row.taxAccount.Value().ToStringView()},
        .roundingAccount = std::string{row.roundingAccount.Value().ToStringView()},
        .taxRate =
            Rate{Rational{Numerator{row.taxRateNum.Value()}, Denominator{row.taxRateDen.Value()}, DecimalPlaces{6}}},
        .approvalHours = Count{Rational{Numerator{row.approvalHours.Value()}, Denominator{1}, DecimalPlaces{0}}},
        .billingDay = Count{Rational{Numerator{row.billingDay.Value()}, Denominator{1}, DecimalPlaces{0}}},
    };
}

[[nodiscard]] BillingClientView toView(db::BillingClientRecord const& row) {
    return BillingClientView{
        .id = BillingClientId{static_cast<std::int64_t>(row.id.Value())},
        .client = ClientId{row.clientId.Value()},
        .company = CompanyId{row.companyId.Value()},
        .name = std::string{row.name.Value().ToStringView()},
        .currency = currencyOf(row.currency.Value(), "billing client " + std::to_string(row.id.Value())),
    };
}

[[nodiscard]] PriceView toView(db::PriceRecord const& row, CurrencyCode currency) {
    return PriceView{
        .id = PriceId{static_cast<std::int64_t>(row.id.Value())},
        .analysis = AnalysisId{row.analysisId.Value()},
        .client = row.clientId.Value() == 0 ? ClientId{} : ClientId{row.clientId.Value()},
        .amount = amountOf(row.amountNum.Value(), row.amountDen.Value(), currency),
    };
}

[[nodiscard]] PriceListView toView(db::PriceListRecord const& row, std::vector<PriceView> prices) {
    return PriceListView{
        .id = PriceListId{static_cast<std::int64_t>(row.id.Value())},
        .company = CompanyId{row.companyId.Value()},
        .name = std::string{row.name.Value().ToStringView()},
        .currency = currencyOf(row.currency.Value(), "price list " + std::to_string(row.id.Value())),
        .validFrom = timestampFromMillis(row.validFromMs.Value()),
        .prices = std::move(prices),
    };
}

/// @brief Whether the caller can see @p company.
/// @param company The company.
/// @return Whether a role of theirs reaches it.
[[nodiscard]] bool visible(CompanyId company) {
    auto const companies = OrgModel::visibleCompanies(currentPrincipal());
    return std::ranges::find(companies, company) != companies.end();
}

/// @brief Throws `NotFound` unless @p company exists.
/// @param where The action, for the message.
/// @param company The company.
void requireCompany(std::string_view where, CompanyId company) {
    if (!OrgModel::findCompany(company).has_value()) {
        throw NotFound{std::string{where} + ": company " + std::to_string(*company) + " does not exist"};
    }
}

/// @brief Runs an insert that a lookup guards, answering the loss of a race on the unique index as the conflict.
///
/// A concurrent call that inserted the same key between this call's lookup and its insert makes the index refuse
/// this one, and this call's transaction is rolled back. The winner's row is there now, so the answer is the same
/// Conflict a later call gets. A failure that is not that race finds no such row and surfaces as it is.
/// @param insert One attempt, in its own transaction; throws the conflict when its lookup finds the key.
/// @param exists Whether a row holds the key now.
/// @param conflict The refusal for a key that is taken.
/// @return What @p insert answers.
template <typename Insert, typename Exists, typename MakeConflict>
auto guardedInsert(Insert const& insert, Exists const& exists, MakeConflict const& conflict) {
    try {
        return insert();
    } catch (Lightweight::SqlException const&) {
        if (exists()) {
            throw conflict();
        }
        throw;
    }
}

[[nodiscard]] std::optional<db::BillingSettingsRecord> settingsRow(Lightweight::DataMapper& mapper,
                                                                   CompanyId company) {
    auto rows = mapper.Query<db::BillingSettingsRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::BillingSettingsRecord::companyId>, "=", *company)
                    .All();
    return rows.empty() ? std::nullopt : std::optional{rows.front()};
}

[[nodiscard]] std::optional<db::BillingClientRecord> clientRow(Lightweight::DataMapper& mapper, ClientId client,
                                                               CompanyId company) {
    auto rows = mapper.Query<db::BillingClientRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::BillingClientRecord::clientId>, "=", *client)
                    .Where(::Lightweight::FieldNameOf<&db::BillingClientRecord::companyId>, "=", *company)
                    .All();
    return rows.empty() ? std::nullopt : std::optional{rows.front()};
}

[[nodiscard]] std::optional<db::PriceListRecord> listRow(Lightweight::DataMapper& mapper, PriceListId list) {
    auto rows = mapper.Query<db::PriceListRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::PriceListRecord::id>, "=", *list)
                    .All();
    return rows.empty() ? std::nullopt : std::optional{rows.front()};
}

[[nodiscard]] std::optional<db::PriceListRecord> listStartRow(Lightweight::DataMapper& mapper,
                                                              CreatePriceList const& action) {
    auto rows =
        mapper.Query<db::PriceListRecord>()
            .Where(::Lightweight::FieldNameOf<&db::PriceListRecord::companyId>, "=", *action.company)
            .Where(::Lightweight::FieldNameOf<&db::PriceListRecord::currency>, "=", static_cast<int>(action.currency))
            .Where(::Lightweight::FieldNameOf<&db::PriceListRecord::validFromMs>, "=", millisOf(action.validFrom))
            .All();
    return rows.empty() ? std::nullopt : std::optional{rows.front()};
}

[[nodiscard]] std::int64_t clientKey(ClientId client) { return client.hasValue() ? *client : 0; }

[[nodiscard]] std::optional<db::PriceRecord> priceRow(Lightweight::DataMapper& mapper, SetPrice const& action) {
    auto rows = mapper.Query<db::PriceRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::PriceRecord::priceList>, "=", *action.priceList)
                    .Where(::Lightweight::FieldNameOf<&db::PriceRecord::analysisId>, "=", *action.analysis)
                    .Where(::Lightweight::FieldNameOf<&db::PriceRecord::clientId>, "=", clientKey(action.client))
                    .All();
    return rows.empty() ? std::nullopt : std::optional{rows.front()};
}

/// @brief Refuses settings whose numbers are outside their ranges.
/// @param action The settings.
/// @throws ValidationError if the tax rate is outside 0 to 1 or finer than six decimals, or the approval hours or
///         billing day are not whole numbers in their ranges.
void requireSettingsInRange(SetBillingSettings const& action) {
    Rational const& tax = *action.taxRate;
    if (tax.isNegative() || tax > Rational{1, DecimalPlaces{6}} || kRateScale % tax.denominator != 0) {
        throw ValidationError{"SetBillingSettings: the tax rate must be from 0 to 1 with at most six decimals"};
    }
    auto const hours = wholeOf(action.approvalHours, "the approval hours");
    if (hours < kMinApprovalHours || hours > kMaxApprovalHours) {
        throw ValidationError{"SetBillingSettings: the approval hours must be from 1 to 720"};
    }
    auto const day = wholeOf(action.billingDay, "the billing day");
    if (day < kMinBillingDay || day > kMaxBillingDay) {
        throw ValidationError{"SetBillingSettings: the billing day must be from 1 to 28"};
    }
}

/// @brief One attempt at storing settings, in its own transaction.
/// @param action The settings.
/// @param betweenLookupAndWrite Called after the lookup found no settings and before the insert.
/// @return The stored settings.
/// @throws Lightweight::SqlException if the write fails.
[[nodiscard]] BillingSettingsView storeSettingsOnce(SetBillingSettings const& action,
                                                    std::function<void()> const& betweenLookupAndWrite) {
    // The lookup runs on a connection of its own, outside the write's transaction: a read inside it would hold a
    // lock that blocks a concurrent writer's commit.
    auto const existing = [&] {
        Lightweight::DataMapper lookup;
        return settingsRow(lookup, action.company);
    }();
    if (!existing.has_value() && betweenLookupAndWrite) {
        betweenLookupAndWrite();
    }
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    db::BillingSettingsRecord row = existing.value_or(db::BillingSettingsRecord{});
    row.companyId = *action.company;
    row.receivableAccount = Lightweight::SqlAnsiString<16>{action.receivableAccount};
    row.revenueAccount = Lightweight::SqlAnsiString<16>{action.revenueAccount};
    row.taxAccount = Lightweight::SqlAnsiString<16>{action.taxAccount};
    row.roundingAccount = Lightweight::SqlAnsiString<16>{action.roundingAccount};
    row.taxRateNum = (*action.taxRate).numerator;
    row.taxRateDen = (*action.taxRate).denominator;
    row.approvalHours = (*action.approvalHours).numerator;
    row.billingDay = (*action.billingDay).numerator;
    if (existing.has_value()) {
        mapper.Update(row);
    } else {
        mapper.Create(row);
    }
    sqlTxn.Commit();
    return toView(row);
}

}  // namespace

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
BillingSettingsView PriceListModel::execute(const SetBillingSettings& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{
            "SetBillingSettings: a company, four account codes of 1 to 16 upper-case letters and digits, a tax rate, "
            "approval hours and a billing day are required"};
    }
    requireSettingsInRange(action);
    requireCompany("SetBillingSettings", action.company);
    return storeSettings(action, {});
}

BillingSettingsView PriceListModel::storeSettings(SetBillingSettings const& action,
                                                  std::function<void()> const& betweenLookupAndWrite) {
    try {
        return storeSettingsOnce(action, betweenLookupAndWrite);
    } catch (Lightweight::SqlException const&) {
        // A concurrent first call for the company inserted its row between this call's lookup and its insert, and
        // this call's transaction is rolled back. Now the row is there, so this call replaces it, as if it had come
        // after. A failure that is not that race finds no row and surfaces as it is.
        Lightweight::DataMapper mapper;
        if (settingsRow(mapper, action.company).has_value()) {
            return storeSettingsOnce(action, {});
        }
        throw;
    }
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
BillingSettingsView PriceListModel::execute(const GetBillingSettings& action) {
    if (!action.validate()) {
        throw ValidationError{"GetBillingSettings: a company is required"};
    }
    auto const settings = visible(action.company) ? settingsOf(action.company) : std::nullopt;
    if (!settings.has_value()) {
        throw NotFound{"GetBillingSettings: company " + std::to_string(*action.company) + " has no billing settings"};
    }
    return *settings;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
BillingClientView PriceListModel::execute(const RegisterBillingClient& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{
            "RegisterBillingClient: a client above zero, a company and a name of 1 to 128 bytes are required"};
    }
    requireCompany("RegisterBillingClient", action.company);
    return storeClient(action, {});
}

BillingClientView PriceListModel::storeClient(RegisterBillingClient const& action,
                                              std::function<void()> const& betweenLookupAndInsert) {
    auto const conflict = [&] {
        return Conflict{"RegisterBillingClient: client " + std::to_string(*action.client) +
                        " is already registered at company " + std::to_string(*action.company)};
    };
    return guardedInsert(
        [&] {
            if (std::invoke([&] {
                    Lightweight::DataMapper lookup;
                    return clientRow(lookup, action.client, action.company).has_value();
                })) {
                throw conflict();
            }
            if (betweenLookupAndInsert) {
                betweenLookupAndInsert();
            }
            Lightweight::DataMapper mapper;
            Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
            db::BillingClientRecord row;
            row.clientId = *action.client;
            row.companyId = *action.company;
            row.name = Lightweight::SqlAnsiString<128>{action.name};
            row.currency = static_cast<int>(action.currency);
            mapper.Create(row);
            sqlTxn.Commit();
            return toView(row);
        },
        [&] {
            Lightweight::DataMapper mapper;
            return clientRow(mapper, action.client, action.company).has_value();
        },
        conflict);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListBillingClientsResult PriceListModel::execute(const ListBillingClients& /*action*/) {
    ListBillingClientsResult result;
    auto const companies = OrgModel::visibleCompanies(currentPrincipal());
    Lightweight::DataMapper mapper;
    for (auto const& row : mapper.Query<db::BillingClientRecord>().All()) {
        if (std::ranges::find(companies, CompanyId{row.companyId.Value()}) != companies.end()) {
            result.clients.push_back(toView(row));
        }
    }
    std::ranges::sort(result.clients, {},
                      [](BillingClientView const& view) { return std::tuple{*view.company, *view.client}; });
    return result;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
PriceListView PriceListModel::execute(const CreatePriceList& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreatePriceList: a company, a start and a name of 1 to 128 bytes are required"};
    }
    requireCompany("CreatePriceList", action.company);
    return storePriceList(action, {});
}

PriceListView PriceListModel::storePriceList(CreatePriceList const& action,
                                             std::function<void()> const& betweenLookupAndInsert) {
    auto const conflict = [&] {
        return Conflict{"CreatePriceList: company " + std::to_string(*action.company) + " already has a " +
                        std::string{currencyName(action.currency)} + " price list from that instant"};
    };
    return guardedInsert(
        [&] {
            if (std::invoke([&] {
                    Lightweight::DataMapper lookup;
                    return listStartRow(lookup, action).has_value();
                })) {
                throw conflict();
            }
            if (betweenLookupAndInsert) {
                betweenLookupAndInsert();
            }
            Lightweight::DataMapper mapper;
            Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
            db::PriceListRecord row;
            row.companyId = *action.company;
            row.name = Lightweight::SqlAnsiString<128>{action.name};
            row.currency = static_cast<int>(action.currency);
            row.validFromMs = millisOf(action.validFrom);
            mapper.Create(row);
            sqlTxn.Commit();
            return toView(row, {});
        },
        [&] {
            Lightweight::DataMapper mapper;
            return listStartRow(mapper, action).has_value();
        },
        conflict);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
PriceView PriceListModel::execute(const SetPrice& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{
            "SetPrice: a price list, an analysis, an amount and, if given, a client above zero are required"};
    }
    if ((*action.amount).isNegative()) {
        throw ValidationError{"SetPrice: a price cannot be negative"};
    }
    std::optional<db::PriceListRecord> list;
    {
        Lightweight::DataMapper mapper;
        list = listRow(mapper, action.priceList);
    }
    if (!list.has_value()) {
        throw NotFound{"SetPrice: price list " + std::to_string(*action.priceList) + " does not exist"};
    }
    auto const currency = currencyOf(list->currency.Value(), "price list " + std::to_string(list->id.Value()));
    if (!fitsCurrency(*action.amount, currency)) {
        throw ValidationError{"SetPrice: a " + std::string{currencyName(currency)} +
                              " price must be a whole number of its minor units"};
    }
    return storePrice(action, {});
}

PriceView PriceListModel::storePrice(SetPrice const& action, std::function<void()> const& betweenLookupAndInsert) {
    auto const conflict = [&] {
        return Conflict{"SetPrice: price list " + std::to_string(*action.priceList) +
                        " already has a price for that analysis and client"};
    };
    return guardedInsert(
        [&] {
            auto const list = std::invoke([&] {
                Lightweight::DataMapper lookup;
                auto found = listRow(lookup, action.priceList);
                if (!found.has_value()) {
                    throw NotFound{"SetPrice: price list " + std::to_string(*action.priceList) + " does not exist"};
                }
                if (priceRow(lookup, action).has_value()) {
                    throw conflict();
                }
                return found;
            });
            if (betweenLookupAndInsert) {
                betweenLookupAndInsert();
            }
            Lightweight::DataMapper mapper;
            Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
            db::PriceRecord row;
            row.priceList = *action.priceList;
            row.analysisId = *action.analysis;
            row.clientId = clientKey(action.client);
            row.amountNum = (*action.amount).numerator;
            row.amountDen = (*action.amount).denominator;
            mapper.Create(row);
            sqlTxn.Commit();
            return toView(row, currencyOf(list->currency.Value(), "price list " + std::to_string(list->id.Value())));
        },
        [&] {
            Lightweight::DataMapper mapper;
            return priceRow(mapper, action).has_value();
        },
        conflict);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListPriceListsResult PriceListModel::execute(const ListPriceLists& action) {
    if (!action.validate()) {
        throw ValidationError{"ListPriceLists: a company is required"};
    }
    ListPriceListsResult result;
    if (!visible(action.company)) {
        return result;
    }
    Lightweight::DataMapper mapper;
    for (auto const& list :
         mapper.Query<db::PriceListRecord>()
             .Where(::Lightweight::FieldNameOf<&db::PriceListRecord::companyId>, "=", *action.company)
             .All()) {
        auto const currency = currencyOf(list.currency.Value(), "price list " + std::to_string(list.id.Value()));
        std::vector<PriceView> prices;
        for (auto const& price : mapper.Query<db::PriceRecord>()
                                     .Where(::Lightweight::FieldNameOf<&db::PriceRecord::priceList>, "=",
                                            static_cast<std::int64_t>(list.id.Value()))
                                     .All()) {
            prices.push_back(toView(price, currency));
        }
        std::ranges::sort(prices, {}, [](PriceView const& price) {
            return std::tuple{*price.analysis, price.client.hasValue() ? *price.client : 0};
        });
        result.priceLists.push_back(toView(list, std::move(prices)));
    }
    std::ranges::sort(result.priceLists, {},
                      [](PriceListView const& view) { return std::tuple{millisOf(view.validFrom), *view.id}; });
    return result;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
PriceQuoteView PriceListModel::execute(const QuotePrice& action) {
    if (!action.validate()) {
        throw ValidationError{"QuotePrice: a company, a client, an analysis and a date are required"};
    }
    auto const quote =
        visible(action.company) ? priceOn(action.company, action.client, action.analysis, action.on) : std::nullopt;
    if (!quote.has_value()) {
        throw NotFound{"QuotePrice: no price for analysis " + std::to_string(*action.analysis) + " for client " +
                       std::to_string(*action.client) + " at company " + std::to_string(*action.company)};
    }
    return *quote;
}

std::optional<BillingSettingsView> PriceListModel::settingsOf(CompanyId company) {
    if (!company.hasValue()) {
        return std::nullopt;
    }
    Lightweight::DataMapper mapper;
    auto const row = settingsRow(mapper, company);
    return row.has_value() ? std::optional{toView(*row)} : std::nullopt;
}

std::optional<BillingClientView> PriceListModel::billingClient(ClientId client, CompanyId company) {
    if (!client.hasValue() || !company.hasValue()) {
        return std::nullopt;
    }
    Lightweight::DataMapper mapper;
    auto const row = clientRow(mapper, client, company);
    return row.has_value() ? std::optional{toView(*row)} : std::nullopt;
}

std::optional<PriceQuoteView> PriceListModel::priceOn(CompanyId company, ClientId client, AnalysisId analysis,
                                                      ::morph::time::Timestamp const& date) {
    auto const billed = billingClient(client, company);
    if (!billed.has_value() || !analysis.hasValue() || !date.hasValue()) {
        return std::nullopt;
    }
    Lightweight::DataMapper mapper;
    std::optional<db::PriceListRecord> inForce;
    for (auto const& list : mapper.Query<db::PriceListRecord>()
                                .Where(::Lightweight::FieldNameOf<&db::PriceListRecord::companyId>, "=", *company)
                                .Where(::Lightweight::FieldNameOf<&db::PriceListRecord::currency>, "=",
                                       static_cast<int>(billed->currency))
                                .All()) {
        if (list.validFromMs.Value() <= millisOf(date) &&
            (!inForce.has_value() || list.validFromMs.Value() > inForce->validFromMs.Value())) {
            inForce = list;
        }
    }
    if (!inForce.has_value()) {
        return std::nullopt;
    }
    std::optional<db::PriceRecord> found;
    for (auto const& price : mapper.Query<db::PriceRecord>()
                                 .Where(::Lightweight::FieldNameOf<&db::PriceRecord::priceList>, "=",
                                        static_cast<std::int64_t>(inForce->id.Value()))
                                 .Where(::Lightweight::FieldNameOf<&db::PriceRecord::analysisId>, "=", *analysis)
                                 .All()) {
        if (price.clientId.Value() == *client) {
            found = price;
            break;
        }
        if (price.clientId.Value() == 0) {
            found = price;
        }
    }
    if (!found.has_value()) {
        return std::nullopt;
    }
    return PriceQuoteView{
        .priceList = PriceListId{static_cast<std::int64_t>(inForce->id.Value())},
        .amount = amountOf(found->amountNum.Value(), found->amountDen.Value(), billed->currency),
        .currency = billed->currency,
    };
}

}  // namespace lims
