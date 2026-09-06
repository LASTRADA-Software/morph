// Fixture for scripts/check_catch_test_names.sh (issue #466) -- the ACCEPT
// direction. Nothing here is compiled; the file exists to be read by the
// checker. Every name below was verified against a real Catch2 binary
// (3.15.3) to select exactly one test case when handed back as the positional
// filter argument the way catch_discover_tests hands it back.
//
// A gate that only ever demonstrates rejection is a gate that could reject
// everything, so these are as important as the invalid/ cases.

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Plain ordinary name") {}

// Tags in the name itself. `[` and `]` are escaped by Catch2's own
// CatchAddTests.cmake (`foreach(char \\ , [ ] ;)`) before the argument
// reaches the binary, so they select correctly and must not be rejected --
// they are also how tags are spelled, so a gate that rejected them would fire
// on ordinary names.
TEST_CASE("Bracket [tagish] name", "[lint][fixture]") {}

// Escaped by the same upstream loop: 404 real test names contain a comma.
TEST_CASE("Comma, in the name, twice") {}

// A `*` anywhere but the first or last character is literal to Catch2's
// WildcardPattern.
TEST_CASE("In*ternal star is literal") {}

// A double quote that does not open the name does not open quoted-name mode.
TEST_CASE("Quote \"inside\" the name") {}

// A backslash is escaped upstream, first, deliberately.
TEST_CASE("Back\\slash in the name") {}

// A tilde that is not leading is an ordinary character.
TEST_CASE("Destructor ~App is discussed mid-name") {}

// So is a hyphen that is not leading.
TEST_CASE("Round-trip through the wire is stable") {}

// `exclude:` is only an operator at the start of the spec.
TEST_CASE("Filters that exclude: nothing still select") {}
