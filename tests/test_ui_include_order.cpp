// SPDX-License-Identifier: Apache-2.0

// The only morph header here is `interpreter.hpp`: it reaches `view.hpp`'s `morph::ui::Key` before
// `value.hpp`'s `TypeKind::Key`, the order in which GCC's -Wshadow reports the enumerator. This
// translation unit is that order, built with morph's warning set.
#include <catch2/catch_test_macros.hpp>
#include <morph/ui/interpreter.hpp>

TEST_CASE("ui headers: interpreter.hpp as the first morph header compiles under the project's warnings", "[ui]") {
    CHECK(morph::ui::Type::of(morph::ui::TypeKind::Key).admits(morph::ui::Value{"k"}));
}
