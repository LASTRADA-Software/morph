// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

/// @file
/// Process-wide database lifecycle for the ledger rung, mirroring
/// bank::db's exact three-function split (examples/bank/include/bank/db/
/// database.hpp) — Lightweight resolves its connection from a
/// process-global default, and each model opens its own `DataMapper`
/// against it. Production bootstrap only: this rung's own tests never
/// call `setup()` (or `configure()`/`applyMigrations()` individually) --
/// they use `morph::ladder::testkit::DbFixture`, which configures its own
/// connection and applies migrations independently, per the ladder-wide
/// test convention (see polls::db::setup's doc comment for the same
/// stated rule in a sibling rung).

namespace ledger::db {

/// @brief Installs @p connectionString as Lightweight's default connection.
/// @param connectionString ODBC connection string, e.g.
///        `"DRIVER=SQLite3;Database=ledger.db"`.
void configure(const std::string& connectionString);

/// @brief Applies any pending schema migrations against the default connection.
///
/// Idempotent: migrations already recorded in the database's migration
/// history are skipped, so this is safe to call on every startup.
void applyMigrations();

/// @brief Convenience: `configure(connectionString)` followed by `applyMigrations()`.
void setup(const std::string& connectionString);

}  // namespace ledger::db
