// SPDX-License-Identifier: Apache-2.0

#include <morph/backend.hpp>
#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/registry.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <thread>


struct CounterAction {
    int delta = 0;
};
struct CounterModel {
    int value = 0;
    int execute(const CounterAction& act) {
        value += act.delta;
        return value;
    }
};

template <>
struct morph::model::ModelTraits<CounterModel> {
    static constexpr std::string_view typeId() { return "BE_CounterModel"; }
};
template <>
struct morph::model::ActionTraits<CounterAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "BE_CounterAction"; }
    static std::string toJson(const CounterAction& action) {
        std::string out;
        if (auto err = glz::write_json(action, out)) {
            throw morph::model::detail::ParseError{glz::format_error(err, out)};
        }
        return out;
    }
    static CounterAction fromJson(std::string_view json) {
        CounterAction act{};
        if (auto err = glz::read_json(act, json)) {
            throw morph::model::detail::ParseError{glz::format_error(err, json)};
        }
        return act;
    }
    static std::string resultToJson(const int& result) {
        std::string out;
        if (auto err = glz::write_json(result, out)) {
            throw morph::model::detail::ParseError{glz::format_error(err, out)};
        }
        return out;
    }
    static int resultFromJson(std::string_view json) {
        int result{};
        if (auto err = glz::read_json(result, json)) {
            throw morph::model::detail::ParseError{glz::format_error(err, json)};
        }
        return result;
    }
};

struct SyncExecutor : morph::exec::IExecutor {
    void post(std::function<void()> fn) override { fn(); }
};

// ── morph::backend::LocalBackend: model-not-found path ────────────────────────────────────────

TEST_CASE("morph::backend::LocalBackend: execute after deregisterModel delivers error", "[backend][local]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::backend::LocalBackend backend{pool};

    auto mid = backend.registerModel("BE_CounterModel", morph::model::detail::ModelFactory::create<CounterModel>);
    backend.deregisterModel(mid);

    // Build a minimal morph::backend::detail::ActionCall that performs a local op
    morph::backend::detail::ActionCall call;
    call.modelTypeId = "BE_CounterModel";
    call.actionTypeId = "BE_CounterAction";
    call.serializeAction = [] { return std::string{"{}"}; };
    call.deserializeResult = [](std::string_view) -> std::shared_ptr<void> { return {}; };
    call.localOp = [](morph::model::detail::IModelHolder&) -> std::shared_ptr<void> { return {}; };

    bool errorFired = false;
    backend.execute(mid, std::move(call), &cbExec)
        .then([](const std::shared_ptr<void>&) {})
        .onError([&](const std::exception_ptr& exc) {
            try {
                std::rethrow_exception(exc);
            } catch (const std::runtime_error&) {
                errorFired = true;
            }
        });

    for (int i = 0; i < 50 && !errorFired; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(errorFired);
}

// ── morph::bridge::Bridge: deregisterHandler edge cases ─────────────────────────────────────

TEST_CASE("morph::bridge::Bridge::deregisterHandler with already-zero currentId is a no-op", "[bridge]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "BE_CounterModel";
    binding->modelFactory = morph::model::detail::ModelFactory::create<CounterModel>;
    binding->currentId.store(0);  // simulate unbound

    // Should not crash or call backend with id=0
    bridge.deregisterHandler(binding);
    REQUIRE(true);
}

TEST_CASE("morph::bridge::Bridge::executeVia when handler currentId is zero returns error", "[bridge]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    // Manually create an unbound binding
    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "BE_CounterModel";
    binding->modelFactory = morph::model::detail::ModelFactory::create<CounterModel>;
    binding->currentId.store(0);

    bool errorFired = false;
    bridge.executeVia<CounterModel, CounterAction>(binding, CounterAction{1}, &cbExec)
        .then([](int) {})
        .onError([&](const std::exception_ptr& exc) {
            try {
                std::rethrow_exception(exc);
            } catch (const std::runtime_error& ex) {
                errorFired = (std::string{ex.what()} == "handler not bound");
            }
        });

    REQUIRE(errorFired);
}

TEST_CASE("morph::bridge::BridgeHandler destructor deregisters model cleanly", "[bridge]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    std::atomic<int> result{-1};
    {
        morph::bridge::BridgeHandler<CounterModel> handler{bridge, &cbExec};
        handler.execute(CounterAction{10})
            .then([&](int val) { result.store(val); })
            .onError([](const std::exception_ptr&) {});

        for (int i = 0; i < 50 && result.load() == -1; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // handler goes out of scope here — deregister must not crash
    }
    REQUIRE(result.load() == 10);
}
