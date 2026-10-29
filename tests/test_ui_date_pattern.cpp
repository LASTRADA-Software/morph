// SPDX-License-Identifier: Apache-2.0
//
// Date patterns: the one form an application shows and reads dates in, and the ISO forms it always reads too.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/ui/date_pattern.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string_view>

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

namespace {

DateTime at(int year, unsigned month, unsigned day, int hour, int minute, int second) {
    return DateTime{at(year, month, day, hour, minute).value + std::chrono::seconds{second}};
}

}  // namespace

TEST_CASE("ui date pattern: every numeric token writes zero-padded and reads back", "[ui][dates]") {
    DateTime const instant = at(2026, 3, 5, 7, 8, 9);
    CHECK(ui::formatDate("dd/MM/yyyy HH:mm:ss", instant, 0) == "05/03/2026 07:08:09");
    CHECK(ui::parseDate("dd/MM/yyyy HH:mm:ss", "05/03/2026 07:08:09", 0) == instant);
    CHECK(ui::formatDate("yyyy.MM.dd, HH:mm", instant, 0) == "2026.03.05, 07:08");
    // A year before the epoch's calendar zero keeps its sign ahead of the padding.
    CHECK(ui::formatDate("yyyy-MM-dd", at(-5, 1, 2, 0, 0), 0) == "-0005-01-02");
}

TEST_CASE("ui date pattern: a pattern that is not one writes nothing", "[ui][dates]") {
    DateTime const instant = at(2026, 3, 5, 7, 8);
    CHECK(ui::formatDate("dd-MM-yy", instant, 0).empty());
    CHECK(ui::formatDate("dd-qq-yyyy", instant, 0).empty());
    CHECK(ui::formatDate("dd-MM-yyyy hh", instant, 0).empty());
    CHECK(ui::formatDate("d-MM-yyyy", instant, 0).empty());
    CHECK(ui::formatDate("dd-MM-yyyy xx", instant, 0).empty());
    CHECK(ui::formatDate("dd-MM-yyyy HH:mm", instant, 0) == "05-03-2026 07:08");
}

TEST_CASE("ui date pattern: text that does not match the pattern names no instant", "[ui][dates]") {
    std::string_view const pattern = "dd-MMM-yyyy HH:mm:ss";
    CHECK(ui::parseDate(pattern, "05-Mar-2026 07:08:09", 0) == at(2026, 3, 5, 7, 8, 9));
    CHECK(ui::parseDate(pattern, "05/Mar-2026 07:08:09", 0) == std::nullopt);   // a literal
    CHECK(ui::parseDate(pattern, "05-Mar-2026 07:08:09x", 0) == std::nullopt);  // text after the pattern
    CHECK(ui::parseDate(pattern, "05-Mar-2026 07:08:0", 0) == std::nullopt);    // too few digits
    CHECK(ui::parseDate(pattern, "05-Mar-2026 07:0a:09", 0) == std::nullopt);   // a letter for a digit
    CHECK(ui::parseDate(pattern, "05-Mar-2026 07:08:", 0) == std::nullopt);     // the text ends at a token
    CHECK(ui::parseDate(pattern, "05-Mar-2026 07:08", 0) == std::nullopt);      // the text ends at a literal
    CHECK(ui::parseDate(pattern, "05-Mzr-2026 07:08:09", 0) == std::nullopt);   // no month's name
    CHECK(ui::parseDate(pattern, "05-Ma", 0) == std::nullopt);                  // too short for a name
    CHECK(ui::parseDate(pattern, "05-Maz-2026 07:08:09", 0) == std::nullopt);   // two letters of a name
    CHECK(ui::parseDate(pattern, "05-Mar-2026 24:08:09", 0) == std::nullopt);
    CHECK(ui::parseDate(pattern, "05-Mar-2026 07:60:09", 0) == std::nullopt);
    CHECK(ui::parseDate(pattern, "05-Mar-2026 07:08:60", 0) == std::nullopt);
    CHECK(ui::parseDate("dd-MM-yyyy", "05-13-2026", 0) == std::nullopt);
}

TEST_CASE("ui date pattern: xxx reads only a signed hours:minutes offset in range", "[ui][dates]") {
    std::string_view const pattern = "dd-MMM-yyyy HH:mm xxx";
    DateTime const utc = at(2026, 10, 24, 15, 11);
    CHECK(ui::parseDate(pattern, "24-Oct-2026 09:41 -05:30", 0) == utc);
    CHECK(ui::parseDate(pattern, "24-Oct-2026 09:41 ", 0) == std::nullopt);
    CHECK(ui::parseDate(pattern, "24-Oct-2026 09:41 05:30", 0) == std::nullopt);
    CHECK(ui::parseDate(pattern, "24-Oct-2026 09:41 +05", 0) == std::nullopt);
    CHECK(ui::parseDate(pattern, "24-Oct-2026 09:41 +0x:30", 0) == std::nullopt);
    CHECK(ui::parseDate(pattern, "24-Oct-2026 09:41 +05-30", 0) == std::nullopt);
    CHECK(ui::parseDate(pattern, "24-Oct-2026 09:41 +0530", 0) == std::nullopt);
    CHECK(ui::parseDate(pattern, "24-Oct-2026 09:41 +05:3", 0) == std::nullopt);
    CHECK(ui::parseDate(pattern, "24-Oct-2026 09:41 +24:00", 0) == std::nullopt);
    CHECK(ui::parseDate(pattern, "24-Oct-2026 09:41 +05:60", 0) == std::nullopt);
}

TEST_CASE("ui date pattern: with no pattern, only the ISO forms are read", "[ui][dates]") {
    DateTime const instant = at(2026, 10, 24, 9, 41);
    CHECK(ui::parseDate("", "2026-10-24T09:41", 0) == instant);
    CHECK(ui::parseDate("", "2026-10-24 09:41", 60) == at(2026, 10, 24, 8, 41));
    CHECK(ui::parseDate("", "2026-10-24", 0) == at(2026, 10, 24, 0, 0));
    CHECK(ui::parseDate("", "24-Oct-2026", 0) == std::nullopt);
    // A pattern that is not one is skipped rather than refusing the ISO forms.
    CHECK(ui::parseDate("dd-qq-yyyy", "2026-10-24", 0) == at(2026, 10, 24, 0, 0));
}

TEST_CASE("ui date pattern: a date-and-time pattern holds its clock tokens the right number of times", "[ui][dates]") {
    CHECK(ui::checkDatePattern("dd-MM-yyyy HH:mm:ss xxx", true).has_value());
    CHECK_FALSE(ui::checkDatePattern("dd-MM-yyyy mm", true).has_value());
    CHECK_FALSE(ui::checkDatePattern("dd-MM-yyyy HH", true).has_value());
    CHECK_FALSE(ui::checkDatePattern("dd-MM-yyyy HH:mm:ss:ss", true).has_value());
    CHECK_FALSE(ui::checkDatePattern("dd-MM-yyyy-yyyy", false).has_value());
    CHECK_FALSE(ui::checkDatePattern("dd-dd-MM-yyyy", false).has_value());
    CHECK_FALSE(ui::checkDatePattern("dd-MM-MMM-yyyy", false).has_value());
    CHECK_FALSE(ui::checkDatePattern("dd-MM-yyyy ss", false).has_value());
    CHECK_FALSE(ui::checkDatePattern("MMMxxx dd yyyy HH:mm", true).has_value());  // a name beside an offset
    CHECK_FALSE(ui::checkDatePattern("dd yyyy HH:mm xxxMMM", true).has_value());  // an offset beside a name
    CHECK(ui::checkDatePattern("ddMMM yyyy", false).has_value());                 // a number before a name
}
