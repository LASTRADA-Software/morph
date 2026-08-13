// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

/// @file
/// pastebin::db::setup — mirrors bank::db::setup's bootstrap shape
/// (examples/bank/include/bank/db/database.hpp): set the default connection
/// string, then apply every pending LIGHTWEIGHT_SQL_MIGRATION. The
/// migration itself lives in schema.cpp so linking that one TU registers it
/// against MigrationManager's process-wide singleton at static-init time.

namespace pastebin::db {

/// @brief Points Lightweight's default connection at @p connectionString and
///        applies every pending migration.
///
/// Production-bootstrap-only: Task 6's server app calls this once, at
/// process start. Tests never call it — rung 0's `DbFixture` already sets
/// the default connection string exactly once per process and applies every
/// pending migration on each fixture construction; the
/// `LIGHTWEIGHT_SQL_MIGRATION` this module registers is picked up
/// automatically the moment the pastebin library is linked in, `setup()` or
/// not.
///
/// @param connectionString ODBC connection string (SQLite via sqliteodbc in
///        every ladder test/demo context).
void setup(const std::string& connectionString);

}  // namespace pastebin::db
