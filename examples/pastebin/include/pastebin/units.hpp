// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/util/quantity.hpp>

/// @file
/// Pastebin's one-unit system: a dimensionless read count. Modeled on
/// examples/forms/lab_units.hpp's shape — see that file for the full
/// UnitTraits/consteval-algebra contract this mirrors. Rung 1 needs no unit
/// algebra (no products/quotients, no within-dimension conversions), so this
/// file skips `operator*`/`operator/` and `UnitTraits::relations` — both are
/// optional per `morph::units::UnitEnum`/`HasUnitRelations` and only apply
/// once a second unit exists to combine or convert with.

namespace pastebin {

/// @brief Units pastebin works in.
enum class Unit {
    count,  ///< dimensionless read count
};

}  // namespace pastebin

/// @brief Static unit metadata: schema id, display text, default decimals.
template <>
struct morph::units::UnitTraits<pastebin::Unit> {
    static constexpr morph::units::UnitMeta meta(pastebin::Unit unit) noexcept {
        switch (unit) {
            case pastebin::Unit::count:
                return {"count", "", 1};
            default:
                return {"?", "?", 1};
        }
    }
};

namespace pastebin {

/// @brief A whole-number read count (burn-after-N-reads, read_count).
///
/// `morph::units::Quantity<U, DeclaredDecimals>` requires `DeclaredDecimals
/// >= 1` (zero is not legal), so this alias declares `1` even though every
/// value that ever appears is a whole number by construction — the DTOs that
/// use `Reads` (Task 3) enforce the whole-number constraint explicitly in
/// their `validate()`; the type alone cannot.
using Reads = ::morph::units::Quantity<Unit::count, 1>;

}  // namespace pastebin
