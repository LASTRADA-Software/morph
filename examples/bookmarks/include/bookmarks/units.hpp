// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/util/quantity.hpp>

/// @file
/// Bookmarks' one-unit system: a dimensionless count, reused for every
/// whole-number quantity this rung's DTOs carry (a tag's bookmark count, a
/// bulk edit's affected-row count, an import's imported/skipped counts).
/// Modeled on `pastebin/units.hpp` — see that file for the full
/// UnitTraits/consteval-algebra contract this mirrors; this rung needs no
/// unit algebra either, for the same reason.

namespace bookmarks {

/// @brief Units bookmarks works in.
enum class Unit {
    count,  ///< dimensionless whole-number count
};

}  // namespace bookmarks

/// @brief Static unit metadata: schema id, display text, default decimals.
template <>
struct morph::units::UnitTraits<bookmarks::Unit> {
    static constexpr morph::units::UnitMeta meta(bookmarks::Unit unit) noexcept {
        switch (unit) {
            case bookmarks::Unit::count:
                return {"count", "", 1};
            default:
                return {"?", "?", 1};
        }
    }
};

namespace bookmarks {

/// @brief A whole-number count (bookmark counts, affected-row counts,
///        import result counts).
///
/// `morph::units::Quantity<U, DeclaredDecimals>` requires `DeclaredDecimals
/// >= 1` (zero is not legal); every value that ever appears is a whole
/// number by construction. See `pastebin::Reads`'s identical doc comment.
using Count = ::morph::units::Quantity<Unit::count, 1>;

}  // namespace bookmarks
