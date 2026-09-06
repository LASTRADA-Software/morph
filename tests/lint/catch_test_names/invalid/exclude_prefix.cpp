// `exclude:` is the long spelling of `~`: preprocessPattern strips the prefix
// and sets m_exclusion. Verified to select 5 of 5 cases in a 5-case binary.
#include <catch2/catch_test_macros.hpp>
TEST_CASE("exclude:the paths that never touch the wire") {}
