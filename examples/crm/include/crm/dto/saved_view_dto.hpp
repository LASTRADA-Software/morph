// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "crm/dto/opportunity_dto.hpp"

/// @file
/// Saved views/filters (README build order §10, stretch): stored
/// `ListOpportunities` filter definitions, executed by re-dispatching that
/// same list action rather than a stored query result — a saved view always
/// reflects the pipeline's *current* state, the same "always live, never a
/// stale snapshot" property a real product's saved view has. Scoped
/// per-principal (`owner`): "my Negotiation-stage deals" is the everyday
/// shape a sales rep wants, not an org-wide shared view — this pass builds
/// the personal case only; a shared/team-visible view is future work, not
/// silently assumed by this shape (nothing here prevents adding a `shared`
/// flag later without a breaking change).
///
/// No generic filter DSL: a saved view is exactly `ListOpportunities`'s own
/// two optional filters (`accountId`, `stage`), not an arbitrary predicate
/// tree — the spec names "stored definitions executed by list actions",
/// which this takes literally rather than building a query language no
/// other rung in the ladder has a precedent for.

namespace crm {

/// @brief Saves a named `ListOpportunities` filter for the calling principal.
struct CreateSavedView {
    std::string name;
    std::optional<AccountId> accountId;
    std::optional<OpportunityStage> stage;

    [[nodiscard]] bool validate() const noexcept { return !name.empty(); }
};

struct CreateSavedViewResult {
    SavedViewId savedViewId;
};

/// @brief One saved view, as served to its owner.
struct SavedViewView {
    SavedViewId id;
    std::string name;
    std::optional<AccountId> accountId;
    std::optional<OpportunityStage> stage;
};

/// @brief Lists every saved view belonging to the calling principal.
///
/// No `owner` parameter: unlike `ListOpportunities`'s account filter, whose
/// scope is a caller-chosen argument, a saved view's owner is always *the
/// caller* — the same "identity from the session, not a parameter" shape
/// `requirePrincipal()`'s callers already use elsewhere, applied here to a
/// read rather than a write.
struct ListSavedViews {
    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct ListSavedViewsResult {
    std::vector<SavedViewView> views;
};

/// @brief Executes a saved view: re-dispatches `ListOpportunities` with its
///        stored filter, against the pipeline's current state.
struct RunSavedView {
    SavedViewId savedViewId;

    [[nodiscard]] bool validate() const noexcept { return savedViewId.hasValue(); }
};

/// @brief Deletes a saved view. Owner-only (`SavedViewModel`'s own check) —
///        a saved view is a personal convenience, not a shared resource.
struct DeleteSavedView {
    SavedViewId savedViewId;

    [[nodiscard]] bool validate() const noexcept { return savedViewId.hasValue(); }
};

struct DeleteSavedViewResult {
    SavedViewId savedViewId;
};

}  // namespace crm
