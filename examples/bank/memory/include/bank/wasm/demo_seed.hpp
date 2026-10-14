// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

#include "bank/core/demo_hash.hpp"
#include "bank/core/types.hpp"
#include "bank/wasm/store.hpp"
#include "bank/wasm/store_ops.hpp"

/// @file
/// The hosted demo's data: what a browser build of bank starts with, the QML GUI's and the generic client's alike.

namespace bank::wasm {

/// @brief Seeds a demo user (password "demo1234"), two accounts, and an opening
///        balance directly in the in-memory store so the hosted demo has data.
inline void seedDemo() {
    auto& db = sharedDb();
    if (!db.users.all().empty()) {
        return;  // already seeded
    }
    UserRow user;
    user.username = "demo";
    user.displayName = "Demo User";
    user.passwordHash = demoHash(std::string{"demo"} + ":" + "demo1234" + ":morph-bank");
    const auto uid = db.users.insert(user);

    AccountRow checking;
    checking.userId = uid;
    checking.number = "DE00500700100200300400";
    checking.kind = static_cast<int>(AccountKind::Checking);
    checking.currency = static_cast<int>(Currency::EUR);
    checking.status = static_cast<int>(AccountStatus::Open);
    checking.overdraftMinor = 50000;
    const auto chkId = db.accounts.insert(checking);
    auto chkRow = *db.accounts.find(chkId);
    applyCredit(db, chkRow, 480000, TxnKind::Deposit, 0, "opening deposit");

    AccountRow savings;
    savings.userId = uid;
    savings.number = "DE00500700100900800700";
    savings.kind = static_cast<int>(AccountKind::Savings);
    savings.currency = static_cast<int>(Currency::EUR);
    savings.status = static_cast<int>(AccountStatus::Open);
    savings.interestBps = 150;
    db.accounts.insert(savings);
}

}  // namespace bank::wasm
