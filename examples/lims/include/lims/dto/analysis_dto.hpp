// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lims/core/types.hpp"

#include <morph/core/model_key.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/rational.hpp>

#include <cstdint>
#include <optional>
#include <string>
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

struct DefineAnalysisResult {
    AnalysisId analysisId;
    AnalysisVersionId versionId;
    std::int32_t version = 1;
};

}  // namespace lims
