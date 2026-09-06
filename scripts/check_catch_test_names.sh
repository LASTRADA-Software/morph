#!/usr/bin/env bash
# Usage: bash scripts/check_catch_test_names.sh [DIR...]
#
# Rejects a Catch2 test-case name that, used as a filter argument, does not
# select the one test it names -- see issue #466.
#
# catch_discover_tests registers one ctest entry per test case and passes the
# test's own name back to the binary as the positional filter argument:
#
#   add_test( [==[Some test name]==] .../some_tests [==[Some test name]==] )
#
# Catch2 parses that argument as a *test spec*, not as a literal name, so a
# name carrying a metacharacter at a significant position makes the entry
# select something other than the test it is named for. The worst of these is
# silent: a leading `~` is Catch2's exclusion operator, so the entry runs every
# *other* test in the binary, never runs its own, and exits 0 reporting "All
# tests passed" (verified on ladder_pastebin_tests: 56 of 57 cases ran, exit
# 0). That is a ctest entry that is green while measuring nothing -- the
# failure mode docs/spec/testing_charter.md exists to prevent -- and it is
# invisible at the call site, because the test, the entry and the run all look
# correct. It has cost real work once already: morph#391 named its guard tests
# `~App ...` after the destructor under test, and the resulting entries ran 16
# unrelated cases and reported green.
#
# ---- What is rejected, and why each one -----------------------------------
#
# Every rule below was checked against a real Catch2 binary rather than
# assumed. Names are checked in their *registered* form: SCENARIO prepends
# "Scenario: " (Catch2 defines it as TEST_CASE("Scenario: " __VA_ARGS__)), and
# Catch2 trims leading and trailing whitespace off both the spec and the test
# name before matching, so this check trims too.
#
#   leading `~`          exclusion operator. The entry runs the complement of
#                        its own name and passes. SILENT.
#   leading `exclude:`   the long spelling of the same operator (Catch2's
#                        TestSpecParser::preprocessPattern strips the prefix
#                        and sets m_exclusion). Verified: 5 of 5 cases matched,
#                        i.e. the entry runs the whole binary. SILENT.
#   leading/trailing `*` Catch2's wildcard. WildcardPattern treats `*` as a
#                        wildcard only at the start and/or the end of a
#                        pattern, so the entry selects a *superset*. Verified:
#                        with cases named `*Star leads` and `Prefixed *Star
#                        leads`, the first entry matched both, ran 2 tests and
#                        passed. SILENT.
#   leading `-`          Catch2's command line reads the argument as an option
#                        rather than a positional and refuses to start
#                        ("Error(s) in input: Unrecognised token: -Dash"). Loud
#                        (the ctest entry is permanently red), but the test can
#                        never be run through ctest.
#   leading `"`          opens Catch2's quoted-name mode; the resulting spec
#                        matches nothing ("0 matching test cases" / "No tests
#                        ran", exit 2). Loud, same as above.
#
# ---- What is deliberately NOT rejected ------------------------------------
#
#   `[` and `]`          how Catch2 tags are spelled, and they appear in
#                        ordinary prose names. Catch2's own
#                        CatchAddTests.cmake already escapes them in the filter
#                        argument -- `foreach(char \\ , [ ] ;)` -- in both the
#                        version this repository pins via FetchContent (v3.8.1)
#                        and the newest system one checked here (3.15.3).
#                        Verified: `Bracket [tagish] name` registers as
#                        `Bracket \[tagish\] name` and selects exactly 1 test.
#   `,`                  escaped by the same upstream loop; 404 registered test
#                        names contain one and all of them select correctly.
#   `\`                  escaped by the same loop, first, deliberately.
#   `*` in the middle    literal to WildcardPattern. Verified: `In*ternal star`
#                        selects exactly 1 test.
#   `"` not leading      verified: `Quote "inside" name` selects exactly 1.
#   `;`                  escaped upstream for the filter argument. It has a
#                        separate problem -- CMake flattens the discovered test
#                        *list* (morph#173) -- but that is a different mechanism
#                        with its own configure-time guard in
#                        cmake/morph_add_rung.cmake, not a filter defect.
#
# An over-broad gate that failed legitimate names would be reverted, so the
# rule enforced here is exactly one property: the ctest entry Catch2 generates
# must select the single test case it is named for.
#
# ---- Why this gate rather than escaping the filter argument ---------------
#
# Escaping would be the tooling fix, and the spelling works: `\~Name` does
# neutralise the negation (verified on 3.15.3 -- the escaped spec selects
# exactly 1 test), and v3.8.1's TestSpecParser has the same EscapedName mode
# and the same positional escape-stripping in preprocessPattern, so the
# spelling means the same thing on both.
#
# What is not version-safe is where the escaping would have to live. The
# escape table is upstream's, in Catch2's own CatchAddTests.cmake, and morph
# resolves that file from whichever Catch2 CMakeLists.txt finds -- a system
# install (3.15.3 here) or the FetchContent pin (v3.8.1). Catch.cmake exposes
# the path as `_CATCH_DISCOVER_TESTS_SCRIPT`, so overriding it with a vendored
# copy is possible, but the discovery script is version-coupled to the
# library it drives: v3.8.1's captures the JSON listing from stdout, 3.15.3's
# writes it to a temp file and reassembles the array through control-byte
# placeholders, and both hard-fail on a listing version they do not expect.
# Vendoring one of them makes morph responsible for tracking upstream's
# discovery script forever, and gets the "works on one Catch2 version and
# silently changes meaning on another" outcome that issue #466 explicitly
# warns is worse than the bug. The tooling fix belongs upstream (adding `~`
# to that escape loop); this gate makes the trap unrepresentable regardless of
# which Catch2 is resolved, and needs no Catch2 internals to stay true.
#
# Scans each given DIR recursively for *.cpp and *.hpp files (default: tests
# examples src, which excludes tests/lint -- see below). Exits 0 if every
# discovered test-case name is filter-safe. Exits 1 and prints one
# "file:line: ..." diagnostic per offending name on stderr otherwise.
#
# tests/lint/ is never scanned by the default target: it holds this checker's
# own self-test fixtures (tests/lint/catch_test_names/), whose invalid/ cases
# deliberately carry the rejected name shapes. Passing tests/lint (or a path
# under it) explicitly -- as scripts/test_check_catch_test_names.sh does --
# still scans it: only the *default* argument excludes it.
#
# Requires perl (whole-file scanning: a test-case name is regularly written
# across several lines, and 112 of them use adjacent string-literal
# concatenation, neither of which a line-scoped grep can read -- the same tool
# and technique check_deprecated_markers.sh uses).
set -euo pipefail

if [ "$#" -eq 0 ]; then
    dirs=("tests" "examples" "src")
    prune=(-path "tests/lint" -prune -o)
else
    dirs=("$@")
    prune=()
fi

status=0
find "${dirs[@]}" "${prune[@]}" \( -name "*.cpp" -o -name "*.hpp" \) -print0 | perl -0 -ne '
    use strict;
    use warnings;

    our $status;
    our $checked;   # names actually parsed -- reported, so a gate that has
    our $files;     # stopped matching anything cannot look like a clean pass

    # The Catch2 test-registration macros. Every one of them registers a test
    # case whose name becomes a ctest entry, and thus a filter argument. The
    # *_METHOD and METHOD_AS_TEST_CASE forms take a fixture type (or a method)
    # as their FIRST argument and the name second.
    my $MACRO_RE = join "|", qw(
        TEST_CASE
        TEST_CASE_METHOD
        SCENARIO
        SCENARIO_METHOD
        TEMPLATE_TEST_CASE
        TEMPLATE_TEST_CASE_SIG
        TEMPLATE_TEST_CASE_METHOD
        TEMPLATE_TEST_CASE_METHOD_SIG
        TEMPLATE_PRODUCT_TEST_CASE
        TEMPLATE_PRODUCT_TEST_CASE_SIG
        TEMPLATE_PRODUCT_TEST_CASE_METHOD
        TEMPLATE_PRODUCT_TEST_CASE_METHOD_SIG
        TEMPLATE_LIST_TEST_CASE
        TEMPLATE_LIST_TEST_CASE_METHOD
        METHOD_AS_TEST_CASE
    );

    my %SKIP_FIRST_ARG = map { $_ => 1 } qw(
        TEST_CASE_METHOD
        SCENARIO_METHOD
        TEMPLATE_TEST_CASE_METHOD
        TEMPLATE_TEST_CASE_METHOD_SIG
        TEMPLATE_PRODUCT_TEST_CASE_METHOD
        TEMPLATE_PRODUCT_TEST_CASE_METHOD_SIG
        TEMPLATE_LIST_TEST_CASE_METHOD
        METHOD_AS_TEST_CASE
    );

    # SCENARIO is TEST_CASE with a fixed prefix, and the prefix is what the
    # filter argument actually begins with -- so SCENARIO("~foo") registers as
    # "Scenario: ~foo", whose tilde is no longer leading and is harmless
    # (verified: its entry selects exactly 1 test). Rejecting it would be a
    # false positive, so the prefix is applied before the checks below.
    my %PREFIX = (
        SCENARIO        => "Scenario: ",
        SCENARIO_METHOD => "Scenario: ",
    );

    chomp(my $file = $_);
    open(my $fh, "<", $file) or die "cannot open $file: $!\n";
    my $src = do { local $/; <$fh> };
    close $fh;
    $files++;

    while ($src =~ /\b($MACRO_RE)\s*\(/g) {
        my $macro  = $1;
        my $before = substr($src, 0, $-[0]);
        my $line   = 1 + ($before =~ tr/\n//);

        # A macro-shaped token starting a comment line is prose, not a
        # registration -- the same test, for the same reason, that
        # check_deprecated_markers.sh applies to attribute-shaped tokens.
        my $linestart = ($before =~ /([^\n]*)\z/) ? $1 : "";
        next if $linestart =~ m{\A\s*(?://|\*|/\*|\#)};

        my $rest = substr($src, pos($src));

        # Step over the fixture/method argument of the *_METHOD forms: scan to
        # the first comma outside any bracket, so a template fixture such as
        # Fixture<std::map<int, int>> is stepped over as a single argument.
        if ($SKIP_FIRST_ARG{$macro}) {
            my $depth = 0;
            my $cut;
            for (my $i = 0; $i < length($rest); $i++) {
                my $c = substr($rest, $i, 1);
                if ($c =~ /[\(\[\{\<]/) { $depth++ }
                elsif ($c =~ /[\)\]\}\>]/) { last if $depth == 0; $depth-- }
                elsif ($c eq "," && $depth == 0) { $cut = $i + 1; last }
            }
            next unless defined $cut;   # no second argument: nothing named
            $rest = substr($rest, $cut);
        }

        # The name: one or more adjacent string literals, possibly split
        # across lines.
        unless ($rest =~ /\A\s*((?:"(?:[^"\\]|\\.)*"\s*)+)/) {
            # TEST_CASE() with no argument is fine: Catch2 generates the name
            # itself and no author-chosen metacharacter can reach it. Anything
            # else in first position is a name this checker cannot read, and an
            # unverifiable name must not be assumed compliant.
            next if $rest =~ /\A\s*\)/;
            print STDERR "error: $file:$line: $macro name is not a plain string "
                       . "literal, so it cannot be checked for Catch2 filter "
                       . "metacharacters; spell the name out in place (issue #466)\n";
            $status = 1;
            next;
        }

        my $literals = $1;
        my $name = join "", ($literals =~ /"((?:[^"\\]|\\.)*)"/g);

        # Undo the C++ escapes that can change the first or last character.
        $name =~ s/\\n/\n/g;
        $name =~ s/\\t/\t/g;
        $name =~ s/\\r/\r/g;
        $name =~ s/\\(["\\])/$1/g;

        $name = ($PREFIX{$macro} // "") . $name;

        # Catch2 normalises (trims) both the spec and the test name before
        # matching, and its spec parser skips leading spaces before reading the
        # first operator -- so " ~foo" negates exactly as "~foo" does.
        $name =~ s/\A\s+//;
        $name =~ s/\s+\z//;
        $checked++;

        my $why;
        if ($name =~ /\A~/) {
            $why = "begins with a tilde, Catch2 exclusion operator: the generated "
                 . "ctest entry runs every OTHER test in the binary, never this "
                 . "one, and reports success";
        } elsif ($name =~ /\Aexclude:/) {
            $why = "begins with exclude:, the long spelling of Catch2 exclusion "
                 . "operator: the generated ctest entry runs every OTHER test in "
                 . "the binary, never this one, and reports success";
        } elsif ($name =~ /\A\*/ or $name =~ /\*\z/) {
            $why = "begins or ends with an asterisk, Catch2 wildcard: the "
                 . "generated ctest entry selects every test matching the "
                 . "pattern, not just this one, and reports their result under "
                 . "this name";
        } elsif ($name =~ /\A-/) {
            $why = "begins with a hyphen: Catch2 command line reads the filter "
                 . "argument as an option and refuses to start (Unrecognised "
                 . "token), so the generated ctest entry can never run this test";
        } elsif ($name =~ /\A"/) {
            $why = "begins with a double quote, which opens Catch2 quoted-name "
                 . "mode: the generated ctest entry matches no test at all (No "
                 . "tests ran) and can never pass";
        }

        if (defined $why) {
            print STDERR "error: $file:$line: $macro name $why -- issue #466.\n"
                       . "       Name: $name\n";
            $status = 1;
        }
    }

    END {
        printf("checked %d test-case name(s) in %d file(s)\n",
               $checked // 0, $files // 0);
        exit(defined $status ? $status : 0);
    }
' -- || status=$?

if [ "$status" -ne 0 ]; then
    echo ""
    echo "Catch2 test-name lint failed. catch_discover_tests passes each test's"
    echo "own name back to the binary as the Catch2 filter argument, and the"
    echo "names above are read as a filter *expression* rather than as a literal"
    echo "name -- so their ctest entries do not run the test they are named for."
    echo "Rename them (e.g. \"App's destructor releases ...\" rather than"
    echo "\"~App releases ...\")."
    exit 1
fi

echo "Catch2 test-name lint OK: every discovered test name selects itself as a filter."
