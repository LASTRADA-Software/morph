// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/level_cache.hpp
/// @brief The server's cache of a slow data level, and the source a reply reads a level through.
///
/// A `LevelCache` holds the values of one level, as its loader returned them, for every principal. A reply takes
/// one `LevelSnapshot` of each level it needs, reads the level's cells from it through a `LeveledSource`, and
/// then says which keys it needed with `want`; the cache loads the keys it neither holds nor is loading, off the
/// caller's thread, stores them, and announces them so that the clients showing those rows ask again.
///
/// See `docs/spec/table/table.md`, "The server level cache".

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <morph/core/change_types.hpp>
#include <morph/core/executor.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/query.hpp>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace morph::table {

/// @brief What a level snapshot holds for one key.
enum class LevelRead : std::uint8_t {
    Loaded,    ///< The value is held; the cells were written.
    Unloaded,  ///< No value is held: never loaded, invalidated or evicted.
    Failed,    ///< The last load of the key returned no value.
};

/// @brief Why a query needs a level's keys.
enum class LevelUse : std::uint8_t {
    Display,  ///< The reply shows the keys' cells.
    Order,    ///< The query sorts or filters by the level, so it needs every candidate row's cells.
};

/// @brief One state of a level. Immutable: nothing it returns changes while it lives, and holding it blocks no one.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
class LevelSnapshot {
public:
    virtual ~LevelSnapshot() = default;

    /// @brief Writes the row's cells for the level's columns, in the order of `LevelSource::columns()`.
    /// @param key The row's key.
    /// @param out One cell per level column; written only when the value is loaded.
    /// @return Whether the value is loaded, unloaded or failed.
    [[nodiscard]] virtual LevelRead read(RowId const& key, std::span<Cell> out) const = 0;

    /// @brief Differs between two snapshots that differ in any value.
    /// @return The snapshot's generation.
    [[nodiscard]] virtual std::uint64_t generation() const = 0;
};

/// @brief What the framework needs of a level, whatever the type of its values.
class LevelSource {
public:
    virtual ~LevelSource() = default;

    /// @brief The level's name, as a column's `ColumnInfo::level` names it.
    /// @return The name.
    [[nodiscard]] virtual std::string_view level() const = 0;

    /// @brief The level's columns. The same for every caller.
    /// @return The columns, in the order a snapshot writes a row's cells.
    [[nodiscard]] virtual std::span<ColumnInfo const> columns() const = 0;

    /// @brief The level as it is now.
    /// @return An immutable snapshot.
    [[nodiscard]] virtual std::shared_ptr<LevelSnapshot const> snapshot() const = 0;

    /// @brief Says the keys are needed.
    /// @param keys  The keys a reply needed.
    /// @param use   Why: to show them, or to sort or filter by them.
    /// @param retry Load the failed keys among @p keys now, without waiting out their retry delay.
    /// @return Nothing, or `Unavailable` when an order use cannot fit the cache's bound.
    virtual std::expected<void, TableError> want(std::span<RowId const> keys, LevelUse use, bool retry = false) = 0;
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

/// @brief How a `LevelCache` is set up; fixed at construction.
struct LevelCacheOptions {
    /// @brief The table records it serves: one, or several that share rows.
    std::vector<std::string> records;
    /// @brief The level's name.
    std::string level;
    /// @brief The level's columns, for every caller, in a fixed order.
    std::vector<ColumnInfo> columns;
    /// @brief The most bytes the values may own before the least recently used are evicted.
    std::size_t maxBytes = 256U << 20U;
    /// @brief A value's size before any is stored; 0 takes 64 bytes a column.
    std::size_t expectedBytes = 0;
    /// @brief Records whose change invalidates values. Empty: `records`.
    std::vector<std::string> invalidatedBy;
    /// @brief Where the loader runs; never the server strand. Null runs it on the thread that wants the keys.
    exec::IExecutor* loads = nullptr;
    /// @brief The most keys handed to one loader call.
    std::size_t batch = 64;
    /// @brief How long an order use keeps its keys from eviction.
    std::chrono::seconds orderLease{60};
    /// @brief How long a failed key waits before it is loaded again; doubles with each failure in a row.
    std::chrono::seconds retryAfter{30};
    /// @brief The longest a failed key waits.
    std::chrono::seconds retryMax{600};
    /// @brief The cache's "now". Empty: the steady clock.
    std::function<std::chrono::steady_clock::time_point()> clock;
};

/// @brief What a `LevelCache` has done since it was made; `bytes` is what it holds now.
struct LevelCacheStats {
    /// @brief Keys a `want` found held.
    std::size_t hits = 0;
    /// @brief Keys a `want` found not held.
    std::size_t misses = 0;
    /// @brief Calls of the loader.
    std::size_t loaderCalls = 0;
    /// @brief Keys stored from a loader's return.
    std::size_t loadedKeys = 0;
    /// @brief Keys a loader call returned no value for, or that a throwing call failed.
    std::size_t failedLoads = 0;
    /// @brief Keys a load returned after an invalidation of them, and that were dropped.
    std::size_t droppedLoads = 0;
    /// @brief Held keys (values or failures) an invalidation removed.
    std::size_t invalidatedKeys = 0;
    /// @brief Values evicted at the bound.
    std::size_t evictedKeys = 0;
    /// @brief Keys an accepted order use leased.
    std::size_t leasedKeys = 0;
    /// @brief Order uses refused because they could not fit.
    std::size_t refusedOrders = 0;
    /// @brief The deep bytes of the values held now.
    std::size_t bytes = 0;
};

template <typename Value>
class LevelCache;

namespace detail {

/// @brief The shards a level cache spreads its keys over.
inline constexpr std::size_t kLevelShards = 256;

/// @brief A cell vector's deep size: the vector, and what each cell owns.
/// @param cells The cells.
/// @return The bytes.
[[nodiscard]] inline std::size_t cellsBytes(std::vector<Cell> const& cells) noexcept {
    std::size_t bytes = sizeof(std::vector<Cell>);
    for (auto const& cell : cells) {
        bytes += cellBytes(cell);
    }
    return bytes;
}

/// @brief What fixtures and benchmarks reach of a `LevelCache` beyond its interface.
struct LevelCacheAccess {
    /// @brief Stores @p values as a load would, without a loader call and without announcing them.
    /// @tparam Value The cache's value type.
    /// @param cache  The cache.
    /// @param values The values by key.
    template <typename Value>
    static void store(LevelCache<Value>& cache, std::vector<std::pair<RowId, Value>> values) {
        cache._core->storeQuietly(std::move(values));
    }

    /// @brief How many shards stores, removals and observer calls have copied.
    /// @tparam Value The cache's value type.
    /// @param cache The cache.
    /// @return The count.
    template <typename Value>
    [[nodiscard]] static std::size_t shardCopies(LevelCache<Value> const& cache) {
        return cache._core->shardCopies.load();
    }
};

}  // namespace detail

/// @brief The server's cache of one slow level: values loaded once, kept until invalidated or evicted, read by
///        every principal through immutable snapshots.
///
/// The values live in 256 shards by key hash, each an immutable map behind a shared pointer; a snapshot is the
/// array of shard pointers. A store, a removal and an observer call copy only the shards they touch and swap
/// them under the cache's mutex, so a reader holding a snapshot blocks no one. A value's recency and order lease
/// are atomics beside it, and no part of a snapshot's state.
///
/// One loader call runs at a time. Wanted keys wait on two stacks, `Display` before `Order`, each want's keys
/// one block in the order given and the newest want on top; a batch takes up to `batch` keys from the top. A
/// load remembers each key's generation and stores only the keys no invalidation touched meanwhile; it publishes
/// the values, then announces their keys to every record in `records`.
/// @tparam Value What the loader returns for one key.
template <typename Value>
class LevelCache final : public LevelSource, public effects::IChangeObserver {
public:
    /// @brief Reads the level for `keys`. Called on `options.loads`; may block. It returns a value for every key it
    ///        read, including a row that has nothing at this level, whose value is then empty. A key it returns no
    ///        value for failed; throwing fails every key of the call.
    using Loader = std::function<std::vector<std::pair<RowId, Value>>(std::span<RowId const> keys)>;
    /// @brief A value's deep bytes.
    using SizeOf = std::function<std::size_t(Value const&)>;
    /// @brief Writes a value's cells, in `LevelCacheOptions::columns` order.
    using Cells = std::function<void(Value const&, std::span<Cell> out)>;

    /// @brief A cache of @p options' level.
    /// @param options How it is bounded, batched and invalidated.
    /// @param loader  Reads values.
    /// @param sizeOf  A value's deep bytes.
    /// @param cells   A value's cells.
    LevelCache(LevelCacheOptions options, Loader loader, SizeOf sizeOf, Cells cells)
        : _core{std::make_shared<Core>(std::move(options), std::move(loader), std::move(sizeOf), std::move(cells))} {}

    /// @brief For `Value` = `std::vector<Cell>` only: the value is the cells, and its size is their deep size.
    /// @param options How it is bounded, batched and invalidated.
    /// @param loader  Reads values.
    LevelCache(LevelCacheOptions options, Loader loader)
        requires std::same_as<Value, std::vector<Cell>>
        : LevelCache{std::move(options), std::move(loader),
                     [](std::vector<Cell> const& value) { return detail::cellsBytes(value); },
                     [](std::vector<Cell> const& value, std::span<Cell> out) {
                         auto const count = std::min(value.size(), out.size());
                         std::copy_n(value.begin(), count, out.begin());
                         std::fill(out.begin() + static_cast<std::ptrdiff_t>(count), out.end(), Cell{});
                     }} {}

    /// @brief The level's name.
    /// @return `LevelCacheOptions::level`.
    [[nodiscard]] std::string_view level() const override { return _core->options.level; }

    /// @brief The level's columns.
    /// @return `LevelCacheOptions::columns`.
    [[nodiscard]] std::span<ColumnInfo const> columns() const override { return _core->options.columns; }

    /// @brief The values held now.
    /// @return An immutable snapshot: the shard pointers and the generation, copied under the mutex.
    [[nodiscard]] std::shared_ptr<LevelSnapshot const> snapshot() const override { return _core->snapshot(); }

    /// @brief Says the keys are needed, and starts a load for those neither held, in flight, queued, nor failed
    ///        inside their retry delay.
    ///
    /// A held key is a hit and its recency is renewed. An order use is first fitted: the keys times the mean
    /// size of the values held (`expectedBytes` while none is), plus the bytes other keys' leases hold, against
    /// `maxBytes`; over it nothing changes. Otherwise every key is leased for `orderLease`.
    /// @param keys  The keys.
    /// @param use   Display or order.
    /// @param retry Load the failed keys among @p keys now.
    /// @return Nothing, or `Unavailable` for an order use that does not fit.
    std::expected<void, TableError> want(std::span<RowId const> keys, LevelUse use, bool retry = false) override {
        return Core::want(_core, keys, use, retry);
    }

    /// @brief Drops what @p changes made wrong. On the server strand; removes only.
    ///
    /// A change of a record in `invalidatedBy` that is one of `records` invalidates its keys, or the level when
    /// it has none or a key that is no row key. A change of any other record in `invalidatedBy` invalidates the
    /// level and announces each of `records` without keys.
    /// @param changes One fan-out's changes.
    void changed(effects::Changes const& changes) override { _core->changed(changes); }

    /// @brief Takes the server's announce sink.
    /// @param sink Where arrivals, failures and invalidations of the cache's own are reported.
    void bind(effects::AnnounceSink sink) override {
        std::scoped_lock const lock{_core->mutex};
        _core->sink = std::move(sink);
    }

    /// @brief Removes the values and failures of @p keys; a load of them in flight does not land.
    /// @param keys The keys.
    void invalidate(std::span<RowId const> keys) { _core->invalidate(keys); }

    /// @brief Removes every value and failure; no load in flight lands.
    void invalidateAll() { _core->invalidateAll(); }

    /// @brief Sets what is called with the keys of each load that failed, after they are stored as failed.
    /// @param handler The handler; called on the loading thread.
    void onLoadFailed(std::function<void(std::span<RowId const>)> handler) {
        std::scoped_lock const lock{_core->mutex};
        _core->failedHandler = std::move(handler);
    }

    /// @brief The counters.
    /// @return What the cache has done, and the bytes it holds.
    [[nodiscard]] LevelCacheStats stats() const { return _core->stats(); }

private:
    friend struct detail::LevelCacheAccess;

    using Clock = std::chrono::steady_clock;

    struct Slot {
        std::optional<Value> value;  // empty: the load failed
        std::uint32_t failures = 0;
        Clock::time_point failedAt{};
        std::size_t bytes = 0;
        mutable std::atomic<std::int64_t> lastUse{0};
        mutable std::atomic<std::int64_t> leaseUntil{0};  // clock ticks; 0: no lease
    };
    using Shard = std::unordered_map<RowId, std::shared_ptr<Slot const>>;
    using Shards = std::array<std::shared_ptr<Shard const>, detail::kLevelShards>;

    class Snapshot final : public LevelSnapshot {
    public:
        Snapshot(Shards shards, std::uint64_t generation, std::shared_ptr<Cells const> cells)
            : _shards{std::move(shards)}, _generation{generation}, _cells{std::move(cells)} {}

        [[nodiscard]] LevelRead read(RowId const& key, std::span<Cell> out) const override {
            auto const& shard = *_shards[Core::shardOf(key)];
            auto const found = shard.find(key);
            if (found == shard.end()) {
                return LevelRead::Unloaded;
            }
            if (!found->second->value) {
                return LevelRead::Failed;
            }
            (*_cells)(*found->second->value, out);
            return LevelRead::Loaded;
        }

        [[nodiscard]] std::uint64_t generation() const override { return _generation; }

    private:
        Shards _shards;
        std::uint64_t _generation;
        std::shared_ptr<Cells const> _cells;
    };

    // A key handed to a loader call, with the generations it was loaded at.
    struct Loading {
        RowId key;
        std::uint64_t epoch = 0;
        std::uint64_t generation = 0;
    };

    struct Block {
        std::vector<std::pair<RowId, std::uint64_t>> keys;  // key, stamp
        std::size_t next = 0;
    };

    struct Queued {
        LevelUse use = LevelUse::Display;
        std::uint64_t stamp = 0;
    };

    // The shards one change touches, copied once each, swapped in by `commit`.
    class Edit {
    public:
        explicit Edit(Shards& shards, std::atomic<std::size_t>& copies) : _shards{&shards}, _copies{&copies} {}

        [[nodiscard]] std::shared_ptr<Slot const> find(RowId const& key) const {
            auto const index = Core::shardOf(key);
            Shard const& shard = _copied[index] != nullptr ? *_copied[index] : *(*_shards)[index];
            auto const found = shard.find(key);
            return found == shard.end() ? nullptr : found->second;
        }

        Shard& shard(RowId const& key) {
            auto const index = Core::shardOf(key);
            if (_copied[index] == nullptr) {
                _copied[index] = std::make_shared<Shard>(*(*_shards)[index]);
                _copies->fetch_add(1);
            }
            return *_copied[index];
        }

        bool commit() {
            bool changed = false;
            for (std::size_t index = 0; index < detail::kLevelShards; ++index) {
                if (_copied[index] != nullptr) {
                    (*_shards)[index] = std::move(_copied[index]);
                    changed = true;
                }
            }
            return changed;
        }

    private:
        Shards* _shards;
        std::atomic<std::size_t>* _copies;
        std::array<std::shared_ptr<Shard>, detail::kLevelShards> _copied{};
    };

    // What a store leaves to be done outside the mutex.
    struct Told {
        effects::AnnounceSink sink;
        std::function<void(std::span<RowId const>)> failedHandler;
        std::vector<RowId> stored;
        std::vector<RowId> failed;
        bool overBound = false;
    };

    struct Core {
        Core(LevelCacheOptions opts, Loader load, SizeOf size, Cells cellsOf)
            : options{std::move(opts)},
              loader{std::move(load)},
              sizeOf{std::move(size)},
              cells{std::make_shared<Cells const>(std::move(cellsOf))} {
            if (options.invalidatedBy.empty()) {
                options.invalidatedBy = options.records;
            }
            if (options.batch == 0) {
                options.batch = 1;
            }
            auto const empty = std::make_shared<Shard const>();
            shards.fill(empty);
        }

        [[nodiscard]] static std::size_t shardOf(RowId const& key) noexcept {
            return std::hash<RowId>{}(key) % detail::kLevelShards;
        }

        [[nodiscard]] Clock::time_point now() const { return options.clock ? options.clock() : Clock::now(); }

        [[nodiscard]] static std::int64_t ticks(Clock::time_point at) noexcept {
            return at.time_since_epoch().count();
        }

        [[nodiscard]] std::shared_ptr<Slot const> findLocked(RowId const& key) const {
            auto const& shard = *shards[shardOf(key)];
            auto const found = shard.find(key);
            return found == shard.end() ? nullptr : found->second;
        }

        [[nodiscard]] std::uint64_t generationOf(RowId const& key) const {
            auto const found = keyGenerations.find(key);
            return found == keyGenerations.end() ? 0 : found->second;
        }

        [[nodiscard]] Clock::duration retryDelay(std::uint32_t failures) const {
            auto const doublings = std::min<std::uint32_t>(failures == 0 ? 0 : failures - 1, 30);
            auto const delay = options.retryAfter * (std::int64_t{1} << doublings);
            return std::min<Clock::duration>(delay, options.retryMax);
        }

        [[nodiscard]] std::size_t meanBytes() const {
            if (values > 0) {
                return valueBytes / values;
            }
            return options.expectedBytes != 0 ? options.expectedBytes : 64 * options.columns.size();
        }

        [[nodiscard]] std::shared_ptr<LevelSnapshot const> snapshot() const {
            Shards copy;
            std::uint64_t at = 0;
            {
                std::scoped_lock const lock{mutex};
                copy = shards;
                at = generation;
            }
            return std::make_shared<Snapshot const>(std::move(copy), at, cells);
        }

        // Puts `key` on top of `use`'s stack unless it is queued for display already.
        void enqueue(RowId const& key, LevelUse use, std::vector<std::pair<RowId, std::uint64_t>>& block) {
            auto const found = queued.find(key);
            if (found != queued.end() && found->second.use == LevelUse::Display && use == LevelUse::Order) {
                return;
            }
            auto const at = ++stamp;
            queued.insert_or_assign(key, Queued{.use = use, .stamp = at});
            block.emplace_back(key, at);
        }

        // Takes the next batch off the stacks, display first, and marks its keys in flight.
        [[nodiscard]] std::vector<Loading> takeBatch() {
            std::vector<Loading> out;
            for (auto const use : {LevelUse::Display, LevelUse::Order}) {
                auto& stack = stacks[static_cast<std::size_t>(use)];
                while (out.size() < options.batch && !stack.empty()) {
                    auto& top = stack.back();
                    while (top.next < top.keys.size() && out.size() < options.batch) {
                        auto& [key, at] = top.keys[top.next++];
                        auto const found = queued.find(key);
                        if (found == queued.end() || found->second.stamp != at) {
                            continue;
                        }
                        queued.erase(found);
                        auto const keyGeneration = generationOf(key);
                        inFlight.insert_or_assign(key, std::pair{epoch, keyGeneration});
                        out.push_back(Loading{.key = std::move(key), .epoch = epoch, .generation = keyGeneration});
                    }
                    if (top.next >= top.keys.size()) {
                        stack.pop_back();
                    }
                }
            }
            running = !out.empty();
            return out;
        }

        [[nodiscard]] bool inFlightNow(RowId const& key) const {
            auto const found = inFlight.find(key);
            return found != inFlight.end() && found->second.first == epoch &&
                   found->second.second == generationOf(key);
        }

        static std::expected<void, TableError> want(std::shared_ptr<Core> const& core, std::span<RowId const> keys,
                                                    LevelUse use, bool retry) {
            auto const at = core->now();
            std::vector<Loading> batch;
            {
                std::scoped_lock const lock{core->mutex};
                if (use == LevelUse::Order) {
                    if (auto fitted = core->fitOrder(keys, at); !fitted) {
                        return fitted;
                    }
                }
                std::vector<std::pair<RowId, std::uint64_t>> block;
                for (auto const& key : keys) {
                    auto const slot = core->findLocked(key);
                    if (slot != nullptr && slot->value) {
                        core->hits.fetch_add(1);
                        slot->lastUse.store(++core->useCounter);
                        continue;
                    }
                    core->misses.fetch_add(1);
                    if (slot != nullptr && !retry && at < slot->failedAt + core->retryDelay(slot->failures)) {
                        continue;
                    }
                    if (core->inFlightNow(key)) {
                        continue;
                    }
                    core->enqueue(key, use, block);
                }
                if (!block.empty()) {
                    core->stacks[static_cast<std::size_t>(use)].push_back(Block{.keys = std::move(block), .next = 0});
                }
                if (!core->running) {
                    batch = core->takeBatch();
                }
            }
            dispatch(core, std::move(batch));
            return {};
        }

        // Under the mutex: refuses an order use that cannot fit, or leases its keys.
        std::expected<void, TableError> fitOrder(std::span<RowId const> keys, Clock::time_point at) {
            auto const nowTicks = ticks(at);
            std::unordered_set<RowId> const own(keys.begin(), keys.end());
            std::size_t leasedOther = 0;
            for (auto const& shard : shards) {
                for (auto const& [key, slot] : *shard) {
                    if (slot->leaseUntil.load() > nowTicks && !own.contains(key)) {
                        leasedOther += slot->bytes;
                    }
                }
            }
            if (own.size() * meanBytes() + leasedOther > options.maxBytes) {
                refusedOrders.fetch_add(1);
                return std::unexpected(
                    TableError{.code = TableErrorCode::Unavailable,
                               .column = {},
                               .message = "the level " + options.level + " cannot hold every row this order needs"});
            }
            auto const until = ticks(at + options.orderLease);
            for (auto const& key : own) {
                if (auto const slot = findLocked(key); slot != nullptr && slot->value) {
                    slot->leaseUntil.store(until);
                } else {
                    pendingLeases.insert_or_assign(key, until);
                }
                leasedKeys.fetch_add(1);
            }
            return {};
        }

        static void dispatch(std::shared_ptr<Core> const& core, std::vector<Loading> batch) {
            if (batch.empty()) {
                return;
            }
            if (core->options.loads == nullptr) {
                while (!batch.empty()) {
                    batch = core->load(std::move(batch));
                }
                return;
            }
            core->options.loads->post(
                [core, batch = std::move(batch)]() mutable { dispatch(core, core->load(std::move(batch))); });
        }

        // Runs one loader call, stores and announces what it returned; returns the next batch to load.
        [[nodiscard]] std::vector<Loading> load(std::vector<Loading> batch) {
            std::vector<RowId> keys;
            keys.reserve(batch.size());
            for (auto const& loading : batch) {
                keys.push_back(loading.key);
            }
            loaderCalls.fetch_add(1);
            std::vector<std::pair<RowId, Value>> loaded;
            try {
                loaded = loader(keys);
            } catch (...) {
                loaded.clear();
            }
            Told told;
            std::vector<Loading> next;
            {
                std::scoped_lock const lock{mutex};
                told = store(batch, loaded);
                next = takeBatch();
            }
            tell(told);
            return next;
        }

        // Under the mutex: stores a load's values and failures, then applies the bound.
        [[nodiscard]] Told store(std::vector<Loading> const& batch, std::vector<std::pair<RowId, Value>>& loaded) {
            std::unordered_map<RowId, Value*> returned;
            returned.reserve(loaded.size());
            for (auto& [key, value] : loaded) {
                returned.emplace(key, &value);
            }
            auto const at = now();
            Told told;
            Edit edit{shards, shardCopies};
            for (auto const& loading : batch) {
                auto const flight = inFlight.find(loading.key);
                if (flight != inFlight.end() && flight->second == std::pair{loading.epoch, loading.generation}) {
                    inFlight.erase(flight);
                }
                if (loading.epoch != epoch || loading.generation != generationOf(loading.key)) {
                    droppedLoads.fetch_add(1);
                    continue;
                }
                auto const old = edit.find(loading.key);
                auto slot = std::make_shared<Slot>();
                if (auto const found = returned.find(loading.key); found != returned.end()) {
                    slot->value = std::move(*found->second);
                    slot->bytes = sizeOf(*slot->value);
                    slot->lastUse.store(++useCounter);
                    if (auto const lease = pendingLeases.find(loading.key); lease != pendingLeases.end()) {
                        slot->leaseUntil.store(lease->second);
                        pendingLeases.erase(lease);
                    }
                    loadedKeys.fetch_add(1);
                    told.stored.push_back(loading.key);
                } else {
                    slot->failures = old != nullptr && !old->value ? old->failures + 1 : 1;
                    slot->failedAt = at;
                    failedLoads.fetch_add(1);
                    told.failed.push_back(loading.key);
                }
                forget(old);
                remember(*slot);
                edit.shard(loading.key).insert_or_assign(loading.key, std::move(slot));
            }
            if (edit.commit()) {
                ++generation;
            }
            told.overBound = dropLeasesOverBound(at);
            evict(told.stored, at);
            told.sink = sink;
            told.failedHandler = failedHandler;
            return told;
        }

        void forget(std::shared_ptr<Slot const> const& slot) {
            if (slot != nullptr && slot->value) {
                valueBytes -= slot->bytes;
                --values;
                bytes.store(valueBytes);
            }
        }

        void remember(Slot const& slot) {
            if (slot.value) {
                valueBytes += slot.bytes;
                ++values;
                bytes.store(valueBytes);
            }
        }

        // Under the mutex: when the leased values exceed the bound, stops loading for order and drops every lease.
        bool dropLeasesOverBound(Clock::time_point at) {
            auto const nowTicks = ticks(at);
            std::size_t leased = 0;
            for (auto const& shard : shards) {
                for (auto const& [key, slot] : *shard) {
                    if (slot->leaseUntil.load() > nowTicks) {
                        leased += slot->bytes;
                    }
                }
            }
            if (leased <= options.maxBytes) {
                return false;
            }
            auto& order = stacks[static_cast<std::size_t>(LevelUse::Order)];
            order.clear();
            std::erase_if(queued, [](auto const& entry) { return entry.second.use == LevelUse::Order; });
            for (auto const& shard : shards) {
                for (auto const& [key, slot] : *shard) {
                    slot->leaseUntil.store(0);
                }
            }
            pendingLeases.clear();
            return true;
        }

        // Under the mutex: evicts the least recently used unleased values until the bound holds. The values just
        // stored are not candidates; when the bound still does not hold, they are marked to go first.
        void evict(std::vector<RowId> const& stored, Clock::time_point at) {
            if (valueBytes <= options.maxBytes) {
                return;
            }
            auto const nowTicks = ticks(at);
            std::unordered_set<RowId> const fresh(stored.begin(), stored.end());
            std::vector<std::pair<std::int64_t, RowId>> candidates;
            for (auto const& shard : shards) {
                for (auto const& [key, slot] : *shard) {
                    if (slot->value && slot->leaseUntil.load() <= nowTicks && !fresh.contains(key)) {
                        candidates.emplace_back(slot->lastUse.load(), key);
                    }
                }
            }
            std::ranges::sort(candidates);
            Edit edit{shards, shardCopies};
            for (auto const& [use, key] : candidates) {
                if (valueBytes <= options.maxBytes) {
                    break;
                }
                auto& shard = edit.shard(key);
                auto const found = shard.find(key);
                forget(found->second);
                shard.erase(found);
                evictedKeys.fetch_add(1);
            }
            if (edit.commit()) {
                ++generation;
            }
            if (valueBytes > options.maxBytes) {
                for (auto const& key : stored) {
                    if (auto const slot = findLocked(key); slot != nullptr) {
                        slot->lastUse.store(0);
                    }
                }
            }
        }

        void tell(Told const& told) const {
            if (told.sink) {
                for (auto const& record : options.records) {
                    if (!told.stored.empty()) {
                        told.sink(record, changeKeys(told.stored));
                    }
                    if (!told.failed.empty()) {
                        told.sink(record, changeKeys(told.failed));
                    }
                }
            }
            if (!told.failed.empty() && told.failedHandler) {
                told.failedHandler(told.failed);
            }
            if (told.overBound && told.sink) {
                for (auto const& record : options.records) {
                    told.sink(record, std::nullopt);
                }
            }
        }

        // Stores values as a load would, counting none as loaded and telling no one.
        void storeQuietly(std::vector<std::pair<RowId, Value>> loaded) {
            std::scoped_lock const lock{mutex};
            std::vector<Loading> batch;
            batch.reserve(loaded.size());
            for (auto const& [key, value] : loaded) {
                batch.push_back(Loading{.key = key, .epoch = epoch, .generation = generationOf(key)});
            }
            auto const told = store(batch, loaded);
            loadedKeys.fetch_sub(told.stored.size());
        }

        void invalidate(std::span<RowId const> keys) {
            std::scoped_lock const lock{mutex};
            Edit edit{shards, shardCopies};
            for (auto const& key : keys) {
                ++keyGenerations[key];
                pendingLeases.erase(key);
                if (auto const old = edit.find(key); old != nullptr) {
                    forget(old);
                    edit.shard(key).erase(key);
                    invalidatedKeys.fetch_add(1);
                }
            }
            if (edit.commit()) {
                ++generation;
            }
        }

        void invalidateAll() {
            std::scoped_lock const lock{mutex};
            ++epoch;
            std::size_t held = 0;
            for (auto const& shard : shards) {
                held += shard->size();
            }
            invalidatedKeys.fetch_add(held);
            auto const empty = std::make_shared<Shard const>();
            shards.fill(empty);
            keyGenerations.clear();
            pendingLeases.clear();
            valueBytes = 0;
            values = 0;
            bytes.store(0);
            ++generation;
        }

        void changed(effects::Changes const& changes) {
            bool other = false;
            for (auto const& change : changes.records) {
                if (std::ranges::find(options.invalidatedBy, change.record) == options.invalidatedBy.end()) {
                    continue;
                }
                if (std::ranges::find(options.records, change.record) == options.records.end()) {
                    other = true;
                    invalidateAll();
                    continue;
                }
                if (!change.keys) {
                    invalidateAll();
                    continue;
                }
                std::vector<RowId> keys;
                keys.reserve(change.keys->size());
                bool readable = true;
                for (auto const& key : *change.keys) {
                    auto id = rowIdOf(key);
                    if (!id) {
                        readable = false;
                        break;
                    }
                    keys.push_back(std::move(*id));
                }
                if (readable) {
                    invalidate(keys);
                } else {
                    invalidateAll();
                }
            }
            if (!other) {
                return;
            }
            effects::AnnounceSink told;
            {
                std::scoped_lock const lock{mutex};
                told = sink;
            }
            if (told) {
                for (auto const& record : options.records) {
                    told(record, std::nullopt);
                }
            }
        }

        [[nodiscard]] LevelCacheStats stats() const {
            return LevelCacheStats{.hits = hits.load(),
                                   .misses = misses.load(),
                                   .loaderCalls = loaderCalls.load(),
                                   .loadedKeys = loadedKeys.load(),
                                   .failedLoads = failedLoads.load(),
                                   .droppedLoads = droppedLoads.load(),
                                   .invalidatedKeys = invalidatedKeys.load(),
                                   .evictedKeys = evictedKeys.load(),
                                   .leasedKeys = leasedKeys.load(),
                                   .refusedOrders = refusedOrders.load(),
                                   .bytes = bytes.load()};
        }

        LevelCacheOptions options;
        Loader loader;
        SizeOf sizeOf;
        std::shared_ptr<Cells const> cells;

        mutable std::mutex mutex;
        Shards shards;
        std::uint64_t generation = 0;
        std::uint64_t epoch = 0;
        std::unordered_map<RowId, std::uint64_t> keyGenerations;
        std::unordered_map<RowId, std::pair<std::uint64_t, std::uint64_t>> inFlight;
        std::array<std::vector<Block>, 2> stacks;
        std::unordered_map<RowId, Queued> queued;
        std::uint64_t stamp = 0;
        bool running = false;
        std::unordered_map<RowId, std::int64_t> pendingLeases;
        std::size_t valueBytes = 0;
        std::size_t values = 0;
        std::int64_t useCounter = 0;
        effects::AnnounceSink sink;
        std::function<void(std::span<RowId const>)> failedHandler;

        std::atomic<std::size_t> hits{0};
        std::atomic<std::size_t> misses{0};
        std::atomic<std::size_t> loaderCalls{0};
        std::atomic<std::size_t> loadedKeys{0};
        std::atomic<std::size_t> failedLoads{0};
        std::atomic<std::size_t> droppedLoads{0};
        std::atomic<std::size_t> invalidatedKeys{0};
        std::atomic<std::size_t> evictedKeys{0};
        std::atomic<std::size_t> leasedKeys{0};
        std::atomic<std::size_t> refusedOrders{0};
        std::atomic<std::size_t> bytes{0};
        std::atomic<std::size_t> shardCopies{0};
    };

    std::shared_ptr<Core> _core;
};

/// @brief The level cache whose value is a row's cells, in the level's column order.
using CellLevelCache = LevelCache<std::vector<Cell>>;

/// @brief One level a reply reads: its source and the snapshot taken at the reply's start.
struct LevelInput {
    /// @brief The level.
    std::shared_ptr<LevelSource> source;
    /// @brief The level's state the reply reads.
    std::shared_ptr<LevelSnapshot const> snapshot;
};

/// @brief One request's immutable view of a table: base-level columns from the table's source, level columns
///        from the level snapshots the request took.
///
/// A level column is read from its level's snapshot at the column's place in `LevelSource::columns()`, found by
/// id, so a caller whose description holds the level's columns in another order or only some of them reads each
/// under its own id. A level column is `Unloaded` unless its snapshot reads the row `Loaded`; a level no input
/// serves is `Unloaded` in every row.
class LeveledSource final : public DataSource {
public:
    /// @brief The view of @p base and @p levels under @p columns.
    /// @param base    The table's rows; its snapshot is taken here, once.
    /// @param columns The columns the view has: the description's.
    /// @param levels  The levels the request read, each with its snapshot.
    LeveledSource(std::shared_ptr<DataSource const> base, std::vector<ColumnInfo> columns,
                  std::vector<LevelInput> levels)
        : _columns{std::move(columns)} {
        auto const baseColumns = base->columns();
        std::vector<Plan> plans;
        plans.reserve(_columns.size());
        for (auto const& column : _columns) {
            Plan plan;
            if (column.level.empty()) {
                if (auto const index = detail::findColumn(baseColumns, column.id)) {
                    plan.base = *index;
                }
            } else {
                for (std::size_t input = 0; input < levels.size(); ++input) {
                    if (levels[input].source == nullptr || levels[input].source->level() != column.level) {
                        continue;
                    }
                    if (auto const place = detail::findColumn(levels[input].source->columns(), column.id)) {
                        plan.level = input;
                        plan.place = *place;
                    }
                    break;
                }
            }
            plans.push_back(plan);
        }
        std::vector<Level> read;
        read.reserve(levels.size());
        for (auto& input : levels) {
            auto const width = input.source != nullptr ? input.source->columns().size() : 0;
            read.push_back(Level{.name = input.source != nullptr ? std::string{input.source->level()} : std::string{},
                                 .snapshot = std::move(input.snapshot),
                                 .width = width});
        }
        _snapshot = std::make_shared<Snapshot const>(base->snapshot(), std::move(plans), std::move(read));
    }

    /// @brief The view's columns.
    /// @return The columns given.
    [[nodiscard]] std::span<ColumnInfo const> columns() const override { return _columns; }

    /// @brief The view's one snapshot.
    /// @return The snapshot.
    [[nodiscard]] std::shared_ptr<RowSnapshot const> snapshot() const override { return _snapshot; }

    /// @brief Does nothing: the view never changes.
    /// @param listener Ignored.
    void subscribe(ChangeListener& listener) override { static_cast<void>(listener); }

    /// @brief Does nothing: the view never changes.
    /// @param listener Ignored.
    void unsubscribe(ChangeListener& listener) override { static_cast<void>(listener); }

    /// @brief What a level holds for a row.
    /// @param row   Row index of the view's snapshot.
    /// @param level The level.
    /// @return The level snapshot's read of the row's key; `Unloaded` for a level no input serves.
    [[nodiscard]] LevelRead state(std::size_t row, std::string_view level) const {
        return _snapshot->state(row, level);
    }

private:
    struct Plan {
        std::optional<std::size_t> base;
        std::optional<std::size_t> level;
        std::size_t place = 0;
    };

    struct Level {
        std::string name;
        std::shared_ptr<LevelSnapshot const> snapshot;
        std::size_t width = 0;
    };

    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- column and level indices come from the plans built against the same lists
    class Snapshot final : public RowSnapshot {
    public:
        Snapshot(std::shared_ptr<RowSnapshot const> base, std::vector<Plan> plans, std::vector<Level> levels)
            : _base{std::move(base)}, _plans{std::move(plans)}, _levels{std::move(levels)} {}

        [[nodiscard]] std::size_t rowCount() const override { return _base->rowCount(); }

        [[nodiscard]] RowId rowId(std::size_t row) const override { return _base->rowId(row); }

        [[nodiscard]] Cell cell(std::size_t row, std::size_t column) const override {
            auto const& plan = _plans[column];
            if (plan.base) {
                return _base->cell(row, *plan.base);
            }
            if (!plan.level) {
                return Unloaded{};
            }
            auto const& level = _levels[*plan.level];
            std::vector<Cell> buffer(level.width);
            if (level.snapshot->read(_base->rowId(row), buffer) != LevelRead::Loaded) {
                return Unloaded{};
            }
            return std::move(buffer[plan.place]);
        }

        void readColumn(std::size_t column, ColumnSink& sink, std::size_t first, std::size_t last) const override {
            auto const& plan = _plans[column];
            if (plan.base) {
                _base->readColumn(*plan.base, sink, first, last);
                return;
            }
            constexpr std::size_t kRun = 256;
            std::vector<Cell> run;
            run.reserve(kRun);
            std::vector<Cell> buffer(plan.level ? _levels[*plan.level].width : 0);
            for (std::size_t begin = first; begin < last; begin += kRun) {
                auto const end = std::min(last, begin + kRun);
                run.clear();
                for (std::size_t row = begin; row < end; ++row) {
                    if (plan.level &&
                        _levels[*plan.level].snapshot->read(_base->rowId(row), buffer) == LevelRead::Loaded) {
                        run.push_back(std::move(buffer[plan.place]));
                    } else {
                        run.emplace_back(Unloaded{});
                    }
                }
                sink.cells(begin, run);
            }
        }

        [[nodiscard]] LevelRead state(std::size_t row, std::string_view level) const {
            for (auto const& entry : _levels) {
                if (entry.name == level) {
                    std::vector<Cell> buffer(entry.width);
                    return entry.snapshot->read(_base->rowId(row), buffer);
                }
            }
            return LevelRead::Unloaded;
        }

    private:
        std::shared_ptr<RowSnapshot const> _base;
        std::vector<Plan> _plans;
        std::vector<Level> _levels;
    };
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

    std::vector<ColumnInfo> _columns;
    std::shared_ptr<Snapshot const> _snapshot;
};

}  // namespace morph::table
