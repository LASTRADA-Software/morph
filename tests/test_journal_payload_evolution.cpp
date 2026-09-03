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

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <map>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/model.hpp>
#include <morph/core/payload_schema.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/journal.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <morph/util/tagged.hpp>
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

// ── Custom-codec fixtures (issue #245) ───────────────────────────────────────
//
// A unit system of this file's own: `UnitTraits` is specialised per enum, and
// two test translation units linking into one binary must not both specialise
// it for the same enum type.

namespace pe_units {

enum class Unit : std::uint16_t { gram, litre };

}  // namespace pe_units

/// @brief Unit metadata for this file's fixture unit enum.
template <>
struct morph::units::UnitTraits<pe_units::Unit> {
    /// @brief Static description of @p unit.
    /// @param unit The unit to describe.
    /// @return Its ascii id, display text, and default decimals.
    static constexpr morph::units::UnitMeta meta(pe_units::Unit unit) noexcept {
        switch (unit) {
            case pe_units::Unit::gram:
                return {"g", "g", 3};
            case pe_units::Unit::litre:
                return {"litre", "L", 3};
            default:
                return {"?", "?", 3};
        }
    }

    /// @brief No within-dimension conversions are needed by these fixtures.
    static constexpr std::array<morph::units::UnitRelation<pe_units::Unit>, 0> relations{};

    /// @brief No alternative spellings are needed by these fixtures.
    static constexpr std::array<morph::units::UnitAlternative<pe_units::Unit>, 0> alternatives{};
};

struct PESpecialFields {
    morph::math::Rational amount;
    morph::time::Timestamp at;
    morph::time::DateTime when;
    morph::util::Tagged<std::string, "acct"> id;
};

// The retype #245 filed: two custom-codec fields swapped for one another.
struct PESpecialFieldsRetyped {
    morph::time::Timestamp amount;
    morph::math::Rational at;
    morph::time::DateTime when;
    morph::util::Tagged<std::string, "acct"> id;
};

struct PEGrams {
    morph::units::Quantity<pe_units::Unit::gram> mass;
};
struct PELitres {
    morph::units::Quantity<pe_units::Unit::litre> mass;
};

struct PETaggedAcct {
    morph::util::Tagged<std::string, "acct"> id;
};
struct PETaggedUser {
    morph::util::Tagged<std::string, "user"> id;
};
struct PETaggedAcctInt {
    morph::util::Tagged<std::int64_t, "acct"> id;
};

struct PEPlainAmount {
    std::string amount;
};
struct PERationalAmount {
    morph::math::Rational amount;
};

// A custom-codec type that declares no PayloadShapeTag: the residual boundary.
struct PEOpaqueBlob {
    std::string bytes;
};

struct PEUndeclaredCodec {
    PEOpaqueBlob blob;
};

/// @brief A custom codec for `PEOpaqueBlob`, so it is not reflected over.
template <>
struct glz::meta<PEOpaqueBlob> {
    /// @brief The single wire field, named as a value rather than an object.
    static constexpr auto value = &PEOpaqueBlob::bytes;
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

// ── The recursion limit, read at its own edge ────────────────────────────────
//
// `payloadShape` stops decomposing and emits the opaque tag once `depth` is
// *past* `kPayloadShapeMaxDepth`, so the deepest level that still renders its
// own structure is `kPayloadShapeMaxDepth` itself. That cut-off is what keeps a
// self-referential payload (`struct Node { std::vector<Node> kids; }` is a legal
// Glaze payload) fingerprinting finitely, and it decides what such a type
// fingerprints *as* -- but every other shape case in this file nests two or
// three levels, far enough from the edge that moving the comparison by one
// changes no rendering at all.
//
// Each `std::vector<>` wrapper costs exactly one level: `payloadShape<vector<T>>
// (d)` renders `[` + `payloadShape<T>(d + 1)` + `]`, and `payloadShapeString`
// enters at depth 0, so the leaf of an N-deep chain is rendered at depth N.
using PEDepthAtLimit = std::vector<
    std::vector<std::vector<std::vector<std::vector<std::vector<std::vector<std::vector<std::int32_t>>>>>>>>;
using PEDepthPastLimit = std::vector<PEDepthAtLimit>;

TEST_CASE("payloadShapeString: the deepest level that still renders is kPayloadShapeMaxDepth itself",
          "[journal][payload_evolution][issue174]") {
    // The literals below are written for a limit of 8; if the constant moves,
    // they are what has to move with it.
    STATIC_REQUIRE(morph::model::detail::kPayloadShapeMaxDepth == 8);

    // Leaf at depth 8 -- at the limit, so still decomposed into its own tag.
    REQUIRE(payloadShapeString<PEDepthAtLimit>() == "[[[[[[[[i4]]]]]]]]");
    // Leaf at depth 9 -- one past it, so the opaque tag stands in and the
    // recursion terminates rather than following the type any further.
    REQUIRE(payloadShapeString<PEDepthPastLimit>() == "[[[[[[[[[x]]]]]]]]]");
    // Which means the two are distinguishable: truncation is not a collision.
    REQUIRE(payloadFingerprint<PEDepthAtLimit>() != payloadFingerprint<PEDepthPastLimit>());
}

TEST_CASE("payloadFingerprint: renders nested shape and carries the scheme prefix",
          "[journal][payload_evolution][issue174]") {
    REQUIRE(payloadShapeString<PEShapeNested>() == "(inner:(count:i4,state:s),lookup:{s>i4},note:?s,xs:[i4])");
    // "<scheme>:<16 hex digits>" — 18 bytes, stable for the lifetime of the
    // process and identical on every build of these sources.
    const std::string& fingerprint = payloadFingerprint<PEShapeNested>();
    REQUIRE(fingerprint.size() == 18);
    REQUIRE(fingerprint.substr(0, 2) == "2:");
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

// ── Custom-codec types are distinguished from one another (issue #245) ───────
//
// Every type carrying its own `glz::meta` used to render as the single opaque
// tag `x`, so swapping `Rational` for `Timestamp` in a recorded action changed
// nothing about the fingerprint and `replay()`'s gate could not fire. Each
// such type now declares a stable name via `morph::model::PayloadShapeTag`
// (`core/payload_shape_tag.hpp`), spelled in these sources rather than derived
// from `glz::name_v`, which is compiler-dependent.

TEST_CASE("payloadShapeString: a custom-codec type renders its declared name, not the bare opaque tag",
          "[journal][payload_evolution][issue245]") {
    REQUIRE(payloadShapeString<PESpecialFields>() ==
            "(amount:x{rational},at:x{timestamp},id:x{tagged.acct:s},when:x{datetime})");
}

TEST_CASE("payloadFingerprint: a retype between two custom-codec types is caught",
          "[journal][payload_evolution][issue245]") {
    // `Rational` and `Timestamp` swapped. Both encode as a custom Glaze codec,
    // so before this the two structs rendered identically -- `(amount:x,at:x,
    // id:x,when:x)` -- and a journal recorded by one build replayed silently
    // against the other.
    REQUIRE(payloadShapeString<PESpecialFields>() != payloadShapeString<PESpecialFieldsRetyped>());
    REQUIRE(payloadFingerprint<PESpecialFields>() != payloadFingerprint<PESpecialFieldsRetyped>());
}

TEST_CASE("payloadFingerprint: a Quantity unit swap is caught, and nothing else can catch it",
          "[journal][payload_evolution][issue245]") {
    // Neither the unit nor the declared precision travels on the wire (a
    // Quantity *is* its nullable Rational payload), so these two produce
    // byte-identical JSON. No decode can tell them apart; the fingerprint is
    // the only place the swap is visible at all.
    REQUIRE(payloadShapeString<PEGrams>() == "(mass:x{quantity.g.3})");
    REQUIRE(payloadShapeString<PELitres>() == "(mass:x{quantity.litre.3})");
    REQUIRE(payloadFingerprint<PEGrams>() != payloadFingerprint<PELitres>());
}

TEST_CASE("payloadFingerprint: Tagged carries both its tag text and the wrapped shape",
          "[journal][payload_evolution][issue245]") {
    // The tag never travels either, so `Tagged<std::string, "acct">` and
    // `Tagged<std::string, "user">` are the same bytes on the wire. The
    // wrapped type, on the other hand, genuinely changes them -- so it is
    // rendered inside the tag rather than hidden behind it.
    REQUIRE(payloadShapeString<PETaggedAcct>() == "(id:x{tagged.acct:s})");
    REQUIRE(payloadShapeString<PETaggedUser>() == "(id:x{tagged.user:s})");
    REQUIRE(payloadShapeString<PETaggedAcctInt>() == "(id:x{tagged.acct:i8})");
    REQUIRE(payloadFingerprint<PETaggedAcct>() != payloadFingerprint<PETaggedUser>());
    REQUIRE(payloadFingerprint<PETaggedAcct>() != payloadFingerprint<PETaggedAcctInt>());
}

TEST_CASE("payloadShapeString: a type that declares no tag still renders as the bare opaque tag",
          "[journal][payload_evolution][issue245]") {
    // The seam is opt-in, and therefore incomplete by construction: this is
    // the residual boundary the spec states, pinned so that it is a recorded
    // decision rather than an assumption.
    REQUIRE(payloadShapeString<PEUndeclaredCodec>() == "(blob:x)");
    // A custom-codec type swapped for a plain one was always caught, and
    // still is: a declared name versus `s`, not `x` versus `x`.
    REQUIRE(payloadFingerprint<PEPlainAmount>() != payloadFingerprint<PERationalAmount>());
}

TEST_CASE("payloadFingerprint: the scheme prefix marks the rendering change, so an old stamp reads as one",
          "[journal][payload_evolution][issue245]") {
    // Rendering a declared name where scheme 1 rendered `x` changes the
    // fingerprint of every payload with such a member. That is what the scheme
    // prefix is for: an entry stamped `1:` is legibly the product of a
    // different algorithm, not of a different payload.
    REQUIRE(morph::model::kPayloadFingerprintScheme == 2);
    REQUIRE(payloadFingerprint<PESpecialFields>().substr(0, 2) == "2:");
}
