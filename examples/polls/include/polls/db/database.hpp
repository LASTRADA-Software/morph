// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace polls::db {

/// @brief Points Lightweight's default connection at @p connectionString and
///        applies every pending migration. Production-bootstrap-only, called
///        once by Task 17's server app -- see `bookmarks::db::setup`'s
///        identical doc comment for why tests never call this.
/// @param connectionString ODBC connection string.
void setup(const std::string& connectionString);

}  // namespace polls::db
