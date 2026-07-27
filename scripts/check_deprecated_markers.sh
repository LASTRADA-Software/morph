#!/usr/bin/env bash
# Usage: bash scripts/check_deprecated_markers.sh [DIR...]
#
# Enforces docs/spec/VERSIONING.md's deprecation-window format: every
# `[[deprecated]]` attribute in a scanned header must carry a message naming
# both a target removal version and a replacement, in the exact shape
#
#   [[deprecated("removed in <major>.<minor>[.<patch>]; use <replacement> instead")]]
#
# e.g. [[deprecated("removed in 2.0.0; use morph::bridge::NewThing instead")]]
#
# Rejected, each with a file:line diagnostic:
#   - the bare `[[deprecated]]` form (no message at all)
#   - a message that does not match the required shape
#   - an attribute whose argument is not a plain string literal (e.g. a macro),
#     which cannot be checked and so must not be assumed compliant
#
# The attribute may span multiple lines and may use adjacent string-literal
# concatenation; both are how a long message gets written in practice, and both
# are checked. It may also appear alongside other attributes in one list
# (`[[nodiscard, deprecated("...")]]`). Line numbers point at the `deprecated`
# token itself.
#
# Scans DIR (default: include/morph) recursively for *.hpp files. Exits 0 if
# every marker found matches the required shape (zero markers found is a
# pass -- nothing is deprecated yet at 0.1.0). Exits 1 and prints
# "file:line: <message>" for each offending marker on stderr otherwise.
#
# Requires perl (whole-file scanning; the previous grep implementation was
# line-scoped, and so was blind to both the bare form and wrapped messages).
set -euo pipefail

dirs=("${@:-include/morph}")

find "${dirs[@]}" -name "*.hpp" -print0 | perl -0 -ne '
    use strict;
    use warnings;

    our $status;
    my $FORMAT  = "removed in X.Y[.Z]; use <replacement> instead";
    my $PATTERN = qr/\Aremoved in [0-9]+\.[0-9]+(?:\.[0-9]+)?; use .+ instead\z/;

    chomp(my $file = $_);
    open(my $fh, "<", $file) or die "cannot open $file: $!\n";
    my $src = do { local $/; <$fh> };
    close $fh;

    # `[[`, then optionally other attributes in the same list, then the token.
    # Anchoring on `[[` keeps prose occurrences of the word "deprecated" in
    # comments from being mistaken for attributes. The [^\[\]"] guard stops the
    # optional group from running past a `]]` into a later attribute list.
    while ($src =~ /\[\[(?:[^\[\]"]*,\s*)?deprecated\b/g) {
        my $before = substr($src, 0, $-[0]);
        my $line   = 1 + ($before =~ tr/\n//);
        my $rest   = substr($src, $+[0]);

        # Skip attribute-shaped tokens sitting in a comment -- a Doxygen block
        # or `//` line showing the required format as an example is prose, not
        # a declaration. Detected by the start of the physical line rather than
        # by parsing comments, which cannot be done reliably without also
        # tracking string literals, character literals, and the digit
        # separators C++ spells with the same quote character. A real
        # attribute never begins a comment line.
        my $linestart = ($before =~ /([^\n]*)\z/) ? $1 : "";
        next if $linestart =~ m{\A\s*(?://|\*|/\*)};

        if ($rest =~ /\A\s*(?:\]\]|,)/) {
            print STDERR "error: $file:$line: bare [[deprecated]] is not allowed; "
                       . "it must carry a message matching: $FORMAT\n";
            $status = 1;
            next;
        }

        # One or more adjacent string literals, possibly split across lines.
        if ($rest =~ /\A\s*\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\)/) {
            my $literals = $1;
            my $message  = join "", ($literals =~ /"((?:[^"\\]|\\.)*)"/g);
            if ($message !~ $PATTERN) {
                print STDERR "error: $file:$line: [[deprecated]] message must match: "
                           . "$FORMAT -- got: $message\n";
                $status = 1;
            }
            next;
        }

        print STDERR "error: $file:$line: [[deprecated]] argument is not a plain "
                   . "string literal, so its format cannot be verified; spell the "
                   . "message out in place\n";
        $status = 1;
    }

    END { exit($status ? 1 : 0) }
'
