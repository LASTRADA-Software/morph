// examples/ledger/tests/test_ledger_model_keys.cpp
// SPDX-License-Identifier: Apache-2.0
//
// The three ledger models' primary keys, after morph#183 replaced their
// hand-written `ModelKeyTraits`/`ActionKeyTraits` specialisations with
// `BRIDGE_MODEL_KEY`/`BRIDGE_KEY_FROM`.
//
// Nothing here needs a database: `ActionKeyTraits<A>::key()` is a pure
// function over the action's own fields, and that is the whole point -- key
// extraction runs inside `BridgeHandler::execute()` *before* the model's own
// `validate()`/`execute()` ever sees the action, so it is the first and only
// place a malformed id can be caught.
//
// Two kinds of assertion live here, and they are deliberately not the same
// kind:
//
//   * **Equivalence pins.** `key()` must return the same bytes the
//     hand-written specialisation returned, for every id it accepted. These
//     pass before and after the migration by construction -- that *is* the
//     claim being made -- and each compares the generated key against the
//     exact expression the deleted body used, not against a re-derived
//     expectation.
//   * **Behaviour that changed.** An empty strong id is now refused by
//     `morph::model::keyToString` instead of unwrapped. The hand-written
//     bodies did `*action.ledgerId`, i.e. `operator*` on a disengaged
//     `std::optional` -- undefined behaviour that yielded a garbage key on a
//     plain libc++ and aborted the process under a hardened standard library.
//     Those assertions fail against the pre-migration header.

#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <cstdint>
#include <morph/core/model_key.hpp>
#include <stdexcept>
#include <string>

#include "ledger/core/types.hpp"
#include "ledger/models/budget_model.hpp"
#include "ledger/models/ledger_model.hpp"
#include "ledger/models/rule_model.hpp"

namespace {

/// @brief Ids the key encoding has to survive intact, including one past
///        `2^53` -- the range morph#286 had to repair elsewhere in the
///        ladder, and the one a key routed through a double would corrupt.
constexpr std::int64_t kIds[] = {1, 7, 4294967297, 9007199254740993};

}  // namespace

// ═════════════════════════════════════════════════════════════════════════
// LedgerModel — key type kept as the raw scalar, on purpose
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("LedgerModel keeps std::int64_t as its PrimaryKey because its keyed actions carry two id types",
          "[ledger][model][key]") {
    // A regression guard, not a migration check: this was `std::int64_t`
    // before morph#183 and still is. `BRIDGE_MODEL_KEY` would have deduced
    // `LedgerId` from `&OpenAccount::ledgerId`, which is the better default
    // -- but `GetReportStatus` keys this same model on a `ReportJobId`
    // (report_dto.hpp), so no single strong id is *the* key type here. The
    // model therefore declares the raw scalar in its own body, which
    // `PrimaryKeyOf` prefers over any deduced type (model_key.hpp's
    // `KeyTypeOf`), and every action uses `BRIDGE_KEY_FROM` purely for the
    // unwrapping. `primary()`/`instances()` keep returning
    // `std::int64_t`, so no call site had to move.
    CHECK((std::same_as<morph::model::PrimaryKeyOf<ledger::LedgerModel>, std::int64_t>));
}

TEST_CASE("Every LedgerModel action encodes exactly the key its hand-written specialisation did",
          "[ledger][model][key]") {
    for (const std::int64_t raw : kIds) {
        const ledger::LedgerId ledgerId{raw};

        // Right-hand sides are the deleted bodies, spelled out.
        const ledger::OpenAccount openAccount{.ledgerId = ledgerId};
        CHECK(morph::model::ActionKeyTraits<ledger::OpenAccount>::key(openAccount) ==
              morph::model::keyToString(*openAccount.ledgerId));

        const ledger::GetLedger getLedger{.ledgerId = ledgerId};
        CHECK(morph::model::ActionKeyTraits<ledger::GetLedger>::key(getLedger) ==
              morph::model::keyToString(*getLedger.ledgerId));

        ledger::StoreTransaction storeTransaction;
        storeTransaction.ledgerId = ledgerId;
        CHECK(morph::model::ActionKeyTraits<ledger::StoreTransaction>::key(storeTransaction) ==
              morph::model::keyToString(*storeTransaction.ledgerId));

        ledger::UndoTransaction undoTransaction;
        undoTransaction.ledgerId = ledgerId;
        CHECK(morph::model::ActionKeyTraits<ledger::UndoTransaction>::key(undoTransaction) ==
              morph::model::keyToString(*undoTransaction.ledgerId));

        ledger::ImportLedgerChunk importChunk;
        importChunk.ledgerId = ledgerId;
        CHECK(morph::model::ActionKeyTraits<ledger::ImportLedgerChunk>::key(importChunk) ==
              morph::model::keyToString(*importChunk.ledgerId));

        ledger::SubmitReport submitReport;
        submitReport.ledgerId = ledgerId;
        CHECK(morph::model::ActionKeyTraits<ledger::SubmitReport>::key(submitReport) ==
              morph::model::keyToString(*submitReport.ledgerId));

        // The one action keyed on something other than a ledger: its key is
        // the job's own id, in the same directory namespace. Unchanged by the
        // migration, and the reason `PrimaryKey` stays a raw scalar.
        ledger::GetReportStatus getReportStatus;
        getReportStatus.jobId = ledger::ReportJobId{raw};
        CHECK(morph::model::ActionKeyTraits<ledger::GetReportStatus>::key(getReportStatus) ==
              morph::model::keyToString(*getReportStatus.jobId));

        // And all of them agree with the canonical decimal encoding, so two
        // clients naming the same ledger through different actions land on
        // one instance.
        CHECK(morph::model::ActionKeyTraits<ledger::OpenAccount>::key(openAccount) == std::to_string(raw));
        CHECK(morph::model::ActionKeyTraits<ledger::GetLedger>::key(getLedger) == std::to_string(raw));
    }
}

TEST_CASE("SetCategory stays keyless: two co-equal ids and no natural single key", "[ledger][model][key]") {
    CHECK_FALSE(morph::model::ActionKeyTraits<ledger::SetCategory>::hasKey);
}

// ═════════════════════════════════════════════════════════════════════════
// BudgetModel — same call, same reason
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("BudgetModel keeps std::int64_t as its PrimaryKey: its actions key on LedgerId and BudgetId alike",
          "[ledger][model][key]") {
    CHECK((std::same_as<morph::model::PrimaryKeyOf<ledger::BudgetModel>, std::int64_t>));
}

TEST_CASE("Every BudgetModel action encodes exactly the key its hand-written specialisation did",
          "[ledger][model][key]") {
    for (const std::int64_t raw : kIds) {
        ledger::CreateCategory createCategory;
        createCategory.ledgerId = ledger::LedgerId{raw};
        CHECK(morph::model::ActionKeyTraits<ledger::CreateCategory>::key(createCategory) ==
              morph::model::keyToString(*createCategory.ledgerId));

        ledger::CreateBudget createBudget;
        createBudget.ledgerId = ledger::LedgerId{raw};
        CHECK(morph::model::ActionKeyTraits<ledger::CreateBudget>::key(createBudget) ==
              morph::model::keyToString(*createBudget.ledgerId));

        ledger::SetBudgetLimit setBudgetLimit;
        setBudgetLimit.budgetId = ledger::BudgetId{raw};
        CHECK(morph::model::ActionKeyTraits<ledger::SetBudgetLimit>::key(setBudgetLimit) ==
              morph::model::keyToString(*setBudgetLimit.budgetId));

        ledger::GetBudgetReport getBudgetReport;
        getBudgetReport.budgetId = ledger::BudgetId{raw};
        CHECK(morph::model::ActionKeyTraits<ledger::GetBudgetReport>::key(getBudgetReport) ==
              morph::model::keyToString(*getBudgetReport.budgetId));
    }
}

TEST_CASE("LinkAccountToCategory stays keyless", "[ledger][model][key]") {
    CHECK_FALSE(morph::model::ActionKeyTraits<ledger::LinkAccountToCategory>::hasKey);
}

// ═════════════════════════════════════════════════════════════════════════
// RuleModel — the one ledger model that adopts the strong id
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("RuleModel adopts LedgerId as its PrimaryKey", "[ledger][model][key]") {
    // Unlike the two above, every keyed action on this model (there is
    // exactly one, `CreateRule`) names a ledger, so `BRIDGE_MODEL_KEY`'s
    // deduced type is the right one and nothing overrides it. This assertion
    // fails against the pre-migration header, where the hand-written
    // `ModelKeyTraits<RuleModel>` declared `std::int64_t`.
    CHECK((std::same_as<morph::model::PrimaryKeyOf<ledger::RuleModel>, ledger::LedgerId>));
}

TEST_CASE("CreateRule encodes exactly the key its hand-written specialisation did", "[ledger][model][key]") {
    for (const std::int64_t raw : kIds) {
        ledger::CreateRule createRule;
        createRule.ledgerId = ledger::LedgerId{raw};
        CHECK(morph::model::ActionKeyTraits<ledger::CreateRule>::key(createRule) ==
              morph::model::keyToString(*createRule.ledgerId));
        CHECK(morph::model::ActionKeyTraits<ledger::CreateRule>::key(createRule) == std::to_string(raw));
    }
}

TEST_CASE("UpdateRule stays keyless: it carries a ruleId, not a ledgerId", "[ledger][model][key]") {
    CHECK_FALSE(morph::model::ActionKeyTraits<ledger::UpdateRule>::hasKey);
}

// ═════════════════════════════════════════════════════════════════════════
// The behaviour that changed: an empty id is refused, not unwrapped
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("An action carrying an empty id fails key extraction instead of routing to a garbage instance",
          "[ledger][model][key]") {
    // This is the failure the hand-written traits were exposed to and the one
    // morph#183 calls out by name. Their bodies were
    // `keyToString(*action.ledgerId)`, and `LedgerId::operator*` is
    // `return *value;` on a `std::optional` (core/types.hpp) -- undefined
    // behaviour for a disengaged id. On a plain libc++ that yields whatever
    // the union held and routes the caller to an arbitrary instance; under a
    // hardened standard library it aborts the process.
    //
    // The macro routes through `morph::model::keyToString`, which refuses an
    // empty strong id rather than unwrapping it, and
    // `BridgeHandler::execute()`'s `catch (...)` around key extraction turns
    // that into a rejected `Completion` the caller can see.
    CHECK_THROWS_AS(morph::model::ActionKeyTraits<ledger::OpenAccount>::key(ledger::OpenAccount{}),
                    std::runtime_error);
    CHECK_THROWS_AS(morph::model::ActionKeyTraits<ledger::GetLedger>::key(ledger::GetLedger{}), std::runtime_error);
    CHECK_THROWS_AS(morph::model::ActionKeyTraits<ledger::CreateRule>::key(ledger::CreateRule{}), std::runtime_error);
    CHECK_THROWS_AS(morph::model::ActionKeyTraits<ledger::CreateCategory>::key(ledger::CreateCategory{}),
                    std::runtime_error);
    CHECK_THROWS_AS(morph::model::ActionKeyTraits<ledger::SetBudgetLimit>::key(ledger::SetBudgetLimit{}),
                    std::runtime_error);
    CHECK_THROWS_AS(morph::model::ActionKeyTraits<ledger::GetReportStatus>::key(ledger::GetReportStatus{}),
                    std::runtime_error);
}

TEST_CASE("An engaged key round-trips back through keyFromString", "[ledger][model][key]") {
    // `primary()` and `instances()` decode with
    // `keyFromString<PrimaryKeyOf<M>>`, so the encoding has to be reversible
    // into whichever type each model declared.
    for (const std::int64_t raw : kIds) {
        const ledger::CreateRule createRule{.ledgerId = ledger::LedgerId{raw}};
        CHECK(morph::model::keyFromString<morph::model::PrimaryKeyOf<ledger::RuleModel>>(
                  morph::model::ActionKeyTraits<ledger::CreateRule>::key(createRule)) == ledger::LedgerId{raw});

        const ledger::OpenAccount openAccount{.ledgerId = ledger::LedgerId{raw}};
        CHECK(morph::model::keyFromString<morph::model::PrimaryKeyOf<ledger::LedgerModel>>(
                  morph::model::ActionKeyTraits<ledger::OpenAccount>::key(openAccount)) == raw);
    }
}
