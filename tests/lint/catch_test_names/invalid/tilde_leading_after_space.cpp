// Catch2's spec parser skips leading spaces before reading the first
// operator, so a space in front of the tilde does not defuse it.
#include <catch2/catch_test_macros.hpp>
TEST_CASE("  ~App releases its strand before the executor is torn down") {}
