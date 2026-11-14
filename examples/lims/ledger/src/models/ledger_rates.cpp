// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <tuple>

#include "lims/ledger/authorization/ledger_authorization.hpp"
#include "lims/ledger/db/ledger_entity.hpp"
#include "lims/ledger/models/ledger_model.hpp"
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

/// @brief The currency a row stores.
/// @param value The stored integer.
/// @param rateId The row, for the message.
/// @return The currency.
/// @throws ValidationError if the integer is outside the enumeration.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- a stored integer and a row id, never interchanged
[[nodiscard]] CurrencyCode currencyOf(int value, std::uint64_t rateId) {
    if (value < 0 || value > kMaxCurrencyCode) {
        throw ValidationError{"exchange rate " + std::to_string(rateId) + " stores a currency outside its range"};
    }
    return static_cast<CurrencyCode>(value);
}

/// @brief Maps a rate row to its view.
/// @param row The row.
/// @return The view.
/// @throws ValidationError if the row stores a currency outside its enumeration.
[[nodiscard]] ExchangeRateView toView(db::ExchangeRateRecord const& row) {
    return ExchangeRateView{
        .id = ExchangeRateId{static_cast<std::int64_t>(row.id.Value())},
        .company = CompanyId{row.companyId.Value()},
        .from = currencyOf(row.fromCurrency.Value(), row.id.Value()),
        .to = currencyOf(row.toCurrency.Value(), row.id.Value()),
        .validFrom = timestampFromMillis(row.validFromMs.Value()),
        .rate = Rate{Rational{Numerator{row.rateNum.Value()}, Denominator{row.rateDen.Value()}, DecimalPlaces{6}}},
        .setBy = std::string{row.setBy.Value().ToStringView()},
    };
}

/// @brief Whether a company already has a rate for a pair from an instant.
/// @param action The rate being entered.
/// @return `true` when one is stored.
[[nodiscard]] bool rateStartExists(SetExchangeRate const& action) {
    Lightweight::DataMapper mapper;
    return !mapper.Query<db::ExchangeRateRecord>()
                .Where(::Lightweight::FieldNameOf<&db::ExchangeRateRecord::companyId>, "=", *action.company)
                .Where(::Lightweight::FieldNameOf<&db::ExchangeRateRecord::fromCurrency>, "=",
                       static_cast<int>(action.from))
                .Where(::Lightweight::FieldNameOf<&db::ExchangeRateRecord::toCurrency>, "=",
                       static_cast<int>(action.to))
                .Where(::Lightweight::FieldNameOf<&db::ExchangeRateRecord::validFromMs>, "=",
                       millisOf(action.validFrom))
                .All()
                .empty();
}

/// @brief The refusal of a start that is already entered.
/// @param action The rate being entered.
/// @return The exception to throw.
[[nodiscard]] Conflict rateConflict(SetExchangeRate const& action) {
    return Conflict{"SetExchangeRate: company " + std::to_string(*action.company) + " already has a " +
                    std::string{currencyName(action.from)} + "/" + std::string{currencyName(action.to)} +
                    " rate from that instant"};
}

/// @brief One attempt at entering a rate, in its own transaction.
/// @param action The rate.
/// @param setBy Who enters it.
/// @param betweenLookupAndInsert Called after the lookup found no such start and before the insert.
/// @return The stored rate.
/// @throws Conflict if the start is already entered.
/// @throws Lightweight::SqlException if the insert fails.
[[nodiscard]] ExchangeRateView enterRateOnce(SetExchangeRate const& action, std::string const& setBy,
                                             std::function<void()> const& betweenLookupAndInsert) {
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    if (rateStartExists(action)) {
        throw rateConflict(action);
    }
    if (betweenLookupAndInsert) {
        betweenLookupAndInsert();
    }
    db::ExchangeRateRecord row;
    row.companyId = *action.company;
    row.fromCurrency = static_cast<int>(action.from);
    row.toCurrency = static_cast<int>(action.to);
    row.validFromMs = millisOf(action.validFrom);
    row.rateNum = (*action.rate).numerator;
    row.rateDen = (*action.rate).denominator;
    row.setBy = Lightweight::SqlAnsiString<64>{setBy};
    mapper.Create(row);
    sqlTxn.Commit();
    return toView(row);
}

}  // namespace

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ExchangeRateView LedgerModel::execute(const SetExchangeRate& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{
            "SetExchangeRate: a company, a start, two different currencies and a positive rate are required"};
    }
    if (!OrgModel::findCompany(action.company).has_value()) {
        throw NotFound{"SetExchangeRate: company " + std::to_string(*action.company) + " does not exist"};
    }
    return enterRate(action, currentPrincipal(), {});
}

ExchangeRateView LedgerModel::enterRate(SetExchangeRate const& action, std::string const& setBy,
                                        std::function<void()> const& betweenLookupAndInsert) {
    try {
        return enterRateOnce(action, setBy, betweenLookupAndInsert);
    } catch (Lightweight::SqlException const&) {
        // A concurrent call entering the same start committed between this call's lookup and its insert (the unique
        // index refuses the second row), and this call's transaction is rolled back. The winner's row is there now:
        // the second entry is a Conflict, as if it had come after. A failure that is not that race has no such row
        // and surfaces as it is.
        if (rateStartExists(action)) {
            throw rateConflict(action);
        }
        throw;
    }
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListExchangeRatesResult LedgerModel::execute(const ListExchangeRates& action) {
    if (!action.validate()) {
        throw ValidationError{"ListExchangeRates: a company is required"};
    }
    ListExchangeRatesResult result;
    auto const companies = OrgModel::visibleCompanies(currentPrincipal());
    if (std::ranges::find(companies, action.company) == companies.end()) {
        return result;
    }
    Lightweight::DataMapper mapper;
    for (auto const& row :
         mapper.Query<db::ExchangeRateRecord>()
             .Where(::Lightweight::FieldNameOf<&db::ExchangeRateRecord::companyId>, "=", *action.company)
             .All()) {
        result.rates.push_back(toView(row));
    }
    std::ranges::sort(result.rates, {}, [](ExchangeRateView const& rate) {
        return std::tuple{static_cast<int>(rate.from), static_cast<int>(rate.to), millisOf(rate.validFrom)};
    });
    return result;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
RateAnswer LedgerModel::execute(const GetRateOn& action) {
    if (!action.validate()) {
        throw ValidationError{"GetRateOn: a company and a date are required"};
    }
    auto const visibleCompanies = OrgModel::visibleCompanies(currentPrincipal());
    if (std::ranges::find(visibleCompanies, action.query.company) == visibleCompanies.end()) {
        // Not an empty answer: "no rate" would send a poster looking for missing data when the caller is missing a
        // grant.
        throw Forbidden{"GetRateOn: the caller may not see company " + std::to_string(*action.query.company)};
    }
    auto const rate = rateOn(action.query.company, action.query.from, action.query.to, action.query.on);
    return rate.has_value() ? RateAnswer{.rate = Rate{*rate}} : RateAnswer{};
}

std::optional<::morph::math::Rational> LedgerModel::rateOn(CompanyId company, CurrencyCode from, CurrencyCode target,
                                                           ::morph::time::Timestamp const& date) {
    if (!company.hasValue() || !date.hasValue()) {
        return std::nullopt;
    }
    Lightweight::DataMapper mapper;
    std::optional<db::ExchangeRateRecord> best;
    for (auto const& row :
         mapper.Query<db::ExchangeRateRecord>()
             .Where(::Lightweight::FieldNameOf<&db::ExchangeRateRecord::companyId>, "=", *company)
             .Where(::Lightweight::FieldNameOf<&db::ExchangeRateRecord::fromCurrency>, "=", static_cast<int>(from))
             .Where(::Lightweight::FieldNameOf<&db::ExchangeRateRecord::toCurrency>, "=", static_cast<int>(target))
             .All()) {
        if (row.validFromMs.Value() <= millisOf(date) &&
            (!best.has_value() || row.validFromMs.Value() > best->validFromMs.Value())) {
            best = row;
        }
    }
    if (!best.has_value()) {
        return std::nullopt;
    }
    return Rational{Numerator{best->rateNum.Value()}, Denominator{best->rateDen.Value()}, DecimalPlaces{6}};
}

}  // namespace lims
