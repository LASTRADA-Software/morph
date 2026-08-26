// SPDX-License-Identifier: Apache-2.0
//
// Coverage for this rung's payload-shape fingerprints
// (`morph::model::payloadShapeString` / `payloadFingerprint`,
// `morph/core/payload_schema.hpp`).
//
// Every one of ledger's strong ids carries its own `glz::meta` -- on the wire
// each *is* its nullable underlying scalar, which is what makes
// `BRIDGE_REGISTER_ACTION` on a DTO carrying one compile at all
// (`ledger/core/types.hpp`'s own `LEDGER_DEFINE_STRONG_ID_WIRE` note). The
// cost, stated in `morph/core/payload_shape_tag.hpp` and in
// `docs/spec/journal/journal.md`'s "Custom-codec types name themselves", is
// that a custom-codec type has no reflected members for `payloadShape` to
// decompose: absent a declared `morph::model::PayloadShapeTag` it renders as
// the bare opaque `x`, indistinguishable from every other such type.
//
// That is not cosmetic here. `UndoTransaction{LedgerId, JournalId}` and
// `SetCategory{AccountId, CategoryId, RuleId}` are all-id payloads, so without
// declared tags their whole fingerprint is a row of interchangeable `x`s:
// swapping two id fields' types -- exactly the edit an id rename or a
// copy-paste in a later rung produces -- leaves the fingerprint unchanged, and
// `journal::replay()`'s mismatch gate has nothing to fire on. The recorded
// integers then decode into the *wrong* ids, and the audit trail reports a
// reversal against a journal row nobody ever named.
//
// These cases pin the tags that close it, and the refusal that follows.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/core/payload_schema.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/journal.hpp>
#include <morph/session/session.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <vector>

#include "ledger/core/import_op_id.hpp"
#include "ledger/core/types.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/dto/transaction_dto.hpp"
#include "ledger/models/ledger_model.hpp"
#include "testkit/db_fixture.hpp"

using morph::model::payloadFingerprint;
using morph::model::payloadShapeString;

// A named namespace, not an anonymous one: glaze's traditional reflection
// derives member names from a pointer-to-member mangling that requires the
// reflected type to have linkage, so a payload struct declared in an anonymous
// namespace does not compile (`get_name.hpp`: "its type does not have
// linkage"). The name is this file's own, so the fixtures cannot collide with
// another translation unit's in the one ladder_ledger_tests binary.
namespace ledger_payload_shape_fixtures {

/// @brief `UndoTransaction` with its two id fields' *types* exchanged and the
///        member names left alone -- the shape a build that made that one edit
///        would stamp its entries with.
///
///        Never registered: it exists only to produce a fingerprint, which is
///        the whole of what a retained journal hands a later reader.
struct UndoTransactionIdsSwapped {
    ledger::JournalId ledgerId;
    ledger::LedgerId journalId;
};

/// @brief `SetCategory` with `categoryId` and `ruleId` exchanged, same idea.
struct SetCategoryIdsSwapped {
    ledger::AccountId accountId;
    ledger::RuleId categoryId;
    ledger::CategoryId ruleId;
    std::int32_t ruleVersion = 0;
};

}  // namespace ledger_payload_shape_fixtures

using ledger_payload_shape_fixtures::SetCategoryIdsSwapped;
using ledger_payload_shape_fixtures::UndoTransactionIdsSwapped;

namespace {

/// @brief A `Context` carrying only @p principal -- not a designated
///        initializer, for the reason `test_ledger_model.cpp`'s own `contextFor`
///        gives (`-Wmissing-designated-field-initializers` under
///        `-Weverything`).
[[nodiscard]] morph::session::Context contextFor(std::string principal) {
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    return ctx;
}

class ScopedPrincipal {
public:
    explicit ScopedPrincipal(std::string principal) : _ctx{contextFor(std::move(principal))}, _scope{_ctx} {}

private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};

}  // namespace

// ── The tags themselves ──────────────────────────────────────────────────────

TEST_CASE("Every ledger strong id renders a distinct payload shape", "[ledger][journal][payload_shape]") {
    INFO("LedgerId    -> " << payloadShapeString<ledger::LedgerId>());
    INFO("AccountId   -> " << payloadShapeString<ledger::AccountId>());
    INFO("JournalId   -> " << payloadShapeString<ledger::JournalId>());
    INFO("CategoryId  -> " << payloadShapeString<ledger::CategoryId>());
    INFO("BudgetId    -> " << payloadShapeString<ledger::BudgetId>());
    INFO("RuleId      -> " << payloadShapeString<ledger::RuleId>());
    INFO("ReportJobId -> " << payloadShapeString<ledger::ReportJobId>());
    INFO("ImportOpId  -> " << payloadShapeString<ledger::ImportOpId>());

    const std::vector<std::string> shapes{
        payloadShapeString<ledger::LedgerId>(),    payloadShapeString<ledger::AccountId>(),
        payloadShapeString<ledger::JournalId>(),   payloadShapeString<ledger::CategoryId>(),
        payloadShapeString<ledger::BudgetId>(),    payloadShapeString<ledger::RuleId>(),
        payloadShapeString<ledger::ReportJobId>(), payloadShapeString<ledger::ImportOpId>(),
    };

    // None of them may still be the bare opaque tag, and no two may collide.
    for (const auto& shape : shapes) {
        CHECK(shape != "x");
    }
    auto sorted = shapes;
    std::ranges::sort(sorted);
    CHECK(std::ranges::adjacent_find(sorted) == sorted.end());
}

// ── What the tags buy: the swaps the fingerprint can now see ─────────────────

TEST_CASE("UndoTransaction's ledgerId and journalId are not interchangeable", "[ledger][journal][payload_shape]") {
    INFO("UndoTransaction -> " << payloadShapeString<ledger::UndoTransaction>());
    INFO("ids swapped     -> " << payloadShapeString<UndoTransactionIdsSwapped>());

    CHECK(payloadShapeString<ledger::UndoTransaction>() != payloadShapeString<UndoTransactionIdsSwapped>());
    CHECK(payloadFingerprint<ledger::UndoTransaction>() != payloadFingerprint<UndoTransactionIdsSwapped>());
}

TEST_CASE("SetCategory's categoryId and ruleId are not interchangeable", "[ledger][journal][payload_shape]") {
    INFO("SetCategory -> " << payloadShapeString<ledger::SetCategory>());
    INFO("ids swapped -> " << payloadShapeString<SetCategoryIdsSwapped>());

    CHECK(payloadShapeString<ledger::SetCategory>() != payloadShapeString<SetCategoryIdsSwapped>());
    CHECK(payloadFingerprint<ledger::SetCategory>() != payloadFingerprint<SetCategoryIdsSwapped>());
}

// ── The refusal ──────────────────────────────────────────────────────────────

TEST_CASE("replay() refuses an UndoTransaction entry stamped by a build whose ids were swapped",
          "[ledger][journal][payload_shape]") {
    // A real recorded entry, not a hand-built one: this also pins that
    // LedgerModel::logAction() stamps the fingerprint this build computes.
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    const ScopedPrincipal principal{"alice"};
    const auto checkingId = model
                                .execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                                             .name = "Checking",
                                                             .kind = ledger::AccountKind::Asset,
                                                             .currency = ledger::Currency::USD})
                                .id;
    const auto groceriesId = model
                                 .execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                                              .name = "Groceries",
                                                              .kind = ledger::AccountKind::Expense,
                                                              .currency = ledger::Currency::USD})
                                 .id;

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    model.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Weekly shop",
        .date = morph::time::Timestamp::now(),
        .legs = {ledger::TransactionLeg{
                     .accountId = checkingId,
                     .amount = morph::math::Rational{Numerator{-5000}, Denominator{1}, DecimalPlaces{2}}},
                 ledger::TransactionLeg{
                     .accountId = groceriesId,
                     .amount = morph::math::Rational{Numerator{5000}, Denominator{1}, DecimalPlaces{2}}}}});

    auto journalRows = mapper.Query<ledger::db::TransactionJournalRecord>()
                           .Where(::Lightweight::FieldNameOf<&ledger::db::TransactionJournalRecord::description>, "=",
                                  Lightweight::SqlAnsiString<256>{"Weekly shop"})
                           .All();
    REQUIRE(journalRows.size() == 1);
    const auto journalId = ledger::JournalId{static_cast<std::int64_t>(journalRows.front().id.Value())};

    // Attach the log only now, so it holds the UndoTransaction entry and
    // nothing that had to happen first.
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    model.attachActionLog(log, std::to_string(*ledgerId));
    model.execute(ledger::UndoTransaction{.ledgerId = ledgerId, .journalId = journalId});

    auto entries = log->entries();
    const auto undoType = std::string{morph::model::ActionTraits<ledger::UndoTransaction>::typeId()};
    auto recorded = std::ranges::find_if(entries, [&](const auto& e) { return e.actionType == undoType; });
    REQUIRE(recorded != entries.end());
    REQUIRE(recorded->schema == payloadFingerprint<ledger::UndoTransaction>());

    // Re-stamp it as the swapped-id build would have. The payload bytes are
    // byte-identical either way -- two JSON integers under two field names --
    // so the fingerprint is the only evidence that the recorded `ledgerId` is
    // not this build's `ledgerId`, and replay() must refuse rather than decode
    // one id into the other's slot.
    std::vector<morph::journal::LogEntry> mismatched{*recorded};
    mismatched.front().schema = payloadFingerprint<UndoTransactionIdsSwapped>();
    REQUIRE_THROWS_AS(morph::journal::replay("LedgerModel", mismatched), morph::journal::SchemaMismatchError);
}
