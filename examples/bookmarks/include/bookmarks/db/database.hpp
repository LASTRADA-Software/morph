// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace bookmarks::db {

/// @brief Points Lightweight's default connection at @p connectionString and
///        applies every pending migration. Production-bootstrap-only, called
///        once by the server's `App`. Tests never call this: they use
///        `morph::ladder::testkit::DbFixture`, which owns the connection and
///        the migrations itself, and a test that ran this would re-point the
///        process-global connection under it.
/// @param connectionString ODBC connection string.
void setup(const std::string& connectionString);

}  // namespace bookmarks::db
