// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace bookmarks::db {

/// @brief Points Lightweight's default connection at @p connectionString and
///        applies every pending migration. Production-bootstrap-only, called
///        once by Task 12's server app — see `pastebin::db::setup`'s
///        identical doc comment for why tests never call this.
/// @param connectionString ODBC connection string.
void setup(const std::string& connectionString);

}  // namespace bookmarks::db
