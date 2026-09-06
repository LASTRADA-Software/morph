// A name this checker cannot read must not be assumed compliant: the macro
// could expand to anything, including a leading tilde.
#include <catch2/catch_test_macros.hpp>
#define GUARD_NAME "~App releases its strand"
TEST_CASE(GUARD_NAME) {}
