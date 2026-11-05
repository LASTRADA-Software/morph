// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "../util/rational.hpp"

/// @file
/// @brief The exact arithmetic of a chart's axes: the padded range of its values and the ticks that label it.
///
/// Specified in `docs/spec/ui/interpreter.md`, "Charts". Everything here is `math::Rational`; a chart converts to
/// `double` only once its range and ticks are known, so no label is derived from a rounded position.

namespace morph::ui {

/// @brief One axis of a chart: its ends, exact.
struct ChartRange {
    /// @brief The lower end.
    math::Rational low;
    /// @brief The upper end.
    math::Rational high;
};

/// @brief The ticks of a value axis: their positions and the places they are shown at.
struct ChartTicks {
    /// @brief The positions, ascending.
    std::vector<math::Rational> values;
    /// @brief The decimal places of the step between them: 0 for a step of 1 or more.
    std::uint32_t places = 0;
};

/// @brief Ten to a power, exactly.
/// @param exponent The power, from -18 to 18.
/// @return `10^exponent`, tagged with as many places as it has after the point.
[[nodiscard]] inline math::Rational chartPowerOfTen(int exponent) {
    if (exponent >= 0) {
        return math::Rational{math::detail::powerOfTen(static_cast<std::uint32_t>(exponent)), math::DecimalPlaces{0}};
    }
    auto const places = static_cast<std::uint32_t>(-exponent);
    return math::Rational{math::Numerator{1}, math::Denominator{math::detail::powerOfTen(places)},
                          math::DecimalPlaces{places}};
}

/// @brief The greatest integer not above a rational.
/// @param value The rational.
/// @return Its floor.
[[nodiscard]] inline std::int64_t chartFloor(math::Rational const& value) {
    std::int64_t const quotient = value.numerator / value.denominator;
    return value.numerator % value.denominator != 0 && value.numerator < 0 ? quotient - 1 : quotient;
}

/// @brief The least integer not below a rational.
/// @param value The rational.
/// @return Its ceiling.
[[nodiscard]] inline std::int64_t chartCeil(math::Rational const& value) {
    std::int64_t const quotient = value.numerator / value.denominator;
    return value.numerator % value.denominator != 0 && value.numerator > 0 ? quotient + 1 : quotient;
}

/// @brief The axis that holds every value, widened by 5 % of their span at each end so the outermost value is
///        inside the plot rather than on its edge.
///
/// A span of zero is widened by one unit in the last of @p places either side; no values at all give 0 to 1.
/// @param values The values the axis must hold.
/// @param places The places a value is shown at, which size the widening of a zero span; at most 18.
/// @return The range, or empty when the arithmetic overflows.
[[nodiscard]] inline std::optional<ChartRange> chartRange(std::span<math::Rational const> values,
                                                          std::uint32_t places) {
    if (values.empty()) {
        return ChartRange{.low = math::Rational{0, math::DecimalPlaces{0}},
                          .high = math::Rational{1, math::DecimalPlaces{0}}};
    }
    auto const [lowest, highest] = std::ranges::minmax(values);
    auto const span = math::checkedSub(highest, lowest);
    if (!span) {
        return std::nullopt;
    }
    static math::Rational const kTwenty{20, math::DecimalPlaces{0}};
    auto const pad = span->numerator == 0 ? std::expected<math::Rational, math::RationalError>{chartPowerOfTen(
                                                -static_cast<int>(std::min(places, math::kMaxDecimalPlaces)))}
                                          : math::checkedDiv(*span, kTwenty);
    if (!pad) {
        return std::nullopt;
    }
    auto const low = math::checkedSub(lowest, *pad);
    auto const high = math::checkedAdd(highest, *pad);
    if (!low || !high) {
        return std::nullopt;
    }
    return ChartRange{.low = *low, .high = *high};
}

/// @brief How many multiples of @p step lie in @p range, ends included.
/// @param range The range.
/// @param step  The step, positive.
/// @return The count, or empty when the arithmetic overflows.
[[nodiscard]] inline std::optional<std::int64_t> chartTickCount(ChartRange const& range, math::Rational const& step) {
    auto const first = math::checkedDiv(range.low, step);
    auto const last = math::checkedDiv(range.high, step);
    if (!first || !last) {
        return std::nullopt;
    }
    return chartFloor(*last) - chartCeil(*first) + 1;
}

/// @brief The largest step of 1, 2 or 5 × 10ᵏ, at most @p power's power of ten, that puts three or more multiples in
///        @p range.
/// @param range The axis.
/// @param power The exponent to start from: the least power of ten not below the range's span.
/// @return The step and its exponent, or empty when none down to 10⁻¹⁸ does or the arithmetic overflows.
[[nodiscard]] inline std::optional<std::pair<math::Rational, int>> chartTickStep(ChartRange const& range, int power) {
    static constexpr int kLowest = -18;
    static constexpr std::array<std::int64_t, 3> kMantissas = {5, 2, 1};
    // Steps in descending order: 10^power, then 5, 2 and 1 times each lower power.
    for (int exponent = power; exponent >= kLowest; --exponent) {
        for (std::int64_t const mantissa : kMantissas) {
            if (exponent == power && mantissa != 1) {
                continue;
            }
            auto const step =
                math::checkedMul(math::Rational{mantissa, math::DecimalPlaces{0}}, chartPowerOfTen(exponent));
            std::optional<std::int64_t> const count =
                step ? chartTickCount(range, *step) : std::optional<std::int64_t>{};
            if (!count) {
                return std::nullopt;
            }
            if (*count >= 3) {
                return std::pair{*step, exponent};
            }
        }
    }
    return std::nullopt;
}

/// @brief The ticks of a value axis: the multiples, inside @p range, of the largest step of 1, 2 or 5 × 10ᵏ that
///        puts three or more there.
///
/// Three to six ticks, except where a step of 5 × 10ᵏ would put two and the next, 2 × 10ᵏ, puts seven or eight.
/// @param range The axis, its high end above its low end.
/// @return The ticks, or empty when the range is too narrow for a step of 10⁻¹⁸ or the arithmetic overflows.
[[nodiscard]] inline std::optional<ChartTicks> chartTicks(ChartRange const& range) {
    static constexpr int kLowest = -18;
    static constexpr int kHighest = 18;
    auto const span = math::checkedSub(range.high, range.low);
    if (!span || span->numerator <= 0) {
        return std::nullopt;
    }
    int power = kLowest;
    while (power < kHighest && chartPowerOfTen(power) < *span) {
        ++power;
    }
    std::optional<std::pair<math::Rational, int>> const step = chartTickStep(range, power);
    if (!step) {
        return std::nullopt;
    }
    auto const first = math::checkedDiv(range.low, step->first);
    auto const last = math::checkedDiv(range.high, step->first);
    if (!first || !last) {
        return std::nullopt;
    }
    ChartTicks ticks{.values = {}, .places = step->second < 0 ? static_cast<std::uint32_t>(-step->second) : 0U};
    for (std::int64_t index = chartCeil(*first); index <= chartFloor(*last); ++index) {
        auto const value = math::checkedMul(math::Rational{index, math::DecimalPlaces{0}}, step->first);
        if (!value) {
            return std::nullopt;
        }
        ticks.values.push_back(*value);
    }
    return ticks;
}

}  // namespace morph::ui
