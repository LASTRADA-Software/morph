// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/offline/offline_queue.hpp>
#include <string>

#include "crm/core/self_journal.hpp"
#include "crm/dto/offline_dto.hpp"
#include "crm/dto/opportunity_dto.hpp"
#include "crm/dto/pipeline_dto.hpp"

/// @file
/// `OpportunityModel` — opportunity CRUD (README build order §1), the
/// guarded, journaled pipeline-stage transition (§3, modelled on
/// `kanban::BoardModel::execute(MoveTaskPosition)`), and offline replay with
/// conflict surfacing (§8, modelled on `lims::SampleModel`'s identical shape).

namespace crm {

class OpportunityModel {
public:
    CreateOpportunityResult execute(const CreateOpportunity& action);
    UpdateOpportunityResult execute(const UpdateOpportunity& action);
    OpportunityView execute(const GetOpportunity& action);
    ListOpportunitiesResult execute(const ListOpportunities& action);
    MoveOpportunityStageResult execute(const MoveOpportunityStage& action);

    /// @brief Replays one queued field edit (README §8) — at-most-once,
    ///        author-checked, and conflict-surfacing rather than silently
    ///        merging or dropping a stale base version. See
    ///        `lims::SampleModel::execute(const QueuedCapture&)` for the
    ///        identical shape this mirrors.
    ReplayOpportunityUpdateResult execute(const QueuedOpportunityUpdate& action);
    ListConflictsResult execute(const ListConflicts& action);
    ConflictView execute(const ResolveConflict& action);

    void attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey) {
        _journal.attach(std::move(log), std::move(entityKey));
    }

    [[nodiscard]] std::vector<::morph::journal::LogEntry> journalEntries() const { return _journal.entries(); }

    /// @brief Attaches the durable offline queue `onBackendChanged()` drains.
    /// @param queue The queue to drain on the next backend switch, or when a
    ///        test calls `onBackendChanged()` directly.
    void attachOfflineQueue(std::shared_ptr<::morph::offline::IOfflineQueue> queue);

    /// @brief Drains the attached offline queue and replays every item
    ///        through `execute(QueuedOpportunityUpdate)`.
    ///
    /// A no-op when no queue is attached (docs/spec/offline/offline.md's
    /// `Model::onBackendChanged()` seam). An item whose payload does not even
    /// decode is journaled and dropped, never left to block the queue behind
    /// it — same as `lims::SampleModel::onBackendChanged()`.
    void onBackendChanged();

private:
    SelfJournal _journal;
    std::shared_ptr<::morph::offline::IOfflineQueue> _queue;
};

}  // namespace crm

BRIDGE_REGISTER_MODEL(crm::OpportunityModel, "OpportunityModel")
BRIDGE_REGISTER_ACTION(crm::OpportunityModel, crm::CreateOpportunity, "CreateOpportunity")
BRIDGE_REGISTER_ACTION(crm::OpportunityModel, crm::UpdateOpportunity, "UpdateOpportunity")
BRIDGE_REGISTER_ACTION(crm::OpportunityModel, crm::GetOpportunity, "GetOpportunity", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(crm::OpportunityModel, crm::ListOpportunities, "ListOpportunities",
                       ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(crm::OpportunityModel, crm::MoveOpportunityStage, "MoveOpportunityStage")
BRIDGE_REGISTER_ACTION(crm::OpportunityModel, crm::QueuedOpportunityUpdate, "QueuedOpportunityUpdate")
BRIDGE_REGISTER_ACTION(crm::OpportunityModel, crm::ListConflicts, "ListConflicts", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(crm::OpportunityModel, crm::ResolveConflict, "ResolveConflict")
