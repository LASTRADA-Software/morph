// examples/ledger/tests/test_account_dto.cpp
// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <string>

#include "ledger/dto/account_dto.hpp"

TEST_CASE("OpenAccount::validate rejects an empty name", "[ledger][dto]") {
    ledger::OpenAccount action{.ledgerId = ledger::LedgerId{1},
                               .name = "",
                               .kind = ledger::AccountKind::Asset,
                               .currency = ledger::Currency::USD};
    CHECK_FALSE(action.validate());
}

TEST_CASE("OpenAccount::validate accepts a fully-engaged action", "[ledger][dto]") {
    ledger::OpenAccount action{.ledgerId = ledger::LedgerId{1},
                               .name = "Checking",
                               .kind = ledger::AccountKind::Asset,
                               .currency = ledger::Currency::USD};
    CHECK(action.validate());
}

TEST_CASE("GetLedger::validate rejects a disengaged ledgerId", "[ledger][dto]") {
    ledger::GetLedger action{.ledgerId = ledger::LedgerId{}};
    CHECK_FALSE(action.validate());
}

TEST_CASE("CreateLedger::validate rejects an empty name", "[ledger][dto]") {
    const ledger::CreateLedger action{.name = ""};
    CHECK_FALSE(action.validate());
}

TEST_CASE("CreateLedger::validate rejects a name longer than the column holds", "[ledger][dto]") {
    // One byte past `kMaxLedgerNameBytes`. Over-length is refused rather than
    // truncated: `Light::SqlFixedString`'s constructor is noexcept and
    // truncates rather than throwing, so without this bound a caller would
    // get back a `CreateLedgerResult` naming a book whose stored name is not
    // the one they asked for.
    const ledger::CreateLedger tooLong{.name = std::string(ledger::kMaxLedgerNameBytes + 1, 'x')};
    CHECK_FALSE(tooLong.validate());

    const ledger::CreateLedger atTheBound{.name = std::string(ledger::kMaxLedgerNameBytes, 'x')};
    CHECK(atTheBound.validate());
}

TEST_CASE("CreateLedger::validate accepts an ordinary name", "[ledger][dto]") {
    const ledger::CreateLedger action{.name = "Personal"};
    CHECK(action.validate());
}
