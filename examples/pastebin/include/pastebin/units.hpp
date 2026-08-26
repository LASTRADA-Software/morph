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
/// >= 1` (zero is not legal), so this alias declares `1` — which means a
/// tenth is a perfectly representable `Reads` value and the type alone cannot
/// carry the whole-number constraint. Enforcing it is therefore an obligation
/// of whatever *accepts* a `Reads` from outside, and there is exactly one such
/// place: `CreatePaste::validate()` (`pastebin/dto/paste_dto.hpp`) rejects a
/// `burnAfterReads` whose `math::Rational` is not an integer, alongside the
/// zero/negative rejection. Every other `Reads` in the rung is *produced* by
/// the model from a whole `std::int64_t` (`paste_model.cpp`'s `readsOf`), so
/// `PasteView`'s two `Reads` members are whole by construction rather than by
/// validation, and `EditPaste` carries no read count at all.
///
/// Nothing downstream re-checks this, and that is deliberate: `countOf`
/// (`paste_model.cpp`) documents the whole-number premise it relies on and
/// names this constraint as what supplies it. A fractional budget reaching
/// `countOf` would be floored into a *different* budget with nothing
/// reporting it, which is why the check lives at the boundary where the value
/// can still be refused instead of rewritten.
using Reads = ::morph::units::Quantity<Unit::count, 1>;

}  // namespace pastebin
