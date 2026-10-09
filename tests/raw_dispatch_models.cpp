// SPDX-License-Identifier: Apache-2.0
//
// The models the raw-dispatch tests drive. This is the only translation unit
// that names them: test_bridge_raw.cpp reaches them by type id alone, which is
// what a client compiled without them does.

#include <chrono>
#include <core/async/Cancellation.hpp>
#include <core/async/Task.hpp>
#include <cstdint>
#include <morph/core/bridge.hpp>
#include <morph/core/coroutine.hpp>
#include <morph/core/registry.hpp>
#include <morph/session/session.hpp>
#include <stdexcept>
#include <string>

#include "raw_dispatch_probe.hpp"

// NOLINTBEGIN(misc-use-internal-linkage)
struct RawAdd {
    std::int64_t by = 0;
};
struct RawRename {
    std::string name;
};
struct RawFail {};
struct RawLoad {
    std::int64_t id = 0;
};
struct RawRequestId {};
struct RawSleep {
    int ms = 0;
};
// Builds a raw handler on Raw_Counter from inside the action: shared and
// attached to `key` when it is not empty, private otherwise.
struct RawSpawn {
    std::string key;
};

struct RawCounter {
    using PrimaryKey = std::int64_t;

    std::int64_t total = 0;

    // NOLINTBEGIN(readability-convert-member-functions-to-static)
    std::int64_t execute(const RawAdd& action) {
        total += action.by;
        return total;
    }
    std::string execute(const RawRename& action) { return "renamed to " + action.name; }
    std::int64_t execute(const RawFail& /*action*/) { throw std::runtime_error{"counter refused"}; }
    std::int64_t execute(const RawLoad& action) {
        total = action.id;
        return total;
    }
    std::string execute(const RawRequestId& /*action*/) {
        auto const* session = morph::session::current();
        return session != nullptr ? session->requestId : std::string{"<none>"};
    }
    core::async::Task<int> execute(RawSleep action) {
        rawprobe::sleeper().started.fetch_add(1);
        try {
            co_await morph::async::delay(*rawprobe::sleeper().scheduler, std::chrono::milliseconds{action.ms});
        } catch (const core::async::OperationCancelled&) {
            rawprobe::sleeper().cancelled.fetch_add(1);
            throw;
        }
        rawprobe::sleeper().finished.fetch_add(1);
        co_return action.ms;
    }
    [[nodiscard]] std::int64_t execute(const RawSpawn& action) const {
        auto& slot = rawprobe::spawnSlot();
        if (slot.bridge == nullptr) {
            throw std::logic_error{"Raw_Spawn executed before the test set spawnSlot().bridge"};
        }
        auto const sharing =
            action.key.empty() ? morph::bridge::BindSharing::Private : morph::bridge::BindSharing::Shared;
        slot.handler =
            std::make_unique<morph::bridge::RawHandler>(*slot.bridge, slot.gui, "Raw_Counter", sharing, action.key);
        return total;
    }
    // NOLINTEND(readability-convert-member-functions-to-static)
};

BRIDGE_REGISTER_MODEL(RawCounter, "Raw_Counter")
BRIDGE_REGISTER_ACTION(RawCounter, RawAdd, "Raw_Add")
BRIDGE_REGISTER_ACTION(RawCounter, RawRename, "Raw_Rename")
BRIDGE_REGISTER_ACTION(RawCounter, RawFail, "Raw_Fail")
BRIDGE_REGISTER_ACTION(RawCounter, RawLoad, "Raw_Load")
BRIDGE_REGISTER_ACTION(RawCounter, RawRequestId, "Raw_RequestId")
BRIDGE_REGISTER_ACTION(RawCounter, RawSleep, "Raw_Sleep")
BRIDGE_REGISTER_ACTION(RawCounter, RawSpawn, "Raw_Spawn")
BRIDGE_MODEL_KEY(RawCounter, RawLoad, &RawLoad::id);

namespace rawprobe {
Sleeper& sleeper() {
    static Sleeper instance;
    return instance;
}
SpawnSlot& spawnSlot() {
    static SpawnSlot slot;
    return slot;
}

class TypedRenameWatch {
public:
    TypedRenameWatch(morph::bridge::Bridge& bridge, morph::exec::IExecutor& owner, const std::string& key)
        : handler{bridge, &owner} {
        handler.attach(std::stoll(key));
        handler.subscribe<std::string>([this](const std::string& /*renamed*/) { ++seen; });
    }
    morph::bridge::BridgeHandler<RawCounter, morph::bridge::AllowShared> handler;
    int seen = 0;
};

std::shared_ptr<TypedRenameWatch> watchRenames(morph::bridge::Bridge& bridge, morph::exec::IExecutor& owner,
                                               const std::string& key) {
    return std::make_shared<TypedRenameWatch>(bridge, owner, key);
}

int renamesSeen(const TypedRenameWatch& watch) { return watch.seen; }

void renameTyped(TypedRenameWatch& watch) { static_cast<void>(watch.handler.execute(RawRename{.name = "typed"})); }
}  // namespace rawprobe
// NOLINTEND(misc-use-internal-linkage)
