// SPDX-License-Identifier: Apache-2.0
//
// Two-binary journal-path skew probe (issue #246).
//
// `examples/lims/README.md` asks for the executable form of the journal's
// data-at-rest contract: an *old* build records a journal, a *new* build reads
// it, and the shapes that changed in between must not decode to plausible
// defaults in silence. `tests/test_journal_payload_evolution.cpp` already
// simulates an older build *inside one process*, by declaring a struct it never
// registers — which proves the gate fires, but proves nothing about two
// separately compiled programs. This file is compiled twice, into two
// executables that never share a translation unit:
//
//   -DMORPH_SKEW_ROLE_OLD  writes the journal, using the old shapes
//   -DMORPH_SKEW_ROLE_NEW  replays it, using the new ones
//
// The old binary is *not* built with MORPH_CLIENT_ONLY. That macro suppresses
// the model-owning registrars, so a client-only binary cannot execute a model
// and therefore cannot journal anything — it is the right gate for the
// *wire*-path skew test, which needs a per-action fingerprint exchanged at
// `hello` before there is anything to assert against, and which issue #207
// tracks. This probe covers the journal path, which ships today.
//
// Three actions, chosen for the three distinct answers:
//
//   Skew_SetLabel    unchanged between the builds  -> replays faithfully
//   Skew_SetState    one field renamed             -> SchemaMismatchError
//   Skew_SetReading  one field added               -> SchemaMismatchError,
//                                                     recoverable by migration
//
// The additive case is deliberately asserted as a *throw*. The data-at-rest
// contract permits an additive field, and `fromJson`'s lenient decode still
// honours it — the migration case below decodes the old payload with the new
// struct and recovers the recorded value exactly. But `replay()`'s gate is
// fingerprint equality, not compatibility, so it refuses the entry rather than
// replaying it unverified, and the caller's answer is the registered
// migration. Asserting "additive round-trips" here would have been asserting
// something this build does not do.
//
// Exit code 0 means every assertion held. Any other value means at least one
// did, and the failing check is printed to stderr.

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/model.hpp>
#include <morph/core/payload_schema.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/file_action_log.hpp>
#include <morph/journal/journal.hpp>
#include <string>
#include <string_view>
#include <vector>

#if !defined(MORPH_SKEW_ROLE_OLD) && !defined(MORPH_SKEW_ROLE_NEW)
#error "journal_skew_probe.cpp: define exactly one of MORPH_SKEW_ROLE_OLD / MORPH_SKEW_ROLE_NEW"
#endif
#if defined(MORPH_SKEW_ROLE_OLD) && defined(MORPH_SKEW_ROLE_NEW)
#error "journal_skew_probe.cpp: define exactly one of MORPH_SKEW_ROLE_OLD / MORPH_SKEW_ROLE_NEW"
#endif

// ── The action shapes, as this binary's build knows them ─────────────────────

/// @brief Unchanged between the two builds: the no-false-positive case.
struct SkewSetLabel {
    std::string label;
};

#ifdef MORPH_SKEW_ROLE_OLD

/// @brief Old shape: the field is called `state`.
struct SkewSetState {
    std::string state;
};

/// @brief Old shape: one field.
struct SkewSetReading {
    std::string sampleId;
};

#else

/// @brief New shape: `state` was renamed to `stateCode`. Both halves of a
///        rename are invisible to a lenient decode — the old key is unknown
///        and dropped, the new one is absent and default-constructed.
struct SkewSetState {
    std::string stateCode;
};

/// @brief New shape: `unit` was added. Permitted by the data-at-rest contract,
///        and still a different payload shape.
struct SkewSetReading {
    std::string sampleId;
    std::string unit;
};

#endif

/// @brief The model both builds reconstruct.
struct SkewModel {
    /// @brief Last label recorded.
    std::string label;

    /// @brief Last state recorded, under whichever name this build spells it.
    std::string state;

    /// @brief Last sample id recorded.
    std::string sampleId;

    /// @brief Last unit recorded; only the new build has a field to carry it.
    std::string unit;

    /// @brief Applies a label change.
    /// @param action The action.
    /// @return The new label.
    std::string execute(const SkewSetLabel& action) {
        label = action.label;
        return label;
    }

    /// @brief Applies a state change.
    /// @param action The action.
    /// @return The new state.
    std::string execute(const SkewSetState& action) {
#ifdef MORPH_SKEW_ROLE_OLD
        state = action.state;
#else
        state = action.stateCode;
#endif
        return state;
    }

    /// @brief Applies a reading.
    /// @param action The action.
    /// @return The sample id.
    std::string execute(const SkewSetReading& action) {
        sampleId = action.sampleId;
#ifndef MORPH_SKEW_ROLE_OLD
        unit = action.unit;
#endif
        return sampleId;
    }
};

BRIDGE_REGISTER_MODEL(SkewModel, "Skew_Model")
BRIDGE_REGISTER_ACTION(SkewModel, SkewSetLabel, "Skew_SetLabel")
BRIDGE_REGISTER_ACTION(SkewModel, SkewSetState, "Skew_SetState")
BRIDGE_REGISTER_ACTION(SkewModel, SkewSetReading, "Skew_SetReading")

namespace {

/// @brief Journal file both roles agree on, inside the directory given as argv[1].
constexpr std::string_view kJournalName = "journal_skew.jsonl";

/// @brief Count of failed checks, reported as the process exit code.
int failures = 0;

/// @brief Records a check result, printing the failing ones.
/// @param ok   Whether the check held.
/// @param what What was being checked.
void check(bool ok, const char* what) {
    if (ok) {
        std::printf("ok: %s\n", what);
        return;
    }
    std::fprintf(stderr, "FAILED: %s\n", what);
    ++failures;
}

/// @brief A fresh registry/dispatcher pair with this build's three actions.
struct SkewFixture {
    /// @brief Model factory.
    morph::model::detail::ModelRegistryFactory registry;

    /// @brief Action dispatcher, carrying this build's fingerprints.
    morph::model::detail::ActionDispatcher dispatcher;

    /// @brief Registers the model and all three actions.
    SkewFixture() {
        registry.registerModel<SkewModel>("Skew_Model");
        dispatcher.registerAction<SkewModel, SkewSetLabel>("Skew_Model", "Skew_SetLabel");
        dispatcher.registerAction<SkewModel, SkewSetState>("Skew_Model", "Skew_SetState");
        dispatcher.registerAction<SkewModel, SkewSetReading>("Skew_Model", "Skew_SetReading");
    }
};

}  // namespace

#ifdef MORPH_SKEW_ROLE_OLD

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: journal_skew_old <directory>\n");
        return 2;
    }
    const std::filesystem::path journal = std::filesystem::path{argv[1]} / std::string{kJournalName};
    std::error_code ec;
    std::filesystem::remove(journal, ec);  // a stale file would replay the previous configure's shapes

    SkewFixture fixture;
    auto log = std::make_shared<morph::journal::FileActionLog>(journal);
    auto holder = fixture.registry.create("Skew_Model");
    holder->attachActionLog(log, "skew-1");

    fixture.dispatcher.dispatch("Skew_Model", "Skew_SetLabel", *holder, R"({"label":"alpha"})");
    fixture.dispatcher.dispatch("Skew_Model", "Skew_SetState", *holder, R"({"state":"quarantined"})");
    fixture.dispatcher.dispatch("Skew_Model", "Skew_SetReading", *holder, R"({"sampleId":"S-1"})");
    log->flush();

    // The whole point of the exercise is that these entries carry the shape
    // that wrote them. If this build recorded them unstamped, the new binary's
    // assertions would be measuring nothing at all, so it is checked here
    // rather than inferred there.
    const auto written = log->entries();
    check(written.size() == 3, "the old build recorded three entries");
    for (const auto& entry : written) {
        check(!entry.schema.empty(), "the old build stamped a fingerprint on every entry it wrote");
    }
    std::printf("old build wrote %s\n", journal.string().c_str());
    return failures == 0 ? 0 : 1;
}

#else

namespace {

/// @brief The one recorded entry for @p actionType.
/// @param entries All recorded entries.
/// @param actionType Action id to select.
/// @return The matching entry.
morph::journal::LogEntry entryFor(const std::vector<morph::journal::LogEntry>& entries, std::string_view actionType) {
    for (const auto& entry : entries) {
        if (entry.actionType == actionType) {
            return entry;
        }
    }
    std::fprintf(stderr, "FAILED: no recorded entry for %s\n", std::string{actionType}.c_str());
    std::exit(3);
}

/// @brief Whether replaying @p entry throws `SchemaMismatchError`.
/// @param fixture This build's registry/dispatcher.
/// @param entry   The entry to replay.
/// @return `true` when the mismatch gate fired.
bool throwsMismatch(SkewFixture& fixture, const morph::journal::LogEntry& entry) {
    try {
        morph::journal::replay("Skew_Model", {entry}, fixture.registry, fixture.dispatcher);
    } catch (const morph::journal::SchemaMismatchError&) {
        return true;
    } catch (const std::exception& err) {
        std::fprintf(stderr, "note: replay threw something else: %s\n", err.what());
        return false;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: journal_skew_new <directory>\n");
        return 2;
    }
    const std::filesystem::path journal = std::filesystem::path{argv[1]} / std::string{kJournalName};
    if (!std::filesystem::exists(journal)) {
        std::fprintf(stderr, "FAILED: %s does not exist; the old build did not run\n", journal.string().c_str());
        return 3;
    }

    SkewFixture fixture;
    const auto recorded = morph::journal::FileActionLog{journal}.entries();
    check(recorded.size() == 3, "the new build read three entries written by the old one");

    // ── 1. Unchanged action: no false positive across two binaries ───────────
    //
    // This is also the only direct evidence that the fingerprint is stable
    // across separate compilations at all -- an in-process test cannot show
    // it, because there is only ever one compilation.
    {
        const auto entry = entryFor(recorded, "Skew_SetLabel");
        check(entry.schema == morph::model::payloadFingerprint<SkewSetLabel>(),
              "an unchanged action fingerprints identically in two separately compiled binaries");
        auto holder = morph::journal::replay("Skew_Model", {entry}, fixture.registry, fixture.dispatcher);
        check(holder->into<SkewModel>().label == "alpha",
              "an unchanged action replays to the value that was recorded");
    }

    // ── 2. Renamed field: loud, not silently wrong ───────────────────────────
    {
        const auto entry = entryFor(recorded, "Skew_SetState");
        check(entry.schema != morph::model::payloadFingerprint<SkewSetState>(),
              "a renamed field makes the old build's fingerprint differ from this build's");
        check(throwsMismatch(fixture, entry), "a renamed field throws SchemaMismatchError instead of replaying");

        // What the stamp is *for*. The same recorded bytes, with the
        // fingerprint stripped, are exactly what a pre-#174 build wrote -- and
        // they replay without complaint into a state nobody ever recorded. If
        // this half also threw, the check above would be indistinguishable
        // from a harness that refuses everything.
        morph::journal::LogEntry unstamped = entry;
        unstamped.schema.clear();
        auto holder = morph::journal::replay("Skew_Model", {unstamped}, fixture.registry, fixture.dispatcher);
        check(holder->into<SkewModel>().state.empty(),
              "the same entry unstamped replays silently to a default -- the failure the stamp exists to catch");
        check(entry.payload.find("quarantined") != std::string::npos,
              "...even though the recorded payload plainly says 'quarantined'");
    }

    // ── 3. Additive field: also refused, and recoverable ─────────────────────
    {
        const auto entry = entryFor(recorded, "Skew_SetReading");
        check(throwsMismatch(fixture, entry),
              "an added field throws too: the gate is shape equality, not compatibility");

        // The decode underneath is still lenient, which is what the
        // data-at-rest contract actually promises. A migration that hands the
        // payload through unchanged proves it: the recorded sampleId survives,
        // and the field this build added defaults.
        morph::journal::PayloadMigrationRegistry migrations;
        migrations.add("Skew_SetReading", entry.schema, [](std::string_view payload) { return std::string{payload}; });
        auto holder = morph::journal::replay("Skew_Model", {entry}, fixture.registry, fixture.dispatcher, migrations);
        check(holder->into<SkewModel>().sampleId == "S-1",
              "an added field decodes the old payload faithfully once a migration admits it");
        check(holder->into<SkewModel>().unit.empty(), "...with the newly added field left at its default");
    }

    return failures == 0 ? 0 : 1;
}

#endif
