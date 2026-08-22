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

}  // namespace lims::db
