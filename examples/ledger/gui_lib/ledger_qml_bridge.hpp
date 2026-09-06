// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

// Guarded exactly like `ledger_presenter.hpp`'s own includes, and for the
// same reason: AUTOMOC runs moc over this header, and moc must not be pointed
// at the presenter's transitive `morph/core/bridge.hpp` or the model header's
// Lightweight ORM -- it mis-parses their namespace structure and nests every
// later namespace inside `Lightweight`. moc needs none of this; the real
// compiler still sees it.
#ifndef Q_MOC_RUN
#include "ledger_presenter.hpp"
#endif

/// @file
/// `LedgerQmlBridge` -- the QML-facing face of `LedgerPresenter`: typed
/// results in, `QVariant` property bags out, plus the `Q_INVOKABLE`s a view
/// calls.

namespace ledger::gui {

/// @brief Adapts `LedgerPresenter` for QML: its typed signals become
///        `QVariantList`/`QVariantMap` properties, and its calls become
///        `Q_INVOKABLE`s taking the plain types QML can pass.
///
/// This is also where idempotency keys are minted. `storeTransaction` mints
/// one `ImportOpId` per user gesture (`QUuid::createUuid()`), so a retry of
/// *that gesture* is deduplicated while a genuinely new transaction is not --
/// the presenter below stays transport-only and never generates one, matching
/// `BoardBridge::moveTask`'s own division of labour and the lesson its fix
/// round recorded: the id belongs to the call it was minted for, captured
/// alongside that call, never stashed on a shared member.
class LedgerQmlBridge : public QObject {
    Q_OBJECT

    /// @brief The attached ledger's accounts, each a map of
    ///        `id`/`name`/`kind`/`currency`/`balance`.
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY accountsChanged)

    /// @brief The last listed month's journal entries, each a map of
    ///        `id`/`description`/`dateText` -- the `id` being the number
    ///        `undoTransaction` asks for, which until morph#428 no screen in
    ///        this rung ever displayed.
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)

    /// @brief `true` while any dispatch is in flight, for a busy indicator.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

    /// @brief The last error surfaced by the presenter, or empty.
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    LedgerQmlBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @return The attached ledger's accounts as QML-ready maps.
    [[nodiscard]] QVariantList accounts() const { return _accounts; }

    /// @return The last listed month's entries as QML-ready maps.
    [[nodiscard]] QVariantList entries() const { return _entries; }

    /// @return Whether a dispatch is in flight.
    [[nodiscard]] bool busy() const;

    /// @return The last error message, or an empty string.
    [[nodiscard]] QString lastError() const { return _lastError; }

    /// @brief Attaches to the ledger with @p ledgerId and reads it.
    /// @param ledgerId The ledger's id, as the plain number QML holds.
    Q_INVOKABLE void openLedger(const QString& ledgerId);

    /// @brief Re-reads the attached ledger.
    Q_INVOKABLE void refresh();

    /// @brief Opens an account on the attached ledger.
    /// @param name     The account's display name.
    /// @param kind     One of `asset`/`liability`/`income`/`expense`/`equity`.
    /// @param currency The ISO code, e.g. `USD`.
    Q_INVOKABLE void openAccount(const QString& name, const QString& kind, const QString& currency);

    /// @brief Stores a two-leg transaction moving @p amountMinor between two
    ///        accounts, minting this gesture's own idempotency key.
    ///
    ///        Two legs rather than an arbitrary list because that is what the
    ///        view offers today; the presenter takes the general form, so a
    ///        richer editor needs no change below it.
    /// @param fromAccountId The account debited, as a plain-number string.
    /// @param toAccountId   The account credited.
    /// @param amountMinor   The amount in minor units (cents), always exact --
    ///        never a float, per design spec §7's no-float rule.
    /// @param description   The transaction's description.
    Q_INVOKABLE void storeTransaction(const QString& fromAccountId, const QString& toAccountId, qlonglong amountMinor,
                                      const QString& description);

    /// @brief Lists the attached ledger's entries for @p month, publishing
    ///        them on `entries`.
    ///
    ///        The Undo control's source: a view lists a month, shows what
    ///        came back, and hands one of those entries' own `id` straight to
    ///        `undoTransaction` below. Re-run automatically after a
    ///        successful undo, so the list a user is looking at is not one
    ///        the reversal has already invalidated.
    /// @param month The month to list, as `"YYYY-MM"`.
    Q_INVOKABLE void listTransactions(const QString& month);

    /// @brief Reverses @p journalId with a compensating entry.
    /// @param journalId The journal to reverse, as a plain-number string --
    ///        an `id` taken from `entries`, never typed.
    Q_INVOKABLE void undoTransaction(const QString& journalId);

signals:
    /// @brief The account list changed.
    void accountsChanged();

    /// @brief The entry list changed.
    void entriesChanged();

    /// @brief `busy()` changed.
    void busyChanged();

    /// @brief `lastError()` changed.
    void lastErrorChanged();

private:
    /// @brief Replaces `_accounts` from @p result and notifies QML.
    /// @param result The ledger state to publish.
    void publishLedger(const GetLedgerResult& result);

    /// @brief Replaces `_entries` from @p result and notifies QML.
    /// @param result The listing to publish.
    void publishEntries(const ListTransactionsResult& result);

    /// @brief Records @p message as `lastError` and notifies QML.
    /// @param message The presenter's error text.
    void publishError(const QString& message);

    LedgerPresenter _presenter;
    LedgerId _ledgerId;
    QVariantList _accounts;
    QVariantList _entries;
    /// The month `listTransactions` was last called with, so an undo can
    /// re-list it. Empty until a view has listed something, which is what
    /// keeps a bare `openLedger` from dispatching a read nobody asked for.
    QString _listedMonth;
    QString _lastError;
};

}  // namespace ledger::gui
