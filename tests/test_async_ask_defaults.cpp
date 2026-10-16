// SPDX-License-Identifier: Apache-2.0

// The defaults of the asynchronous forms of IOfflineQueue, IActionLog and IReplayLedger: an implementation with no
// owner answers where it is called, through a Completion delivered on the caller's executor, and a throw from the
// synchronous call rejects that Completion instead of escaping.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <morph/journal/action_log.hpp>
#include <morph/offline/offline_queue.hpp>
#include <morph/offline/replay_ledger.hpp>
#include <morph/testing/step_executor.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// A queue that keeps its items in memory, and throws from every call once `broken` is set.
class PlainQueue final : public morph::offline::IOfflineQueue {
public:
    [[nodiscard]] std::uint64_t enqueue(std::string payload) override {
        failIfBroken();
        _items.push_back(morph::offline::QueueItem{.id = ++_next, .payload = std::move(payload)});
        return _next;
    }
    using IOfflineQueue::enqueue;
    [[nodiscard]] std::vector<morph::offline::QueueItem> drain() const override {
        failIfBroken();
        return _items;
    }
    using IOfflineQueue::drain;
    void markDone(std::uint64_t itemId) override {
        std::erase_if(_items, [&](auto const& item) { return item.id == itemId; });
    }

    bool broken = false;

private:
    void failIfBroken() const {
        if (broken) {
            throw std::runtime_error{"queue broken"};
        }
    }

    std::vector<morph::offline::QueueItem> _items;
    std::uint64_t _next = 0;
};

/// A log that keeps its entries in memory, and throws from every call once `broken` is set.
class PlainLog final : public morph::journal::IActionLog {
public:
    void append(morph::journal::LogEntry entry) override { _entries.push_back(std::move(entry)); }
    void flush() override {
        if (broken) {
            throw std::runtime_error{"disk full"};
        }
    }
    using IActionLog::flush;
    [[nodiscard]] std::vector<morph::journal::LogEntry> entries(std::string_view entityKey) const override {
        if (broken) {
            throw std::runtime_error{"log broken"};
        }
        std::vector<morph::journal::LogEntry> out;
        for (auto const& entry : _entries) {
            if (entityKey.empty() || entry.entityKey == entityKey) {
                out.push_back(entry);
            }
        }
        return out;
    }
    using IActionLog::entries;

    bool broken = false;

private:
    std::vector<morph::journal::LogEntry> _entries;
};

/// A ledger over one recorded id, and throws from a lookup once `broken` is set.
class PlainLedger final : public morph::offline::IReplayLedger {
public:
    bool broken = false;

protected:
    [[nodiscard]] std::optional<std::string> doLookup(std::string_view scope, std::string_view opId) const override {
        if (broken) {
            throw std::runtime_error{"ledger broken"};
        }
        return scope == _scope && opId == _opId ? std::optional<std::string>{_payload} : std::nullopt;
    }
    void doRecord(std::string_view scope, std::string_view opId, std::string payload) override {
        _scope = scope;
        _opId = opId;
        _payload = std::move(payload);
    }

private:
    std::string _scope;
    std::string _opId;
    std::string _payload;
};

/// The outcome of a Completion, once the executor has run.
template <typename T>
struct Outcome {
    std::optional<T> value;
    std::string error;

    void watch(morph::async::Completion<T>& completion) {
        completion.then([this](T const& answered) { value = answered; })
            .onError([this](std::exception_ptr const& failure) {
                try {
                    std::rethrow_exception(failure);
                } catch (std::exception const& thrown) {
                    error = thrown.what();
                }
            });
    }
};

}  // namespace

// Mutation: let a default ask* throw from the synchronous call (the caller's attach never runs), or deliver the
// answer inline instead of on the reply executor.
TEST_CASE("IOfflineQueue: enqueue, drain and size answer on the reply executor, and a throw rejects",
          "[offline][queue]") {
    morph::testing::StepExecutor reply;
    PlainQueue queue;
    auto enqueued = queue.enqueue(reply, "op", "key-1");
    Outcome<std::uint64_t> id;
    id.watch(enqueued);
    CHECK_FALSE(id.value.has_value());
    reply.runAll();
    CHECK(id.value == 1U);
    auto drained = queue.drain(reply);
    Outcome<std::vector<morph::offline::QueueItem>> items;
    items.watch(drained);
    auto counted = queue.size(reply);
    Outcome<std::size_t> size;
    size.watch(counted);
    reply.runAll();
    REQUIRE(items.value.has_value());
    CHECK(items.value->front().payload == "op");
    CHECK(size.value == 1U);

    queue.broken = true;
    auto refused = queue.enqueue(reply, "op2");
    Outcome<std::uint64_t> failed;
    failed.watch(refused);
    auto refusedDrain = queue.drain(reply);
    Outcome<std::vector<morph::offline::QueueItem>> failedDrain;
    failedDrain.watch(refusedDrain);
    reply.runAll();
    CHECK(failed.error == "queue broken");
    CHECK(failedDrain.error == "queue broken");
}

// Mutation: let a default ask of the log throw instead of rejecting.
TEST_CASE("IActionLog: entries and flush answer on the reply executor, and a throw rejects", "[journal]") {
    morph::testing::StepExecutor reply;
    PlainLog log;
    CHECK(log.owner() == nullptr);
    morph::journal::LogEntry entry;
    entry.entityKey = "e1";
    log.append(entry);
    auto read = log.entries(reply, "e1");
    Outcome<std::vector<morph::journal::LogEntry>> entries;
    entries.watch(read);
    auto flushed = log.flush(reply);
    Outcome<bool> durable;
    durable.watch(flushed);
    reply.runAll();
    REQUIRE(entries.value.has_value());
    CHECK(entries.value->size() == 1U);
    CHECK(durable.value == true);

    log.broken = true;
    auto refusedRead = log.entries(reply);
    Outcome<std::vector<morph::journal::LogEntry>> failedRead;
    failedRead.watch(refusedRead);
    auto refusedFlush = log.flush(reply);
    Outcome<bool> failedFlush;
    failedFlush.watch(refusedFlush);
    reply.runAll();
    CHECK(failedRead.error == "log broken");
    CHECK(failedFlush.error == "disk full");
}

// Mutation: send an empty op id to the ledger (it is not an identity), or let a lookup's throw escape.
TEST_CASE("IReplayLedger: a lookup answers on the reply executor, an empty id is a miss, and a throw rejects",
          "[offline][ledger]") {
    morph::testing::StepExecutor reply;
    PlainLedger ledger;
    ledger.record("scope", "op-1", "done");
    auto hit = ledger.lookup(reply, "scope", "op-1");
    Outcome<std::optional<std::string>> found;
    found.watch(hit);
    auto empty = ledger.lookup(reply, "scope", "");
    Outcome<std::optional<std::string>> none;
    none.watch(empty);
    reply.runAll();
    REQUIRE(found.value.has_value());
    CHECK(*found.value == "done");
    REQUIRE(none.value.has_value());
    CHECK_FALSE(none.value->has_value());

    ledger.broken = true;
    auto refused = ledger.lookup(reply, "scope", "op-1");
    Outcome<std::optional<std::string>> failed;
    failed.watch(refused);
    reply.runAll();
    CHECK(failed.error == "ledger broken");
}
