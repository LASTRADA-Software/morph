// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gui/presenter.hpp"
#include "ledger/core/import_op_id.hpp"
#include "ledger/dto/account_dto.hpp"
#include "ledger/dto/import_dto.hpp"
#include "ledger/dto/transaction_dto.hpp"

// Guarded exactly like `board_presenter.hpp`'s own includes, and for the same
// reason it documents: AUTOMOC runs moc over this header, and moc must not be
// pointed at morph's template-heavy bridge.hpp -- nor, here, at the model
// header, which reaches Lightweight's ORM. moc mis-parses those namespace
// structures and then believes every later namespace is nested inside
// `Lightweight`, emitting `Lightweight::ledger::gui::LedgerPresenter` and
// failing with "no member named 'ledger' in namespace 'Lightweight'". moc
// needs none of these declarations; the real compiler still sees them all.
#ifndef Q_MOC_RUN
#include "ledger/models/ledger_model.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#endif

#include <QObject>
#include <QString>

#include <cstdint>
#include <exception>
#include <vector>

/// @file
/// `LedgerPresenter` -- the transport half of rung 5's ledger screen
/// (examples/TESTING.md's "Presenter architecture" rules): it dispatches
/// through a `BridgeHandler` and translates each typed result into a Qt
/// signal, and does nothing else.

namespace ledger::gui {

/// @brief Dispatches `LedgerModel`'s action surface and re-emits each result
///        as a Qt signal.
///
/// `BridgeHandler<LedgerModel, AllowShared>`, not a plain handler:
/// `LedgerModel` is keyed per ledger (its hand-written `ModelKeyTraits`, see
/// `ledger_model.hpp`), so a second client opening the same ledger must join
/// the same shared-instance directory the keyed attach relies on. A
/// `NoSharing` handler registers its own private instance eagerly and never
/// attaches to another's, which would give every client a private ledger --
/// the same rationale `BoardPresenter`/`PollPresenter` document.
///
/// Translates and routes, never decides (examples/IMPLEMENTATION.md rule 2):
/// every method here maps one gesture to one dispatch, and the only thing it
/// interprets is which signal a given result type belongs on. Idempotency
/// keys are minted by the *bridge* above it, never here -- see
/// `storeTransaction`'s `opId` parameter.
class LedgerPresenter : public ::morph::ladder::gui::Presenter {
    Q_OBJECT
  public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    LedgerPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                    QObject* parent = nullptr);

    /// @brief Attaches to @p ledgerId and reads its current state. Emits
    ///        `ledgerListed` on success, `failed` on error.
    /// @param ledgerId The ledger to attach to and read.
    void refreshLedger(LedgerId ledgerId);

    /// @brief Opens a new account on @p ledgerId. Emits `accountOpened` on
    ///        success, `failed` on error.
    /// @param ledgerId The ledger to open the account on.
    /// @param name     The account's display name.
    /// @param kind     Asset/Liability/Income/Expense/Equity.
    /// @param currency The account's currency.
    void openAccount(LedgerId ledgerId, const QString& name, AccountKind kind, Currency currency);

    /// @brief Stores a balanced multi-leg transaction. Emits
    ///        `transactionStored` with the post-store ledger state on
    ///        success, `failed` on error (including the per-currency
    ///        zero-sum refusal, which is a model decision this presenter
    ///        only relays).
    /// @param ledgerId    The ledger to store into.
    /// @param description The transaction's description.
    /// @param date        The transaction's instant, in UTC.
    /// @param legs        The transaction's legs; must net to zero per currency.
    /// @param opId        The idempotency key this gesture was minted with,
    ///        by the bridge -- passed through so a retry of the *same*
    ///        gesture is not applied twice. Never generated here. A
    ///        disengaged `ImportOpId` takes the ordinary insert-only path,
    ///        exactly as `StoreTransaction` documents.
    void storeTransaction(LedgerId ledgerId, const QString& description, morph::time::Timestamp date,
                          std::vector<TransactionLeg> legs, const ImportOpId& opId);

    /// @brief Reverses @p journalId with a compensating entry. Emits
    ///        `transactionUndone` with the post-undo ledger state on
    ///        success, `failed` on error.
    /// @param ledgerId  The ledger the journal belongs to.
    /// @param journalId The journal to reverse.
    void undoTransaction(LedgerId ledgerId, JournalId journalId);

    /// @brief Imports one CSV chunk against @p counterAccountId. Emits
    ///        `importCompleted(imported, duplicates)` on success, `failed`
    ///        on error.
    /// @param ledgerId         The ledger to import into.
    /// @param counterAccountId The account every imported row books against.
    /// @param csvChunk         The chunk's raw CSV text.
    /// @param opId             The import's idempotency key, minted by the
    ///        bridge -- re-importing the same statement is what this
    ///        deduplicates, so it is per-import, not per-row.
    void importChunk(LedgerId ledgerId, AccountId counterAccountId, const QString& csvChunk,
                     const ImportOpId& opId);

  signals:
    /// @brief A ledger's state was read successfully.
    /// @param result The ledger's accounts and their balances.
    void ledgerListed(ledger::GetLedgerResult result);

    /// @brief An account was opened.
    /// @param account The newly opened account.
    void accountOpened(ledger::AccountInfo account);

    /// @brief A transaction was stored.
    /// @param result The ledger's state after the store.
    void transactionStored(ledger::GetLedgerResult result);

    /// @brief A transaction was reversed.
    /// @param result The ledger's state after the reversal.
    void transactionUndone(ledger::GetLedgerResult result);

    /// @brief An import chunk finished.
    /// @param imported   Rows that produced a new transaction.
    /// @param duplicates Rows skipped as already-imported.
    void importCompleted(std::int64_t imported, std::int64_t duplicates);

    /// @brief Any dispatch above failed.
    /// @param message The exception's `what()`.
    void failed(QString message);

  private:
    /// @brief Re-emits @p err as `failed` carrying its `what()`.
    /// @param err The exception the completion carried.
    void reportError(const std::exception_ptr& err);

    ::morph::bridge::BridgeHandler<LedgerModel, ::morph::bridge::AllowShared> _handler;
};

}  // namespace ledger::gui
