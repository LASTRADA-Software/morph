// SPDX-License-Identifier: Apache-2.0

#include <cassert>
#include <catch2/catch_test_macros.hpp>
#include <morph/core/model.hpp>

// This test compares floats with == intentionally; -Wfloat-equal is a false positive here.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
#endif
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"

struct Foo {
    int x = 0;
};
struct Bar {
    double y = 0.0;
};

TEST_CASE("morph::model::detail::ModelHolder stores the model and returns correct type index", "[model]") {
    auto holder = std::make_unique<morph::model::detail::ModelHolder<Foo>>();
    REQUIRE(holder->type() == std::type_index(typeid(Foo)));
}

TEST_CASE("morph::model::detail::IModelHolder::into returns reference to the contained model", "[model]") {
    auto holder = std::make_unique<morph::model::detail::ModelHolder<Foo>>();
    holder->model.x = 42;
    Foo& ref = holder->into<Foo>();
    REQUIRE(ref.x == 42);
    ref.x = 99;
    REQUIRE(holder->model.x == 99);
}

TEST_CASE("morph::model::detail::ModelFactory::create returns correctly typed holder", "[model]") {
    auto holder = morph::model::detail::ModelFactory::create<Bar>();
    REQUIRE(holder->type() == std::type_index(typeid(Bar)));
    Bar& bar = holder->into<Bar>();
    bar.y = 3.14;
    REQUIRE(holder->into<Bar>().y == bar.y);
}

TEST_CASE("morph::model::detail::ModelHolder constructors with arguments forward correctly", "[model]") {
    struct Constructed {
        int a;
        double b;
        explicit Constructed(int intVal, double dblVal) : a{intVal}, b{dblVal} {}
    };
    auto holder = std::make_unique<morph::model::detail::ModelHolder<Constructed>>(7, 2.5);
    REQUIRE(holder->model.a == 7);
    REQUIRE(holder->model.b == 2.5);
}

TEST_CASE("morph::exec::detail::ModelId equality and hash", "[model]") {
    morph::exec::detail::ModelId idA{5};
    morph::exec::detail::ModelId idB{5};
    morph::exec::detail::ModelId idC{6};
    REQUIRE(idA == idB);
    REQUIRE_FALSE(idA == idC);

    morph::exec::detail::ModelIdHash hasher;
    REQUIRE(hasher(idA) == hasher(idB));
    // Different values should (almost always) hash differently
    REQUIRE(hasher(idA) != hasher(idC));
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#pragma GCC diagnostic pop
