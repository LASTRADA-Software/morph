// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <format>
#include <morph/util/datetime.hpp>
#include <string>

#include "lims/org/core/types.hpp"

/// @file
/// The calendar facts the books need: a UTC instant's month, the label it is shown under, and the fiscal year it
/// falls in for a company whose fiscal year starts in a given month.

namespace lims {

/// @brief A calendar month: a period of the books.
struct CalendarMonth {
    /// @brief The year.
    std::int32_t year = 1970;
    /// @brief The month.
    Month month = Month::January;

    /// @brief Chronological order.
    /// @param other The other month.
    /// @return The ordering.
    auto operator<=>(CalendarMonth const& other) const = default;
};

/// @brief Milliseconds since the epoch of an engaged @p at; 0 for an empty one.
/// @param at The instant.
/// @return The milliseconds.
[[nodiscard]] inline std::int64_t millisOf(::morph::time::Timestamp const& at) {
    return at.hasValue() ? (*at).value.time_since_epoch().count() : 0;
}

/// @brief The UTC calendar day of @p at.
/// @param at The instant.
/// @return Its year, month and day.
[[nodiscard]] inline std::chrono::year_month_day calendarDayOf(::morph::time::Timestamp const& at) {
    auto const instant = std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{millisOf(at)}};
    return std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(instant)};
}

/// @brief The month @p at falls in, in UTC.
/// @param at The instant.
/// @return The month.
[[nodiscard]] inline CalendarMonth calendarMonthOf(::morph::time::Timestamp const& at) {
    auto const day = calendarDayOf(at);
    return CalendarMonth{.year = static_cast<std::int32_t>(day.year()),
                         .month = static_cast<Month>(static_cast<unsigned>(day.month()) - 1U)};
}

/// @brief The day of the month @p at falls on, in UTC.
/// @param at The instant.
/// @return 1 to 31.
[[nodiscard]] inline std::uint32_t dayOfMonth(::morph::time::Timestamp const& at) {
    return static_cast<unsigned>(calendarDayOf(at).day());
}

/// @brief The first instant of @p month, in milliseconds since the epoch.
/// @param month The month.
/// @return Midnight UTC on its first day.
[[nodiscard]] inline std::int64_t monthStartMillis(CalendarMonth month) {
    auto const first =
        std::chrono::year_month_day{std::chrono::year{month.year},
                                    std::chrono::month{static_cast<unsigned>(month.month) + 1U}, std::chrono::day{1}};
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::sys_days{first}.time_since_epoch())
        .count();
}

/// @brief The label a period is shown under.
/// @param month The month.
/// @return `YYYY-MM`.
[[nodiscard]] inline std::string monthLabel(CalendarMonth month) {
    return std::format("{:04}-{:02}", month.year, static_cast<unsigned>(month.month) + 1U);
}

/// @brief The fiscal year @p at falls in, named by the calendar year it begins in.
/// @param at The instant.
/// @param fiscalYearStart The month the company's fiscal year starts.
/// @return The year: with an April start, February 2027 is in fiscal year 2026.
[[nodiscard]] inline std::int32_t fiscalYearOf(::morph::time::Timestamp const& at, Month fiscalYearStart) {
    auto const month = calendarMonthOf(at);
    return month.month < fiscalYearStart ? month.year - 1 : month.year;
}

}  // namespace lims
