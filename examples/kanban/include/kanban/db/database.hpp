// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

/// @file
/// Kanban's database bootstrap entry point -- mirrors
/// `bookmarks::db::setup`/`polls::db::setup` exactly: point Lightweight's
/// default connection at @p connectionString, create the migration
/// history table, apply every pending `LIGHTWEIGHT_SQL_MIGRATION`.

namespace kanban::db {

/// @brief Configures the default SQL connection and applies pending
///        migrations. Call once at process startup.
/// @param connectionString ODBC connection string (see
///        `Lightweight::SqlConnectionString`).
void setup(const std::string& connectionString);

}  // namespace kanban::db
