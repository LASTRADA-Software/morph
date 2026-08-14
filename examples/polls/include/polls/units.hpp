// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/util/quantity.hpp>

/// @file
/// Polls' one-unit system: a dimensionless count, reused for every
/// vote tally in a poll (yes/no/ifNeedBe counts per option).
/// Modeled on `bookmarks/units.hpp` — see that file for the full
/// UnitTraits/consteval-algebra contract this mirrors; this rung needs no
/// unit algebra either, for the same reason.

namespace polls {

/// @brief Units polls works in.
enum class Unit {
    count,  ///< dimensionless whole-number count
};

}  // namespace polls

/// @brief Static unit metadata: schema id, display text, default decimals.
template <>
struct morph::units::UnitTraits<polls::Unit> {
    static constexpr morph::units::UnitMeta meta(polls::Unit unit) noexcept {
        switch (unit) {
            case polls::Unit::count:
                return {"count", "", 1};
            default:
                return {"?", "?", 1};
        }
    }
};

namespace polls {

/// @brief A whole-number count (vote tallies in poll results).
///
/// `morph::units::Quantity<U, DeclaredDecimals>` requires `DeclaredDecimals
/// >= 1` (zero is not legal); every value that ever appears is a whole
/// number by construction. See `bookmarks::Count`'s identical pattern.
using Count = ::morph::units::Quantity<Unit::count, 1>;

}  // namespace polls
