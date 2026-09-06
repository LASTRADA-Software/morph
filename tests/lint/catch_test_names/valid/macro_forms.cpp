// Fixture for scripts/check_catch_test_names.sh (issue #466) -- the ACCEPT
// direction, for the macro forms whose name is not simply the first argument.

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

// SCENARIO expands to TEST_CASE("Scenario: " ...), so the registered name --
// and therefore the filter argument -- begins with "Scenario: ". A leading
// tilde in the SCENARIO text is consequently NOT leading in the registered
// name, and its entry selects exactly one test (verified). Rejecting this
// would be a false positive.
SCENARIO("~Tilde is defused by the Scenario: prefix") {}

// The fixture type is the first argument and the name is the second, so the
// checker has to step over one argument before it reads the name.
struct Fixture {};
TEST_CASE_METHOD(Fixture, "Fixture-based name is read from the second argument") {}

// An anonymous test case: Catch2 generates the name, so no author-chosen
// metacharacter can reach the filter argument.
TEST_CASE() {}

// A name written across lines with adjacent string-literal concatenation --
// 112 real test names are written this way, and the checker has to join them
// before it can look at the first and last character.
TEST_CASE(
    "A name long enough that clang-format wraps it across two string "
    "literals, ending in an ordinary character") {}

// Prose in a comment is not a registration: TEST_CASE("~not a real one")
/// and neither is TEST_CASE_METHOD(SqlTestFixture,
/// ...) in a Doxygen block.
