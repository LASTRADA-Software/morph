// examples/ledger/tests/test_ledger_types.cpp
// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "ledger/core/errors.hpp"
#include "ledger/core/types.hpp"

TEST_CASE("AccountId default-constructs empty and engages via explicit int64_t", "[ledger][types]") {
    ledger::AccountId empty;
    CHECK_FALSE(empty.hasValue());

    ledger::AccountId engaged{42};
    REQUIRE(engaged.hasValue());
    CHECK(*engaged == 42);
}

TEST_CASE("AccountId::fromOptional adopts the payload as-is", "[ledger][types]") {
    auto engaged = ledger::AccountId::fromOptional(std::optional<std::int64_t>{7});
    REQUIRE(engaged.hasValue());
    CHECK(*engaged == 7);

    auto empty = ledger::AccountId::fromOptional(std::nullopt);
    CHECK_FALSE(empty.hasValue());
}

TEST_CASE("AccountKind enumerators are distinct", "[ledger][types]") {
    CHECK(ledger::AccountKind::Asset != ledger::AccountKind::Expense);
    CHECK(ledger::AccountKind::Revenue != ledger::AccountKind::Liability);
}

TEST_CASE("ZeroSumViolation carries currency and message", "[ledger][errors]") {
    ledger::ZeroSumViolation err{"USD", "legs did not sum to zero"};
    CHECK(err.currencyCode == "USD");
    CHECK(std::string{err.what()}.find("USD") != std::string::npos);
}
