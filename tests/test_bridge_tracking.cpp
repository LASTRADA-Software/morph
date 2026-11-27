// SPDX-License-Identifier: Apache-2.0

// How the Bridge tracks its handlers: removing one in any order must leave
// every other live handler visible to whatever walks the list (a backend
// switch), exactly once, and never revive a removed one.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <map>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <random>
#include <string>
#include <utility>
#include <vector>

// NOLINTBEGIN(misc-use-internal-linkage)
struct BtkPing {
    int unused = 0;
};
struct BtkModel {
    static int execute(const BtkPing& /*act*/) { return 1; }
};
BRIDGE_REGISTER_MODEL(BtkModel, "BTK_Model")
BRIDGE_REGISTER_ACTION(BtkModel, BtkPing, "BTK_Ping")
// NOLINTEND(misc-use-internal-linkage)

namespace {

using morph::bridge::Bridge;
using Binding = std::shared_ptr<morph::bridge::detail::HandlerBinding>;

/// Counts the binds it is asked for, by the context key of the binding.
class CountingBackend : public morph::backend::LocalBackend {
public:
    explicit CountingBackend(morph::exec::IExecutor& pool) : LocalBackend{pool} {}

    morph::async::Completion<morph::exec::detail::ModelId> bindModel(morph::backend::detail::BindRequest request,
                                                                     morph::exec::IExecutor& cbExec) override {
        ++binds[request.contextKey];
        return LocalBackend::bindModel(std::move(request), cbExec);
    }

    std::map<std::string, int> binds;
};

struct Rig {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    std::vector<Binding> bindings;
    std::vector<Binding> removed;

    explicit Rig(std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            add();
        }
    }

    void add() {
        auto binding = bridge.registerHandler<BtkModel>();
        binding->contextKey = "h" + std::to_string(nextKey++);
        bindings.push_back(std::move(binding));
    }

    void destroy(std::size_t position) {
        bridge.deregisterHandler(bindings[position]);
        // Kept alive so a removal that left its entry behind would show up as
        // a rebind of a handler that no longer exists.
        removed.push_back(bindings[position]);
        bindings.erase(bindings.begin() + static_cast<std::ptrdiff_t>(position));
    }

    /// Switches to a fresh counting backend and checks each surviving binding
    /// was rebound exactly once, and no removed one was touched.
    void requireSwitchRebindsSurvivors() {
        auto next = std::make_unique<CountingBackend>(pool);
        auto const& binds = next->binds;
        bridge.switchBackend(std::move(next));
        REQUIRE(binds.size() == bindings.size());
        for (auto const& binding : bindings) {
            auto const found = binds.find(binding->contextKey);
            REQUIRE(found != binds.end());
            REQUIRE(found->second == 1);
            REQUIRE(Bridge::isBound(binding));
        }
    }

    int nextKey = 0;
};

}  // namespace

TEST_CASE("Bridge tracking keeps survivors after removing the first, last and middle handler",
          "[bridge][tracking]") {
    Rig rig{7};
    rig.destroy(0);
    rig.destroy(rig.bindings.size() - 1);
    rig.destroy(2);
    rig.requireSwitchRebindsSurvivors();
    REQUIRE(rig.bindings.size() == 4);
}

TEST_CASE("Bridge tracking keeps survivors when removal interleaves with creation", "[bridge][tracking]") {
    Rig rig{4};
    rig.destroy(1);
    rig.add();
    rig.add();
    rig.destroy(0);
    rig.add();
    rig.destroy(3);
    rig.requireSwitchRebindsSurvivors();
}

TEST_CASE("Bridge tracking removes handlers correctly after a backend switch", "[bridge][tracking]") {
    Rig rig{6};
    rig.destroy(1);
    rig.requireSwitchRebindsSurvivors();
    rig.destroy(0);
    rig.destroy(rig.bindings.size() - 1);
    rig.add();
    rig.requireSwitchRebindsSurvivors();
}

TEST_CASE("Bridge tracking survives removing every handler in a random order", "[bridge][tracking]") {
    Rig rig{40};
    std::mt19937 gen{12345};
    while (rig.bindings.size() > 1) {
        rig.destroy(std::uniform_int_distribution<std::size_t>{0, rig.bindings.size() - 1}(gen));
        if (rig.bindings.size() % 7 == 0) {
            rig.requireSwitchRebindsSurvivors();
        }
    }
    rig.requireSwitchRebindsSurvivors();
}

TEST_CASE("Bridge tracking ignores a second removal of the same handler", "[bridge][tracking]") {
    Rig rig{3};
    auto const dropped = rig.bindings[1];
    rig.destroy(1);
    rig.bridge.deregisterHandler(dropped);
    rig.requireSwitchRebindsSurvivors();
    REQUIRE(rig.bindings.size() == 2);
}

TEST_CASE("Bridge tracking drops handlers destroyed without removal", "[bridge][tracking]") {
    Rig rig{3};
    for (int i = 0; i < 500; ++i) {
        auto const lost = rig.bridge.registerHandler<BtkModel>();
        (void)lost;
    }
    rig.destroy(0);
    rig.add();
    rig.destroy(1);
    rig.requireSwitchRebindsSurvivors();
}
