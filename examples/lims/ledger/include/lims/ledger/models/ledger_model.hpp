// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>

#include "lims/ledger/dto/account_dto.hpp"
#include "lims/ledger/dto/entry_dto.hpp"
#include "lims/posting/posting.hpp"

/// @file
/// `LedgerModel`: a company's chart of accounts, its monthly periods and its journal entries.

namespace lims {

/// @brief The books of every company.
///
/// Unkeyed, holding no connection: each `execute()` opens a `Lightweight::DataMapper` for its own duration. Knows
/// nothing of invoices or stock: a poster's document reaches it as a `PostingRequest`.
class LedgerModel {
public:
    /// @brief Adds an account to a company's chart.
    /// @param action The company, code, name and type.
    /// @return The account.
    /// @throws ValidationError if the account is malformed.
    /// @throws NotFound if the company does not exist.
    /// @throws Conflict if the company's chart already has the code.
    /// @throws Forbidden if the caller is not an accountant at the company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    AccountView execute(const CreateAccount& action);

    /// @brief Lists a company's accounts, when the caller can see the company.
    /// @param action The company.
    /// @return Its accounts by code; none for a company the caller cannot see.
    /// @throws ValidationError if no company is named, or a row holds a type outside its enumeration.
    ListAccountsResult execute(const ListAccounts& action);

    /// @brief Opens the month an instant falls in.
    /// @param action The company and an instant in the month.
    /// @return The period, open.
    /// @throws ValidationError if the request is malformed.
    /// @throws NotFound if the company does not exist.
    /// @throws Conflict if the month is already a period of the company.
    /// @throws Forbidden if the caller is not an accountant at the company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    PeriodView execute(const OpenPeriod& action);

    /// @brief Lists a company's periods, when the caller can see the company.
    /// @param action The company.
    /// @return Its periods by start.
    /// @throws ValidationError if no company is named, or a row holds a state outside its enumeration.
    ListPeriodsResult execute(const ListPeriods& action);

    /// @brief `Open → Closing`.
    /// @param action The period.
    /// @return The period, closing.
    /// @throws ValidationError if no period is named.
    /// @throws NotFound if it does not exist.
    /// @throws IllegalTransition if it is not open.
    /// @throws Forbidden if the caller is not an accountant at its company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    PeriodView execute(const BeginClosePeriod& action);

    /// @brief Drafts an entry in the period its date falls in.
    ///
    /// The ledger never rounds or balances for the caller: the lines must already balance in the transaction currency
    /// and in the company's currency, each at its currency's decimals.
    /// @param action The company, date, memo, currency, rate and lines.
    /// @return The draft.
    /// @throws ValidationError if the request is malformed, the lines do not balance or are not rounded, the rate is
    /// not positive, or the rate is not one for an entry in the company's own currency.
    /// @throws NotFound if the company, the date's period or an account code does not exist.
    /// @throws Conflict if the period is closing or closed.
    /// @throws Forbidden if the caller is not an accountant at the company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    JournalEntryView execute(const CreateDraftEntry& action);

    /// @brief `Draft → Posted`: the entry enters the balances and can no longer change.
    /// @param action The entry.
    /// @return The entry, posted.
    /// @throws ValidationError if no entry is named.
    /// @throws NotFound if it does not exist.
    /// @throws IllegalTransition if it is already posted.
    /// @throws Conflict if its period is closed.
    /// @throws Forbidden if the caller is not an accountant at its company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    JournalEntryView execute(const PostEntry& action);

    /// @brief Corrects a posted entry with a posted reversal that names it; the original is not edited.
    /// @param action The entry and the reversal's date.
    /// @return The reversal.
    /// @throws ValidationError if the request is malformed.
    /// @throws NotFound if the entry, or the period of the date, does not exist.
    /// @throws IllegalTransition if the entry is a draft.
    /// @throws Conflict if the entry is already reversed or the date's period is closed.
    /// @throws Forbidden if the caller is not an accountant at its company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    JournalEntryView execute(const ReverseEntry& action);

    /// @brief Reads one entry, when the caller can see its company.
    /// @param action The entry.
    /// @return The entry.
    /// @throws ValidationError if no entry is named.
    /// @throws NotFound if it does not exist or its company is not visible to the caller.
    JournalEntryView execute(const GetEntry& action);

    /// @brief Lists a company's entries, when the caller can see the company.
    /// @param action The company.
    /// @return Its entries by id; none for a company the caller cannot see.
    /// @throws ValidationError if no company is named.
    ListEntriesResult execute(const ListEntries& action);
};

}  // namespace lims

// The registration macros declare a non-const status object each.
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_MODEL(lims::LedgerModel, "LedgerModel")
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::CreateAccount, "CreateAccount")
BRIDGE_REGISTER_EFFECTS(lims::CreateAccount, ::morph::effects::changes<lims::AccountView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::ListAccounts, "ListAccounts", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::OpenPeriod, "OpenPeriod")
BRIDGE_REGISTER_EFFECTS(lims::OpenPeriod, ::morph::effects::changes<lims::PeriodView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::ListPeriods, "ListPeriods", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::BeginClosePeriod, "BeginClosePeriod")
BRIDGE_REGISTER_EFFECTS(lims::BeginClosePeriod, ::morph::effects::updates<lims::PeriodView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::CreateDraftEntry, "CreateDraftEntry")
BRIDGE_REGISTER_EFFECTS(lims::CreateDraftEntry, ::morph::effects::changes<lims::JournalEntryView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::PostEntry, "PostEntry")
BRIDGE_REGISTER_EFFECTS(lims::PostEntry, ::morph::effects::updates<lims::JournalEntryView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::ReverseEntry, "ReverseEntry")
BRIDGE_REGISTER_EFFECTS(lims::ReverseEntry, ::morph::effects::changes<lims::JournalEntryView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::GetEntry, "GetEntry", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::ListEntries, "ListEntries", ::morph::model::Loggable::No)
// NOLINTEND(misc-const-correctness)
