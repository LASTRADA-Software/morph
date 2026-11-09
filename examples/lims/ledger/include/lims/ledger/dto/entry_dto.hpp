// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <vector>

#include "lims/ledger/core/types.hpp"
#include "lims/posting/posting.hpp"

/// @file
/// Journal entries as the wire sees them. An entry's lines are `PostingLine`s: the same shape a poster sends.

namespace lims {

/// @brief One journal entry.
struct JournalEntryView {
    /// @brief Its id.
    JournalEntryId id;
    /// @brief The company.
    CompanyId company;
    /// @brief The period its date falls in.
    PeriodId period;
    /// @brief Its date.
    ::morph::time::Timestamp date;
    /// @brief Draft or posted.
    EntryState state = EntryState::Draft;
    /// @brief The poster's key; empty for an entry made by hand.
    std::string sourceKey;
    /// @brief What it shows as its source.
    std::string sourceReference;
    /// @brief A note.
    std::string memo;
    /// @brief The transaction currency.
    CurrencyCode currency = CurrencyCode::EUR;
    /// @brief The rate its company values were converted with.
    Rate rate;
    /// @brief The entry it reverses; empty unless it is a reversal.
    JournalEntryId reverses;
    /// @brief The entry that reverses it; empty unless it was reversed.
    JournalEntryId reversedBy;
    /// @brief Its lines, in order.
    std::vector<PostingLine> lines;
    /// @brief Who made it.
    std::string createdBy;
    /// @brief Who posted it; empty while a draft.
    std::string postedBy;
};

/// @brief Drafts an entry by hand.
struct CreateDraftEntry {
    /// @brief The company.
    CompanyId company;
    /// @brief Its date.
    ::morph::time::Timestamp date;
    /// @brief A note, at most 255 bytes.
    std::string memo;
    /// @brief The transaction currency.
    CurrencyCode currency = CurrencyCode::EUR;
    /// @brief The rate its company values were converted with.
    Rate rate;
    /// @brief Two or more lines.
    std::vector<PostingLine> lines;

    /// @brief Whether the draft is well-formed.
    /// @return `true` when a company, a date, a rate and two or more lines are named and the memo fits.
    [[nodiscard]] bool validate() const noexcept {
        return company.hasValue() && date.hasValue() && rate.hasValue() && lines.size() >= 2U && memo.size() <= 255;
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief Posts a draft: it enters the balances and can no longer change.
struct PostEntry {
    /// @brief The entry.
    JournalEntryId entryId;

    /// @brief Whether an entry is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return entryId.hasValue(); }
};

/// @brief Corrects a posted entry with a posted reversal on @p date that names it.
struct ReverseEntry {
    /// @brief The entry.
    JournalEntryId entryId;
    /// @brief The reversal's date.
    ::morph::time::Timestamp date;

    /// @brief Whether an entry and a date are named.
    /// @return `true` when they are.
    [[nodiscard]] bool validate() const noexcept { return entryId.hasValue() && date.hasValue(); }
};

/// @brief Reads one entry.
struct GetEntry {
    /// @brief The entry.
    JournalEntryId entryId;

    /// @brief Whether an entry is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return entryId.hasValue(); }
};

/// @brief Lists a company's entries.
struct ListEntries {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief The entries, by id.
struct ListEntriesResult {
    /// @brief The entries.
    std::vector<JournalEntryView> entries;
};

/// @brief `Closing → Closed`: freezes the period's balances. Refused while a draft remains in it.
struct ClosePeriod {
    /// @brief The period.
    PeriodId periodId;

    /// @brief Whether a period is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return periodId.hasValue(); }
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::JournalEntryView, "Ledger_Entry")
