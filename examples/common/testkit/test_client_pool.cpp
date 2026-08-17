// SPDX-License-Identifier: Apache-2.0
#include "testkit/client_pool.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "testkit/backend_rig.hpp"
#include "testkit/pump.hpp"

#include <morph/core/bridge.hpp>

#include <cstddef>
#include <stdexcept>

// Deliberately at namespace scope, not inside an anonymous namespace: glz's
// reflection (which BRIDGE_REGISTER_MODEL/BRIDGE_REGISTER_ACTION rely on to
// serialize these types across the wire, exercised by Mode::Socket) needs
// external linkage on the type — see glaze/reflection/get_name.hpp's
// `extern const T external` — so an anonymous-namespace type fails to link.
// Mirrors test_backend_rig.cpp's RigCounterModel: a stateful accumulator, so
// a test can tell genuine per-client instance isolation apart from every
// client accidentally sharing one instance.
struct PoolAddAction {
    int by = 0;
};
struct PoolCounterModel {
    int value = 0;
    int execute(PoolAddAction action) {
        value += action.by;
        return value;
    }
};

BRIDGE_REGISTER_MODEL(PoolCounterModel, "PoolCounterModel")
BRIDGE_REGISTER_ACTION(PoolCounterModel, PoolAddAction, "PoolAddAction")

namespace {

/// @brief Minimal stand-in for a rung's real Presenter: takes the same
///        `(Bridge&, IExecutor*)` pair every rung's presenter constructor
///        takes (`examples/TESTING.md`'s presenter-architecture rule 2),
///        and drives one `BridgeHandler<PoolCounterModel>` built over that
///        pair. Exercises `ClientPool<Presenter>` without depending on any
///        rung's concrete presenter type, which the shared testkit must not
///        do (rungs depend on the testkit, not the reverse).
class FakePresenter {
  public:
    FakePresenter(morph::bridge::Bridge& bridge, morph::exec::IExecutor* executor) : _handler{bridge, executor} {}

    [[nodiscard]] int add(int by) { return morph::ladder::testkit::awaitQt(_handler.execute(PoolAddAction{by})); }

  private:
    morph::bridge::BridgeHandler<PoolCounterModel> _handler;
};

}  // namespace

TEST_CASE("ClientPool constructs one presenter per client, each over its own bridge", "[testkit][client_pool]") {
    auto mode = GENERATE(morph::ladder::testkit::Mode::Local, morph::ladder::testkit::Mode::LocalSingleThread,
                          morph::ladder::testkit::Mode::Socket);

    constexpr std::size_t kClients = 3;
    morph::ladder::testkit::BackendRig rig{mode, kClients};
    morph::ladder::testkit::ClientPool<FakePresenter> pool{rig, kClients};

    REQUIRE(pool.size() == kClients);

    // Every presenter builds its own BridgeHandler<PoolCounterModel>, and a
    // BridgeHandler construction registers a fresh model instance
    // server-side (BackendRig::client<Model>()'s own doc comment: "the
    // handler itself is still per-call, constructed fresh here") — true in
    // every mode, even Local/LocalSingleThread where all three presenters
    // share one underlying Bridge. So each presenter's running total stays
    // independent of the other two, in every mode.
    REQUIRE(pool.at(0).add(10) == 10);
    REQUIRE(pool.at(1).add(1) == 1);
    REQUIRE(pool.at(2).add(100) == 100);
}

TEST_CASE("ClientPool::at throws out_of_range past its constructed size", "[testkit][client_pool]") {
    morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Local, /*nClients=*/1};
    morph::ladder::testkit::ClientPool<FakePresenter> pool{rig, /*nClients=*/1};

    REQUIRE(pool.size() == 1);
    REQUIRE_NOTHROW(pool.at(0));
    REQUIRE_THROWS_AS(pool.at(1), std::out_of_range);
}
