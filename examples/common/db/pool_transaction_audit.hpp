// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/SqlConnection.hpp>
#include <Lightweight/SqlLogger.hpp>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

/// @file
/// A process-wide check that no pooled `Lightweight::DataMapper` is ever
/// returned to `Lightweight::GlobalDataMapperPool()` with a transaction still
/// open on its connection.
///
/// @par The defect this exists to catch
/// `Lightweight::Pool<Config>::Return` (`src/Lightweight/DataMapper/Pool.hpp`
/// at the pinned revision `bbb972a78e1962b968a2c6ad93f7dade736eaa01`) performs
/// **no transaction cleanup** on a returned connection. All three
/// growth-strategy overloads do the same three things:
///
/// @code
/// void Return(std::unique_ptr<DataMapper> dm) noexcept
/// {
///     DropAsyncBackend(*dm);
///     SqlLogger::GetLogger().OnConnectionIdle(dm->Connection());
///     std::scoped_lock lock(_mutex);
///     _idleDataMappers.push_back(std::move(dm));
/// }
/// @endcode
///
/// and `DropAsyncBackend` is `dm.Connection().DisableAsync();` and nothing
/// else (`Pool.hpp:133-136`), which is `m_data->asyncBackend.reset();`
/// (`SqlConnection.cpp:214-217`). No `SQLEndTran`, no autocommit reset.
///
/// A connection returned with `SQL_ATTR_AUTOCOMMIT` still `OFF` therefore
/// carries an open transaction into the pool, where it is handed to whichever
/// **unrelated** caller acquires next. That caller inherits a transaction it
/// never began, and the first write it attempts blocks for the full SQLite
/// busy timeout -- **60 seconds**, the value
/// `Lightweight::SqlConnection::PostConnect()` sets, not the `Timeout=5000` in
/// a connection string -- before reporting `database is locked`. The failure
/// lands on code that did nothing wrong, with no diagnostic naming the leak.
///
/// @par What actually prevents it today, and why that is not enough
/// `Lightweight::SqlTransaction::Commit()` and `::Rollback()` each restore
/// autocommit themselves, immediately (`SqlTransaction.cpp:52-91`), so a
/// handler that commits explicitly is safe from that line onward. A handler
/// that does *not* -- one relying on the destructor's default
/// `COMMIT`/`ROLLBACK` -- is safe only because the `SqlTransaction` local is
/// declared **after** the pooled mapper and so destructs **before** it. That
/// is a correctness property held by declaration order, which nothing checks
/// and one refactor undoes: moving the transaction into a member, a
/// `std::optional`, or any longer-lived scope breaks it silently.
///
/// @par How this catches it
/// `Pool::Return` and `Pool::Acquire` both announce every hand-off through the
/// process-wide `Lightweight::SqlLogger` -- `OnConnectionIdle` when a mapper is
/// parked, `OnConnectionReuse` when one changes hands. At each of those points
/// the connection belongs to nobody, so its autocommit flag must be `ON`.
/// `PoolTransactionAudit` installs itself as that logger and reads
/// `SQL_ATTR_AUTOCOMMIT` at every hand-off; an `OFF` fails **at the leak**
/// rather than sixty seconds later in someone else's call.
///
/// @par What it does not catch
/// A raw `BEGIN` issued through `SqlStatement` (rather than through
/// `SqlTransaction`) leaves ODBC's autocommit flag `ON` while SQLite holds a
/// transaction open, and no ODBC connection attribute reports that. The ledger
/// rung's `DeferredReadTransactionGuard` is the one place in this tree that
/// does it, and it is RAII-scoped around a single aggregation; see its own doc
/// comment.

namespace morph::ladder::db {

/// @brief What `SQL_ATTR_AUTOCOMMIT` says about a connection.
enum class AutocommitState : unsigned char {
    On,       ///< Autocommit is on: no ODBC-level transaction is open.
    Off,      ///< Autocommit is off: a transaction is open on this connection.
    Unknown,  ///< The driver refused the query; nothing can be concluded.
};

/// @brief Reads `SQL_ATTR_AUTOCOMMIT` off @p connection.
///
/// Driver-local: `SQLGetConnectAttr` for this attribute answers from the
/// driver's own state and issues no statement, so this is cheap enough to run
/// on every pool hand-off.
///
/// @param connection The connection to interrogate.
/// @return `On`, `Off`, or `Unknown` if the driver did not answer.
[[nodiscard]] inline AutocommitState autocommitStateOf(const ::Lightweight::SqlConnection& connection) noexcept {
    SQLUINTEGER value = 0;
    const SQLRETURN status = SQLGetConnectAttr(connection.NativeHandle(), SQL_ATTR_AUTOCOMMIT, &value, 0, nullptr);
    if (status != SQL_SUCCESS && status != SQL_SUCCESS_WITH_INFO) {
        return AutocommitState::Unknown;
    }
    return value == SQL_AUTOCOMMIT_ON ? AutocommitState::On : AutocommitState::Off;
}

/// @brief A `Lightweight::SqlLogger` that fails a pooled hand-off carrying an
///        open transaction.
///
/// Installs itself as the process-wide logger on construction and restores
/// whatever was there before on destruction (only if it is still the installed
/// one, so nesting is safe). Every other hook stays the `SqlLogger::Null`
/// no-op: this is a check wearing a logger's clothes, not a log.
class PoolTransactionAudit : public ::Lightweight::SqlLogger::Null {
public:
    /// @brief What to do when a hand-off is found carrying a transaction.
    ///
    /// Takes the diagnostic message. The default aborts; a test passes its own
    /// to observe the detection instead of dying of it.
    using LeakHandler = std::function<void(std::string_view)>;

    /// @brief Installs this audit as the process-wide `SqlLogger`.
    /// @param onLeak Called with a diagnostic naming the hand-off, once per
    ///        detection. Empty selects the default: write the diagnostic to
    ///        `stderr` and `std::abort()`, so a leak fails at the leak.
    explicit PoolTransactionAudit(LeakHandler onLeak = {})
        : _onLeak{std::move(onLeak)}, _previous{&::Lightweight::SqlLogger::GetLogger()} {
        ::Lightweight::SqlLogger::SetLogger(*this);
    }

    PoolTransactionAudit(const PoolTransactionAudit&) = delete;
    PoolTransactionAudit& operator=(const PoolTransactionAudit&) = delete;
    PoolTransactionAudit(PoolTransactionAudit&&) = delete;
    PoolTransactionAudit& operator=(PoolTransactionAudit&&) = delete;

    /// @brief Restores the previously installed logger, if this one is still
    ///        the installed one.
    ~PoolTransactionAudit() override {
        if (&::Lightweight::SqlLogger::GetLogger() == this) {
            ::Lightweight::SqlLogger::SetLogger(*_previous);
        }
    }

    /// @brief Checks the connection `Pool::Return` is about to park.
    /// @param connection The connection being idled.
    void OnConnectionIdle(const ::Lightweight::SqlConnection& connection) override {
        check(connection, "idled into the pool");
    }

    /// @brief Checks the connection the pool is about to hand to another
    ///        owner (`Pool::Return` to a waiter, or `Pool::Acquire` off the
    ///        idle list).
    /// @param connection The connection changing hands.
    void OnConnectionReuse(const ::Lightweight::SqlConnection& connection) override {
        check(connection, "handed to another borrower");
    }

    /// @brief How many hand-offs this audit has rejected.
    /// @return The detection count since construction.
    [[nodiscard]] unsigned long detections() const noexcept { return _detections; }

private:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- a connection and a phrase, not interchangeable
    void check(const ::Lightweight::SqlConnection& connection, std::string_view what) {
        if (autocommitStateOf(connection) != AutocommitState::Off) {
            // `On` is the good case. `Unknown` means the driver would not
            // answer -- reporting that as a leak would turn an unsupported
            // driver into a spurious abort, so it is deliberately not one.
            return;
        }
        ++_detections;
        std::string message = "PoolTransactionAudit: a pooled DataMapper was ";
        message += what;
        message +=
            " with SQL_ATTR_AUTOCOMMIT still OFF -- a transaction is open on it. "
            "DataMapperPool::Return performs no transaction cleanup, so the next, unrelated borrower of this "
            "connection inherits the transaction and stalls for the full 60s busy_timeout on its first write. "
            "Commit or roll back before the pooled mapper goes out of scope, and do not rely on the "
            "SqlTransaction local being declared after the mapper.";
        if (_onLeak) {
            _onLeak(message);
            return;
        }
        (void)std::fputs(message.c_str(), stderr);
        (void)std::fputc('\n', stderr);
        std::abort();
    }

    LeakHandler _onLeak;
    ::Lightweight::SqlLogger* _previous;
    unsigned long _detections = 0;
};

/// @brief Installs a process-wide `PoolTransactionAudit` with the default
///        fatal handler, exactly once per process.
///
/// Idempotent and safe to call from every rung's `db::configure()`: the second
/// and later calls do nothing. The audit outlives every pooled mapper because
/// pooled mappers are function locals, and it de-registers itself if it is
/// destroyed at exit while still installed.
///
/// Returns the audit rather than `void` because the object is genuinely
/// non-`const` -- `Lightweight::SqlLogger::SetLogger` stores a non-`const`
/// pointer to it and the pool calls back through that to mutate its detection
/// count -- and a `void` version left the local looking like a `const`
/// candidate to `misc-const-correctness`, which it is not: making it `const`
/// would leave the pool writing to a `const` object.
///
/// @return The process-wide audit. Callers that only want it installed may
///         discard it.
inline PoolTransactionAudit& installPoolTransactionAudit() {
    static PoolTransactionAudit audit;
    return audit;
}

}  // namespace morph::ladder::db
