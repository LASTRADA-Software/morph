// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <vector>

#include "lims/org/core/money.hpp"
#include "lims/org/core/strong_id.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// The posting contract: a poster (billing, and inventory later) maps its document to a `PostingRequest`; the ledger
/// validates and records it and answers a `PostingResult`. DTOs only. Neither side includes the other: the app hands
/// one to the other.

namespace lims {

LIMS_DEFINE_STRONG_ID(JournalEntryId);

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

/// @brief What the ledger recorded.
struct PostingResult {
    /// @brief The journal entry.
    JournalEntryId entryId;
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
