// The name of a fixture-based case is its SECOND argument, and it reaches the
// filter argument exactly like any other.
#include <catch2/catch_test_macros.hpp>
struct Fixture {};
TEST_CASE_METHOD(Fixture, "~Fixture tears the connection down exactly once") {}
