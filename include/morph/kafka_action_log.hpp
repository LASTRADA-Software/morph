// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "action_log.hpp"
#include "registry.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace morph::journal::kafka {

/// @brief Minimal producer surface `KafkaActionLog` needs.
///
/// Deliberately narrow — this is the seam where a real librdkafka-backed
/// implementation plugs in later. Nothing in this header depends on librdkafka
/// or a live broker; `FakeProducer` below is the in-memory reference
/// implementation used for testing the partitioning/coalescing logic in isolation.
struct IProducer {
    virtual ~IProducer() = default;

    /// @brief Publishes @p value under @p key to @p topic.
    ///
    /// Ordering guarantee mirrors real Kafka: messages sharing @p key within
    /// @p topic are expected to preserve relative order (as a real partitioner
    /// keyed on @p key would provide by routing them to the same partition).
    /// Messages with different keys have no ordering guarantee relative to
    /// each other.
    /// @param topic Topic to publish to.
    /// @param key   Partition/compaction key.
    /// @param value Opaque message payload.
    virtual void produce(std::string_view topic, std::string_view key, std::string value) = 0;

    /// @brief Blocks until all previously produced messages are durably sent.
    virtual void flush() = 0;
};

/// @brief In-memory `IProducer` — no Kafka broker or client library required.
///
/// Records every produced message, in order, per topic. `compactedView()`
/// additionally shows what a **compacted** Kafka topic would retain: only the
/// latest value per key, first-seen position preserved — exactly what real
/// log compaction provides, computed here for the fake so the key scheme in
/// `KafkaActionLog` can be tested without a broker.
class FakeProducer : public IProducer {
public:
    /// @brief One recorded `produce()` call.
    struct Message {
        /// @brief Partition/compaction key the message was produced with.
        std::string key;
        /// @brief Opaque message payload.
        std::string value;
    };

    /// @brief Appends the message to @p topic's in-memory log. Thread-safe.
    /// @param topic Topic to publish to.
    /// @param key   Partition/compaction key.
    /// @param value Opaque message payload.
    void produce(std::string_view topic, std::string_view key, std::string value) override {
        std::scoped_lock lock{_mtx};
        _topics[std::string{topic}].push_back(Message{.key = std::string{key}, .value = std::move(value)});
    }

    /// @brief Records that a flush happened; nothing to actually wait for in-memory.
    void flush() override {
        std::scoped_lock lock{_mtx};
        ++_flushCount;
    }

    /// @brief Every message produced to @p topic, in produce order. Thread-safe.
    /// @param topic Topic to read.
    /// @return Messages in produce order; empty if the topic has never been produced to.
    [[nodiscard]] std::vector<Message> raw(std::string_view topic) const {
        std::scoped_lock lock{_mtx};
        auto iter = _topics.find(std::string{topic});
        return iter == _topics.end() ? std::vector<Message>{} : iter->second;
    }

    /// @brief The latest message per key in @p topic, first-seen key order —
    ///        what a compacted Kafka topic would retain. Thread-safe.
    /// @param topic Topic to read.
    /// @return One message per distinct key, in first-seen order.
    [[nodiscard]] std::vector<Message> compactedView(std::string_view topic) const {
        std::vector<Message> out;
        std::unordered_map<std::string, std::size_t> indexOfKey;
        for (auto& msg : raw(topic)) {
            auto iter = indexOfKey.find(msg.key);
            if (iter == indexOfKey.end()) {
                indexOfKey.emplace(msg.key, out.size());
                out.push_back(msg);
            } else {
                out[iter->second] = msg;
            }
        }
        return out;
    }

    /// @brief Number of `flush()` calls so far. Thread-safe.
    /// @return Total `flush()` call count.
    [[nodiscard]] int flushCount() const {
        std::scoped_lock lock{_mtx};
        return _flushCount;
    }

private:
    mutable std::mutex _mtx;
    std::unordered_map<std::string, std::vector<Message>> _topics;
    int _flushCount{0};
};

/// @brief `IActionLog` that publishes entries to a Kafka-shaped `IProducer`.
///
/// The key scheme is what lets a single compacted topic serve both coalescing
/// policies without any coalescing logic living in this class:
/// - `ActionLogPolicy::coalesce == true`  → key = `(modelType, entityKey, actionType)`.
///   Kafka's own log compaction keeps only the latest message per key —
///   last-write-wins, for free, exactly like `SessionLog::checkpoint()`'s
///   in-process coalescing but performed by the broker instead.
/// - `ActionLogPolicy::coalesce == false` → the entry's `seq` is folded into the
///   key, so every entry gets a unique key and compaction never merges
///   distinct events (deposits, withdrawals, ...).
///
/// `append()` looks up each action's policy via the same
/// `ActionDispatcher::coalesce()` query `SessionLog` uses — pass the same
/// dispatcher the actions were registered against (defaults to the
/// process-level singleton).
///
/// @par What this does not do
/// `entries()` has no cheap implementation against a write-oriented producer —
/// reading history back out of Kafka is a consumer's job (a real deployment
/// would run a dedicated consumer or Kafka Streams app; see the design note on
/// issue #3, phase 4). It throws rather than silently returning nothing.
class KafkaActionLog : public IActionLog {
public:
    /// @param producer   Sink entries are published to. Referenced, not owned —
    ///                   must outlive this `KafkaActionLog`.
    /// @param topic      Kafka topic name entries are published to.
    /// @param dispatcher Supplies each action's coalesce policy; defaults to
    ///                   the process-level singleton (the same one actions
    ///                   were registered against via `BRIDGE_REGISTER_ACTION`).
    explicit KafkaActionLog(
        IProducer& producer, std::string topic,
        ::morph::model::detail::ActionDispatcher& dispatcher = ::morph::model::detail::defaultDispatcher())
        : _producer{producer}, _topic{std::move(topic)}, _dispatcher{dispatcher} {}

    /// @brief Publishes @p entry under a key derived from its coalesce policy.
    /// @param entry Entry to publish; `seq` is overwritten regardless of the input value.
    void append(LogEntry entry) override {
        entry.seq = _nextSeq.fetch_add(1) + 1;
        auto key = keyFor(entry);
        _producer.produce(_topic, key, toJson(entry));
    }

    /// @brief Forwards to the underlying producer's `flush()`.
    void flush() override { _producer.flush(); }

    /// @brief Not supported for a write-oriented Kafka sink.
    /// @return Never returns.
    /// @throws std::logic_error always.
    [[nodiscard]] std::vector<LogEntry> entries(std::string_view /*entityKey*/ = {}) const override {
        throw std::logic_error(
            "KafkaActionLog::entries() is not supported — read the topic with a consumer instead");
    }

private:
    [[nodiscard]] std::string keyFor(const LogEntry& entry) const {
        std::string key = entry.modelType + '\0' + entry.entityKey + '\0' + entry.actionType;
        if (!_dispatcher.coalesce(entry.modelType, entry.actionType)) {
            key += '\0' + std::to_string(entry.seq);
        }
        return key;
    }

    IProducer& _producer;
    std::string _topic;
    ::morph::model::detail::ActionDispatcher& _dispatcher;
    std::atomic<uint64_t> _nextSeq{0};
};

}  // namespace morph::journal::kafka
