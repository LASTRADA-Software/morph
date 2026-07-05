// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file datetime.hpp
/// @brief UTC timestamps for morph actions.
///
/// Two types, mirroring the `Rational`/`Quantity` split:
///
/// - `morph::time::DateTime` — the *value*: a UTC instant with millisecond
///   precision over `std::chrono::sys_time` (adapted from LASTRADA
///   `Toolbox/Chrono.hpp`). Cheap to copy, ordered, duration arithmetic.
/// - `morph::time::Timestamp` — the *field*: an optionally-empty `DateTime`
///   with the same one-kind-of-empty semantics as `Quantity` (a form draft
///   starts blank; required-ness is derived by `morph::forms`).
///
/// @par Wire format
/// A `DateTime` travels as the ISO-8601 string
/// `"YYYY-MM-DDTHH:MM:SS[.mmm]Z"` (UTC only; the fractional part and the
/// trailing `Z` are optional on input, always written on output). Parsing is
/// a strict, hand-rolled routine — no locale, no `std::chrono::parse`
/// dependency — so behaviour is identical across libstdc++, libc++ (WASM),
/// and MSVC. Unlike the `Rational` codec, which clamps hostile input into a
/// valid value, a malformed timestamp has no meaningful clamp: the codec
/// rejects it as a JSON **read error**.
///
/// @par Schema
/// A `Timestamp` field renders as `{"type": ["string","null"],
/// "format": "date-time"}` — the standard JSON-Schema vocabulary form
/// renderers key on (the demo clients render a date-time input for it).

#include <glaze/glaze.hpp>

#include <charconv>
#include <chrono>
#include <compare>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace morph::time {

/// @brief A UTC instant with millisecond precision.
///
/// The default state is the Unix epoch — `DateTime` itself has no empty
/// state; use `Timestamp` for fields that may be blank.
struct DateTime {
    /// @brief The underlying UTC instant (Unix time, leap seconds ignored).
    std::chrono::sys_time<std::chrono::milliseconds> value;

    /// @brief Constructs the Unix epoch (1970-01-01T00:00:00.000Z).
    constexpr DateTime() noexcept = default;

    /// @brief Wraps an existing UTC instant.
    /// @param instant The UTC instant to wrap.
    constexpr DateTime(std::chrono::sys_time<std::chrono::milliseconds> instant) noexcept : value{instant} {}

    /// @brief Composes a UTC instant from calendar/clock components.
    /// @param year        Calendar year.
    /// @param month       Month-of-year in [1, 12].
    /// @param day         Day-of-month in [1, 31].
    /// @param hour        Hours in [0, 23].
    /// @param minute      Minutes in [0, 59].
    /// @param second      Seconds in [0, 59].
    /// @param millisecond Milliseconds in [0, 999].
    constexpr DateTime(std::chrono::year year, std::chrono::month month, std::chrono::day day,
                       std::chrono::hours hour, std::chrono::minutes minute, std::chrono::seconds second,
                       std::chrono::milliseconds millisecond = std::chrono::milliseconds{0}) noexcept
        : value{static_cast<std::chrono::sys_days>(std::chrono::year_month_day{year, month, day}) + hour + minute +
                second + millisecond} {}

    /// @brief The current UTC instant, truncated to milliseconds.
    /// @return The current UTC date-time.
    [[nodiscard]] static DateTime now() noexcept {
        return DateTime{std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now())};
    }

    /// @brief Renders as ISO-8601 UTC: `YYYY-MM-DDTHH:MM:SS.mmmZ`.
    /// @return The formatted instant.
    [[nodiscard]] std::string toIso8601() const {
        auto const wholeDays = std::chrono::floor<std::chrono::days>(value);
        auto const calendar = std::chrono::year_month_day{wholeDays};
        auto const clock = std::chrono::hh_mm_ss{value - wholeDays};
        return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", static_cast<int>(calendar.year()),
                           static_cast<unsigned>(calendar.month()), static_cast<unsigned>(calendar.day()),
                           clock.hours().count(), clock.minutes().count(), clock.seconds().count(),
                           clock.subseconds().count());
    }

    /// @brief Strictly parses ISO-8601 UTC: `YYYY-MM-DDTHH:MM:SS`, optionally
    ///        followed by `.m`/`.mm`/`.mmm` and an optional trailing `Z`.
    ///
    /// The calendar date must exist (2026-02-30 is rejected) and clock
    /// components must be in range; no time zones other than `Z`, no
    /// lowercase separators, no trailing input.
    /// @param text Candidate ISO-8601 string.
    /// @return The parsed instant, or `std::nullopt` when @p text is malformed.
    [[nodiscard]] static std::optional<DateTime> fromIso8601(std::string_view text) noexcept {
        // Callers only pass offsets within the length-checked prefix below.
        auto const number = [&text](std::size_t offset, std::size_t count) noexcept -> std::optional<int> {
            int parsed = 0;
            auto const* first = text.data() + offset;
            auto const [end, errc] = std::from_chars(first, first + count, parsed);
            if (errc != std::errc{} || end != first + count) {
                return std::nullopt;
            }
            return parsed;
        };
        auto const separator = [&text](std::size_t offset, char expected) noexcept {
            return offset < text.size() && text[offset] == expected;
        };

        if (text.size() < 19 || !separator(4, '-') || !separator(7, '-') || !separator(10, 'T') ||
            !separator(13, ':') || !separator(16, ':')) {
            return std::nullopt;
        }
        auto const year = number(0, 4);
        auto const month = number(5, 2);
        auto const day = number(8, 2);
        auto const hour = number(11, 2);
        auto const minute = number(14, 2);
        auto const second = number(17, 2);
        if (!year || !month || !day || !hour || !minute || !second) {
            return std::nullopt;
        }

        std::size_t cursor = 19;
        int milliseconds = 0;
        if (separator(cursor, '.')) {
            ++cursor;
            std::size_t digits = 0;
            int fraction = 0;
            while (digits < 3 && cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
                fraction = (fraction * 10) + (text[cursor] - '0');
                ++cursor;
                ++digits;
            }
            if (digits == 0) {
                return std::nullopt;
            }
            for (std::size_t pad = digits; pad < 3; ++pad) {
                fraction *= 10;
            }
            milliseconds = fraction;
        }
        if (separator(cursor, 'Z')) {
            ++cursor;
        }
        if (cursor != text.size()) {
            return std::nullopt;
        }

        auto const calendar = std::chrono::year_month_day{std::chrono::year{*year},
                                                          std::chrono::month{static_cast<unsigned>(*month)},
                                                          std::chrono::day{static_cast<unsigned>(*day)}};
        if (!calendar.ok() || *hour > 23 || *minute > 59 || *second > 59) {
            return std::nullopt;
        }
        return DateTime{std::chrono::year{*year},
                        std::chrono::month{static_cast<unsigned>(*month)},
                        std::chrono::day{static_cast<unsigned>(*day)},
                        std::chrono::hours{*hour},
                        std::chrono::minutes{*minute},
                        std::chrono::seconds{*second},
                        std::chrono::milliseconds{milliseconds}};
    }

    /// @brief Ordering on the underlying instant.
    /// @param other Instant to compare against.
    /// @return The relative ordering of the two instants.
    [[nodiscard]] constexpr std::strong_ordering operator<=>(const DateTime& other) const noexcept = default;

    /// @brief Shifts this instant by an arbitrary chrono duration.
    /// @param delta Duration to add (may be negative).
    /// @return `*this`.
    template <typename Rep, typename Period>
    constexpr DateTime& operator+=(std::chrono::duration<Rep, Period> delta) noexcept {
        value += std::chrono::duration_cast<std::chrono::milliseconds>(delta);
        return *this;
    }

    /// @brief Shifts this instant backwards by an arbitrary chrono duration.
    /// @param delta Duration to subtract (may be negative).
    /// @return `*this`.
    template <typename Rep, typename Period>
    constexpr DateTime& operator-=(std::chrono::duration<Rep, Period> delta) noexcept {
        value -= std::chrono::duration_cast<std::chrono::milliseconds>(delta);
        return *this;
    }

    /// @brief An instant @p delta after @p lhs.
    /// @param lhs   Starting instant.
    /// @param delta Duration offset.
    /// @return The shifted instant.
    template <typename Rep, typename Period>
    [[nodiscard]] friend constexpr DateTime operator+(DateTime lhs, std::chrono::duration<Rep, Period> delta) noexcept {
        lhs += delta;
        return lhs;
    }

    /// @brief An instant @p delta before @p lhs.
    /// @param lhs   Starting instant.
    /// @param delta Duration offset.
    /// @return The shifted instant.
    template <typename Rep, typename Period>
    [[nodiscard]] friend constexpr DateTime operator-(DateTime lhs, std::chrono::duration<Rep, Period> delta) noexcept {
        lhs -= delta;
        return lhs;
    }

    /// @brief The signed difference between two instants.
    /// @param lhs Minuend.
    /// @param rhs Subtrahend.
    /// @return `lhs - rhs` as a chrono duration.
    [[nodiscard]] friend constexpr auto operator-(const DateTime& lhs, const DateTime& rhs) noexcept {
        return lhs.value - rhs.value;
    }
};

/// @brief An optionally-empty timestamp field.
///
/// The blank state ("not entered / not recorded") lives inside, exactly like
/// `morph::units::Quantity`: action structs never wrap a `Timestamp` in
/// `std::optional`, and a non-optional `Timestamp` member is *required* by
/// the `morph::forms` rules.
struct Timestamp {
    /// @brief The payload; `std::nullopt` means "not entered".
    std::optional<DateTime> value;

    /// @brief Constructs the empty state.
    constexpr Timestamp() noexcept = default;

    /// @brief Engages with @p instant.
    /// @param instant The instant to hold.
    constexpr Timestamp(DateTime instant) noexcept : value{instant} {}

    /// @brief Adopts an optional payload as-is.
    /// @param payload Engaged or empty payload.
    constexpr Timestamp(std::optional<DateTime> payload) noexcept : value{payload} {}

    /// @brief An engaged timestamp holding the current UTC instant.
    /// @return `Timestamp{DateTime::now()}`.
    [[nodiscard]] static Timestamp now() noexcept { return Timestamp{DateTime::now()}; }

    /// @brief Whether an instant has been entered.
    /// @return `true` if the payload is engaged.
    [[nodiscard]] constexpr bool hasValue() const noexcept { return value.has_value(); }

    /// @brief Unchecked access to the engaged instant (UB when empty, exactly
    ///        like `std::optional` — the unchecked contract is the point).
    /// @return The engaged instant.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] constexpr const DateTime& operator*() const noexcept { return *value; }

    /// @brief Ordering/equality on the payload; empty sorts before engaged.
    /// @param other Timestamp to compare against.
    /// @return The ordering of the two payloads.
    [[nodiscard]] constexpr auto operator<=>(const Timestamp& other) const noexcept = default;
};

}  // namespace morph::time

// ---------------------------------------------------------------------------
// Glaze wire codec: ISO-8601 string, strict — malformed input is a read error.
// ---------------------------------------------------------------------------

namespace glz {

/// @brief Reads a `DateTime` from a JSON ISO-8601 string; malformed content
///        sets a syntax error on the context (it is not clampable input).
template <>
struct from<JSON, morph::time::DateTime> {
    template <auto Opts>
    static void op(auto&& value, is_context auto&& ctx, auto&& iter, auto end) {
        std::string text{};
        parse<JSON>::op<Opts>(text, ctx, iter, end);
        if (bool(ctx.error)) [[unlikely]] {
            return;
        }
        if (auto parsed = morph::time::DateTime::fromIso8601(text); parsed.has_value()) {
            value = *parsed;
            return;
        }
        ctx.error = error_code::syntax_error;
    }
};

/// @brief Writes a `DateTime` as its ISO-8601 JSON string.
template <>
struct to<JSON, morph::time::DateTime> {
    template <auto Opts>
    static void op(auto&& value, is_context auto&& ctx, auto&&... args) {
        serialize<JSON>::op<Opts>(value.toIso8601(), ctx, args...);
    }
};

/// @brief On the wire a Timestamp is its nullable ISO-8601 string.
template <>
struct meta<morph::time::Timestamp> {
    static constexpr auto value = &morph::time::Timestamp::value;
    static constexpr std::string_view name = "Timestamp";
};

}  // namespace glz

namespace glz::detail {

/// @brief Schema for `DateTime`: a string carrying the standard JSON-Schema
///        `date-time` format annotation (renderers key on it).
template <>
struct to_json_schema<morph::time::DateTime> {
    template <auto Opts>
    static void op(auto& outSchema, auto& defs) {
        to_json_schema<std::string>::template op<Opts>(outSchema, defs);
        outSchema.format = defined_formats::datetime;
    }
};

}  // namespace glz::detail

// ---------------------------------------------------------------------------
// std::format support.
// ---------------------------------------------------------------------------

/// @brief std::format support for `morph::time::DateTime` (empty spec only):
///        renders the ISO-8601 UTC form.
template <>
struct std::formatter<morph::time::DateTime> {
    constexpr auto parse(std::format_parse_context& ctx) {
        if (ctx.begin() != ctx.end() && *ctx.begin() != '}') {
            throw std::format_error{"morph::time::DateTime accepts only the empty format spec '{}'"};
        }
        return ctx.begin();
    }

    auto format(const morph::time::DateTime& value, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", value.toIso8601());
    }
};
