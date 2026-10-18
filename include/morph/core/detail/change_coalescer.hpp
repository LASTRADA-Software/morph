// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../change_types.hpp"
#include "../wire.hpp"

/// @file
/// @brief What `RemoteServer` has queued for one connection's next `changes`
///        event.

namespace morph::backend::detail {

/// @brief The queue size at which `RemoteServer` sends a connection's queued
///        changes without waiting for its coalescing window to close.
///
/// Counted as `ChangeCoalescer::keyCount()` counts. It bounds how much one event
/// carries, so a burst of keyed writes is sent in pieces a client can refetch
/// for, rather than held until one body nears `effects::kMaxChangeKeys`.
inline constexpr std::size_t kCoalesceKeyLimit = 256;

/// @brief The changes queued for one connection, merged into the body of its
///        next `changes` event.
///
/// - **Records are merged per type.** Keys are appended in arrival order, each
///   key text once.
/// - **A whole-type change absorbs keys.** A record queued without keys (a
///   `changes` effect, or a type whose keys were stripped for this recipient)
///   stays whole-type: the keys queued before it are discarded and the keys
///   queued after it are ignored. A merge never adds keys to a whole-type entry,
///   so a recipient that was sent no keys for a record is sent none.
/// - **Never more than `effects::kMaxChangeKeys` keys in one record.** A record
///   whose merged keys would exceed it becomes whole-type, which over-notifies
///   but never misses, and which the client's reader would otherwise refuse.
/// - **Instance topics are deduplicated** on `{modelType, primary, instance}`.
///
/// Not synchronised: `RemoteServer` keeps one per connection on its strand.
class ChangeCoalescer {
public:
    /// @brief Queues what one execute changed.
    /// @param changes The changes to merge into the queue.
    void add(::morph::effects::Changes const& changes) {
        for (auto const& change : changes.records) {
            addRecord(change);
        }
        for (auto const& change : changes.instances) {
            addInstance(change.modelType, change.primary, change.instance);
        }
    }

    /// @brief Queues one recipient's share of a fan-out, already narrowed for it.
    /// @param body The topics this connection is to be sent.
    void add(::morph::wire::ChangesBody const& body) {
        for (auto const& change : body.records) {
            addRecord(change);
        }
        for (auto const& topic : body.instances) {
            addInstance(topic.modelType, topic.primary, topic.instance);
        }
    }

    /// @brief How much is queued: one per key, plus one per whole-type record
    ///        and one per instance topic.
    ///
    /// A topic without keys is counted, so the count bounds the topics of the
    /// next event as well as its keys.
    /// @return The count `kCoalesceKeyLimit` is compared with.
    [[nodiscard]] std::size_t keyCount() const { return _keyCount; }

    /// @brief Whether nothing is queued.
    /// @return `true` when `take()` would return an empty body.
    [[nodiscard]] bool empty() const { return _records.empty() && _instances.empty(); }

    /// @brief Hands over the merged body and empties the queue.
    /// @return Every queued record, in the order its type was first queued, and
    ///         every distinct instance topic, in the order first queued.
    [[nodiscard]] ::morph::wire::ChangesBody take() {
        ::morph::wire::ChangesBody body;
        body.records.reserve(_records.size());
        for (auto& entry : _records) {
            ::morph::effects::RecordChange change{.record = std::move(entry.record), .keys = std::nullopt};
            if (!entry.whole) {
                change.keys = std::move(entry.keys);
            }
            body.records.push_back(std::move(change));
        }
        body.instances = std::move(_instances);
        _records.clear();
        _recordIndex.clear();
        _instances.clear();
        _seenInstances.clear();
        _keyCount = 0;
        return body;
    }

private:
    /// One record type's merged change.
    struct Entry {
        std::string record;
        bool whole{false};
        std::vector<glz::raw_json> keys;
        std::unordered_set<std::string> seen;
    };

    void addRecord(::morph::effects::RecordChange const& change) {
        auto [found, inserted] = _recordIndex.try_emplace(change.record, _records.size());
        if (inserted) {
            _records.push_back(Entry{.record = change.record, .whole = false, .keys = {}, .seen = {}});
        }
        Entry& entry = _records[found->second];
        if (entry.whole) {
            return;
        }
        if (!change.keys) {
            makeWhole(entry);
            return;
        }
        for (auto const& key : *change.keys) {
            if (entry.seen.insert(key.str).second) {
                entry.keys.push_back(key);
                ++_keyCount;
            }
        }
        if (entry.keys.size() > ::morph::effects::kMaxChangeKeys) {
            makeWhole(entry);
        }
    }

    void makeWhole(Entry& entry) {
        _keyCount -= entry.keys.size();
        ++_keyCount;
        entry.whole = true;
        entry.keys = {};
        entry.seen = {};
    }

    void addInstance(std::string_view modelType, std::string_view primary, std::uint64_t instance) {
        if (_seenInstances.emplace(std::string{modelType}, std::string{primary}, instance).second) {
            _instances.push_back(::morph::wire::InstanceTopic{
                .modelType = std::string{modelType}, .primary = std::string{primary}, .instance = instance});
            ++_keyCount;
        }
    }

    std::vector<Entry> _records;
    std::unordered_map<std::string, std::size_t> _recordIndex;
    std::vector<::morph::wire::InstanceTopic> _instances;
    std::set<std::tuple<std::string, std::string, std::uint64_t>> _seenInstances;
    std::size_t _keyCount{0};
};

}  // namespace morph::backend::detail
