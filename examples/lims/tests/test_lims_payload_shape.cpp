// SPDX-License-Identifier: Apache-2.0
//
// Coverage for this rung's payload-shape fingerprints
// (`morph::model::payloadShapeString` / `payloadFingerprint`,
// `morph/core/payload_schema.hpp`).
//
// All eight of this rung's strong ids are generated with their own
// `glz::meta` (`LIMS_DEFINE_STRONG_ID_WIRE`, `lims/core/types.hpp`) -- on the
// wire each *is* its nullable underlying integer, which is what makes
// `BRIDGE_REGISTER_ACTION` on a DTO carrying one compile at all. The cost,
// stated in `morph/core/payload_shape_tag.hpp` and in
// `docs/spec/journal/journal.md`'s "Custom-codec types name themselves", is
// that a custom-codec type has no reflected members for `payloadShape` to
// decompose: absent a declared `morph::model::PayloadShapeTag` it renders as
// the bare opaque `x`, indistinguishable from every other such type.
//
// This rung is where that matters most, because its journal *is* the product:
// `SelfJournal` (`lims/core/self_journal.hpp`) stamps every entry so the
// regulatory trail can be replayed, and `GetAuditTrail` renders it back to a
// human. Every id is `std::optional<std::int64_t>` on the wire, so a retype
// between two of them -- `AnalysisId` for the `AnalysisVersionId` a reading
// was captured under, `ClientId` for the `SampleId` a queued capture names --
// produces byte-identical JSON that no decode on any path can catch. Absent
// declared tags the fingerprint is byte-identical too, so
// `journal::replay()`'s mismatch gate has nothing to fire on and the recorded
// integer decodes into the wrong slot: a reading attributed to the wrong
// analysis version is a wrong result with a valid-looking audit trail behind
// it, which is worse than no trail.
//
// These cases pin the tags that close it, and the refusal that follows.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/core/payload_schema.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/journal.hpp>
#include <string>
#include <vector>

#include "lims/core/types.hpp"
#include "lims/dto/offline_dto.hpp"
#include "lims/dto/result_dto.hpp"
#include "lims/dto/sample_dto.hpp"
#include "lims/dto/verification_dto.hpp"
#include "lims/models/sample_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;
using morph::model::payloadFingerprint;
using morph::model::payloadShapeString;

// A named namespace, not an anonymous one: glaze's traditional reflection
// derives member names from a pointer-to-member mangling that requires the
// reflected type to have linkage, so a payload struct declared in an anonymous
// namespace does not compile -- see ledger's identical fixture namespace
// (`examples/ledger/tests/test_ledger_payload_shape.cpp`) for the same
// rationale, spelled out in full there.
namespace lims_payload_shape_fixtures {

/// @brief `CaptureConcentration` with `analysisVersionId` *retyped* to
///        `SampleId`, so that `QueuedCaptureIdsSwapped` below can exchange
///        the two ids a queued capture carries. Member names are left alone.
///
///        Never registered: it exists only to produce a fingerprint, which is
///        the whole of what a retained journal hands a later reader.
struct CaptureConcentrationIdRetyped {
    lims::SampleId analysisVersionId;
    lims::Concentration value;
    lims::QualifierChoice qualifier;
    lims::DilutionChoice dilution;
    lims::DilutionFactor dilutionFactor;
};

/// @brief `QueuedCapture` with its own `sampleId` and the
///        `analysisVersionId` nested inside its `capture` exchanged -- the
///        shape a build that made that one edit would stamp its entries with.
struct QueuedCaptureIdsSwapped {
    lims::AnalysisVersionId sampleId;
    lims::SampleVersion baseVersion;
    std::string capturedBy;
    lims::OperationKey operationKey;
    CaptureConcentrationIdRetyped capture;
};

/// @brief `VerifyResult` with its `resultId` retyped to `SampleId` -- "verify
///        this sample" for "verify this result", the four-eyes action's one
///        plausible confusion.
struct VerifyResultIdRetyped {
    lims::SampleId resultId;
};

/// @brief `RegisterSample` with its `clientId` retyped to `SampleId`, same
///        idea: the lifecycle's entry point names the owning client, in a
///        rung where nearly every other action's id names a sample.
struct RegisterSampleClientIdRetyped {
    lims::SampleId clientId;
    std::string reference;
};

}  // namespace lims_payload_shape_fixtures

using lims_payload_shape_fixtures::QueuedCaptureIdsSwapped;
using lims_payload_shape_fixtures::RegisterSampleClientIdRetyped;
using lims_payload_shape_fixtures::VerifyResultIdRetyped;

// ── The tags themselves ──────────────────────────────────────────────────────

TEST_CASE("Every lims strong id renders a distinct payload shape", "[lims][journal][payload_shape]") {
    INFO("ClientId          -> " << payloadShapeString<lims::ClientId>());
    INFO("SampleId          -> " << payloadShapeString<lims::SampleId>());
    INFO("AnalysisId        -> " << payloadShapeString<lims::AnalysisId>());
    INFO("AnalysisVersionId -> " << payloadShapeString<lims::AnalysisVersionId>());
    INFO("ResultId          -> " << payloadShapeString<lims::ResultId>());
    INFO("WorksheetId       -> " << payloadShapeString<lims::WorksheetId>());
    INFO("VerificationId    -> " << payloadShapeString<lims::VerificationId>());
    INFO("ConflictId        -> " << payloadShapeString<lims::ConflictId>());

    const std::vector<std::string> shapes{
        payloadShapeString<lims::ClientId>(),       payloadShapeString<lims::SampleId>(),
        payloadShapeString<lims::AnalysisId>(),     payloadShapeString<lims::AnalysisVersionId>(),
        payloadShapeString<lims::ResultId>(),       payloadShapeString<lims::WorksheetId>(),
        payloadShapeString<lims::VerificationId>(), payloadShapeString<lims::ConflictId>(),
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

TEST_CASE("QueuedCapture's sampleId and its capture's analysisVersionId are not interchangeable",
          "[lims][journal][payload_shape]") {
    INFO("QueuedCapture -> " << payloadShapeString<lims::QueuedCapture>());
    INFO("ids swapped   -> " << payloadShapeString<QueuedCaptureIdsSwapped>());

    CHECK(payloadShapeString<lims::QueuedCapture>() != payloadShapeString<QueuedCaptureIdsSwapped>());
    CHECK(payloadFingerprint<lims::QueuedCapture>() != payloadFingerprint<QueuedCaptureIdsSwapped>());
}

TEST_CASE("VerifyResult's resultId is not interchangeable with a sample id", "[lims][journal][payload_shape]") {
    INFO("VerifyResult -> " << payloadShapeString<lims::VerifyResult>());
    INFO("id retyped   -> " << payloadShapeString<VerifyResultIdRetyped>());

    CHECK(payloadShapeString<lims::VerifyResult>() != payloadShapeString<VerifyResultIdRetyped>());
    CHECK(payloadFingerprint<lims::VerifyResult>() != payloadFingerprint<VerifyResultIdRetyped>());
}

// ── The refusal ──────────────────────────────────────────────────────────────

TEST_CASE("replay() refuses a RegisterSample entry stamped by a build whose clientId was a sample id",
          "[lims][journal][payload_shape]") {
    // A real recorded entry, not a hand-built one: this also pins that
    // SelfJournal stamps the fingerprint this build computes.
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();

    lims::SampleModel model;
    model.attachActionLog(log, std::string{});
    const auto client = model.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    const auto sample = model.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-2026-0001"});
    REQUIRE(sample.id.hasValue());

    auto entries = log->entries(std::to_string(*sample.id));
    const auto registerType = std::string{morph::model::ActionTraits<lims::RegisterSample>::typeId()};
    auto recorded = std::ranges::find_if(entries, [&](const auto& e) { return e.actionType == registerType; });
    REQUIRE(recorded != entries.end());
    REQUIRE(recorded->schema == payloadFingerprint<lims::RegisterSample>());

    // Re-stamp it as the retyped build would have. The payload bytes are
    // byte-identical either way -- one JSON integer and one string under two
    // field names -- so the fingerprint is the only evidence that the recorded
    // `clientId` is not this build's `ClientId`, and replay() must refuse
    // rather than register a sample against whichever client happens to share
    // that row id.
    std::vector<morph::journal::LogEntry> mismatched{*recorded};
    mismatched.front().schema = payloadFingerprint<RegisterSampleClientIdRetyped>();
    REQUIRE_THROWS_AS(morph::journal::replay("SampleModel", mismatched), morph::journal::SchemaMismatchError);
}
