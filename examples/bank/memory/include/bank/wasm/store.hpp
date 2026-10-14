// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

/// @file
/// A tiny in-memory data store for the WebAssembly build of the bank.
///
/// Lightweight (ODBC/SQLite) cannot run in a browser, so the WASM models persist
/// here instead. morph runs each model on its own strand and WebAssembly is
/// single-threaded, so no locking is needed. Rows mirror the native entity
/// records (`include/bank/db/*_entity.hpp`) but with plain types and plain
/// integer foreign keys — the relations the native schema expresses with
/// `BelongsTo`/`HasMany` are just `*_id` fields filtered here.

namespace bank::wasm {

/// @brief A generic in-memory table keyed by an auto-incrementing id.
template <typename Row>
class Table {
public:
    /// Inserts @p row, assigning it a fresh id, and returns that id.
    std::uint64_t insert(Row row) {
        const std::uint64_t id = _nextId++;
        row.id = id;
        _rows.emplace(id, std::move(row));
        return id;
    }

    /// Returns a pointer to the row with @p id, or nullptr if absent.
    [[nodiscard]] Row* find(std::uint64_t id) {
        auto it = _rows.find(id);
        return it == _rows.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const Row* find(std::uint64_t id) const {
        auto it = _rows.find(id);
        return it == _rows.end() ? nullptr : &it->second;
    }

    /// Overwrites the stored row that shares @p row's id.
    void update(const Row& row) { _rows[row.id] = row; }

    /// Removes the row with @p id (no-op if absent).
    void erase(std::uint64_t id) { _rows.erase(id); }

    /// Returns copies of all rows satisfying @p pred.
    template <typename Pred>
    [[nodiscard]] std::vector<Row> where(Pred pred) const {
        std::vector<Row> out;
        for (const auto& [id, row] : _rows) {
            if (pred(row)) {
                out.push_back(row);
            }
        }
        return out;
    }

    /// Returns copies of every row.
    [[nodiscard]] std::vector<Row> all() const {
        return where([](const Row&) { return true; });
    }

private:
    std::unordered_map<std::uint64_t, Row> _rows;
    std::uint64_t _nextId = 1;
};

// ── Row types (one per table) ────────────────────────────────────────────────

struct UserRow {
    std::uint64_t id = 0;
    std::string username;
    std::string passwordHash;
    std::string displayName;
    int status = 0;
};

struct AccountRow {
    std::uint64_t id = 0;
    std::uint64_t userId = 0;
    std::string number;
    int kind = 0;
    int currency = 0;
    std::int64_t balanceMinor = 0;
    std::int64_t overdraftMinor = 0;
    int status = 0;
    int interestBps = 0;
};

struct TxnRow {
    std::uint64_t id = 0;
    std::uint64_t accountId = 0;
    std::int64_t counterpartyId = 0;  ///< 0 = none (deposits/withdrawals)
    int direction = 0;
    int kind = 0;
    std::int64_t amountMinor = 0;
    int currency = 0;
    std::int64_t balanceAfterMinor = 0;
    std::string description;
    std::int64_t createdAtMs = 0;
};

struct CardRow {
    std::uint64_t id = 0;
    std::uint64_t accountId = 0;
    std::uint64_t userId = 0;
    int kind = 0;
    std::string panLast4;
    int status = 0;
    std::int64_t dailyLimitMinor = 0;
    std::string pinHash;
};

struct PayeeRow {
    std::uint64_t id = 0;
    std::uint64_t userId = 0;
    std::string name;
    std::string iban;
    std::string bankName;
};

struct PaymentRow {
    std::uint64_t id = 0;
    std::uint64_t userId = 0;
    std::uint64_t fromAccountId = 0;
    std::uint64_t payeeId = 0;
    std::int64_t amountMinor = 0;
    int currency = 0;
    int schedule = 0;
    int status = 0;
    std::int64_t dueAtMs = 0;
    int intervalDays = 0;
    std::string description;
};

struct LoanRow {
    std::uint64_t id = 0;
    std::uint64_t userId = 0;
    std::uint64_t accountId = 0;
    std::int64_t principalMinor = 0;
    std::int64_t outstandingMinor = 0;
    int currency = 0;
    int rateBps = 0;
    int termMonths = 0;
    int status = 0;
    std::int64_t createdAtMs = 0;
};

/// @brief The whole database — one shared instance for all models.
struct Db {
    Table<UserRow> users;
    Table<AccountRow> accounts;
    Table<TxnRow> txns;
    Table<CardRow> cards;
    Table<PayeeRow> payees;
    Table<PaymentRow> payments;
    Table<LoanRow> loans;
};

/// @brief The process-wide store shared by every model (mirrors the single
///        on-disk SQLite file the native build shares across its models).
[[nodiscard]] inline Db& sharedDb() {
    static Db db;
    return db;
}

}  // namespace bank::wasm
