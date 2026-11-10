// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cctype>
#include <glaze/glaze.hpp>
#include <map>
#include <morph/ui/author.hpp>
#include <string>
#include <string_view>

#include "lims/billing/core/types.hpp"
#include "lims/lab/core/types.hpp"
#include "lims/ledger/core/types.hpp"
#include "lims/org/dto/role_dto.hpp"

/// @file
/// What every lims screen formats the same way: the width class, the page head, money, balances, days, quantities,
/// states in words and refusals. No screen formats a number, a date or an enumerator by hand.

namespace lims::screens::kit {

namespace ui = morph::ui::author;

/// @brief The width class the screen lays out for.
/// @param screen The screen.
/// @return The width class.
[[nodiscard]] inline ui::AnyHandle width(ui::Scope& screen) { return ui::widthClass(screen); }

/// @brief The page head every screen owns: the module's name over the title.
/// @param screen The screen.
/// @param module The module's id: `lab`, `billing`, `ledger`, `inventory`, `org` or `shell`.
/// @param title The title.
void pageHead(ui::Scope& screen, std::string_view module, ui::AnyHandle const& title);

/// @brief @p amount in @p currency, in the session's locale, at the currency's minor digits.
/// @param amount The amount.
/// @param currency The currency's code.
/// @return The text, as the `money` function answers it: `13,196.40 EUR`, `1,234,567 JPY`.
[[nodiscard]] inline ui::Handle<std::string> money(ui::AnyHandle const& amount, ui::AnyHandle const& currency) {
    return ui::call<std::string>("money", {amount, currency});
}

/// @brief A balance: a debit as money, a credit (a negative amount) as its magnitude followed by the credit mark.
/// @param amount The balance, debit positive.
/// @param currency The currency's code.
/// @return The text: `13,196.40 EUR` or `13,196.40 EUR Cr`.
[[nodiscard]] ui::Handle<std::string> balance(ui::AnyHandle const& amount, ui::AnyHandle const& currency);

/// @brief The day of @p instant, as the screens show a date.
/// @param instant The instant.
/// @return The text, in the session's locale.
[[nodiscard]] inline ui::Handle<std::string> day(ui::AnyHandle const& instant) {
    return ui::call<std::string>("date", {instant, ui::AnyHandle{"medium"}});
}

/// @brief @p quantity followed by @p unit's symbol, in the session's locale.
/// @param quantity The quantity.
/// @param unit The unit's symbol.
/// @return The text: `0.250 L`.
[[nodiscard]] inline ui::Handle<std::string> measure(ui::AnyHandle const& quantity, ui::AnyHandle const& unit) {
    return ui::call<std::string>("quantity", {quantity, unit});
}

/// @brief The name a reflected enumeration carries in the catalogue's keys, `lims.status.<name>.<enumerator>`.
///
/// Specialised for each enumeration a screen shows, because the compiler-provided type name is not the same on every
/// toolchain.
/// @tparam E A reflected enumeration.
template <typename E>
struct StatusName;

/// @brief The catalogue name of `InvoiceState`.
template <>
struct StatusName<InvoiceState> {
    /// @brief The name.
    static constexpr std::string_view value = "InvoiceState";
};

/// @brief The catalogue name of `BillableLineState`.
template <>
struct StatusName<BillableLineState> {
    /// @brief The name.
    static constexpr std::string_view value = "BillableLineState";
};

/// @brief The catalogue name of `ApprovalLevel`.
template <>
struct StatusName<ApprovalLevel> {
    /// @brief The name.
    static constexpr std::string_view value = "ApprovalLevel";
};

/// @brief The catalogue name of `BillingRunState`.
template <>
struct StatusName<BillingRunState> {
    /// @brief The name.
    static constexpr std::string_view value = "BillingRunState";
};

/// @brief The catalogue name of `SampleState`.
template <>
struct StatusName<SampleState> {
    /// @brief The name.
    static constexpr std::string_view value = "SampleState";
};

/// @brief The catalogue name of `AccountType`.
template <>
struct StatusName<AccountType> {
    /// @brief The name.
    static constexpr std::string_view value = "AccountType";
};

/// @brief The catalogue name of `PeriodState`.
template <>
struct StatusName<PeriodState> {
    /// @brief The name.
    static constexpr std::string_view value = "PeriodState";
};

/// @brief The catalogue name of `EntryState`.
template <>
struct StatusName<EntryState> {
    /// @brief The name.
    static constexpr std::string_view value = "EntryState";
};

/// @brief The catalogue name of `LimsRole`.
template <>
struct StatusName<LimsRole> {
    /// @brief The name.
    static constexpr std::string_view value = "LimsRole";
};

namespace detail {

/// @brief An enumerator's name split into words: `FinishedWithFailures` as `Finished with failures`.
/// @param name The enumerator's name.
/// @return The words, the first capitalised and the rest lower case.
[[nodiscard]] inline std::string words(std::string_view name) {
    std::string out;
    for (char const letter : name) {
        auto const upper = std::isupper(static_cast<unsigned char>(letter)) != 0;
        if (upper && !out.empty()) {
            out += ' ';
        }
        out += out.empty() ? letter : static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    }
    return out;
}

}  // namespace detail

/// @brief The words for @p value, an enumerator of @p E by its glaze name: the catalogue's
///        `lims.status.<E>.<name>`, with the name split into words as the fallback.
/// @tparam E A reflected enumeration with a `StatusName`.
/// @param value The enumerator's name, as the record carries it.
/// @return The words.
template <typename E>
[[nodiscard]] ui::Handle<std::string> statusWord(ui::AnyHandle const& value) {
    auto words = ui::Handle<std::string>::raw(value.json());
    auto const& keys = glz::reflect<E>::keys;
    for (auto index = keys.size(); index-- > 0;) {
        std::string const name{keys[index]};
        words = ui::cond(ui::eq(value, name),
                         ui::t("lims.status." + std::string{StatusName<E>::value} + "." + name, detail::words(name)),
                         words);
    }
    return words;
}

/// @brief A badge with @p value's words, in the tone @p tones names for it (neutral otherwise).
/// @tparam E A reflected enumeration with a `StatusName`.
/// @param value The enumerator's name.
/// @param tones Tone per enumerator name: `ok`, `warn`, `err`, `info`.
/// @return The badge.
template <typename E>
[[nodiscard]] ui::Node statusBadge(ui::AnyHandle const& value,
                                   std::map<std::string_view, std::string_view> const& tones) {
    ui::Handle<std::string> tone{"neutral"};
    for (auto const& [name, named] : tones) {
        tone = ui::cond(ui::eq(value, std::string{name}), ui::Handle<std::string>{std::string{named}}, tone);
    }
    return ui::badge(statusWord<E>(value), tone);
}

/// @brief An error banner over @p message, shown only while it is engaged.
/// @param message The text.
/// @return The banner.
[[nodiscard]] ui::Node errorBanner(ui::AnyHandle const& message);

}  // namespace lims::screens::kit
