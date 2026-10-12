// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/forms/engine/typed_form.hpp>
#include <morph/forms/forms.hpp>
#include <morph/reactive/runtime.hpp>
#include <optional>
#include <string>

#include "test_support.hpp"

// NOLINTBEGIN(misc-use-internal-linkage)
struct FtSum {
    int a = 0;
    int b = 0;
    std::optional<std::string> note{};

    // The action's own rule, which the schema cannot state: the form must not submit past it.
    [[nodiscard]] bool validate() const { return a < 100; }
};
struct FtSumResult {
    int sum = 0;
};
struct FtModel {
    FtSumResult execute(FtSum const& action) { return FtSumResult{.sum = action.a + action.b}; }
};
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(FtModel, "Test_FormsEngineTyped_Model")
BRIDGE_REGISTER_ACTION(FtModel, FtSum, "Test_FormsEngineTyped_Sum")

namespace {

struct TypedStack {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner{};
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::reactive::Runtime rt{owner};
    morph::bridge::BridgeHandler<FtModel> handler{bridge, &owner};
};

}  // namespace

TEST_CASE("Form<A>: the typed model is the runtime model of the same schema", "[forms-engine][typed]") {
    TypedStack stack;
    morph::forms::Form<FtSum, FtModel> form{stack.rt, stack.handler, morph::forms::ChoiceFetcher{}};
    auto const runtime =
        morph::forms::FormModel::fromSchema("Test_FormsEngineTyped_Sum", morph::forms::schemaJson<FtSum>());
    REQUIRE(runtime.has_value());
    auto const& typed = form.session().model();
    CHECK(typed.actionType() == "Test_FormsEngineTyped_Sum");
    REQUIRE(typed.fields().size() == runtime->fields().size());
    for (std::size_t i = 0; i < typed.fields().size(); ++i) {
        CHECK(typed.fields()[i].name == runtime->fields()[i].name);
        CHECK(typed.fields()[i].kind == runtime->fields()[i].kind);
        CHECK(typed.fields()[i].required == runtime->fields()[i].required);
    }
}

TEST_CASE("Form<A>: set<> is a typed, programmatic change; the next edit submits through the handler",
          "[forms-engine][typed]") {
    TypedStack stack;
    morph::forms::Form<FtSum, FtModel> form{stack.rt, stack.handler, morph::forms::ChoiceFetcher{}};
    form.set<&FtSum::a>(3);
    form.set<&FtSum::b>(4);
    REQUIRE(form.ready());
    CHECK(form.session().body() == R"({"a":3,"b":4})");
    REQUIRE(form.value().has_value());
    CHECK(form.value()->a == 3);
    CHECK_FALSE(form.lastResult().has_value());  // set<> never submits

    form.session().field("b").text().set("5");
    REQUIRE(morph::testing::pumpOwnerUntil(stack.owner, [&] { return form.lastResult().has_value(); }));
    CHECK(form.lastResult()->sum == 8);
    CHECK(form.session().lastReply() == R"({"sum":8})");
    CHECK(form.error() == nullptr);
}

TEST_CASE("Form<A>: the action's validate() joins readiness", "[forms-engine][typed]") {
    TypedStack stack;
    morph::forms::Form<FtSum, FtModel> form{stack.rt, stack.handler, morph::forms::ChoiceFetcher{}};
    form.set<&FtSum::a>(150);
    form.set<&FtSum::b>(1);
    CHECK(form.session().field("a").error().get() == std::nullopt);  // every field encodes...
    CHECK_FALSE(form.ready());                                       // ...but the action refuses
    CHECK_FALSE(form.session().body().has_value());
    form.set<&FtSum::a>(99);
    CHECK(form.ready());
    form.set<&FtSum::note>(std::string{"why"});
    CHECK(form.session().body() == R"({"a":99,"b":1,"note":"why"})");
}
