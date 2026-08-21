// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/util/datetime.hpp>

#include <chrono>
#include <utility>

/// @file
/// Local-calendar to UTC-instant conversion for report period bounds
/// (design spec §9's "local-time month boundary vs. UTC storage"
/// requirement).

namespace ledger {

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
    const auto localEnd = static_cast<std::chrono::sys_days>((localMonth + std::chrono::months{1}) /
                                                             std::chrono::day{1});

    // local = utc + offset, so utc = local - offset.
    const std::chrono::minutes offset{timezoneOffsetMinutes};
    return {morph::time::DateTime{std::chrono::time_point_cast<std::chrono::milliseconds>(localStart) - offset},
            morph::time::DateTime{std::chrono::time_point_cast<std::chrono::milliseconds>(localEnd) - offset}};
}

}  // namespace ledger
