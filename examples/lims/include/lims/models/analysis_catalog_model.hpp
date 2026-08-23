// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "lims/core/self_journal.hpp"
#include "lims/dto/analysis_dto.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>

#include <memory>
#include <optional>
#include <string>

namespace lims {

/// @brief The analysis catalogue: definitions and their versions.
///
/// Unkeyed and plain default-constructible. Unlike `SampleModel`, which is
/// keyed per sample so a bench and an office client attach to the same
/// instance, the catalogue is lab-wide — there is no per-instance identity to
/// shard on.
///
/// **Revising never edits.** `ReviseAnalysis` appends version N+1 and leaves
/// N untouched, which is what makes "an old result stays bound to the
/// definition it was captured under" a property of the data rather than a
/// convention the code has to keep remembering.
class AnalysisCatalogModel {
  public:
    DefineAnalysisResult execute(const DefineAnalysis& action);
    DefineAnalysisResult execute(const ReviseAnalysis& action);
    ListAnalysesResult execute(const ListAnalyses& action);
    AnalysisVersionView execute(const GetAnalysisVersion& action);

    /// @brief Returns the result-entry form for one analysis version.
    ///
    /// The compiled action's schema with that version's own precision, spec
    /// range and detection limits merged in. Rendering is therefore
    /// version-bound; validation is not — see `AnalysisSchemaView::schemaJson`.
    /// @param action The version whose form is wanted.
    /// @return The rendered schema.
    /// @throws ValidationError if no version is named, or if the version's
    ///         canonical unit has no compiled result-entry action in this rung.
    /// @throws NotFound if the version does not exist.
    AnalysisSchemaView execute(const GetAnalysisSchema& action);

    /// @brief Attaches a durable action log and this instance's identity, so
    ///        every mutating `execute()` records a `LogEntry`.
    ///
    /// Same reason `ledger::RuleModel::attachActionLog` exists: a
    /// plain-constructed model never goes through the registry/dispatcher
    /// path, so the framework's own auto-append never fires for it.
    /// @param log Sink entries are forwarded to.
    /// @param entityKey Stable identity stamped onto every entry.
    void attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey);

  private:
    /// @brief This instance's own journal (inert until `attachActionLog`).
    ///
    /// A shared helper rather than a fourth hand-rolled copy of the
    /// `logAction` shape ledger and kanban each carry — see
    /// `lims::SelfJournal`'s file comment for why models have to do this
    /// themselves at all.
    SelfJournal _journal;

};

}  // namespace lims

BRIDGE_REGISTER_MODEL(lims::AnalysisCatalogModel, "AnalysisCatalogModel")
BRIDGE_REGISTER_ACTION(lims::AnalysisCatalogModel, lims::DefineAnalysis, "DefineAnalysis")
BRIDGE_REGISTER_ACTION(lims::AnalysisCatalogModel, lims::ReviseAnalysis, "ReviseAnalysis")
BRIDGE_REGISTER_ACTION(lims::AnalysisCatalogModel, lims::ListAnalyses, "ListAnalyses")
BRIDGE_REGISTER_ACTION(lims::AnalysisCatalogModel, lims::GetAnalysisVersion, "GetAnalysisVersion")
BRIDGE_REGISTER_ACTION(lims::AnalysisCatalogModel, lims::GetAnalysisSchema, "GetAnalysisSchema",
                       ::morph::model::Loggable::No)
