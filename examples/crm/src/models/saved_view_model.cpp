// SPDX-License-Identifier: Apache-2.0
#include "crm/models/saved_view_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <morph/session/session.hpp>

#include "crm/core/errors.hpp"
#include "crm/core/model_support.hpp"
#include "crm/db/crm_entity.hpp"
#include "crm/models/opportunity_model.hpp"

namespace crm {

namespace {

SavedViewView toView(const db::SavedViewRecord& row) {
    SavedViewView view{
        .id = SavedViewId{static_cast<std::int64_t>(row.id.Value())},
        .name = std::string{row.name.Value().ToStringView()},
    };
    if (row.accountId.Value().has_value()) {
        view.accountId = AccountId{static_cast<std::int64_t>(*row.accountId.Value())};
    }
    if (row.stage.Value().has_value()) {
        view.stage = static_cast<OpportunityStage>(*row.stage.Value());
    }
    return view;
}

}  // namespace

CreateSavedViewResult SavedViewModel::execute(const CreateSavedView& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateSavedView: name is required"};
    }

    Lightweight::DataMapper mapper;
    db::SavedViewRecord row;
    row.owner = Lightweight::SqlAnsiString<64>{::morph::session::current()->principal};
    row.name = Lightweight::SqlAnsiString<128>{action.name};
    if (action.accountId.has_value() && action.accountId->hasValue()) {
        row.accountId = static_cast<std::uint64_t>(**action.accountId);
    }
    if (action.stage.has_value()) {
        row.stage = static_cast<int>(*action.stage);
    }
    mapper.Create(row);

    CreateSavedViewResult result{.savedViewId = SavedViewId{static_cast<std::int64_t>(row.id.Value())}};
    _journal.recordSuccess<SavedViewModel>(action, result, nowMillis());
    return result;
}

ListSavedViewsResult SavedViewModel::execute(const ListSavedViews& action) {
    (void)action;
    requirePrincipal();

    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::SavedViewRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::SavedViewRecord::owner>, "=",
                           ::morph::session::current()->principal)
                    .All();
    ListSavedViewsResult result;
    result.views.reserve(rows.size());
    for (const auto& row : rows) {
        result.views.push_back(toView(row));
    }
    return result;
}

ListOpportunitiesResult SavedViewModel::execute(const RunSavedView& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"RunSavedView: savedViewId is required"};
    }

    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::SavedViewRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::SavedViewRecord::id>, "=", *action.savedViewId)
                    .Where(::Lightweight::FieldNameOf<&db::SavedViewRecord::owner>, "=",
                           ::morph::session::current()->principal)
                    .All();
    if (rows.empty()) {
        // Deliberately the same NotFound whether the id does not exist at
        // all or belongs to a different principal — a saved view is a
        // personal resource (this file's own doc comment), so this must not
        // leak whether *someone else's* view with this id exists.
        throw NotFound{"RunSavedView: no such saved view"};
    }
    const auto view = toView(rows.front());

    // Re-dispatched through a fresh, in-process OpportunityModel rather than
    // duplicating ListOpportunities's own query logic here — a saved view is
    // a stored *definition executed by* the list action (README §10's exact
    // wording), not a second, parallel implementation of it. No cross-model
    // orchestration hazard here (unlike ConvertLead's own doc comment on
    // nested-dispatch pool-starvation): this is a plain, unkeyed,
    // uncoordinated read with no shared strand or transaction to deadlock on.
    OpportunityModel opportunities;
    ListOpportunities filter{.accountId = view.accountId, .stage = view.stage};
    return opportunities.execute(filter);
}

DeleteSavedViewResult SavedViewModel::execute(const DeleteSavedView& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"DeleteSavedView: savedViewId is required"};
    }

    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::SavedViewRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::SavedViewRecord::id>, "=", *action.savedViewId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"DeleteSavedView: no such saved view"};
    }
    auto& row = rows.front();
    if (std::string{row.owner.Value().ToStringView()} != ::morph::session::current()->principal) {
        // Owner-only, not merely "exists" — a caller naming someone else's
        // saved view by id is refused, not told it does not exist: unlike
        // RunSavedView, DeleteSavedView is a mutation, and Forbidden (not a
        // disguised NotFound) is this rung's existing convention for "the id
        // is real but you may not act on it" (e.g. QueuedOpportunityUpdate's
        // author check).
        throw Forbidden{"DeleteSavedView: only the owning principal may delete a saved view"};
    }
    mapper.Delete(row);

    DeleteSavedViewResult result{.savedViewId = action.savedViewId};
    _journal.recordSuccess<SavedViewModel>(action, result, nowMillis());
    return result;
}

}  // namespace crm
