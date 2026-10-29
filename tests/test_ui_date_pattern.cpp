// SPDX-License-Identifier: Apache-2.0
//
// Date patterns: the one form an application shows and reads dates in, and the ISO forms it always reads too.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/ui/date_pattern.hpp>
#include <morph/util/datetime.hpp>
#include <optional>

namespace ui = morph::ui;
using morph::time::DateTime;

namespace {

DateTime at(int year, unsigned month, unsigned day, int hour, int minute) {
    return DateTime{std::chrono::sys_days{std::chrono::year_month_day{
                        std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}}} +
                    std::chrono::hours{hour} + std::chrono::minutes{minute}};
}

}  // namespace

// Mutation: write `MMM` as the month's number (the text reads `24-10-2026 09:41`).
TEST_CASE("ui date pattern: a pattern writes and reads a date the regulated way, and ISO is always read",
          "[ui][dates]") {
    DateTime const instant = at(2026, 10, 24, 9, 41);
    CHECK(ui::formatDate("dd-MMM-yyyy HH:mm", instant, 0) == "24-Oct-2026 09:41");
    CHECK(ui::parseDate("dd-MMM-yyyy HH:mm", "24-Oct-2026 09:41", 0) == instant);
    CHECK(ui::parseDate("dd-MMM-yyyy HH:mm", "24-oct-2026 09:41", 0) == instant);
    CHECK(ui::parseDate("dd-MMM-yyyy HH:mm", "2026-10-24T09:41:00Z", 0) == instant);
    CHECK(ui::parseDate("dd-MMM-yyyy HH:mm", "2026-10-24 09:41", 0) == instant);
    CHECK(ui::parseDate("dd-MMM-yyyy", "31-Feb-2026", 0) == std::nullopt);
    CHECK(ui::parseDate("dd-MMM-yyyy", "24-Oct-2026", 0) == at(2026, 10, 24, 0, 0));
}

TEST_CASE("ui date pattern: a pattern that cannot read back what it writes is refused", "[ui][dates]") {
    CHECK(ui::checkDatePattern("dd-MMM-yyyy", false).has_value());
    CHECK(ui::checkDatePattern("dd-MMM-yyyy HH:mm", true).has_value());
    CHECK(ui::checkDatePattern("dd MMMyyyy", false).has_value());  // MMM beside a number
    CHECK_FALSE(ui::checkDatePattern("dd-MMMM", false).has_value());
    CHECK_FALSE(ui::checkDatePattern("MMM-yyyy", false).has_value());
    CHECK_FALSE(ui::checkDatePattern("dd-MM-yyyy HH:mm", false).has_value());
    CHECK_FALSE(ui::checkDatePattern("ddMMyyyy", false).has_value());
    CHECK_FALSE(ui::checkDatePattern("dd-MM-yyyy xxx", false).has_value());
    CHECK_FALSE(ui::checkDatePattern("dd-MM-yyyy HH:mm xxx xxx", true).has_value());
    CHECK_FALSE(ui::checkDatePattern("dd-MM-yyyy", true).has_value());
}

// Mutation: read the text in the display zone whatever its `xxx` (the instant is then 09:41 UTC).
TEST_CASE("ui date pattern: xxx writes the display zone's offset and reads the instant from the offset it names",
          "[ui][dates]") {
    DateTime const utc = at(2026, 10, 24, 7, 41);
    DateTime const local = DateTime{utc.value + std::chrono::minutes{120}};
    CHECK(ui::formatDate("dd-MMM-yyyy HH:mm xxx", local, 120) == "24-Oct-2026 09:41 +02:00");
    CHECK(ui::formatDate("dd-MMM-yyyy HH:mm xxx", utc, 0) == "24-Oct-2026 07:41 +00:00");
    CHECK(ui::formatDate("dd-MMM-yyyy HH:mm xxx", DateTime{utc.value - std::chrono::minutes{330}}, -330) ==
          "24-Oct-2026 02:11 -05:30");
    CHECK(ui::parseDate("dd-MMM-yyyy HH:mm xxx", "24-Oct-2026 09:41 +02:00", 0) == utc);
    CHECK(ui::parseDate("dd-MMM-yyyy HH:mm", "24-Oct-2026 09:41", 120) == utc);
}
