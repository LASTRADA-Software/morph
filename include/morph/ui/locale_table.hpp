// SPDX-License-Identifier: Apache-2.0
//
// The facts a locale writes numbers, money and dates with. See docs/spec/ui/locale_table.md.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "../render/locale_format.hpp"

namespace morph::ui {

/// @brief Where a locale puts a currency symbol relative to the amount.
enum class SymbolPlacement : std::uint8_t {
    Before,  ///< `€13,196.40`.
    After,   ///< `13.196,40 €`.
};

/// @brief The symbol a locale writes for one currency.
struct CurrencySymbol {
    /// @brief The ISO 4217 code, such as `EUR`.
    std::string_view code;
    /// @brief The symbol, such as `€`.
    std::string_view symbol;
};

/// @brief How a locale writes money.
struct CurrencyFacts {
    /// @brief Where the symbol stands.
    SymbolPlacement placement = SymbolPlacement::After;
    /// @brief The symbols the locale writes. Empty: every currency is written by its code. Must outlive the facts.
    std::span<CurrencySymbol const> symbols;
};

/// @brief Everything a locale writes numbers, money and dates with.
struct LocaleFacts {
    /// @brief The BCP 47 tag the row answers to.
    std::string_view tag;
    /// @brief The separators, sign and digits.
    render::NumericLocale numeric;
    /// @brief The currency symbols and their placement.
    CurrencyFacts currency;
    /// @brief The default date pattern; empty keeps ISO.
    std::string_view datePattern;
    /// @brief The default date-and-time pattern; empty keeps ISO.
    std::string_view dateTimePattern;
};

/// @brief Whether `money` writes a currency by its symbol or by its code.
enum class MoneyForm : std::uint8_t {
    Symbol,  ///< The locale's symbol where it lists one.
    Code,    ///< The ISO code after the amount.
};

/// @brief The symbols English writes.
inline constexpr std::array<CurrencySymbol, 5> kEnglishSymbols{{
    {.code = "EUR", .symbol = "€"},
    {.code = "USD", .symbol = "$"},
    {.code = "GBP", .symbol = "£"},
    {.code = "JPY", .symbol = "¥"},
    {.code = "CHF", .symbol = "CHF"},
}};

/// @brief The symbols German writes.
inline constexpr std::array<CurrencySymbol, 5> kGermanSymbols{{
    {.code = "EUR", .symbol = "€"},
    {.code = "USD", .symbol = "$"},
    {.code = "GBP", .symbol = "£"},
    {.code = "JPY", .symbol = "¥"},
    {.code = "CHF", .symbol = "CHF"},
}};

/// @brief The rows. Static storage: `NumericLocale` and the symbol spans hold views.
inline constexpr std::array<LocaleFacts, 2> kLocaleTable{{
    {.tag = "en",
     .numeric = {.decimalSeparator = ".", .groupSeparator = ","},
     .currency = {.placement = SymbolPlacement::Before, .symbols = kEnglishSymbols},
     .datePattern = {},
     .dateTimePattern = {}},
    {.tag = "de",
     .numeric = {.decimalSeparator = ",", .groupSeparator = "."},
     .currency = {.placement = SymbolPlacement::After, .symbols = kGermanSymbols},
     .datePattern = "dd.MM.yyyy",
     .dateTimePattern = "dd.MM.yyyy HH:mm"},
}};

/// @brief The rows of the table.
/// @return They, in a fixed order (`en`, `de`).
[[nodiscard]] inline std::span<LocaleFacts const> localeTable() noexcept { return kLocaleTable; }

/// @brief The facts of a locale: the exact tag, then each shorter one (`de-AT`, then `de`).
/// @param tag The BCP 47 tag.
/// @return The row, or null when no prefix of @p tag has one.
[[nodiscard]] inline LocaleFacts const* findLocaleFacts(std::string_view tag) noexcept {
    while (!tag.empty()) {
        for (LocaleFacts const& row : kLocaleTable) {
            if (row.tag == tag) {
                return &row;
            }
        }
        std::size_t const cut = tag.rfind('-');
        if (cut == std::string_view::npos) {
            break;
        }
        tag = tag.substr(0, cut);
    }
    return nullptr;
}

/// @brief The facts of a locale, `en`'s where it has none.
/// @param tag The BCP 47 tag.
/// @return The row `findLocaleFacts` finds, else `en`'s.
[[nodiscard]] inline LocaleFacts const& localeFacts(std::string_view tag) noexcept {
    LocaleFacts const* const found = findLocaleFacts(tag);
    return found != nullptr ? *found : kLocaleTable.front();
}

/// @brief Writes an amount with its currency.
/// @param number   The amount as `formatCanonicalNumber` wrote it, sign included.
/// @param currency The ISO 4217 code.
/// @param facts    The locale's currency facts.
/// @param numeric  The locale's separators and sign; its sign is the one @p number starts with.
/// @param form     The symbol or the code.
/// @return `number code` for the code form, with no symbol facts or for a currency @p facts lists no symbol for;
///         otherwise the symbol placed by `facts.placement`, the sign first, a no-break space between a symbol that
///         ends in an ASCII letter and the digits (and always between the digits and a trailing symbol).
[[nodiscard]] inline std::string placeCurrency(std::string_view number, std::string_view currency,
                                               CurrencyFacts const& facts, render::NumericLocale const& numeric,
                                               MoneyForm form) {
    auto const listed = [&]() -> CurrencySymbol const* {
        for (CurrencySymbol const& entry : facts.symbols) {
            if (entry.code == currency) {
                return &entry;
            }
        }
        return nullptr;
    }();
    if (form == MoneyForm::Code || listed == nullptr) {
        return std::string{number} + " " + std::string{currency};
    }
    std::string_view const sign = numeric.negativeSign.empty() ? std::string_view{"-"} : numeric.negativeSign;
    std::string_view digits = number;
    std::string_view shownSign;
    if (digits.starts_with(sign)) {
        shownSign = sign;
        digits.remove_prefix(sign.size());
    }
    std::string out{shownSign};
    if (facts.placement == SymbolPlacement::Before) {
        out += listed->symbol;
        char const last = listed->symbol.back();
        if ((last >= 'A' && last <= 'Z') || (last >= 'a' && last <= 'z')) {
            out += " ";
        }
        out += digits;
    } else {
        out += digits;
        out += " ";
        out += listed->symbol;
    }
    return out;
}

/// @brief The separators with grouping off: how a number is shown or typed outside `number`, `quantity` and `money`.
/// @param numeric The locale's separators.
/// @return @p numeric with no group separator.
[[nodiscard]] constexpr render::NumericLocale withoutGrouping(render::NumericLocale numeric) noexcept {
    numeric.groupSeparator = {};
    return numeric;
}

}  // namespace morph::ui
