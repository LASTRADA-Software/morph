// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lims/core/types.hpp"

#include <morph/core/model_key.hpp>
#include <morph/forms/forms.hpp>
#include <morph/forms/instance_constraints.hpp>
#include <morph/util/rational.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lims {

/// @brief An exact bound on an analysis, as a rational.
///
/// A plain `Rational` rather than a `Quantity`: the bound is always expressed
/// in its version's own canonical unit, so carrying a second unit tag here
/// would let a definition disagree with itself.
struct AnalysisBound {
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;

    /// @brief Whether this bound is usable as a rational.
    /// @return `true` if the denominator is non-zero.
    [[nodiscard]] bool validate() const noexcept { return denominator != 0; }
};

/// @brief Rebuilds a bound from the nullable numerator/denominator pair a row
///        stores it as.
/// @param numerator The stored numerator, or null.
/// @param denominator The stored denominator, or null.
/// @return The bound, or `std::nullopt` when either half is absent.
[[nodiscard]] inline std::optional<AnalysisBound> makeBound(const std::optional<std::int64_t>& numerator,
                                                            const std::optional<std::int64_t>& denominator) {
    if (!numerator.has_value() || !denominator.has_value()) {
        return std::nullopt;
    }
    return AnalysisBound{.numerator = *numerator, .denominator = *denominator};
}

/// @brief The per-instance constraints a result-entry field inherits from one
///        analysis version — the single declaration that both decorates the
///        served schema and checks the submitted reading.
///
/// This is the rung's use of `morph::forms::InstanceConstraints` (upstream
/// issue #164). Before it existed the version's precision had to be served as
/// a second, app-private key beside the framework's `x-decimalPlaces` and the
/// check re-implemented by hand in `SampleModel`, and the specification range
/// could only be served as a key no code anywhere read.
/// @param field The wire (JSON) field name the reading is submitted under.
/// @param decimalPlaces The version's declared decimal places.
/// @param specLow The version's inclusive lower specification bound, if any.
/// @param specHigh The version's inclusive upper specification bound, if any.
/// @return The constraint set for @p field.
[[nodiscard]] inline ::morph::forms::InstanceConstraints versionConstraints(
    std::string_view field, std::int32_t decimalPlaces, const std::optional<AnalysisBound>& specLow,
    const std::optional<AnalysisBound>& specHigh) {
    const auto places = static_cast<std::uint32_t>(decimalPlaces < 0 ? 0 : decimalPlaces);
    const auto asRational =
        [places](const std::optional<AnalysisBound>& bound) -> std::optional<::morph::math::Rational> {
        if (!bound.has_value() || !bound->validate()) {
            return std::nullopt;
        }
        return ::morph::math::Rational{::morph::math::Numerator{bound->numerator},
                                       ::morph::math::Denominator{bound->denominator},
                                       ::morph::math::DecimalPlaces{places}};
    };
    ::morph::forms::InstanceConstraints constraints;
    constraints.declare(::morph::forms::FieldConstraint{.field = std::string{field},
                                                        .decimalPlaces = places,
                                                        .minimum = asRational(specLow),
                                                        .maximum = asRational(specHigh)});
    return constraints;
}

/// @brief Defines a new analysis, creating its version 1.
struct DefineAnalysis {
    std::string name;
    /// @brief `UnitMeta::id` of the unit results are stored in.
    std::string canonicalUnit;
    std::int32_t decimalPlaces = 3;
    std::optional<AnalysisBound> specLow;
    std::optional<AnalysisBound> specHigh;
    std::optional<AnalysisBound> limitOfDetection;
    std::optional<AnalysisBound> upperDetectionLimit;

    /// @brief Whether this definition is well-formed.
    ///
    /// Bounds are checked individually; whether `specLow <= specHigh` is a
    /// model-level check, since it needs both to be present and comparable.
    /// @return `true` if the required fields are present and each bound is usable.
    [[nodiscard]] bool validate() const noexcept {
        const auto boundOk = [](const std::optional<AnalysisBound>& bound) {
            return !bound.has_value() || bound->validate();
        };
        return !name.empty() && !canonicalUnit.empty() && decimalPlaces >= 0 && decimalPlaces <= 18
            && boundOk(specLow) && boundOk(specHigh) && boundOk(limitOfDetection)
            && boundOk(upperDetectionLimit);
    }
};

/// @brief Appends a new version to an existing analysis.
///
/// Never edits the current version in place. `AnalysisVersionRecord` rows are
/// append-only precisely so a result captured under version N keeps naming
/// version N after the definition moves on.
struct ReviseAnalysis {
    AnalysisId analysisId;
    std::string canonicalUnit;
    std::int32_t decimalPlaces = 3;
    std::optional<AnalysisBound> specLow;
    std::optional<AnalysisBound> specHigh;
    std::optional<AnalysisBound> limitOfDetection;
    std::optional<AnalysisBound> upperDetectionLimit;

    /// @brief Whether this revision is well-formed.
    /// @return `true` if the analysis is named and each bound is usable.
    [[nodiscard]] bool validate() const noexcept {
        const auto boundOk = [](const std::optional<AnalysisBound>& bound) {
            return !bound.has_value() || bound->validate();
        };
        return analysisId.hasValue() && !canonicalUnit.empty() && decimalPlaces >= 0 && decimalPlaces <= 18
            && boundOk(specLow) && boundOk(specHigh) && boundOk(limitOfDetection)
            && boundOk(upperDetectionLimit);
    }
};

/// @brief One version of an analysis definition, as served to a client.
struct AnalysisVersionView {
    AnalysisVersionId id;
    AnalysisId analysisId;
    std::string name;
    std::int32_t version = 1;
    std::string canonicalUnit;
    std::int32_t decimalPlaces = 3;
    std::optional<AnalysisBound> specLow;
    std::optional<AnalysisBound> specHigh;
    std::optional<AnalysisBound> limitOfDetection;
    std::optional<AnalysisBound> upperDetectionLimit;
};

/// @brief Returns every analysis's current version.
struct ListAnalyses {
    /// @brief Always ready: a catalogue listing takes no arguments.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct ListAnalysesResult {
    std::vector<AnalysisVersionView> analyses;
};

/// @brief Returns one specific version, by id.
///
/// Distinct from `ListAnalyses` on purpose: rendering a stored result needs
/// the version that result names, which may be several revisions behind
/// current.
struct GetAnalysisVersion {
    AnalysisVersionId versionId;

    /// @brief Whether the requested version is named.
    /// @return `true` if `versionId` carries a value.
    [[nodiscard]] bool validate() const noexcept { return versionId.hasValue(); }
};

/// @brief Returns the *result-entry form* for one analysis version — the
///        schema a client renders to capture a result under that version.
///
/// Distinct from `GetAnalysisVersion`, which returns the definition as data.
/// This returns the definition already merged into the compiled action's JSON
/// Schema, which is what the README's ODK-style "clients render the version
/// the result was captured with" needs.
struct GetAnalysisSchema {
    /// @brief The version whose form is wanted.
    AnalysisVersionId versionId;

    /// @brief Whether a version is named.
    /// @return `true` if `versionId` carries a value.
    [[nodiscard]] bool validate() const noexcept { return versionId.hasValue(); }
};

/// @brief One version's rendered result-entry form.
struct AnalysisSchemaView {
    /// @brief The version the form belongs to.
    AnalysisVersionId versionId;

    /// @brief That version's ordinal.
    std::int32_t version = 1;

    /// @brief The JSON Schema text a client renders.
    ///
    /// **Rendering is version-bound; validation is not.** The bounds,
    /// detection limits and per-version precision in here are *data* merged
    /// into the schema by the model. Everything `morph::forms` itself
    /// enforces — the `required` array, the `x-rules` list, and the
    /// `x-decimalPlaces` glaze emits for the field — comes from the compiled
    /// `CaptureConcentration` struct and is identical for every version. See
    /// the rung README's §4 decision and `test_schema_versioning.cpp`.
    std::string schemaJson;
};

struct DefineAnalysisResult {
    AnalysisId analysisId;
    AnalysisVersionId versionId;
    std::int32_t version = 1;
};

}  // namespace lims
