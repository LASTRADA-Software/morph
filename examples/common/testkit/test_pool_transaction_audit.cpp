// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/Pool.hpp>
#include <Lightweight/SqlConnection.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>
#include <vector>

#include "db/pool_transaction_audit.hpp"
#include "testkit/db_fixture.hpp"

// The audit's own acceptance. The claim under test is not "the suite passes"
// -- it is that a mapper deliberately returned to the pool mid-transaction is
// named by the audit, and that the audit is silent when nothing leaks. Both
// directions are asserted below, because a check that never fires and a check
// that always fires are indistinguishable from one that measures nothing.

using morph::ladder::db::AutocommitState;
using morph::ladder::db::autocommitStateOf;
using morph::ladder::db::PoolTransactionAudit;

namespace {

/// @brief Collects the audit's diagnostics instead of aborting on them.
struct LeakCollector {
    std::vector<std::string> messages;

    [[nodiscard]] PoolTransactionAudit::LeakHandler handler() {
        return [this](std::string_view message) { messages.emplace_back(message); };
    }
};

}  // namespace

TEST_CASE("autocommitStateOf reports what SqlTransaction does to a connection", "[ladder][testkit][db][pool]") {
    const morph::ladder::testkit::DbFixture fixture;

    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();

    // The premise every other assertion in this file rests on: the SQLite ODBC
    // driver answers SQLGetConnectAttr(SQL_ATTR_AUTOCOMMIT) at all. If it
    // returned Unknown, the audit would be a control that measures nothing.
    REQUIRE(autocommitStateOf(mapper->Connection()) == AutocommitState::On);

    {
        const ::Lightweight::SqlTransaction transaction{mapper->Connection(),
                                                        ::Lightweight::SqlTransactionMode::ROLLBACK};
        CHECK(autocommitStateOf(mapper->Connection()) == AutocommitState::Off);
    }
    CHECK(autocommitStateOf(mapper->Connection()) == AutocommitState::On);

    // And the part that is easy to get backwards: Commit() restores
    // autocommit itself, there and then -- it does not wait for the
    // destructor. Every statement after an explicit Commit() is therefore
    // already back in autocommit, however long the SqlTransaction local
    // stays in scope.
    {
        ::Lightweight::SqlTransaction transaction{mapper->Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
        REQUIRE(autocommitStateOf(mapper->Connection()) == AutocommitState::Off);
        transaction.Commit();
        CHECK(autocommitStateOf(mapper->Connection()) == AutocommitState::On);
    }
}

TEST_CASE("PoolTransactionAudit is silent when a pooled mapper is returned cleanly", "[ladder][testkit][db][pool]") {
    const morph::ladder::testkit::DbFixture fixture;

    LeakCollector collector;
    const PoolTransactionAudit audit{collector.handler()};

    // The ordinary, correct shape: the mapper is acquired first, the
    // transaction declared after it, so the transaction destructs (restoring
    // autocommit) before the mapper is returned.
    {
        auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
        ::Lightweight::SqlTransaction transaction{mapper->Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
        transaction.Commit();
    }

    CHECK(collector.messages.empty());
    CHECK(audit.detections() == 0);
}

TEST_CASE("PoolTransactionAudit names a pooled mapper returned mid-transaction", "[ladder][testkit][db][pool]") {
    const morph::ladder::testkit::DbFixture fixture;

    LeakCollector collector;
    const PoolTransactionAudit audit{collector.handler()};

    // The leak, built deliberately. `transaction` is declared *before*
    // `mapper`, so `mapper` is destroyed first: the pooled DataMapper goes
    // back into the pool while the SqlTransaction is still alive and
    // autocommit is still OFF. This is exactly what a refactor that hoists a
    // transaction into a member or a longer-lived scope produces, and on
    // master nothing reports it.
    {
        std::optional<::Lightweight::SqlTransaction> transaction;
        {
            auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
            transaction.emplace(mapper->Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK);
            REQUIRE(autocommitStateOf(mapper->Connection()) == AutocommitState::Off);
        }  // <- mapper returned to the pool here, transaction still open

        CHECK(audit.detections() >= 1);
        REQUIRE_FALSE(collector.messages.empty());
        CHECK(collector.messages.front().contains("PoolTransactionAudit"));
        CHECK(collector.messages.front().contains("SQL_ATTR_AUTOCOMMIT still OFF"));

        // Roll back before leaving, so the leaked connection is clean again
        // for every test that runs after this one in the same process.
        transaction->Rollback();
    }
}
