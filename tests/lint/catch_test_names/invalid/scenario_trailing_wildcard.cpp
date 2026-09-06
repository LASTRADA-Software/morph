// The "Scenario: " prefix defuses a LEADING metacharacter, not a trailing
// one: this registers as "Scenario: ... wildcard*" and still over-selects.
#include <catch2/catch_test_macros.hpp>
SCENARIO("a scenario whose name ends in a wildcard*") {}
