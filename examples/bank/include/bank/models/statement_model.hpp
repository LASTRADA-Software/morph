// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/registry.hpp>
#include <morph/core/bridge.hpp>

#include "bank/dto/statement_dto.hpp"

/// @file
/// The Statement model: a read-only aggregation across an owner's accounts and
/// the ledger, producing per-account credit/debit totals for a date range.

namespace bank {

/// @brief Produces date-ranged statements across an owner's accounts.
///
/// Holds no database state itself: `execute()` acquires a
/// `Lightweight::GlobalDataMapperPool()` connection for its own duration and
/// returns it before returning, rather than owning one for its own lifetime.
class StatementModel {
public:
    dto::Statement execute(const dto::GenerateStatement& action);
};

}  // namespace bank

using bank::StatementModel;
using bank::dto::GenerateStatement;

BRIDGE_REGISTER_MODEL(StatementModel, "StatementModel")
BRIDGE_REGISTER_ACTION(StatementModel, GenerateStatement, "GenerateStatement", ::morph::model::Loggable::No)
