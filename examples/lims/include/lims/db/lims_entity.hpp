// SPDX-License-Identifier: Apache-2.0
#pragma once

// Same include as bookmarks::db::ImportedOpRecord and ledger's own entity
// header: the narrower Field.hpp/BelongsTo.hpp pair does not transitively
// provide Light::SqlAnsiString/SqlRealName/PrimaryKey.
#include <Lightweight/DataMapper/DataMapper.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace lims::db {

struct ClientRecord {
    static constexpr std::string_view TableName = "lims_clients";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                              // 1
};

/// @brief An analysis definition's *identity*, stable across versions.
///
/// Deliberately holds no schema fields. Everything a version can change
/// (unit, precision, spec range, detection limits) lives on
/// `AnalysisVersionRecord`, so an old result can keep pointing at the exact
/// version it was captured under — the ODK form-version model the README
/// asks for.
struct AnalysisRecord {
    static constexpr std::string_view TableName = "lims_analyses";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                              // 1
};

/// @brief One immutable version of an analysis definition.
///
/// Rows are never updated after insert: editing an analysis appends version
/// N+1. That is what makes "old results stay bound to their version"
/// mechanically true rather than a convention.
struct AnalysisVersionRecord {
    static constexpr std::string_view TableName = "lims_analysis_versions";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&AnalysisRecord::id, Light::SqlRealName{"analysis_id"}> analysis;                     // 1
    Light::Field<std::int32_t, Light::SqlRealName{"version"}> version{1};                                  // 2
    /// @brief `UnitMeta::id` of the canonical unit results are stored in.
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"canonical_unit"}> canonicalUnit;  // 3
    Light::Field<int, Light::SqlRealName{"decimal_places"}> decimalPlaces{3};                    // 4
    /// @brief Specification range, as exact rationals. Nullable: not every
    ///        analysis has a spec.
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"spec_low_num"}> specLowNum;    // 5
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"spec_low_den"}> specLowDen;    // 6
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"spec_high_num"}> specHighNum;  // 7
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"spec_high_den"}> specHighDen;  // 8
    /// @brief Limit of detection / upper detection limit, as exact rationals.
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"lod_num"}> lodNum;  // 9
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"lod_den"}> lodDen;  // 10
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"udl_num"}> udlNum;  // 11
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"udl_den"}> udlDen;  // 12
    /// @brief Epoch millis this version was created, for the audit trail.
    Light::Field<std::int64_t, Light::SqlRealName{"created_at"}> createdAt{0};  // 13
};

struct SampleRecord {
    static constexpr std::string_view TableName = "lims_samples";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&ClientRecord::id, Light::SqlRealName{"client_id"}> client;                           // 1
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"reference"}> reference;                     // 2
    /// @brief `SampleState` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"state"}> state{0};  // 3
    /// @brief Monotonic base version, bumped on every mutating transition.
    ///
    /// This is what an offline update carries and what replay compares
    /// against to detect a stale base (README build order §7).
    Light::Field<std::int32_t, Light::SqlRealName{"version"}> version{1};       // 4
    Light::Field<std::int64_t, Light::SqlRealName{"registered_at"}> registeredAt{0};  // 5
};

/// @brief An analysis assigned to a sample, and the result captured for it.
struct ResultRecord {
    static constexpr std::string_view TableName = "lims_results";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&SampleRecord::id, Light::SqlRealName{"sample_id"}> sample;                           // 1
    /// @brief The exact analysis *version* this result was captured under.
    ///        Never the analysis identity: that is the whole point.
    Light::BelongsTo<&AnalysisVersionRecord::id, Light::SqlRealName{"analysis_version_id"}> analysisVersion;  // 2
    /// @brief `ResultQualifier` as its underlying integer. Distinguishes the
    ///        three "no number" meanings the README requires to stay
    ///        distinguishable through wire, journal and offline payloads.
    Light::Field<int, Light::SqlRealName{"qualifier"}> qualifier{0};  // 3
    /// @brief The reading, in the version's canonical unit, as an exact
    ///        rational. Null for every qualifier except `Measured`.
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"value_num"}> valueNum;  // 4
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"value_den"}> valueDen;  // 5
    Light::Field<int, Light::SqlRealName{"value_dp"}> valueDp{0};                         // 6
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"captured_by"}> capturedBy;  // 7
    Light::Field<std::int64_t, Light::SqlRealName{"captured_at"}> capturedAt{0};           // 8
};

/// @brief A four-eyes verification of one result.
///
/// Separate row rather than a flag on `ResultRecord` so the verifier and the
/// capturer are both first-class, and so a verification cannot be silently
/// overwritten by a later result edit.
struct VerificationRecord {
    static constexpr std::string_view TableName = "lims_verifications";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&ResultRecord::id, Light::SqlRealName{"result_id"}> result;                           // 1
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"verified_by"}> verifiedBy;                  // 2
    Light::Field<std::int64_t, Light::SqlRealName{"verified_at"}> verifiedAt{0};                           // 3
};

/// @brief A queued field update replay could not apply, held for a human.
///
/// The README's ODK answer: a stale-base update is **flagged**, never silently
/// merged and never silently dropped. The row keeps the queued payload
/// verbatim so the resolver can see exactly what the field client meant, and
/// both versions so they can see exactly how far it had drifted.
struct OfflineConflictRecord {
    static constexpr std::string_view TableName = "lims_offline_conflicts";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&SampleRecord::id, Light::SqlRealName{"sample_id"}> sample;                           // 1
    /// @brief The sample version the queued update was prepared against.
    Light::Field<std::int32_t, Light::SqlRealName{"base_version"}> baseVersion{0};  // 2
    /// @brief The sample version the server actually held at replay time.
    Light::Field<std::int32_t, Light::SqlRealName{"server_version"}> serverVersion{0};  // 3
    /// @brief `ConflictReason` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"reason"}> reason{0};  // 4
    /// @brief `ConflictStatus` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"status"}> status{0};  // 5
    /// @brief The queued payload, verbatim, as the field client serialised it.
    ///
    /// Deliberately stored as text rather than re-encoded from a decoded
    /// struct: re-encoding would silently normalise away anything the current
    /// action struct no longer understands, which is the exact journal-payload
    /// evolution failure the rung README warns about.
    Light::Field<Light::SqlDynamicAnsiString<4096>, Light::SqlRealName{"payload"}> payload;  // 6
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"detected_by"}> detectedBy;    // 7
    Light::Field<std::int64_t, Light::SqlRealName{"detected_at"}> detectedAt{0};             // 8
    /// @brief Who resolved it, empty while the conflict is still open.
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"resolved_by"}> resolvedBy;  // 9
    Light::Field<std::int64_t, Light::SqlRealName{"resolved_at"}> resolvedAt{0};           // 10
    /// @brief The resolver's stated rationale, empty while still open.
    Light::Field<Light::SqlAnsiString<255>, Light::SqlRealName{"resolution_note"}> resolutionNote;  // 11
};

/// @brief One queued operation this server has already *decided*.
///
/// `docs/spec/offline/offline.md` puts idempotency-key enforcement on the
/// **replay consumer**, not on the queue ("The queue stores the key verbatim
/// and never interprets, requires, or enforces uniqueness on it"). This table
/// is that enforcement, made durable: a second delivery of the same logical
/// field update is skipped rather than acted on again — which matters because
/// the shipped queues disagree about whether they dedup at enqueue time (see
/// docs/findings/007).
///
/// A row is written once the operation reaches a *terminal* decision, which is
/// applied **or** flagged as a conflict — not only applied. A flagged item is
/// owned by its conflict row from then on, so redelivering it must not raise a
/// second conflict about the same edit.
struct ReplayedOpRecord {
    static constexpr std::string_view TableName = "lims_replayed_ops";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    /// @brief The queued item's `idempotencyKey`, verbatim. Unique.
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"op_key"}> opKey;  // 1
    Light::Field<std::int64_t, Light::SqlRealName{"decided_at"}> decidedAt{0};    // 2
};

}  // namespace lims::db
