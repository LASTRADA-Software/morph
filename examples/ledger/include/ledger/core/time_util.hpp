// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <morph/util/datetime.hpp>
#include <string>
#include <system_error>
#include <utility>

#include "ledger/core/errors.hpp"

/// @file
/// Calendar handling shared across this rung: local-calendar to UTC-instant
/// conversion for report period bounds (design spec §9's "local-time month
/// boundary vs. UTC storage" requirement), plus the `"YYYY-MM"` month string's
/// own validator and its UTC millisecond range.
///
/// The two month helpers live here rather than beside their first caller
/// because they now have more than one. `isValidYearMonth` was in
/// `ledger/dto/budget_dto.hpp` and `monthRangeMs` in an anonymous namespace in
/// `src/models/budget_model.cpp`; `ListTransactions` takes the same
/// `"YYYY-MM"` bound as `GetBudgetReport` (morph#428) and must parse it the
/// same way, and a second copy of a date parser is a second thing to get
/// wrong.

namespace ledger {

namespace detail {

/// @brief Checks that `month` is a well-formed `"YYYY-MM"` string: exactly
///        7 characters, digits in the year/month positions, a literal `-`
///        at index 4, and a month value in `[1, 12]`. Used by
///        `SetBudgetLimit::validate()`/`GetBudgetReport::validate()`/
///        `ListTransactions::validate()` to reject malformed input at the
///        DTO boundary, before it ever reaches `monthRangeMs`'s date
///        arithmetic (a malformed month like `"2026-13"` would otherwise
///        produce a garbage range rather than a refusal).
/// @param month The candidate month string to check.
/// @return `true` if `month` is a syntactically valid `"YYYY-MM"` string
///         naming a real calendar month (1-12).
[[nodiscard]] inline bool isValidYearMonth(const std::string& month) noexcept {
    if (month.size() != 7 || month[4] != '-') {
        return false;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(month[i]))) {
            return false;
        }
    }
    if (!std::isdigit(static_cast<unsigned char>(month[5])) || !std::isdigit(static_cast<unsigned char>(month[6]))) {
        return false;
    }
    const int monthNum = (month[5] - '0') * 10 + (month[6] - '0');
    return monthNum >= 1 && monthNum <= 12;
}

}  // namespace detail

/// @brief Parses a `"YYYY-MM"` month string into a half-open UTC
///        `[start, end)` millisecond range, matching
///        `TransactionJournalRecord::date`'s stored epoch-millis form.
///
///        A plain UTC month range -- not the local-timezone month-boundary
///        machinery `localMonthToUtcRange` below provides for reports; the
///        callers of this one (`GetBudgetReport`, `ListTransactions`) take a
///        month with no offset alongside it, so there is no local calendar to
///        convert from. Callers reject a malformed month -- wrong length,
///        non-digit characters, or a month number outside `[1, 12]` -- through
///        `detail::isValidYearMonth` in their own `validate()` before this
///        ever runs. This function still double-checks both the `from_chars`
///        parse status and `year_month_day::ok()` and throws `ValidationError`
///        rather than silently returning a garbage range, as defense in depth
///        against a caller that bypasses `validate()`.
/// @param month The `"YYYY-MM"` string to parse. Must already have passed
///        `detail::isValidYearMonth`.
/// @return The `[start, end)` epoch-millisecond range for that UTC month.
/// @throws ValidationError if `month` cannot be parsed as digits or does
///         not name a valid calendar month.
[[nodiscard]] inline std::pair<std::int64_t, std::int64_t> monthRangeMs(const std::string& month) {
    int year = 0;
    int monthNum = 0;
    const auto yearResult = std::from_chars(month.data(), month.data() + 4, year);
    const auto monthResult = std::from_chars(month.data() + 5, month.data() + 7, monthNum);
    if (yearResult.ec != std::errc{} || monthResult.ec != std::errc{}) {
        throw ValidationError{"monthRangeMs: \"" + month + "\" is not a well-formed YYYY-MM month"};
    }

    const auto startDate = std::chrono::year_month_day{
        std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(monthNum)}, std::chrono::day{1}};
    if (!startDate.ok()) {
        throw ValidationError{"monthRangeMs: \"" + month + "\" is not a valid calendar month"};
    }
    const auto startSysDays = static_cast<std::chrono::sys_days>(startDate);
    const auto endSysDays =
        static_cast<std::chrono::sys_days>(startDate.year() / startDate.month() / std::chrono::last) +
        std::chrono::days{1};

    const auto startMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(startSysDays.time_since_epoch()).count();
    const auto endMs = std::chrono::duration_cast<std::chrono::milliseconds>(endSysDays.time_since_epoch()).count();
    return {startMs, endMs};
}

/// @brief Converts a local calendar month to the half-open `[start, end)`
///        range of UTC instants covering it, for a fixed offset from UTC.
///
/// Transaction dates are stored as UTC instants, but "January's statement"
/// is a claim about the *client's* calendar: at UTC-5, a transaction booked
/// 2026-01-31T23:30 local is already 2026-02-01T04:30 UTC, and belongs in
/// January regardless. Converting the month's own boundaries once, here, and
/// then comparing stored UTC instants against them keeps that correct
/// without ever comparing local-time strings against UTC-stored rows
/// row-by-row -- which is the shape that silently misfiles every transaction
/// in the boundary hours.
///
/// The range is half-open deliberately: an instant exactly at `end` is the
/// first moment of the *next* local month, so `>= start && < end` is the
/// membership test, and consecutive months tile without overlap or gap.
///
/// A fixed offset, not a named zone: the client sends the offset it was in,
/// stored with the job. That is exact for a report over a period with no DST
/// transition inside it, and this rung does not claim more (a zone-aware
/// version would need the transition history, not a single number).
///
/// @param year Calendar year of the local month, e.g. 2026.
/// @param month Calendar month of the local month, 1-12.
/// @param timezoneOffsetMinutes The client's offset from UTC in minutes,
///        positive east of Greenwich -- e.g. -300 for UTC-5, +330 for
///        UTC+5:30.
/// @return `{start, end}`: the first UTC instant inside the local month, and
///         the first UTC instant after it.
[[nodiscard]] constexpr std::pair<morph::time::DateTime, morph::time::DateTime> localMonthToUtcRange(
    int year, unsigned month, int timezoneOffsetMinutes) noexcept {
    const std::chrono::year_month localMonth{std::chrono::year{year}, std::chrono::month{month}};

    // The month's own boundaries as local wall-clock midnight, held for the
    // moment in a UTC-typed time_point -- they become real UTC instants only
    // after the offset is removed below.
    const auto localStart = static_cast<std::chrono::sys_days>(localMonth / std::chrono::day{1});
    const auto localEnd =
        static_cast<std::chrono::sys_days>((localMonth + std::chrono::months{1}) / std::chrono::day{1});

    // local = utc + offset, so utc = local - offset.
    const std::chrono::minutes offset{timezoneOffsetMinutes};
    return {morph::time::DateTime{std::chrono::time_point_cast<std::chrono::milliseconds>(localStart) - offset},
            morph::time::DateTime{std::chrono::time_point_cast<std::chrono::milliseconds>(localEnd) - offset}};
}

}  // namespace ledger
