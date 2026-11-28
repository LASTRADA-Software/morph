// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/table/view_cache.hpp>.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <future>
#include <list>
#include <map>
#include <memory>
#include <morph/table/data_source.hpp>
#include <morph/table/query.hpp>
#include <morph/table/runtime.hpp>
#include <morph/table/view_cache.hpp>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- source rows are below the snapshot's row count

namespace morph::table {

std::span<std::uint32_t const> ViewEntry::viewRowOf() const {
    build();
    return _viewRowOf;
}

std::optional<std::uint32_t> ViewEntry::sourceRowOf(RowId const& key) const {
    build();
    auto const hash = std::hash<RowId>{}(key);
    auto const [first, last] =
        std::ranges::equal_range(_byHash, hash, {}, &std::pair<std::size_t, std::uint32_t>::first);
    for (auto at = first; at != last; ++at) {
        if (snapshot->rowId(at->second) == key) {
            return at->second;
        }
    }
    return std::nullopt;
}

std::size_t ViewEntry::bytes() const {
    auto out = (4 * (view.rows.size() + view.undecided.size())) + _orderKeyBytes;
    if (_built.load()) {
        out += (sizeof(std::uint32_t) + sizeof(std::pair<std::size_t, std::uint32_t>)) * snapshot->rowCount();
    }
    return out;
}

std::size_t ViewEntry::keysBytes(std::vector<RowId> const* keys) noexcept {
    if (keys == nullptr) {
        return 0;
    }
    auto out = sizeof(std::vector<RowId>) + ((keys->capacity() - keys->size()) * sizeof(RowId));
    for (auto const& key : *keys) {
        out += rowIdBytes(key);
    }
    return out;
}

void ViewEntry::build() const {
    std::call_once(_once, [this] {
        auto const rows = snapshot->rowCount();
        _viewRowOf.assign(rows, detail::kNotInView);
        for (std::size_t at = 0; at < view.rows.size(); ++at) {
            _viewRowOf[view.rows[at]] = static_cast<std::uint32_t>(at);
        }
        _byHash.reserve(rows);
        for (std::size_t row = 0; row < rows; ++row) {
            _byHash.emplace_back(std::hash<RowId>{}(snapshot->rowId(row)), static_cast<std::uint32_t>(row));
        }
        std::ranges::sort(_byHash);
        _built.store(true);
    });
}

ViewCache::Found ViewCache::find(ViewKey const& key, std::shared_ptr<RowSnapshot const> const& snapshot,
                                 std::span<std::pair<std::string, std::uint64_t> const> levels,
                                 std::function<std::expected<ViewComputation, TableError>()> const& compute,
                                 std::uint64_t newness) {
    std::vector<std::pair<std::string, std::uint64_t>> const wanted(levels.begin(), levels.end());
    auto const* const identity = snapshot->identity();
    std::shared_ptr<Running> running;
    // Entries leaving the cache: released after the mutex, since the last of one may free a whole snapshot.
    std::vector<std::shared_ptr<ViewEntry const>> released;
    {
        std::unique_lock lock{_mutex};
        if (auto const found = _entries.find(key); found != _entries.end()) {
            auto const& slot = found->second;
            if (slot.entry->snapshot->identity() == identity && slot.entry->levels == wanted) {
                _hits.fetch_add(1);
                _recency.splice(_recency.begin(), _recency, slot.recency);
                auto entry = slot.entry;
                remeasure(found->second);
                bound(released);
                lock.unlock();
                released.clear();
                return entry;
            }
            if (atLeastAsNew(newness, wanted, slot.newness, slot.entry->levels)) {
                drop(found, released);
                _invalidations.fetch_add(1);
            }
        }
        _misses.fetch_add(1);
        for (auto const& other : _running) {
            if (other->key == key && other->identity == identity && other->levels == wanted) {
                _waits.fetch_add(1);
                auto const done = other->done;
                lock.unlock();
                return done.get();
            }
        }
        running = std::make_shared<Running>();
        running->key = key;
        running->identity = identity;
        running->levels = wanted;
        running->snapshot = snapshot;
        running->newness = newness;
        running->order = ++_started;
        running->done = running->promise.get_future().share();
        _running.push_back(running);
        _computations.fetch_add(1);
    }
    Found result;
    try {
        if (_options.onCompute) {
            _options.onCompute();
        }
        auto computed = compute();
        if (computed) {
            result = std::make_shared<ViewEntry const>(snapshot, wanted, std::move(*computed));
        } else {
            result = std::unexpected(std::move(computed.error()));
        }
    } catch (...) {
        finish(running, {});
        running->promise.set_exception(std::current_exception());
        throw;
    }
    finish(running, result);
    running->promise.set_value(result);
    return result;
}

void ViewCache::finish(std::shared_ptr<Running> const& running, Found const& result) {
    std::vector<std::shared_ptr<ViewEntry const>> released;
    std::unique_lock lock{_mutex};
    std::erase(_running, running);
    if (!result || *result == nullptr) {
        return;
    }
    if (auto const found = _entries.find(running->key); found != _entries.end()) {
        auto const& slot = found->second;
        // Replaced only by an entry at least as new in its rows and in every order level; of two equally new,
        // the later computation's.
        bool const newer = atLeastAsNew(running->newness, running->levels, slot.newness, slot.entry->levels);
        bool const same = newer && atLeastAsNew(slot.newness, slot.entry->levels, running->newness, running->levels);
        if (!newer || (same && slot.order > running->order)) {
            return;
        }
        drop(found, released);
    }
    _recency.push_front(running->key);
    auto [slot, inserted] = _entries.emplace(running->key, Slot{.entry = *result,
                                                                .newness = running->newness,
                                                                .order = running->order,
                                                                .bytes = 0,
                                                                .recency = _recency.begin()});
    static_cast<void>(inserted);
    remeasure(slot->second);
    bound(released);
    lock.unlock();
    released.clear();
}

void ViewCache::remeasure(Slot& slot) {
    auto const now = slot.entry->bytes();
    _held = _held - slot.bytes + now;
    slot.bytes = now;
    _bytes.store(_held);
}

bool ViewCache::atLeastAsNew(std::uint64_t newness, std::vector<std::pair<std::string, std::uint64_t>> const& levels,
                             std::uint64_t otherNewness,
                             std::vector<std::pair<std::string, std::uint64_t>> const& otherLevels) {
    if (newness < otherNewness) {
        return false;
    }
    for (auto const& [level, generation] : otherLevels) {
        auto const found = std::ranges::find(levels, level, &std::pair<std::string, std::uint64_t>::first);
        if (found != levels.end() && found->second < generation) {
            return false;
        }
    }
    return true;
}

void ViewCache::drop(Entries::iterator found, std::vector<std::shared_ptr<ViewEntry const>>& released) {
    _held -= found->second.bytes;
    _bytes.store(_held);
    _recency.erase(found->second.recency);
    released.push_back(std::move(found->second.entry));
    _entries.erase(found);
}

void ViewCache::bound(std::vector<std::shared_ptr<ViewEntry const>>& released) {
    while (_held > _options.maxBytes && !_recency.empty()) {
        auto const found = _entries.find(_recency.back());
        drop(found, released);
        _evictions.fetch_add(1);
    }
}

}  // namespace morph::table

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
