// A leading `"` opens Catch2's quoted-name mode; the resulting spec matches
// no test at all ("No tests ran", exit 2).
#include <catch2/catch_test_macros.hpp>
TEST_CASE("\"Quoted whole\" leads and matches nothing") {}
