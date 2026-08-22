// SPDX-License-Identifier: Apache-2.0
#include "ledger_qml_bridge.hpp"

#include "ledger/core/units.hpp"

#include <QUuid>

#include <string>
#include <utility>

namespace ledger::gui {

namespace {

/// @brief A strong id as the plain number QML holds, or `-1` when
///        unengaged -- same convention as `kanban::gui::idNumber`.
/// @tparam IdT The strong-id type.
/// @param id The id to render.
/// @return The payload, or `-1`.
template <typename IdT>
[[nodiscard]] qlonglong idNumber(const IdT& id) {
    return id.hasValue() ? static_cast<qlonglong>(*id) : -1;
}

/// @brief Parses a plain-number id string back into a strong id.
/// @tparam IdT The strong-id type to build.
/// @param text The QML-side id text.
/// @return The parsed id, or a disengaged one when @p text is not a number --
///         the model refuses a disengaged id with a typed error, which the
///         presenter relays as `failed`, so this needs no throw of its own.
template <typename IdT>
[[nodiscard]] IdT idFromText(const QString& text) {
    bool ok = false;
    const auto value = text.toLongLong(&ok);
    return ok ? IdT{value} : IdT{};
}

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
/// @param account The account to render.
/// @return The QML-ready map.
[[nodiscard]] QVariantMap toVariantMap(const AccountInfo& account) {
    const auto& balance = account.balance;
    return QVariantMap{
        {"id", idNumber(account.id)},
        {"name", QString::fromStdString(account.name)},
        {"kind", kindToText(account.kind)},
        {"currency", QString::fromUtf8(currencyToCode(account.currency).data(),
                                       static_cast<qsizetype>(currencyToCode(account.currency).size()))},
        {"balanceNumerator", static_cast<qlonglong>(balance.numerator)},
        {"balanceDenominator", static_cast<qlonglong>(balance.denominator)},
        {"balanceDecimalPlaces", static_cast<qlonglong>(balance.decimalPlaces.value)},
    };
}

}  // namespace

LedgerQmlBridge::LedgerQmlBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                                 QObject* parent)
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
    connect(&_presenter, &LedgerPresenter::failed, this,
            [this](const QString& message) { publishError(message); });
    connect(&_presenter, &LedgerPresenter::idle, this, &LedgerQmlBridge::busyChanged);
}

bool LedgerQmlBridge::busy() const {
    return _presenter.busy();
}

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
    _presenter.openAccount(_ledgerId, name, kindFromText(kind),
                           codeToCurrency(currency.toStdString()));
    emit busyChanged();
}

void LedgerQmlBridge::storeTransaction(const QString& fromAccountId, const QString& toAccountId,
                                       qlonglong amountMinor, const QString& description) {
    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;

    const auto from = idFromText<AccountId>(fromAccountId);
    const auto to = idFromText<AccountId>(toAccountId);
    // Minor units in, exact Rational out: cents are numerator over a
    // denominator of 1 at 2 decimal places, so nothing is ever rounded on the
    // way through this boundary.
    constexpr std::uint32_t kMinorUnitPlaces = 2;
    const auto debit = morph::math::Rational{Numerator{-static_cast<std::int64_t>(amountMinor)}, Denominator{1},
                                             DecimalPlaces{kMinorUnitPlaces}};
    const auto credit = morph::math::Rational{Numerator{static_cast<std::int64_t>(amountMinor)}, Denominator{1},
                                              DecimalPlaces{kMinorUnitPlaces}};

    // One key per gesture, minted here and passed down -- see this class's
    // own doc comment for why the presenter never mints one.
    const ImportOpId opId{QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()};
    _presenter.storeTransaction(_ledgerId, description, morph::time::Timestamp::now(),
                                {TransactionLeg{.accountId = from, .amount = debit},
                                 TransactionLeg{.accountId = to, .amount = credit}},
                                opId);
    emit busyChanged();
}

void LedgerQmlBridge::undoTransaction(const QString& journalId) {
    _presenter.undoTransaction(_ledgerId, idFromText<JournalId>(journalId));
    emit busyChanged();
}

}  // namespace ledger::gui
