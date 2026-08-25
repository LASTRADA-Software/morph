// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// One-stop include for every Lightweight entity record, pulled in *dependency
/// order* so that every referenced type is complete.
///
/// Why an aggregator: the entities form a relationship graph. A `BelongsTo<&T::id>`
/// needs `T` complete, and — crucially — any translation unit that *loads* a
/// record's relations (`DataMapper::Query`/`Create` configure on-demand loaders
/// for its `HasMany`/`BelongsTo` members) instantiates lambdas that reference
/// the related record types, so those must be complete at the call site too.
/// Including this header guarantees that, instead of every model guessing which
/// child entity headers it transitively needs.
///
/// ── Relationship map ─────────────────────────────────────────────────────────
///   UserRecord    1───* AccountRecord            (UserRecord::accounts)
///   AccountRecord 1───* CardRecord, LoanRecord,  (AccountRecord::cards/loans/
///                       TxnRecord, PaymentRecord   transactions/payments)
///   PayeeRecord   1───* PaymentRecord            (PayeeRecord::payments)
///   AccountRecord, PayeeRecord, CardRecord, LoanRecord, PaymentRecord,
///   BudgetRecord, NotificationRecord  *───1 UserRecord   (… ::user)
///   TxnRecord     *───1 AccountRecord (account) and *───0..1 AccountRecord
///                       (counterparty, nullable)
///
/// See each entity header for the member-ordering constraint that Lightweight's
/// index-based `HasMany` resolution imposes.

#include "bank/db/account_entity.hpp"
#include "bank/db/budget_entity.hpp"
#include "bank/db/card_entity.hpp"
#include "bank/db/loan_entity.hpp"
#include "bank/db/notification_entity.hpp"
#include "bank/db/payee_entity.hpp"
#include "bank/db/payment_entity.hpp"
#include "bank/db/txn_entity.hpp"
#include "bank/db/user_entity.hpp"
