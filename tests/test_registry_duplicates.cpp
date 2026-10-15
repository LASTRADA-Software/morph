// SPDX-License-Identifier: Apache-2.0
//
// A model id, and a model's action id, belongs to one type. Registering a second type under it is refused; the same
// type again, as every translation unit including a registration header does, is not a conflict.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <string>

// File-scope, not anonymous-namespaced: glaze's reflection of an action needs a type with linkage.
// NOLINTBEGIN(misc-use-internal-linkage)
struct DupFirstModel {
    int value = 0;
};
struct DupSecondModel {
    int value = 0;
};
struct DupStore {
    int amount = 0;
};
struct DupErase {
    int amount = 0;
};
struct DupHandlingModel {
    int execute(DupStore const& action) { return action.amount; }
    int execute(DupErase const& action) { return -action.amount; }
};
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(DupHandlingModel, "DupHandlingModel")
BRIDGE_REGISTER_ACTION(DupHandlingModel, DupStore, "DupStore")
BRIDGE_REGISTER_ACTION(DupHandlingModel, DupErase, "DupErase")

using morph::model::DuplicateRegistrationError;

// Mutation: drop claimId from ModelRegistryFactory::registerModel -> the second type silently replaces the first.
TEST_CASE("registry: a model id registered for a second type is refused", "[registry][duplicates]") {
    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<DupFirstModel>("Dup_Model");
    // The same type again, from another translation unit, is the ordinary case.
    CHECK_NOTHROW(registry.registerModel<DupFirstModel>("Dup_Model"));
    // So is the same type replacing its factory with a construction hook.
    CHECK_NOTHROW(registry.registerModel<DupFirstModel>(
        "Dup_Model", [] { return std::make_unique<morph::model::detail::ModelHolder<DupFirstModel>>(); }));
    CHECK_THROWS_AS(registry.registerModel<DupSecondModel>("Dup_Model"), DuplicateRegistrationError);
    CHECK_THROWS_AS(
        registry.registerModel<DupSecondModel>(
            "Dup_Model", [] { return std::make_unique<morph::model::detail::ModelHolder<DupSecondModel>>(); }),
        DuplicateRegistrationError);
    // The first registration still answers.
    CHECK(registry.create("Dup_Model")->type() == std::type_index{typeid(DupFirstModel)});
    try {
        registry.registerModel<DupSecondModel>("Dup_Model");
    } catch (DuplicateRegistrationError const& error) {
        CHECK(std::string{error.what()}.contains("the model id `Dup_Model` is registered for two types"));
    }
}

// Mutation: drop claimId from ActionDispatcher::registerAction -> the second action type replaces the first.
TEST_CASE("registry: a model's action id registered for a second action type is refused", "[registry][duplicates]") {
    morph::model::detail::ActionDispatcher dispatcher;
    dispatcher.registerAction<DupHandlingModel, DupStore>("Dup_Model", "Dup_Action");
    CHECK_NOTHROW(dispatcher.registerAction<DupHandlingModel, DupStore>("Dup_Model", "Dup_Action"));
    CHECK_THROWS_AS((dispatcher.registerAction<DupHandlingModel, DupErase>("Dup_Model", "Dup_Action")),
                    DuplicateRegistrationError);
    // One action id under two models is two ids.
    CHECK_NOTHROW(dispatcher.registerAction<DupHandlingModel, DupErase>("Dup_Other", "Dup_Action"));
}

// Mutation: drop claimId from ActionExecuteRegistry::registerAction -> the client path replaces silently.
TEST_CASE("registry: the client's executor registry refuses a second action type too", "[registry][duplicates]") {
    morph::bridge::ActionExecuteRegistry executors;
    executors.registerAction<DupHandlingModel, DupStore>("Dup_Model", "Dup_Action");
    CHECK_NOTHROW(executors.registerAction<DupHandlingModel, DupStore>("Dup_Model", "Dup_Action"));
    CHECK_THROWS_AS((executors.registerAction<DupHandlingModel, DupErase>("Dup_Model", "Dup_Action")),
                    DuplicateRegistrationError);
}
