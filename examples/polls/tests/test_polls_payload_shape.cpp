// SPDX-License-Identifier: Apache-2.0
//
// Coverage for this rung's payload-shape fingerprints
// (`morph::model::payloadShapeString` / `payloadFingerprint`,
// `morph/core/payload_schema.hpp`).
//
// `OptionId`, `PollEventId`, `AdminToken`, and `ParticipantToken` all carry
// their own `glz::meta` -- on the wire each *is* its underlying scalar, which
// is what makes `BRIDGE_REGISTER_ACTION` on a DTO carrying one compile at all
// (`polls/core/types.hpp`'s own `glz::meta<polls::OptionId>` note). The cost,
// stated in `morph/core/payload_shape_tag.hpp` and in
// `docs/spec/journal/journal.md`'s "Custom-codec types name themselves", is
// that a custom-codec type has no reflected members for `payloadShape` to
// decompose: absent a declared `morph::model::PayloadShapeTag` it renders as
// the bare opaque `x`, indistinguishable from every other such type.
//
// That is not cosmetic here. `GetPollStateResult` carries both an `OptionId`
// (`finalizedOptionId`) and a `PollEventId` (`lastEventId`) as sibling
// fields, and `CreatePollResult` carries both an `AdminToken` and a
// `ParticipantToken` -- deliberately distinct types per `AdminToken`'s own
// doc comment ("never interchangeable"). Without declared tags, each pair
// renders as two interchangeable `x`s: swapping the two fields' types --
// exactly the edit a rename or a copy-paste produces -- would leave the
// fingerprint unchanged. These cases pin the tags that close it.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/core/payload_schema.hpp>
#include <string>
#include <vector>

#include "polls/core/types.hpp"
#include "polls/dto/poll_dto.hpp"

using morph::model::payloadFingerprint;
using morph::model::payloadShapeString;

// A named namespace, not an anonymous one: glaze's traditional reflection
// derives member names from a pointer-to-member mangling that requires the
// reflected type to have linkage, so a payload struct declared in an
// anonymous namespace does not compile -- see ledger's identical fixture
// namespace (`examples/ledger/tests/test_ledger_payload_shape.cpp`) for the
// same rationale, spelled out in full there.
namespace polls_payload_shape_fixtures {

/// @brief `GetPollStateResult`'s two id fields with their *types* exchanged
///        and the member names left alone -- the shape a build that made
///        that one edit would stamp its entries with.
///
///        Never registered: it exists only to produce a fingerprint, which
///        is the whole of what a retained journal hands a later reader.
struct PollStateIdsSwapped {
    polls::PollEventId finalizedOptionId;
    polls::OptionId lastEventId;
};

/// @brief `CreatePollResult` with `adminToken`/`participantToken` exchanged,
///        same idea.
struct CreatePollResultTokensSwapped {
    std::string pollId;
    polls::ParticipantToken adminToken;
    polls::AdminToken participantToken;
};

}  // namespace polls_payload_shape_fixtures

using polls_payload_shape_fixtures::CreatePollResultTokensSwapped;
using polls_payload_shape_fixtures::PollStateIdsSwapped;

// ── The tags themselves ──────────────────────────────────────────────────────

TEST_CASE("Every polls strong id/token renders a distinct payload shape", "[polls][journal][payload_shape]") {
    INFO("OptionId          -> " << payloadShapeString<polls::OptionId>());
    INFO("PollEventId       -> " << payloadShapeString<polls::PollEventId>());
    INFO("AdminToken        -> " << payloadShapeString<polls::AdminToken>());
    INFO("ParticipantToken  -> " << payloadShapeString<polls::ParticipantToken>());

    const std::vector<std::string> shapes{
        payloadShapeString<polls::OptionId>(),
        payloadShapeString<polls::PollEventId>(),
        payloadShapeString<polls::AdminToken>(),
        payloadShapeString<polls::ParticipantToken>(),
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

TEST_CASE("GetPollStateResult's finalizedOptionId and lastEventId are not interchangeable",
          "[polls][journal][payload_shape]") {
    INFO("GetPollStateResult -> " << payloadShapeString<polls::GetPollStateResult>());
    INFO("ids swapped        -> " << payloadShapeString<PollStateIdsSwapped>());

    CHECK(payloadShapeString<polls::GetPollStateResult>() != payloadShapeString<PollStateIdsSwapped>());
    CHECK(payloadFingerprint<polls::GetPollStateResult>() != payloadFingerprint<PollStateIdsSwapped>());
}

TEST_CASE("CreatePollResult's adminToken and participantToken are not interchangeable",
          "[polls][journal][payload_shape]") {
    INFO("CreatePollResult -> " << payloadShapeString<polls::CreatePollResult>());
    INFO("tokens swapped   -> " << payloadShapeString<CreatePollResultTokensSwapped>());

    CHECK(payloadShapeString<polls::CreatePollResult>() != payloadShapeString<CreatePollResultTokensSwapped>());
    CHECK(payloadFingerprint<polls::CreatePollResult>() != payloadFingerprint<CreatePollResultTokensSwapped>());
}
