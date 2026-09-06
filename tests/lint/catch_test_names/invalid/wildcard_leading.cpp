// A leading `*` is Catch2's wildcard: the entry selects every test whose name
// ends this way, not just this one.
#include <catch2/catch_test_macros.hpp>
TEST_CASE("*Star leads and over-selects") {}
