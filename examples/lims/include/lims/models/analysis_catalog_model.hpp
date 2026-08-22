// SPDX-License-Identifier: Apache-2.0
#pragma once
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
    /// @brief Records @p action/@p result as a `LogEntry` if a log is attached.
    /// @tparam Action Concrete action type.
    /// @tparam Result Concrete result type.
    /// @param action The executed action.
    /// @param result The action's result.
    template <typename Action, typename Result>
    void logAction(const Action& action, const Result& result) const;

    std::optional<std::string> _entityKeyStr;
    std::shared_ptr<::morph::journal::IActionLog> _log;
};

}  // namespace lims

BRIDGE_REGISTER_MODEL(lims::AnalysisCatalogModel, "AnalysisCatalogModel")
BRIDGE_REGISTER_ACTION(lims::AnalysisCatalogModel, lims::DefineAnalysis, "DefineAnalysis")
BRIDGE_REGISTER_ACTION(lims::AnalysisCatalogModel, lims::ReviseAnalysis, "ReviseAnalysis")
BRIDGE_REGISTER_ACTION(lims::AnalysisCatalogModel, lims::ListAnalyses, "ListAnalyses")
BRIDGE_REGISTER_ACTION(lims::AnalysisCatalogModel, lims::GetAnalysisVersion, "GetAnalysisVersion")
