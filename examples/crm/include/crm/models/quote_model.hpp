// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <string>

#include "crm/core/self_journal.hpp"
#include "crm/dto/quote_dto.hpp"

/// @file
/// `QuoteModel` — quotes/pricing (README build order §4). See quote_dto.hpp's
/// header comment for the computed-fields-are-top-level-only design decision
/// this model implements around: per-line totals are recomputed by hand
/// (`recomputeAll<QuoteLine>`), and the grand total is a plain accumulation
/// loop, matching `ledger`'s identical precedent for summing amounts.

namespace crm {

class QuoteModel {
public:
    CreateQuoteResult execute(const CreateQuote& action);
    UpdateQuoteResult execute(const UpdateQuote& action);
    QuoteView execute(const SendQuote& action);
    QuoteView execute(const DecideQuote& action);
    QuoteView execute(const GetQuote& action);
    ListQuotesResult execute(const ListQuotes& action);

    void attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey) {
        _journal.attach(std::move(log), std::move(entityKey));
    }

    [[nodiscard]] std::vector<::morph::journal::LogEntry> journalEntries() const { return _journal.entries(); }

private:
    SelfJournal _journal;
};

}  // namespace crm

BRIDGE_REGISTER_MODEL(crm::QuoteModel, "QuoteModel")
BRIDGE_REGISTER_ACTION(crm::QuoteModel, crm::CreateQuote, "CreateQuote")
BRIDGE_REGISTER_ACTION(crm::QuoteModel, crm::UpdateQuote, "UpdateQuote")
BRIDGE_REGISTER_ACTION(crm::QuoteModel, crm::SendQuote, "SendQuote")
BRIDGE_REGISTER_ACTION(crm::QuoteModel, crm::DecideQuote, "DecideQuote")
BRIDGE_REGISTER_ACTION(crm::QuoteModel, crm::GetQuote, "GetQuote", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(crm::QuoteModel, crm::ListQuotes, "ListQuotes", ::morph::model::Loggable::No)
