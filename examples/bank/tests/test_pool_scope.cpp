// SPDX-License-Identifier: Apache-2.0

#include <Lightweight/DataMapper/Pool.hpp>
#include <Lightweight/SqlConnection.hpp>
#include <Lightweight/SqlLogger.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <morph/core/bridge.hpp>

#include "bank/app/app.hpp"
#include "bank/core/types.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/dto/transaction_dto.hpp"
#include "bank/models/customer_model.hpp"
#include "bank/models/transaction_model.hpp"
#include "bank_test_support.hpp"

/// @file
/// Why `examples/common/db/pool_transaction_audit.hpp` is **not** installed in
/// bank (morph#752).
///
/// morph#740's `PoolTransactionAudit` detects a pooled `Lightweight::DataMapper`
/// handed on with `SQL_ATTR_AUTOCOMMIT` still `OFF`. It is installed from all
/// seven ladder rungs and from `testkit/DbFixture`; morph#752 proposed
/// extending it to bank, on the premise that "bank's models acquire from
/// `Lightweight::GlobalDataMapperPool()` like every rung does".
///
/// That premise is false, and this file is the measurement that says so.
/// `bank::db::WithMapper::mapper()` (`bank/db/db_model.hpp`) constructs a
/// `Lightweight::DataMapper` directly, one per model, and
/// `LightweightOfflineQueue` owns another; nothing under `examples/bank/`
/// names the pool. At the pinned Lightweight revision
/// (`bbb972a78e1962b968a2c6ad93f7dade736eaa01`) the two hooks the audit reads,
/// `SqlLogger::OnConnectionIdle` and `::OnConnectionReuse`, are emitted from
/// `src/Lightweight/DataMapper/Pool.hpp` and from nowhere else -- so an audit
/// installed in bank would inspect zero hand-offs and could not fail. That is
/// a control measuring nothing, which is the failure mode `AGENTS.md` names
/// first, so bank keeps the audit out and keeps this instead.
///
/// The test is in two halves on purpose. A bare "bank performs no pool
/// hand-offs" assertion is indistinguishable from a counter that was never
/// wired up, so the first half drives a real hand-off through the same counter
/// and requires it to move. Only then is the second half's zero evidence.
///
/// It is also the tripwire the audit would have been: if bank ever moves its
/// models onto `GlobalDataMapperPool()`, this case fails and morph#752 becomes
/// live again.

namespace {

/// @brief Counts the two pool hand-off hooks and leaves every other one a no-op.
///
/// Installs itself as the process-wide `Lightweight::SqlLogger` and restores
/// the previous one on destruction, exactly as `PoolTransactionAudit` does.
/// The counter is atomic because bank runs its models on a worker strand, so
/// the hooks fire on a thread other than the one reading the count.
class PoolHandoffCounter : public Lightweight::SqlLogger::Null {
public:
    PoolHandoffCounter() : _previous{&Lightweight::SqlLogger::GetLogger()} {
        Lightweight::SqlLogger::SetLogger(*this);
    }

    PoolHandoffCounter(const PoolHandoffCounter&) = delete;
    PoolHandoffCounter& operator=(const PoolHandoffCounter&) = delete;
    PoolHandoffCounter(PoolHandoffCounter&&) = delete;
    PoolHandoffCounter& operator=(PoolHandoffCounter&&) = delete;

    ~PoolHandoffCounter() override {
        if (&Lightweight::SqlLogger::GetLogger() == this) {
            Lightweight::SqlLogger::SetLogger(*_previous);
        }
    }

    void OnConnectionIdle(const Lightweight::SqlConnection& /*connection*/) override { _handoffs.fetch_add(1); }

    void OnConnectionReuse(const Lightweight::SqlConnection& /*connection*/) override { _handoffs.fetch_add(1); }

    [[nodiscard]] unsigned long handoffs() const noexcept { return _handoffs.load(); }

    void reset() noexcept { _handoffs.store(0); }

private:
    std::atomic<unsigned long> _handoffs{0};
    Lightweight::SqlLogger* _previous;
};

}  // namespace

TEST_CASE("bank's persistence never hands a DataMapper through Lightweight's pool", "[bank][pool]") {
    bank::testing::ensureDatabase();

    PoolHandoffCounter counter;

    // ── Half one: the instrument, proven on a real hand-off ──────────────────
    // Acquiring from the global pool and returning the mapper drives
    // OnConnectionReuse and/or OnConnectionIdle. If this does not move, the
    // zero below would mean nothing.
    {
        auto pooled = Lightweight::GlobalDataMapperPool().Acquire();
        REQUIRE(pooled.Get().Connection().NativeHandle() != nullptr);
    }  // <- returned to the pool here
    REQUIRE(counter.handoffs() > 0);

    // ── Half two: the measurement ────────────────────────────────────────────
    counter.reset();

    bank::app::App app{bank::testing::connectionString()};
    app.login("pool-scope-probe");
    morph::bridge::BridgeHandler<bank::CustomerModel> customer{app.bridge(), app.gui()};
    morph::bridge::BridgeHandler<bank::TransactionModel> txns{app.bridge(), app.gui()};

    // The two money-movement paths that open a `SqlTransaction` over
    // `mapper().Connection()` -- `TransactionModel::execute(Deposit)` and
    // `execute(Transfer)` -- are precisely what the audit would police, so they
    // are what is driven here.
    const auto source = bank::testing::await(customer.execute(bank::dto::OpenAccount{
                                                 .kind = static_cast<int>(bank::AccountKind::Checking),
                                                 .currency = static_cast<int>(bank::Currency::EUR),
                                             }),
                                             app.guiLoop());
    const auto destination = bank::testing::await(customer.execute(bank::dto::OpenAccount{
                                                      .kind = static_cast<int>(bank::AccountKind::Checking),
                                                      .currency = static_cast<int>(bank::Currency::EUR),
                                                  }),
                                                  app.guiLoop());

    bank::testing::await(
        txns.execute(bank::dto::Deposit{.accountId = source.id, .amountMinor = 5000, .description = "probe"}),
        app.guiLoop());
    bank::testing::await(
        txns.execute(bank::dto::Transfer{
            .fromAccountId = source.id, .toAccountId = destination.id, .amountMinor = 2500, .description = "probe"}),
        app.guiLoop());

    // Zero, and it is evidence because half one showed the same counter moving.
    CHECK(counter.handoffs() == 0);
}
