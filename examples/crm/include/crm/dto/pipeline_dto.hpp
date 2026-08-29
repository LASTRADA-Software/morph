// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

#include "crm/core/types.hpp"

/// @file
/// The guarded, journaled opportunity-stage transition (README build order
/// §3) — modelled directly on `kanban::BoardModel::execute(MoveTaskPosition)`'s
/// validate/idempotency-ledger/re-check/journal sequence
/// (`examples/kanban/src/models/board_model.cpp`), minus the WIP-limit
/// check kanban has and this rung's README does not ask for.

namespace crm {

/// @brief Whether moving from @p from to @p to is a legal pipeline edge.
///
/// `Won` and `Lost` are terminal — the state machine crm/README.md build
/// order §3 calls for. Every other transition (including "backwards", e.g.
/// `Negotiation` back to `Proposal`) is legal: a real sales pipeline is not
/// strictly forward-only (a deal can stall and need re-qualification), and
/// the README names no ordering constraint beyond the two terminal stages.
[[nodiscard]] constexpr bool isLegalStageTransition(OpportunityStage from, OpportunityStage to) noexcept {
    static_cast<void>(to);  // no destination-specific rule today; every stage but the two terminal ones is reachable
    return from != OpportunityStage::Won && from != OpportunityStage::Lost;
}

/// @brief Moves an opportunity to a new pipeline stage.
///
/// `opId` is the exactly-once dedup key (LADDER.md strain 5): a client
/// retrying a lost reply frame after a server-side commit must not
/// double-apply the move. Empty `opId` opts out of the ledger (matching
/// `kanban::MoveTaskPosition`'s `if (!action.opId.empty())` guard) — for
/// callers (tests, replay) that do not need it.
struct MoveOpportunityStage {
    OpportunityId opportunityId;
    OpportunityStage stage = OpportunityStage::Prospecting;
    std::string opId;

    [[nodiscard]] bool validate() const noexcept { return opportunityId.hasValue(); }
};

struct MoveOpportunityStageResult {
    OpportunityId opportunityId;
    OpportunityStage stage = OpportunityStage::Prospecting;
    std::int32_t version = 0;
};

}  // namespace crm
