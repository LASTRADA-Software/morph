// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "lims/org/core/money.hpp"
#include "lims/org/core/strong_id.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// The posting contract: a poster (billing, inventory) maps its document to a `PostingRequest`; the ledger validates
/// and records it and answers a `PostingResult`. This header holds the contract's DTOs; `ledger_port.hpp` holds the
/// one port every poster reaches the ledger through, here rather than in a poster because two posters share it and the
/// app installs it once. Neither side includes the other: the app hands one to the other. It also holds the rule an
/// account code must satisfy, which the ledger's chart applies and every poster's settings check.

namespace lims {

LIMS_DEFINE_STRONG_ID(JournalEntryId);

/// @brief The code of the ledger's refusal of a source key that is already bound to a different posting body. A poster
///        tells it from the ledger's other refusals by this code, not by its wording.
inline constexpr std::string_view kSourceKeyOtherBodyCode = "lims.posting.sourceKeyOtherBody";

/// @brief Whether @p code can name a ledger account: 1 to 16 upper-case letters and digits. Upper-case only, so that
/// two
///        codes differing only in case cannot coexist in a chart.
/// @param code The code.
/// @return `true` when it can.
[[nodiscard]] inline bool isAccountCode(std::string_view code) noexcept {
    return !code.empty() && code.size() <= 16 &&
           std::ranges::all_of(code, [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'); });
}

/// @brief Which side of the entry a line is on.
enum class PostingSide : std::uint8_t {
    Debit,   ///< Debit.
    Credit,  ///< Credit.
};

/// @brief One line of a posting.
struct PostingLine {
    /// @brief The account's code in the company's chart, such as `1200`.
    std::string accountCode;
    /// @brief The side.
    PostingSide side = PostingSide::Debit;
    /// @brief The amount in the request's transaction currency; positive, or zero for an exchange-rounding line.
    Amount amount;
    /// @brief Its value in the company's currency, rounded by the poster to that currency's decimals.
    Amount companyAmount;
};

/// @brief One fact a poster asks the ledger to record.
struct PostingRequest {
    /// @brief The company whose books it goes in.
    CompanyId company;
    /// @brief The entry's date; it picks the period and the exchange rate.
    ::morph::time::Timestamp date;
    /// @brief The poster's unique key for the document, such as `billing.invoice:7`. The ledger records each key
    ///        once: a request repeated with the same key answers the entry the first one recorded.
    std::string sourceKey;
    /// @brief What the entry shows as its source, such as an invoice number.
    std::string sourceReference;
    /// @brief The transaction currency of every line's `amount`.
    CurrencyCode currency = CurrencyCode::EUR;
    /// @brief The rate the poster converted with: company-currency units per transaction-currency unit. One when the
    ///        currencies are the same; otherwise it must be the ledger's rate valid on `date`.
    Rate rate;
    /// @brief Two or more lines.
    std::vector<PostingLine> lines;
};

/// @brief What a poster asks the ledger before it retries: the entry already recorded under a source key.
struct PostingQuery {
    /// @brief The company whose books are asked.
    CompanyId company;
    /// @brief The poster's source key.
    std::string sourceKey;
};

/// @brief What the ledger recorded.
struct PostingResult {
    /// @brief The journal entry.
    JournalEntryId entryId;
    /// @brief Its date: the request's, or, for a repeated key, the first request's.
    ::morph::time::Timestamp date;
};

/// @brief What a poster asks the ledger before it converts: the rate from one currency to another valid on a date.
struct RateQuery {
    /// @brief The company whose rates are asked.
    CompanyId company;
    /// @brief The currency converted from.
    CurrencyCode from = CurrencyCode::EUR;
    /// @brief The currency converted to.
    CurrencyCode to = CurrencyCode::EUR;
    /// @brief The date the rate must be valid on.
    ::morph::time::Timestamp on;
};

/// @brief The ledger's answer to a `RateQuery`.
struct RateAnswer {
    /// @brief Units of `to` per unit of `from`; empty when no rate is valid on the date.
    Rate rate;
};

}  // namespace lims

LIMS_DEFINE_STRONG_ID_WIRE(JournalEntryId);
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(JournalEntryId, "lims.journalEntryId");

/// @brief Reflects `PostingSide` as its enumerator name.
template <>
struct glz::meta<lims::PostingSide> {
    using enum lims::PostingSide;

    /// @brief The enumerator <-> name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Debit, Credit);
};
