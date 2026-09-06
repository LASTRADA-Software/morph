// The defect issue #466 is filed for: a leading `~` is Catch2's exclusion
// operator, so this entry runs every OTHER test in the binary and passes.
#include <catch2/catch_test_macros.hpp>
TEST_CASE("~App releases its strand before the executor is torn down") {}
