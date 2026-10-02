// SPDX-License-Identifier: Apache-2.0
//
// Storage has an owner: an action log, an offline queue and a replay ledger
// keep their state on the executor they were given. A write from another
// thread is posted there; a read from another thread is answered there and
// delivered on the reader's own executor. See docs/spec/journal/journal.md
// and docs/spec/offline/offline.md, "One owner".
//
// Every check here reads the executor scope through the owner probe
// (`owner_probe_recorder.hpp`), not the component's own answer: a body run
// inline on the calling thread is recorded as off the owner.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/model.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/file_action_log.hpp>
#include <morph/journal/journal.hpp>
#include <morph/offline/file_offline_queue.hpp>
#include <morph/offline/offline_queue.hpp>
#include <morph/offline/replay_ledger.hpp>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "owner_probe_recorder.hpp"
#include "test_support.hpp"

namespace {

using morph::journal::LogEntry;
using morph::testing::OwnerProbeRecorder;

}  // namespace

// Defined outside the anonymous namespace: the registration macros and the
// codec name them.
struct SOAdd {
    int amount = 0;
};

struct SOStoreModel {
    int total = 0;
    int execute(const SOAdd& add) {
        total += add.amount;
        return total;
    }
};

BRIDGE_REGISTER_MODEL(SOStoreModel, "SO_Model")
BRIDGE_REGISTER_ACTION(SOStoreModel, SOAdd, "SO_Add")

namespace {

/// Runs @p work as a task of @p pool and waits for it.
template <typename F>
void onPool(morph::exec::ThreadPoolExecutor& pool, F work) {
    std::atomic<bool> done{false};
    pool.post([&] {
        work();
        done = true;
    });
    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
}

/// Builds a @p T inside a task of @p owner, so the thread running the test is
/// not its owner: only the owner's tasks are.
template <typename T, typename... Args>
std::unique_ptr<T> buildOnOwner(morph::exec::MainThreadExecutor& owner, Args&&... args) {
    std::unique_ptr<T> built;
    owner.post([&] { built = std::make_unique<T>(owner, std::forward<Args>(args)...); });
    owner.drain();
    REQUIRE(built != nullptr);
    return built;
}

/// What a read delivered, and whether it was delivered inside a task of the
/// reply executor.
template <typename T>
struct Delivered {
    std::optional<T> value;
    bool onReplyExec = false;
};

/// Attaches to @p completion on @p app, pumps @p owner (where the read runs)
/// and then @p app (where it is delivered), and reports the delivery.
template <typename T>
Delivered<T> deliverOn(morph::exec::MainThreadExecutor& owner, morph::exec::MainThreadExecutor& app,
                       morph::async::Completion<T> completion) {
    Delivered<T> seen;
    app.post([&] {
        completion.then([&](const T& value) {
            seen.value = value;
            seen.onReplyExec = morph::exec::runningOn(app);
        });
    });
    owner.drain();
    app.drain();
    return seen;
}

LogEntry entryFor(std::string entityKey, std::string actionType) {
    LogEntry entry;
    entry.modelType = "SO_Model";
    entry.entityKey = std::move(entityKey);
    entry.actionType = std::move(actionType);
    return entry;
}

/// A file path removed when it goes out of scope.
struct ScratchFile {
    std::filesystem::path path;
    explicit ScratchFile(std::string const& name)
        : path{std::filesystem::temp_directory_path() /
               // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the address only makes the name unique.
               ("morph_storage_owner_" + name + "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)))} {
        std::filesystem::remove(path);
    }
    ~ScratchFile() { std::filesystem::remove(path); }
    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;
    ScratchFile(ScratchFile&&) = delete;
    ScratchFile& operator=(ScratchFile&&) = delete;
};

}  // namespace

// ── Action logs ──────────────────────────────────────────────────────────────

TEST_CASE("InMemoryActionLog: an append made on a model's strand runs in a task of the log's owner",
          "[storage][owner]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    std::shared_ptr<morph::journal::InMemoryActionLog> log = buildOnOwner<morph::journal::InMemoryActionLog>(owner);

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "SO_Model";
    binding->modelFactory = [log] {
        auto holder = morph::model::detail::ModelFactory::create<SOStoreModel>();
        holder->attachActionLog(log, "so-1");
        return holder;
    };
    morph::bridge::BridgeHandler<SOStoreModel> handler{bridge, &owner, binding};

    OwnerProbeRecorder const recorder{owner.coreExecutor()};
    std::atomic<int> result{-1};
    handler.execute(SOAdd{.amount = 3}).then([&](int total) { result = total; });
    REQUIRE(morph::testing::pumpOwnerUntil(owner, [&] { return result.load() == 3; }));

    // The model's strand appended, on the pool; the body ran in the owner's task.
    CHECK(recorder.count("InMemoryActionLog::append") == 1U);
    CHECK(recorder.allPosted("InMemoryActionLog::append"));

    // And the entry is there for a reader on the owner.
    std::vector<LogEntry> entries;
    owner.post([&] { entries = log->entries(); });
    owner.drain();
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().actionType == "SO_Add");
}

TEST_CASE("InMemoryActionLog: a read from another executor is answered on the owner and delivered on the reader's",
          "[storage][owner]") {
    morph::exec::MainThreadExecutor owner;
    morph::exec::MainThreadExecutor app;
    morph::exec::ThreadPoolExecutor pool{1};
    auto log = buildOnOwner<morph::journal::InMemoryActionLog>(owner);
    onPool(pool, [&] { log->append(entryFor("a", "SO_Add")); });

    OwnerProbeRecorder const recorder{owner.coreExecutor()};
    auto seen = deliverOn(owner, app, log->entries(app, "a"));
    CHECK(recorder.allPosted("InMemoryActionLog::entries"));
    REQUIRE(seen.value.has_value());
    CHECK(seen.value->size() == 1U);
    CHECK(seen.onReplyExec);
}

TEST_CASE("SessionLog: an append from a pool thread runs in a task of the log's owner", "[storage][owner]") {
    morph::exec::MainThreadExecutor owner;
    morph::exec::ThreadPoolExecutor pool{1};
    auto session = buildOnOwner<morph::journal::SessionLog>(owner);

    OwnerProbeRecorder const recorder{owner.coreExecutor()};
    onPool(pool, [&] { session->append(entryFor("a", "SO_Add")); });
    CHECK(recorder.count("SessionLog::append") == 0U);  // posted: nothing has run yet
    owner.drain();
    CHECK(recorder.count("SessionLog::append") == 1U);
    CHECK(recorder.allPosted("SessionLog::append"));
}

TEST_CASE("SessionLog: a read from another executor is answered on the owner and delivered on the reader's",
          "[storage][owner]") {
    morph::exec::MainThreadExecutor owner;
    morph::exec::MainThreadExecutor app;
    auto session = buildOnOwner<morph::journal::SessionLog>(owner);
    owner.post([&] { session->append(entryFor("a", "SO_Add")); });
    owner.drain();

    OwnerProbeRecorder const recorder{owner.coreExecutor()};
    auto seen = deliverOn(owner, app, session->entries(app));
    CHECK(recorder.allPosted("SessionLog::entries"));
    REQUIRE(seen.value.has_value());
    CHECK(seen.value->size() == 1U);
    CHECK(seen.onReplyExec);
}

TEST_CASE("SessionLog: a checkpoint runs on the owner between appends posted from pool threads, never inside one",
          "[storage][owner]") {
    morph::exec::MainThreadExecutor owner;
    morph::exec::ThreadPoolExecutor pool{4};
    auto session = buildOnOwner<morph::journal::SessionLog>(owner);
    auto durable = buildOnOwner<morph::journal::InMemoryActionLog>(owner);

    OwnerProbeRecorder const recorder{owner.coreExecutor()};
    constexpr int kAppends = 40;
    std::atomic<int> posted{0};
    for (int i = 0; i < kAppends; ++i) {
        pool.post([&, i] {
            session->append(entryFor("e" + std::to_string(i), "SO_Add"));
            ++posted;
        });
        if (i % 10 == 9) {
            owner.post([&] { session->checkpoint(*durable); });
        }
    }
    REQUIRE(morph::testing::waitUntil([&] { return posted.load() == kAppends; }));
    owner.post([&] { session->checkpoint(*durable); });
    owner.drain();

    // Every append and every checkpoint ran in a task of the owner, so a
    // checkpoint's slice and its forwarding saw no append in between.
    CHECK(recorder.count("SessionLog::append") == static_cast<std::size_t>(kAppends));
    CHECK(recorder.allPosted("SessionLog::append"));
    CHECK(recorder.allPosted("SessionLog::checkpoint"));

    // Forwarded once each, in append order.
    std::vector<LogEntry> forwarded;
    owner.post([&] { forwarded = durable->entries(); });
    owner.drain();
    REQUIRE(forwarded.size() == static_cast<std::size_t>(kAppends));
    for (std::size_t i = 1; i < forwarded.size(); ++i) {
        CHECK(forwarded[i - 1].seq < forwarded[i].seq);
    }
}

TEST_CASE("SessionLog: a checkpoint called off its owner is reported", "[storage][owner]") {
    morph::exec::MainThreadExecutor owner;
    morph::exec::ThreadPoolExecutor pool{1};
    auto session = buildOnOwner<morph::journal::SessionLog>(owner);
    auto durable = buildOnOwner<morph::journal::InMemoryActionLog>(owner);

    OwnerProbeRecorder const recorder{owner.coreExecutor()};
    onPool(pool, [&] { session->checkpoint(*durable); });
    REQUIRE(recorder.count("SessionLog::checkpoint") == 1U);
    CHECK_FALSE(recorder.at("SessionLog::checkpoint").front().onOwner);
}

TEST_CASE("FileActionLog: an append from a pool thread runs in a task of the log's owner", "[storage][owner]") {
    ScratchFile const file{"append"};
    morph::exec::MainThreadExecutor owner;
    morph::exec::ThreadPoolExecutor pool{1};
    auto log = buildOnOwner<morph::journal::FileActionLog>(owner, file.path);

    OwnerProbeRecorder const recorder{owner.coreExecutor()};
    onPool(pool, [&] { log->append(entryFor("a", "SO_Add")); });
    owner.drain();
    CHECK(recorder.count("FileActionLog::append") == 1U);
    CHECK(recorder.allPosted("FileActionLog::append"));
}

TEST_CASE("FileActionLog: a read and a flush from another executor are answered on the owner", "[storage][owner]") {
    ScratchFile const file{"read"};
    morph::exec::MainThreadExecutor owner;
    morph::exec::MainThreadExecutor app;
    morph::exec::ThreadPoolExecutor pool{1};
    auto log = buildOnOwner<morph::journal::FileActionLog>(owner, file.path);
    onPool(pool, [&] { log->append(entryFor("a", "SO_Add")); });

    OwnerProbeRecorder const recorder{owner.coreExecutor()};
    auto flushed = deliverOn(owner, app, log->flush(app));
    auto seen = deliverOn(owner, app, log->entries(app, "a"));
    CHECK(recorder.allPosted("FileActionLog::flush"));
    CHECK(recorder.allPosted("FileActionLog::entries"));
    CHECK(flushed.value == true);
    CHECK(flushed.onReplyExec);
    REQUIRE(seen.value.has_value());
    CHECK(seen.value->size() == 1U);
    CHECK(seen.onReplyExec);
}

TEST_CASE("FileActionLog: an append posted from another thread that fails is reported by the next flush",
          "[storage][owner]") {
    ScratchFile const file{"failed_post"};
    morph::exec::MainThreadExecutor owner;
    morph::exec::ThreadPoolExecutor pool{1};
    morph::core::FileIoOps ioOps;
    std::atomic<bool> failWrites{false};
    ioOps.fwrite = [&failWrites](const void* data, std::size_t size, std::FILE* stream) -> std::size_t {
        if (failWrites.load()) {
            return 0;
        }
        return std::fwrite(data, 1, size, stream);
    };
    auto log = buildOnOwner<morph::journal::FileActionLog>(owner, file.path, ioOps);

    failWrites = true;
    onPool(pool, [&] { log->append(entryFor("a", "SO_Add")); });
    owner.drain();  // the posted write fails on the owner, where nobody waits for it
    failWrites = false;

    bool threw = false;
    owner.post([&] {
        try {
            log->flush();
        } catch (const std::runtime_error&) {
            threw = true;
        }
    });
    owner.drain();
    CHECK(threw);

    // Reported once: the next flush has nothing new to report.
    bool threwAgain = false;
    owner.post([&] {
        try {
            log->flush();
        } catch (const std::runtime_error&) {
            threwAgain = true;
        }
    });
    owner.drain();
    CHECK_FALSE(threwAgain);
}

TEST_CASE("Storage: a write posted before the storage object is destroyed still runs on its owner",
          "[storage][owner]") {
    ScratchFile const file{"teardown"};
    morph::exec::MainThreadExecutor owner;
    morph::exec::ThreadPoolExecutor pool{1};
    {
        auto log = buildOnOwner<morph::journal::FileActionLog>(owner, file.path);
        onPool(pool, [&] { log->append(entryFor("a", "SO_Add")); });
        // The log goes here, its append still queued on the owner.
    }
    owner.drain();
    morph::exec::MainThreadExecutor reader;
    morph::journal::FileActionLog const reopened{reader, file.path};
    CHECK(reopened.entries().size() == 1U);
}

// ── Offline queues ───────────────────────────────────────────────────────────

namespace {

/// The posted-write and completion-read checks every queue answers the same way.
template <typename Queue, typename... Args>
void checkQueueOwner(char const* name, Args&&... args) {
    std::string const prefix{name};
    morph::exec::MainThreadExecutor owner;
    morph::exec::MainThreadExecutor app;
    morph::exec::ThreadPoolExecutor pool{1};
    auto queue = buildOnOwner<Queue>(owner, std::forward<Args>(args)...);

    OwnerProbeRecorder const recorder{owner.coreExecutor()};

    // enqueue from the app: answered on the owner, the id delivered on the app.
    auto enqueued = deliverOn(owner, app, queue->enqueue(app, "payload", "key-1"));
    CHECK(recorder.allPosted(prefix + "::enqueue"));
    REQUIRE(enqueued.value.has_value());
    CHECK(enqueued.onReplyExec);
    uint64_t const itemId = *enqueued.value;

    // setAttempts from a pool thread: posted to the owner.
    onPool(pool, [&] { queue->setAttempts(itemId, 2); });
    owner.drain();
    CHECK(recorder.allPosted(prefix + "::setAttempts"));

    // drain and size from the app: answered on the owner.
    auto drained = deliverOn(owner, app, queue->drain(app));
    CHECK(recorder.allPosted(prefix + "::drain"));
    REQUIRE(drained.value.has_value());
    REQUIRE(drained.value->size() == 1U);
    CHECK(drained.value->front().attempts == 2U);
    CHECK(drained.onReplyExec);

    // markDone from a pool thread: posted to the owner.
    onPool(pool, [&] { queue->markDone(itemId); });
    owner.drain();
    CHECK(recorder.allPosted(prefix + "::markDone"));

    auto counted = deliverOn(owner, app, queue->size(app));
    CHECK(recorder.allPosted(prefix + "::size"));
    CHECK(counted.value == std::size_t{0});
    CHECK(counted.onReplyExec);
}

}  // namespace

TEST_CASE("InMemoryOfflineQueue: writes from a pool thread and reads from the app run on the queue's owner",
          "[storage][owner]") {
    checkQueueOwner<morph::offline::InMemoryOfflineQueue>("InMemoryOfflineQueue");
}

TEST_CASE("FileOfflineQueue: writes from a pool thread and reads from the app run on the queue's owner",
          "[storage][owner]") {
    ScratchFile const file{"queue"};
    checkQueueOwner<morph::offline::FileOfflineQueue>("FileOfflineQueue", file.path);
}

TEST_CASE("InMemoryOfflineQueue: a full queue rejects an enqueue asked from the app", "[storage][owner]") {
    morph::exec::MainThreadExecutor owner;
    morph::exec::MainThreadExecutor app;
    auto queue = buildOnOwner<morph::offline::InMemoryOfflineQueue>(owner, std::optional<std::size_t>{1});
    std::optional<uint64_t> first;
    bool refused = false;
    auto one = queue->enqueue(app, "a");
    auto two = queue->enqueue(app, "b");
    app.post([&] {
        one.then([&](uint64_t itemId) { first = itemId; });
        two.onError([&](const std::exception_ptr& error) {
            try {
                std::rethrow_exception(error);
            } catch (const morph::offline::OfflineQueueFullError&) {
                refused = true;
            } catch (...) {  // NOLINT(bugprone-empty-catch) -- any other error leaves `refused` false
            }
        });
    });
    owner.drain();
    app.drain();
    CHECK(first.has_value());
    CHECK(refused);
}

// ── Replay ledger ────────────────────────────────────────────────────────────

TEST_CASE("InMemoryReplayLedger: a record from a pool thread and a lookup from the app run on the ledger's owner",
          "[storage][owner]") {
    morph::exec::MainThreadExecutor owner;
    morph::exec::MainThreadExecutor app;
    morph::exec::ThreadPoolExecutor pool{1};
    auto ledger = buildOnOwner<morph::offline::InMemoryReplayLedger>(owner);

    OwnerProbeRecorder const recorder{owner.coreExecutor()};
    onPool(pool, [&] { ledger->record("scope", "op-1", "payload"); });
    owner.drain();
    CHECK(recorder.allPosted("InMemoryReplayLedger::record"));

    auto seen = deliverOn(owner, app, ledger->lookup(app, "scope", "op-1"));
    CHECK(recorder.allPosted("InMemoryReplayLedger::lookup"));
    REQUIRE(seen.value.has_value());
    CHECK(*seen.value == std::optional<std::string>{"payload"});
    CHECK(seen.onReplyExec);
}
