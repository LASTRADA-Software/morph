// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

/// @file
/// Process-wide database lifecycle for the bank example.
///
/// Lightweight resolves its connection from a process-global default
/// connection string, and each morph model opens its own `DataMapper` against
/// that default (one connection per model, since each model runs single-
/// threaded on its own strand). The schema is owned by the migration runner.

namespace bank::db {

/// @brief Installs @p connectionString as Lightweight's default connection.
///
/// @param connectionString ODBC connection string, e.g.
///        `"DRIVER=SQLite3;Database=bank.db"`.
void configure(const std::string& connectionString);

/// @brief Applies any pending schema migrations against the default connection.
///
/// Idempotent: migrations already recorded in the database's migration history
/// are skipped, so this is safe to call on every startup.
void applyMigrations();

/// @brief Convenience: `configure(connectionString)` followed by `applyMigrations()`.
void setup(const std::string& connectionString);

}  // namespace bank::db
