// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lims/org/core/strong_id.hpp"

/// @file
/// The ids of records one module owns and another refers to: lab's clients, samples, analysis definitions and
/// results, which billing bills. Shared so a module that refers to one does not include its owner. The shape tags are
/// part of every journal entry that carries one, and clients decode them, so they must not change.

namespace lims {

LIMS_DEFINE_STRONG_ID(ClientId);
LIMS_DEFINE_STRONG_ID(SampleId);
LIMS_DEFINE_STRONG_ID(AnalysisId);
LIMS_DEFINE_STRONG_ID(ResultId);

}  // namespace lims

LIMS_DEFINE_STRONG_ID_WIRE(ClientId);
LIMS_DEFINE_STRONG_ID_WIRE(SampleId);
LIMS_DEFINE_STRONG_ID_WIRE(AnalysisId);
LIMS_DEFINE_STRONG_ID_WIRE(ResultId);

LIMS_DEFINE_STRONG_ID_SHAPE_TAG(ClientId, "lims.clientId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(SampleId, "lims.sampleId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(AnalysisId, "lims.analysisId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(ResultId, "lims.resultId");
