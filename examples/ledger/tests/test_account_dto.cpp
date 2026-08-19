// examples/ledger/tests/test_account_dto.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/dto/account_dto.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("OpenAccount::validate rejects an empty name", "[ledger][dto]") {
    ledger::OpenAccount action{.ledgerId = ledger::LedgerId{1}, .name = "", .kind = ledger::AccountKind::Asset,
                                .currency = ledger::Currency::USD};
    CHECK_FALSE(action.validate());
}

TEST_CASE("OpenAccount::validate accepts a fully-engaged action", "[ledger][dto]") {
    ledger::OpenAccount action{.ledgerId = ledger::LedgerId{1}, .name = "Checking",
                                .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD};
    CHECK(action.validate());
}

TEST_CASE("GetLedger::validate rejects a disengaged ledgerId", "[ledger][dto]") {
    ledger::GetLedger action{.ledgerId = ledger::LedgerId{}};
    CHECK_FALSE(action.validate());
}
