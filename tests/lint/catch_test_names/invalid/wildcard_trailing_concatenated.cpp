// The trailing character of a name written as adjacent string literals is the
// last character of the LAST literal, which a line-scoped check cannot see.
#include <catch2/catch_test_macros.hpp>
TEST_CASE(
    "A name long enough that clang-format wraps it across two string "
    "literals, and whose last literal ends in a wildcard*") {}
