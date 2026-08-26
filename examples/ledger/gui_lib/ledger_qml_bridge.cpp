// SPDX-License-Identifier: Apache-2.0
#include "ledger_qml_bridge.hpp"

#include <QUuid>
#include <string>
#include <utility>

#include "gui/id_qml.hpp"
#include "ledger/core/money.hpp"
#include "ledger/core/units.hpp"

namespace ledger::gui {

namespace {

using ::morph::ladder::gui::idFromText;
using ::morph::ladder::gui::idNumber;

/// @brief `AccountKind` as the lowercase token QML passes.
/// @param text The token.
/// @return The matching kind; `Asset` for anything unrecognised, which the
///         view's own fixed set of choices makes unreachable in practice.
[[nodiscard]] AccountKind kindFromText(const QString& text) {
    if (text == "expense") {
        return AccountKind::Expense;
    }
    if (text == "revenue") {
        return AccountKind::Revenue;
    }
    if (text == "liability") {
        return AccountKind::Liability;
    }
    return AccountKind::Asset;
}

/// @brief `AccountKind` rendered back to QML's lowercase token.
/// @param kind The kind to render.
/// @return The token.
[[nodiscard]] QString kindToText(AccountKind kind) {
    // `default:` shares the `Asset` arm rather than sitting alone after it.
    // The repo's warning policy requires a default (-Wswitch-default), but a
    // standalone one would be permanently unreachable and therefore
    // permanently uncovered -- the exact artefact codecov.yml's own comments
    // catalogue for `UnitTraits<Unit>::meta`. Sharing the arm satisfies the
    // warning with no dead line.
    switch (kind) {
        case AccountKind::Expense:
            return QStringLiteral("expense");
        case AccountKind::Revenue:
            return QStringLiteral("revenue");
        case AccountKind::Liability:
            return QStringLiteral("liability");
        case AccountKind::Asset:
        default:
            return QStringLiteral("asset");
    }
}

/// @brief An account as the map QML binds to.
///
///        `balance` is carried as its exact `Rational` triple plus a
///        pre-rendered `balanceText`, never a float: design spec §7's
///        no-float rule applies at the QML boundary exactly as it does on
///        the wire, and a view that wants to format differently still has
///        the exact numerator/denominator to do it from.
///
///        `balanceText` is what `LedgerView` actually binds. It has to be
///        rendered here rather than in the view because QML has only IEEE
///        doubles: the `numerator / denominator / Math.pow(10, places)` the
///        view used to compute re-introduced, in the last three lines of the
///        path, exactly the imprecision `Rational` exists to remove, and
///        drifted past 2^53 while the payload beneath it stayed exact.
///        `ledger::formatMoney` is exact integer long division through
///        `Money<C>` and `morph::units::toDecimalString`.
/// @param account The account to render.
/// @return The QML-ready map.
[[nodiscard]] QVariantMap toVariantMap(const AccountInfo& account) {
    const auto& balance = account.balance;
    const auto code = currencyToCode(account.currency);
    return QVariantMap{
        {"id", idNumber(account.id)},
        {"name", QString::fromStdString(account.name)},
        {"kind", kindToText(account.kind)},
        {"currency", QString::fromUtf8(code.data(), static_cast<qsizetype>(code.size()))},
        {"balanceNumerator", static_cast<qlonglong>(balance.numerator)},
        {"balanceDenominator", static_cast<qlonglong>(balance.denominator)},
        {"balanceDecimalPlaces", static_cast<qlonglong>(balance.decimalPlaces.value)},
        {"balanceText", QString::fromStdString(formatMoney(account.currency, balance))},
    };
}

}  // namespace

LedgerQmlBridge::LedgerQmlBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : QObject{parent}, _presenter{bridge, executor} {
    connect(&_presenter, &LedgerPresenter::ledgerListed, this,
            [this](const GetLedgerResult& result) { publishLedger(result); });
    connect(&_presenter, &LedgerPresenter::transactionStored, this,
            [this](const GetLedgerResult& result) { publishLedger(result); });
    connect(&_presenter, &LedgerPresenter::transactionUndone, this,
            [this](const GetLedgerResult& result) { publishLedger(result); });
    // An opened account does not carry the whole ledger, so re-read rather
    // than append: the balances of *other* accounts are unaffected, but the
    // model is the authority on the list's order and contents.
    connect(&_presenter, &LedgerPresenter::accountOpened, this, [this](const AccountInfo&) { refresh(); });
    connect(&_presenter, &LedgerPresenter::failed, this, [this](const QString& message) { publishError(message); });
    connect(&_presenter, &LedgerPresenter::idle, this, &LedgerQmlBridge::busyChanged);
}

bool LedgerQmlBridge::busy() const { return _presenter.busy(); }

void LedgerQmlBridge::publishLedger(const GetLedgerResult& result) {
    QVariantList accounts;
    accounts.reserve(static_cast<qsizetype>(result.accounts.size()));
    for (const auto& account : result.accounts) {
        accounts.push_back(toVariantMap(account));
    }
    _accounts = std::move(accounts);
    emit accountsChanged();
}

void LedgerQmlBridge::publishError(const QString& message) {
    _lastError = message;
    emit lastErrorChanged();
}

void LedgerQmlBridge::openLedger(const QString& ledgerId) {
    _ledgerId = idFromText<LedgerId>(ledgerId);
    _presenter.refreshLedger(_ledgerId);
    emit busyChanged();
}

void LedgerQmlBridge::refresh() {
    _presenter.refreshLedger(_ledgerId);
    emit busyChanged();
}

void LedgerQmlBridge::openAccount(const QString& name, const QString& kind, const QString& currency) {
    _presenter.openAccount(_ledgerId, name, kindFromText(kind), codeToCurrency(currency.toStdString()));
    emit busyChanged();
}

void LedgerQmlBridge::storeTransaction(const QString& fromAccountId, const QString& toAccountId, qlonglong amountMinor,
                                       const QString& description) {
    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;

    const auto from = idFromText<AccountId>(fromAccountId);
    const auto to = idFromText<AccountId>(toAccountId);
    // Minor units in, exact Rational out: cents are numerator over a
    // denominator of 1 at 2 decimal places, so nothing is ever rounded on the
    // way through this boundary. The scale is the gesture's own, not the
    // accounts' -- this view only knows account ids -- and `LedgerModel`
    // restates each leg onto its account currency's scale on arrival, so a
    // transfer between two zero-decimal accounts entered here as cents lands
    // as the whole units it divides into.
    constexpr std::uint32_t kMinorUnitPlaces = 2;
    const auto debit = morph::math::Rational{Numerator{-static_cast<std::int64_t>(amountMinor)}, Denominator{1},
                                             DecimalPlaces{kMinorUnitPlaces}};
    const auto credit = morph::math::Rational{Numerator{static_cast<std::int64_t>(amountMinor)}, Denominator{1},
                                              DecimalPlaces{kMinorUnitPlaces}};

    // One key per gesture, minted here and passed down -- see this class's
    // own doc comment for why the presenter never mints one.
    const ImportOpId opId{QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()};
    _presenter.storeTransaction(
        _ledgerId, description, morph::time::Timestamp::now(),
        {TransactionLeg{.accountId = from, .amount = debit}, TransactionLeg{.accountId = to, .amount = credit}}, opId);
    emit busyChanged();
}

void LedgerQmlBridge::undoTransaction(const QString& journalId) {
    _presenter.undoTransaction(_ledgerId, idFromText<JournalId>(journalId));
    emit busyChanged();
}

}  // namespace ledger::gui
