// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <string>

#include "lims/lab/db/lims_entity.hpp"

/// @file
/// Writing lab's events, inside the caller's transaction. Lab-internal: it names the ORM, which model headers do not.

namespace lims {

/// @brief Writes one `lab.AnalysisPublished` row per result of @p sample, through the caller's open mapper, so the
///        rows commit with the publication or not at all.
/// @param mapper The caller's mapper, inside its transaction.
/// @param sample The sample, as it is being published (its version already moved).
/// @param principal Who published it.
void writeAnalysisPublished(Lightweight::DataMapper& mapper, db::SampleRecord const& sample,
                            std::string const& principal);

/// @brief Writes the `lab.ResultCaptured` row of a result, through the caller's open mapper, so the row commits with
///        the capture or not at all.
/// @param mapper The caller's mapper, inside its transaction.
/// @param sample The sample, as the capture left it (its version already moved).
/// @param result The result that was captured, with its id.
/// @param analysisId The analysis definition the result's version belongs to.
/// @param principal Who captured it.
void writeResultCaptured(Lightweight::DataMapper& mapper, db::SampleRecord const& sample,
                         db::ResultRecord const& result, std::int64_t analysisId, std::string const& principal);

}  // namespace lims
