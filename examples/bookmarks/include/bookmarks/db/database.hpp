// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/Pool.hpp>
#include <morph/core/profiler.hpp>
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

/// @brief Takes a data mapper from Lightweight's global pool, inside a profiler
///        zone of its own: under load, waiting for a pooled connection is a
///        cost of its own, distinct from the query that follows.
/// @return The pooled mapper, returned to the pool when it is destroyed.
[[nodiscard]] inline auto acquireMapper() {
    MORPH_ZONE("bookmarks::db::acquire");
    return ::Lightweight::GlobalDataMapperPool().Acquire();
}

}  // namespace bookmarks::db
