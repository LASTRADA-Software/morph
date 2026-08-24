// SPDX-License-Identifier: Apache-2.0
//
// Coverage for journal payload evolution (issue #174): the payload schema
// fingerprint (morph::model::payloadFingerprint, core/payload_schema.hpp),
// LogEntry::schema, ActionDispatcher::schemaFor, and replay()'s mismatch gate
// with its migration seam.
//
// The defect this file exists for: replay decodes a stored payload with the
// *current* action struct through a lenient reader. Rename a field and the old
// key is dropped as unknown while the new one is default-constructed, so an
// entry that recorded "quarantined" reconstructs as "" — no error, no warning,
// and an audit trail that reports a state nobody ever recorded. See the
// "reproduction" test cases below, which pin both halves: the silence that
// remains for an unstamped (pre-fingerprint) entry, and the throw that now
// replaces it for a stamped one.

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <cstdint>
#include <map>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/model.hpp>
#include <morph/core/payload_schema.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/journal.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using morph::journal::InMemoryActionLog;
using morph::journal::LogEntry;
using morph::journal::PayloadMigrationRegistry;
using morph::journal::SchemaMismatchError;
using morph::journal::UnstampedPayloadPolicy;
using morph::model::payloadFingerprint;
using morph::model::payloadShapeString;

// ── The action, in the shape this build knows ────────────────────────────────
//
// PESetState is "today". PESetStateV1 is the shape an older build recorded
// with: same action, one field renamed (`state` -> `stateCode`). It is never
// registered — it exists only to produce (a) the payload bytes that build
// would have written and (b) the fingerprint it would have stamped, which is
// exactly the pair a retained journal hands to a newer reader.

struct PESetState {
    std::string stateCode;
};

struct PESetStateV1 {
    std::string state;
};

struct PEModel {
    std::string stateCode;
    std::string execute(const PESetState& action) {
        stateCode = action.stateCode;
        return stateCode;
    }
};

BRIDGE_REGISTER_MODEL(PEModel, "PE_Model")
BRIDGE_REGISTER_ACTION(PEModel, PESetState, "PE_SetState")

// ── Shape-rendering fixtures ─────────────────────────────────────────────────

struct PEShapeBase {
    std::string state;
    std::int32_t count = 0;
};
struct PEShapeRenamed {
    std::string stateCode;
    std::int32_t count = 0;
};
struct PEShapeReordered {
    std::int32_t count = 0;
    std::string state;
};
struct PEShapeRetyped {
    std::string state;
    double count = 0;
};
struct PEShapeAdded {
    std::string state;
    std::int32_t count = 0;
    bool flagged = false;
};
struct PEShapeNested {
    PEShapeBase inner;
    std::vector<std::int32_t> xs;
    std::optional<std::string> note;
    std::map<std::string, std::int32_t> lookup;
};

namespace {

/// Builds the entry a *previous* build of this program would have written for
/// `PESetStateV1{.state = value}` — the old payload bytes, stamped with the old
/// build's fingerprint.
LogEntry entryFromOldBuild(std::string_view value) {
    return LogEntry{
        .seq = 0,
        .modelType = "PE_Model",
        .entityKey = {},
        .actionType = "PE_SetState",
        .payload = R"({"state":")" + std::string{value} + R"("})",
        .schema = payloadFingerprint<PESetStateV1>(),
        .result = {},
        .principal = {},
        .timestampMs = 0,
    };
}

/// A fresh dispatcher/registry pair with PE_Model + PE_SetState registered, so
/// no test leans on whatever the process-wide singletons happen to hold.
struct PEFixture {
    morph::model::detail::ModelRegistryFactory registry;
    morph::model::detail::ActionDispatcher dispatcher;
    PEFixture() {
        registry.registerModel<PEModel>("PE_Model");
        dispatcher.registerAction<PEModel, PESetState>("PE_Model", "PE_SetState");
    }
};

}  // namespace

// ── payloadFingerprint: what it does and does not distinguish ────────────────

TEST_CASE("payloadFingerprint: a renamed field changes the fingerprint", "[journal][payload_evolution][issue174]") {
    REQUIRE(payloadShapeString<PEShapeBase>() == "(count:i4,state:s)");
    REQUIRE(payloadShapeString<PEShapeRenamed>() == "(count:i4,stateCode:s)");
    REQUIRE(payloadFingerprint<PEShapeBase>() != payloadFingerprint<PEShapeRenamed>());
}

TEST_CASE("payloadFingerprint: adding or retyping a field changes the fingerprint",
          "[journal][payload_evolution][issue174]") {
    REQUIRE(payloadFingerprint<PEShapeBase>() != payloadFingerprint<PEShapeAdded>());
    REQUIRE(payloadShapeString<PEShapeRetyped>() == "(count:f8,state:s)");
    REQUIRE(payloadFingerprint<PEShapeBase>() != payloadFingerprint<PEShapeRetyped>());
}

TEST_CASE("payloadFingerprint: reordering members does NOT change the fingerprint",
          "[journal][payload_evolution][issue174]") {
    // JSON objects are unordered and the decode matches by name, so moving a
    // field within the struct changes nothing about which bytes decode where.
    // Making the fingerprint order-sensitive would turn a cosmetic edit into a
    // replay break for every retained journal.
    REQUIRE(payloadShapeString<PEShapeReordered>() == payloadShapeString<PEShapeBase>());
    REQUIRE(payloadFingerprint<PEShapeReordered>() == payloadFingerprint<PEShapeBase>());
}

TEST_CASE("payloadFingerprint: renders nested shape and carries the scheme prefix",
          "[journal][payload_evolution][issue174]") {
    REQUIRE(payloadShapeString<PEShapeNested>() == "(inner:(count:i4,state:s),lookup:{s>i4},note:?s,xs:[i4])");
    // "<scheme>:<16 hex digits>" — 18 bytes, stable for the lifetime of the
    // process and identical on every build of these sources.
    const std::string& fingerprint = payloadFingerprint<PEShapeNested>();
    REQUIRE(fingerprint.size() == 18);
    REQUIRE(fingerprint.substr(0, 2) == "1:");
    REQUIRE(fingerprint == payloadFingerprint<PEShapeNested>());
}

// ── The stamp is written by the real execution path, not only by tests ───────

TEST_CASE("ActionDispatcher: a journaled execution stamps LogEntry::schema",
          "[journal][payload_evolution][issue174]") {
    PEFixture fixture;
    auto log = std::make_shared<InMemoryActionLog>();
    auto holder = fixture.registry.create("PE_Model");
    holder->attachActionLog(log, "e1");

    fixture.dispatcher.dispatch("PE_Model", "PE_SetState", *holder, R"({"stateCode":"quarantined"})");

    const auto entries = log->entries();
    REQUIRE(entries.size() == 1);
    REQUIRE(entries.front().schema == payloadFingerprint<PESetState>());
    REQUIRE_FALSE(entries.front().schema.empty());

    // Round-trips through the on-disk encoding, which is where it has to survive.
    const LogEntry decoded = morph::journal::fromJson(morph::journal::toJson(entries.front()));
    REQUIRE(decoded.schema == entries.front().schema);
}

TEST_CASE("ActionDispatcher::schemaFor: reports the registered fingerprint, empty for unknown pairs",
          "[journal][payload_evolution][issue174]") {
    PEFixture fixture;
    REQUIRE(fixture.dispatcher.schemaFor("PE_Model", "PE_SetState") == payloadFingerprint<PESetState>());
    REQUIRE(fixture.dispatcher.schemaFor("PE_Model", "PE_NoSuchAction").empty());
    REQUIRE(fixture.dispatcher.schemaFor("PE_NoSuchModel", "PE_SetState").empty());
}

// ── Reproduction ─────────────────────────────────────────────────────────────

TEST_CASE("journal::replay: an unstamped entry written against a renamed field still reconstructs the WRONG state",
          "[journal][payload_evolution][issue174]") {
    // This is the defect, exactly as it stands for entries already written
    // without a fingerprint. The entry plainly records "quarantined"; replay
    // reports "". Nothing throws, nothing warns, and the reconstruction is
    // indistinguishable from a faithful one.
    //
    // It is pinned rather than fixed because there is nothing to fix: an entry
    // with no fingerprint carries no evidence of the shape that wrote it, so no
    // check can be performed on it. UnstampedPayloadPolicy::Refuse (below) is
    // the only other honest answer, and it is not the default.
    PEFixture fixture;
    LogEntry entry = entryFromOldBuild("quarantined");
    entry.schema.clear();  // an entry written before LogEntry::schema existed

    auto holder = morph::journal::replay("PE_Model", {entry}, fixture.registry, fixture.dispatcher);

    REQUIRE(holder->into<PEModel>().stateCode.empty());  // <- recorded "quarantined", reports ""
}

TEST_CASE("journal::replay: a STAMPED entry written against a renamed field throws instead of reconstructing",
          "[journal][payload_evolution][issue174]") {
    // Same bytes, same rename — but the entry says which shape wrote it, so the
    // silence above becomes a signal.
    PEFixture fixture;
    const LogEntry entry = entryFromOldBuild("quarantined");
    REQUIRE(entry.schema != payloadFingerprint<PESetState>());

    REQUIRE_THROWS_AS(morph::journal::replay("PE_Model", {entry}, fixture.registry, fixture.dispatcher),
                      SchemaMismatchError);
}

TEST_CASE("journal::replay: the mismatch names the action and both fingerprints",
          "[journal][payload_evolution][issue174]") {
    PEFixture fixture;
    const LogEntry entry = entryFromOldBuild("quarantined");
    std::string what;
    try {
        morph::journal::replay("PE_Model", {entry}, fixture.registry, fixture.dispatcher);
    } catch (const SchemaMismatchError& err) {
        what = err.what();
    }
    REQUIRE(what.find("PE_Model/PE_SetState") != std::string::npos);
    REQUIRE(what.find(payloadFingerprint<PESetStateV1>()) != std::string::npos);
    REQUIRE(what.find(payloadFingerprint<PESetState>()) != std::string::npos);
}

// ── No false positives ───────────────────────────────────────────────────────

TEST_CASE("journal::replay: an entry stamped with this build's fingerprint replays normally",
          "[journal][payload_evolution][issue174]") {
    PEFixture fixture;
    auto log = std::make_shared<InMemoryActionLog>();
    auto recorder = fixture.registry.create("PE_Model");
    recorder->attachActionLog(log, "e1");
    fixture.dispatcher.dispatch("PE_Model", "PE_SetState", *recorder, R"({"stateCode":"released"})");

    auto holder = morph::journal::replay("PE_Model", log->entries(), fixture.registry, fixture.dispatcher);
    REQUIRE(holder->into<PEModel>().stateCode == "released");
}

TEST_CASE("journal::replay: an unregistered action still fails as 'unknown action', not as a schema mismatch",
          "[journal][payload_evolution][issue174]") {
    // schemaFor() returns empty for an unregistered pair. Reporting that as a
    // schema *change* would mislabel a missing registration.
    PEFixture fixture;
    LogEntry entry = entryFromOldBuild("quarantined");
    entry.actionType = "PE_NeverRegistered";
    try {
        morph::journal::replay("PE_Model", {entry}, fixture.registry, fixture.dispatcher);
        FAIL("expected replay to throw");
    } catch (const SchemaMismatchError&) {
        FAIL("expected 'unknown action', got a schema mismatch");
    } catch (const std::runtime_error& err) {
        REQUIRE(std::string{err.what()}.find("unknown action") != std::string::npos);
    }
}

// ── UnstampedPayloadPolicy ───────────────────────────────────────────────────

TEST_CASE("journal::replay: Refuse rejects an unstamped entry rather than replaying it unverified",
          "[journal][payload_evolution][issue174]") {
    PEFixture fixture;
    LogEntry entry = entryFromOldBuild("quarantined");
    entry.schema.clear();

    REQUIRE_THROWS_AS(
        morph::journal::replay("PE_Model", {entry}, fixture.registry, fixture.dispatcher,
                               morph::journal::defaultPayloadMigrations(), UnstampedPayloadPolicy::Refuse),
        SchemaMismatchError);
}

TEST_CASE("journal::replay: Refuse still accepts a correctly stamped entry",
          "[journal][payload_evolution][issue174]") {
    PEFixture fixture;
    auto log = std::make_shared<InMemoryActionLog>();
    auto recorder = fixture.registry.create("PE_Model");
    recorder->attachActionLog(log, "e1");
    fixture.dispatcher.dispatch("PE_Model", "PE_SetState", *recorder, R"({"stateCode":"released"})");

    auto holder = morph::journal::replay("PE_Model", log->entries(), fixture.registry, fixture.dispatcher,
                                         morph::journal::defaultPayloadMigrations(), UnstampedPayloadPolicy::Refuse);
    REQUIRE(holder->into<PEModel>().stateCode == "released");
}

// ── The migration seam ───────────────────────────────────────────────────────

TEST_CASE("journal::replay: a registered migration reconstructs the state that was actually recorded",
          "[journal][payload_evolution][issue174]") {
    PEFixture fixture;
    const LogEntry entry = entryFromOldBuild("quarantined");

    PayloadMigrationRegistry migrations;
    migrations.add("PE_SetState", payloadFingerprint<PESetStateV1>(), [](std::string_view payload) {
        // The rename, done honestly: decode the old shape, emit the new one.
        PESetStateV1 legacy{};
        static constexpr glz::opts kOpts{.null_terminated = false, .error_on_unknown_keys = false};
        if (glz::read<kOpts>(legacy, payload)) {
            throw std::runtime_error("PE_SetState migration: undecodable legacy payload");
        }
        return morph::model::ActionTraits<PESetState>::toJson(PESetState{.stateCode = legacy.state});
    });

    auto holder = morph::journal::replay("PE_Model", {entry}, fixture.registry, fixture.dispatcher, migrations);
    REQUIRE(holder->into<PEModel>().stateCode == "quarantined");
}

TEST_CASE("journal::replay: a migration registered for a different fingerprint does not apply",
          "[journal][payload_evolution][issue174]") {
    PEFixture fixture;
    const LogEntry entry = entryFromOldBuild("quarantined");

    PayloadMigrationRegistry migrations;
    migrations.add("PE_SetState", "1:0000000000000000",
                   [](std::string_view) { return std::string{R"({"stateCode":"wrong"})"}; });
    REQUIRE(migrations.size() == 1);
    REQUIRE(migrations.find("PE_SetState", payloadFingerprint<PESetStateV1>()) == nullptr);

    REQUIRE_THROWS_AS(morph::journal::replay("PE_Model", {entry}, fixture.registry, fixture.dispatcher, migrations),
                      SchemaMismatchError);
}

TEST_CASE("journal::replay: a migration never rewrites the stored entry", "[journal][payload_evolution][issue174]") {
    PEFixture fixture;
    std::vector<LogEntry> entries{entryFromOldBuild("quarantined")};
    const std::string recordedPayload = entries.front().payload;
    const std::string recordedSchema = entries.front().schema;

    PayloadMigrationRegistry migrations;
    migrations.add("PE_SetState", payloadFingerprint<PESetStateV1>(),
                   [](std::string_view) { return std::string{R"({"stateCode":"quarantined"})"}; });

    auto holder = morph::journal::replay("PE_Model", entries, fixture.registry, fixture.dispatcher, migrations);
    REQUIRE(holder->into<PEModel>().stateCode == "quarantined");
    // History is read, never upgraded on read.
    REQUIRE(entries.front().payload == recordedPayload);
    REQUIRE(entries.front().schema == recordedSchema);
}
