// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/util/quantity.hpp>

#include <cstdint>

namespace ledger {

/// @brief The unit system for every money value in this rung
///        (`IMPLEMENTATION.md` rule 3's "each rung defines its unit system
///        once"). Four currencies: two at dp=2 (USD, EUR) and two at dp=0
///        (JPY, KRW), deliberately chosen to exercise both --
///        `DecimalPlaces` has no floor of 1 (design spec §2's correction to
///        the round-5 draft), so JPY/KRW are natively representable, no
///        app-side workaround needed.
enum class Currency : std::uint8_t { USD, EUR, JPY, KRW };

/// @brief Alias onto `morph::units::UnitTraits`, so call sites can spell the
///        customization point as `ledger::UnitTraits<Currency>` without an
///        explicit `morph::units::` qualifier.
template <typename E>
using UnitTraits = morph::units::UnitTraits<E>;

}  // namespace ledger

template <>
struct morph::units::UnitTraits<ledger::Currency> {
    /// @brief Static metadata for one `Currency` enumerator.
    /// @param c The currency to describe.
    /// @return The `UnitMeta` (id, display text, default decimal places) for @p c.
    static constexpr morph::units::UnitMeta meta(ledger::Currency c) {
        switch (c) {
            case ledger::Currency::USD:
                return {.id = "USD", .display = "US Dollar", .defaultDecimals = 2};
            case ledger::Currency::EUR:
                return {.id = "EUR", .display = "Euro", .defaultDecimals = 2};
            case ledger::Currency::JPY:
                return {.id = "JPY", .display = "Japanese Yen", .defaultDecimals = 0};
            case ledger::Currency::KRW:
                return {.id = "KRW", .display = "Korean Won", .defaultDecimals = 0};
            default:
                return {.id = "USD", .display = "US Dollar", .defaultDecimals = 2};
        }
    }
};

namespace ledger {

/// @brief DTO-level money type for one specific currency enumerator.
///
/// `Quantity`'s unit tag (`auto U`) is a compile-time enumerator value, not a
/// runtime enum -- there is no single type generic over "whichever `Currency`
/// this account happens to hold". `Money<C>` fixes @p C at compile time and
/// keeps the framework's default declared precision (`UnitTraits<Currency>
/// ::meta(C).defaultDecimals`): 2 for `USD`/`EUR`, 0 for `JPY`/`KRW`. DTO
/// sites that only need a precision *hint* ahead of knowing the account's
/// real currency use `Quantity<Currency::USD, 2>` directly as that hint's
/// shape (2 is the majority default); the model layer re-derives and
/// re-quantizes to the account's actual currency's precision from
/// `UnitTraits<Currency>::meta`, not from this DTO-level hint.
template <Currency C>
using Money = ::morph::units::Quantity<C>;

}  // namespace ledger
