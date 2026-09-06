// SPDX-License-Identifier: Apache-2.0
#include "ledger_presenter.hpp"

#include <utility>

#include "gui/error_text.hpp"

namespace ledger::gui {

LedgerPresenter::LedgerPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : Presenter{parent}, _handler{bridge, executor} {
    trackBound(_handler.whenBound());
}

void LedgerPresenter::reportError(const std::exception_ptr& err) { emit failed(::morph::ladder::gui::errorText(err)); }

void LedgerPresenter::refreshLedger(LedgerId ledgerId) {
    track<GetLedgerResult>(
        _handler.execute(GetLedger{.ledgerId = ledgerId}),
        [this](GetLedgerResult result) { emit ledgerListed(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void LedgerPresenter::openAccount(LedgerId ledgerId, const QString& name, AccountKind kind, Currency currency) {
    track<AccountInfo>(
        _handler.execute(
            OpenAccount{.ledgerId = ledgerId, .name = name.toStdString(), .kind = kind, .currency = currency}),
        [this](AccountInfo account) { emit accountOpened(std::move(account)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void LedgerPresenter::storeTransaction(LedgerId ledgerId, const QString& description, morph::time::Timestamp date,
                                       std::vector<TransactionLeg> legs, const ImportOpId& opId) {
    track<GetLedgerResult>(
        _handler.execute(StoreTransaction{.ledgerId = ledgerId,
                                          .description = description.toStdString(),
                                          .date = date,
                                          .legs = std::move(legs),
                                          .opId = opId}),
        [this](GetLedgerResult result) { emit transactionStored(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void LedgerPresenter::listTransactions(LedgerId ledgerId, const QString& month) {
    track<ListTransactionsResult>(
        _handler.execute(ListTransactions{.ledgerId = ledgerId, .month = month.toStdString()}),
        [this](ListTransactionsResult result) { emit transactionsListed(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void LedgerPresenter::undoTransaction(LedgerId ledgerId, JournalId journalId) {
    track<GetLedgerResult>(
        _handler.execute(UndoTransaction{.ledgerId = ledgerId, .journalId = journalId}),
        [this](GetLedgerResult result) { emit transactionUndone(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void LedgerPresenter::importChunk(LedgerId ledgerId, AccountId counterAccountId, const QString& csvChunk,
                                  const ImportOpId& opId) {
    track<ImportResult>(
        _handler.execute(ImportLedgerChunk{.ledgerId = ledgerId,
                                           .counterAccountId = counterAccountId,
                                           .csvChunk = csvChunk.toStdString(),
                                           .opId = opId}),
        // Two plain integers rather than the result struct: `importCompleted`
        // is what a progress indicator binds to, and a QML-facing signal
        // carrying a registered DTO would need that DTO on the QML type
        // system for no gain here.
        [this](ImportResult result) { emit importCompleted(result.imported, result.duplicates); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

}  // namespace ledger::gui
