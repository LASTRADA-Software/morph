// SPDX-License-Identifier: Apache-2.0

#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <stdexcept>
#include <string>


struct RxAction {
    int val = 0;
};
struct RxModel {
    int execute(const RxAction& act) { return act.val + 1; }
};

template <>
struct morph::model::ModelTraits<RxModel> {
    static constexpr std::string_view typeId() { return "REG_RxModel"; }
};
template <>
struct morph::model::ActionTraits<RxAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "REG_RxAction"; }
    static std::string toJson(const RxAction& act) {
        std::string out;
        if (auto errCode = glz::write_json(act, out)) {
            throw morph::model::detail::ParseError{glz::format_error(errCode, out)};
        }
        return out;
    }
    static RxAction fromJson(std::string_view json) {
        RxAction action{};
        if (auto errCode = glz::read_json(action, json)) {
            throw morph::model::detail::ParseError{glz::format_error(errCode, json)};
        }
        return action;
    }
    static std::string resultToJson(const int& res) {
        std::string out;
        if (auto errCode = glz::write_json(res, out)) {
            throw morph::model::detail::ParseError{glz::format_error(errCode, out)};
        }
        return out;
    }
    static int resultFromJson(std::string_view json) {
        int result{};
        if (auto errCode = glz::read_json(result, json)) {
            throw morph::model::detail::ParseError{glz::format_error(errCode, json)};
        }
        return result;
    }
};

// ── morph::model::detail::ParseError ────────────────────────────────────────────────────────────────

TEST_CASE("morph::model::detail::ParseError is a std::runtime_error with message", "[registry]") {
    morph::model::detail::ParseError errCode{"bad json"};
    REQUIRE(std::string{errCode.what()} == "bad json");
    REQUIRE(dynamic_cast<const std::runtime_error*>(&errCode) != nullptr);
}

// ── morph::model::ActionTraits: JSON round-trip via real glaze ───────────────────────────────

TEST_CASE("morph::model::ActionTraits: toJson/fromJson round-trips correctly", "[registry]") {
    RxAction action{42};
    auto json = morph::model::ActionTraits<RxAction>::toJson(action);
    auto action2 = morph::model::ActionTraits<RxAction>::fromJson(json);
    REQUIRE(action2.val == 42);
}

TEST_CASE("morph::model::ActionTraits: resultToJson/resultFromJson round-trips correctly", "[registry]") {
    int res = 99;
    auto json = morph::model::ActionTraits<RxAction>::resultToJson(res);
    auto result2 = morph::model::ActionTraits<RxAction>::resultFromJson(json);
    REQUIRE(result2 == 99);
}

TEST_CASE("morph::model::ActionTraits: fromJson with bad input throws morph::model::detail::ParseError", "[registry]") {
    // Non-numeric JSON for an int field
    REQUIRE_THROWS_AS(morph::model::ActionTraits<RxAction>::fromJson("not-json"), morph::model::detail::ParseError);
}

TEST_CASE("morph::model::ActionTraits: resultFromJson with bad input throws morph::model::detail::ParseError", "[registry]") {
    REQUIRE_THROWS_AS(morph::model::ActionTraits<RxAction>::resultFromJson("not-a-number"), morph::model::detail::ParseError);
}

// ── morph::model::detail::ModelRegistryFactory: insert_or_assign overwrites ─────────────────────────

TEST_CASE("morph::model::detail::ModelRegistryFactory: re-registering same typeId overwrites factory", "[registry]") {
    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<RxModel>("REG_RxModel");
    // Second registration should not throw and should still produce a valid holder
    registry.registerModel<RxModel>("REG_RxModel");
    auto holder = registry.create("REG_RxModel");
    REQUIRE(holder != nullptr);
    REQUIRE(holder->type() == std::type_index(typeid(RxModel)));
}

// ── morph::model::detail::ModelRegistryFactory: custom factory DI seam ──────────────────────────────

namespace {
struct DiModel {
    int seed = 0;
    int execute(const RxAction& act) const { return act.val + seed; }
};
}  // namespace

template <>
struct morph::model::ModelTraits<DiModel> {
    static constexpr std::string_view typeId() { return "REG_DiModel"; }
};

TEST_CASE("morph::model::detail::ModelRegistryFactory: registerModel accepts a custom factory closure",
          "[registry]") {
    morph::model::detail::ModelRegistryFactory registry;
    // The DI seam: a factory closure capturing a per-instance dependency
    // (here, a plain int standing in for an injected clock/log/feature flag),
    // equivalent to Bridge::HandlerBinding::modelFactory for Local-mode. The
    // closure returns an owning holder pointer -- the caller controls
    // construction end-to-end, including which constructor overload of
    // ModelHolder<Model> runs.
    int const injectedSeed = 100;
    registry.registerModel<DiModel>("REG_DiModel", [injectedSeed] {
        return std::make_unique<morph::model::detail::ModelHolder<DiModel>>(injectedSeed);
    });

    auto holder = registry.create("REG_DiModel");
    REQUIRE(holder != nullptr);
    REQUIRE(holder->type() == std::type_index(typeid(DiModel)));
    auto& model = holder->into<DiModel>();
    REQUIRE(model.seed == 100);
    REQUIRE(model.execute(RxAction{5}) == 105);
}

TEST_CASE("morph::model::detail::ModelRegistryFactory: custom factory produces independent instances per create()",
          "[registry]") {
    morph::model::detail::ModelRegistryFactory registry;
    int counter = 0;
    registry.registerModel<DiModel>("REG_DiModel", [&counter] {
        return std::make_unique<morph::model::detail::ModelHolder<DiModel>>(DiModel{++counter});
    });

    auto holder1 = registry.create("REG_DiModel");
    auto holder2 = registry.create("REG_DiModel");
    REQUIRE(holder1->into<DiModel>().seed == 1);
    REQUIRE(holder2->into<DiModel>().seed == 2);
}

TEST_CASE(
    "morph::model::detail::ModelRegistryFactory: custom factory overload does NOT auto-attach the default action log",
    "[registry]") {
    // Unlike the single-argument (default-construction) overload -- which
    // goes through ModelFactory::create and auto-attaches
    // morph::journal::defaultActionLog() -- the custom-factory overload
    // leaves attaching a log entirely up to the factory closure (or a later
    // explicit attachActionLog call). ScopedActionLog bounds the process-wide
    // default log to this test's scope so it never leaks into others.
    morph::journal::ScopedActionLog const logGuard{std::make_shared<morph::journal::InMemoryActionLog>()};

    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<DiModel>("REG_DiModel_NoAutoLog",
                                    [] { return std::make_unique<morph::model::detail::ModelHolder<DiModel>>(); });

    auto holder = registry.create("REG_DiModel_NoAutoLog");
    REQUIRE_FALSE(holder->hasActionLog());
}

TEST_CASE("morph::model::detail::ModelRegistryFactory: default-construction overload DOES auto-attach the "
          "default action log",
          "[registry]") {
    // The contrasting case: the plain registerModel<Model>(modelId) overload
    // is unchanged and still goes through ModelFactory::create, which
    // auto-attaches the process-wide default log when one is installed.
    morph::journal::ScopedActionLog const logGuard{std::make_shared<morph::journal::InMemoryActionLog>()};

    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<DiModel>("REG_DiModel_AutoLog");

    auto holder = registry.create("REG_DiModel_AutoLog");
    REQUIRE(holder->hasActionLog());
}

// ── morph::model::detail::ActionDispatcher: KeyHash collision avoidance (basic) ─────────────────────

TEST_CASE("morph::model::detail::ActionDispatcher: different (model,action) pairs are independent", "[registry]") {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<RxModel>("REG_RxModel");
    dispatcher.registerAction<RxModel, RxAction>("REG_RxModel", "REG_RxAction");

    auto holder = registry.create("REG_RxModel");

    // Known action returns value+1
    auto result = dispatcher.dispatch("REG_RxModel", "REG_RxAction", *holder, R"({"val":9})");
    REQUIRE(result == "10");

    // A different action name on same model should throw
    REQUIRE_THROWS_AS(dispatcher.dispatch("REG_RxModel", "REG_Unknown", *holder, "{}"), std::runtime_error);
}
