// Catch2's command line reads a leading `-` as an option, not a positional:
// "Error(s) in input: Unrecognised token: -Dash". The entry cannot run.
#include <catch2/catch_test_macros.hpp>
TEST_CASE("-Dash leads and is read as an option") {}
