// SPDX-License-Identifier: Apache-2.0

// Soak test: cycles morph::bridge::Bridge::switchBackend() between a LocalBackend
// and a SimulatedRemoteBackend under continuous execute load, for many cycles,
// and checks that resource usage stays flat -- a leaked model instance, a missed
// cancelPending, or a stuck strand would show up as monotonic growth instead.
// See docs/spec/testing_strategy.md and docs/spec/core/backend.md.
//
// Opt-in: built only under -DMORPH_BUILD_LOAD_TESTS=ON (see tests/soak/CMakeLists.txt),
// never part of the default `morph_tests` target or the fast ctest sweep.
//
// Scale: default cycle counts are small enough to run in a few seconds so this
// still works as a CI smoke check. For an actual multi-hour soak run, override:
//   MORPH_SOAK_CYCLES=200000 MORPH_SOAK_EXECUTES_PER_CYCLE=50 ./morph_soak "[soak][switch-backend]"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "test_support.hpp"

namespace {
// Internal linkage: this counter is only ever touched from this translation
// unit (SoakModel's ctor/dtor below and the TEST_CASE at the bottom), so it
// stays out of -Wmissing-variable-declarations' external-linkage-global check
// (same convention as tests/test_limit_policy.cpp's gLPSlowStarted).
std::atomic<int> gSoakLiveInstances{0};
}  // namespace

struct SoakAction {
    int x = 0;
};

struct SoakModel {
    SoakModel() { gSoakLiveInstances.fetch_add(1, std::memory_order_relaxed); }
    ~SoakModel() { gSoakLiveInstances.fetch_sub(1, std::memory_order_relaxed); }
    SoakModel(const SoakModel&) = delete;
    SoakModel& operator=(const SoakModel&) = delete;
    SoakModel(SoakModel&&) = delete;
    SoakModel& operator=(SoakModel&&) = delete;

    int value = 0;
    int execute(const SoakAction& act) {
        value += act.x;
        return value;
    }
};

BRIDGE_REGISTER_MODEL(SoakModel, "Soak_SwitchModel")
BRIDGE_REGISTER_ACTION(SoakModel, SoakAction, "Soak_SwitchAction")

namespace {

// Reads an environment variable as a positive int, falling back to `def` if
// unset or unparsable. Used to scale this smoke-sized default run up to a
// real multi-hour soak run without touching code.
int envIntOr(const char* name, int def) {
    const char* raw = std::getenv(name);
    if (!raw || *raw == '\0') {
        return def;
    }
    try {
        int parsed = std::stoi(raw);
        return parsed > 0 ? parsed : def;
    } catch (const std::exception&) {
        return def;
    }
}

// Resident set size in KiB, read from /proc/self/status. Returns nullopt on
// any platform where that file doesn't exist (the RSS check is then skipped
// rather than failed -- the completion-accounting and instance-count checks
// below do not depend on it).
std::optional<long> readRssKb() {
#if defined(__linux__)
    std::ifstream in{"/proc/self/status"};
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream iss{line.substr(6)};
            long kb = 0;
            iss >> kb;
            return kb;
        }
    }
    return std::nullopt;
#else
    return std::nullopt;
#endif
}

}  // namespace

TEST_CASE("soak: switchBackend churn between LocalBackend and SimulatedRemoteBackend", "[soak][switch-backend]") {
    const int cycles = envIntOr("MORPH_SOAK_CYCLES", 200);
    const int executesPerCycle = envIntOr("MORPH_SOAK_EXECUTES_PER_CYCLE", 20);
    const int rssSampleEvery = envIntOr("MORPH_SOAK_RSS_SAMPLE_EVERY", 20);
    const long rssGrowthKbMax = envIntOr("MORPH_SOAK_RSS_GROWTH_KB_MAX", 100 * 1024);

    morph::exec::ThreadPoolExecutor poolLocal{2};
    morph::exec::ThreadPoolExecutor poolRemote{2};
    morph::testing::InlineExecutor cbExec;

    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(poolLocal)};
    morph::bridge::BridgeHandler<SoakModel> handler{bridge, &cbExec};

    std::atomic<uint64_t> issued{0};
    std::atomic<uint64_t> resolved{0};
    std::vector<long> rssSamplesKb;
    std::shared_ptr<morph::backend::RemoteServer> currentServer;  // kept alive only while "remote" is active

    for (int cycle = 0; cycle < cycles; ++cycle) {
        for (int i = 0; i < executesPerCycle; ++i) {
            issued.fetch_add(1, std::memory_order_relaxed);
            handler.execute(SoakAction{1})
                .then([&](int) { resolved.fetch_add(1, std::memory_order_relaxed); })
                .onError([&](const std::exception_ptr&) { resolved.fetch_add(1, std::memory_order_relaxed); });
        }

        if (cycle % 2 == 0) {
            currentServer.reset();  // drop the previous remote server, if any
            bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(poolLocal));
        } else {
            // A fresh RemoteServer per remote cycle: this deliberately avoids
            // accumulating orphaned registrations on one long-lived server,
            // which is the ALREADY-documented "no connection-scoped cleanup"
            // limitation (see backend.md Limitations) and not what this soak
            // test is checking. Each server here is fully destroyed (taking
            // its one registered model with it) once `currentServer` is reset
            // or reassigned and its in-flight strand tasks finish.
            currentServer = std::make_shared<morph::backend::RemoteServer>(poolRemote);
            bridge.switchBackend(std::make_unique<morph::backend::SimulatedRemoteBackend>(*currentServer));
        }

        if (cycle % rssSampleEvery == 0) {
            if (auto kb = readRssKb()) {
                rssSamplesKb.push_back(*kb);
            }
        }
    }
    // The loop's last iteration may have left the bridge's active backend as a
    // SimulatedRemoteBackend referencing *currentServer (a plain reference, not
    // shared ownership -- see backend.md's Lifetime & ownership). Switching to
    // one final LocalBackend guarantees the active backend never dangles once
    // currentServer is dropped below, regardless of which branch ran last.
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(poolLocal));
    currentServer.reset();

    REQUIRE(
        morph::testing::waitUntil([&] { return resolved.load() == issued.load(); }, std::chrono::milliseconds(20000)));
    REQUIRE(resolved.load() == issued.load());

    // No more than a couple of model instances should ever be alive at once
    // (the outgoing and incoming backend's models briefly overlap during a
    // switch); once every completion has resolved every backend has settled.
    REQUIRE(morph::testing::waitUntil([&] { return gSoakLiveInstances.load() <= 2; }));
    CHECK(gSoakLiveInstances.load() <= 2);

    if (rssSamplesKb.size() >= 4) {
        const std::size_t quarter = rssSamplesKb.size() / 4;
        long firstQuarterSum = 0;
        for (std::size_t i = 0; i < quarter; ++i) {
            firstQuarterSum += rssSamplesKb[i];
        }
        long lastQuarterSum = 0;
        for (std::size_t i = rssSamplesKb.size() - quarter; i < rssSamplesKb.size(); ++i) {
            lastQuarterSum += rssSamplesKb[i];
        }
        const long firstQuarterAvg = firstQuarterSum / static_cast<long>(quarter);
        const long lastQuarterAvg = lastQuarterSum / static_cast<long>(quarter);
        INFO("RSS first-quarter avg (KiB): " << firstQuarterAvg << ", last-quarter avg (KiB): " << lastQuarterAvg);
        CHECK(lastQuarterAvg - firstQuarterAvg < rssGrowthKbMax);
    }
}
