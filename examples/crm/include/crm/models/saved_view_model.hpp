// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <string>

#include "crm/core/self_journal.hpp"
#include "crm/dto/opportunity_dto.hpp"
#include "crm/dto/saved_view_dto.hpp"

/// @file
/// `SavedViewModel` — `CreateSavedView`/`ListSavedViews`/`RunSavedView`/
/// `DeleteSavedView` (README build order §10, stretch). Registered plain,
/// unkeyed: a saved view is scoped by its `owner` column, not by a per-model
/// key — same reasoning as `CustomFieldModel`'s own unkeyed registration.

namespace crm {

class SavedViewModel {
public:
    CreateSavedViewResult execute(const CreateSavedView& action);
    ListSavedViewsResult execute(const ListSavedViews& action);

    /// @brief Re-dispatches `ListOpportunities` with the saved view's stored
    ///        filter, against the pipeline's current state — a saved view
    ///        is a stored *definition*, never a cached result (this file's
    ///        own doc comment, and `saved_view_dto.hpp`'s).
    ListOpportunitiesResult execute(const RunSavedView& action);
    DeleteSavedViewResult execute(const DeleteSavedView& action);

    void attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey) {
        _journal.attach(std::move(log), std::move(entityKey));
    }

    [[nodiscard]] std::vector<::morph::journal::LogEntry> journalEntries() const { return _journal.entries(); }

private:
    SelfJournal _journal;
};

}  // namespace crm

BRIDGE_REGISTER_MODEL(crm::SavedViewModel, "SavedViewModel")
BRIDGE_REGISTER_ACTION(crm::SavedViewModel, crm::CreateSavedView, "CreateSavedView")
BRIDGE_REGISTER_ACTION(crm::SavedViewModel, crm::ListSavedViews, "ListSavedViews", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(crm::SavedViewModel, crm::RunSavedView, "RunSavedView", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(crm::SavedViewModel, crm::DeleteSavedView, "DeleteSavedView")
