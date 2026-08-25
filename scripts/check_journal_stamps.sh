#!/usr/bin/env bash
# Usage: bash scripts/check_journal_stamps.sh [DIR...]
#
# Enforces docs/spec/journal/journal.md's "Payload schema fingerprint": a
# hand-rolled `morph::journal::LogEntry` must stamp `schema`, or say in place
# why it deliberately cannot.
#
# morph stamps the fingerprint automatically at its two execution sites
# (`ActionDispatcher::registerAction`'s runner and `Bridge::executeVia`'s
# `localOp`). A model constructed directly never reaches either, so every
# ladder rung that wanted a journal built its own `logAction` and its own
# `LogEntry`. An entry those sites leave unstamped is not "verified unchanged"
# — it is *unverifiable*, and `journal::replay()`'s default
# `UnstampedPayloadPolicy::Replay` replays it exactly as a pre-fingerprint
# build did: a renamed field decodes to its default and the reconstruction is
# confidently wrong. The rungs whose definition of done is "reconstructible
# from the journal alone" are therefore exactly the ones the check did not
# cover. This gate is what stops a new unstamped copy from appearing.
#
# Rejected, each with a file:line diagnostic:
#   - a `LogEntry` local declared and populated field-by-field, where nothing
#     in its enclosing block ever assigns `<name>.schema`
#   - a `LogEntry` built by brace initialisation whose braces contain no
#     `.schema` designator
#
# Deliberate non-stamps opt out in place, with a reason, by writing
#
#   // journal-stamp-exempt: <why this entry cannot carry a fingerprint>
#
# anywhere in the comment block immediately above the declaration. The reason
# is not parsed -- the point is that the choice is visible to a reader of the
# construction site, which is where the question comes up.
#
# Scans DIR (default: examples) recursively for *.cpp and *.hpp. Test sources
# (any path with a `tests/` component) are never scanned: a test that pins
# what an older, pre-fingerprint build wrote has to construct an unstamped
# entry, and a gate that forbade it would forbid the regression tests for this
# very defect.
#
# Exits 0 if every construction found stamps or is exempt (zero constructions
# found is a pass). Exits 1 and prints "file:line: <message>" for each
# offending construction on stderr otherwise.
#
# Requires perl (the declaration and its `.schema` assignment are lines apart,
# so this cannot be a line-scoped grep).
set -euo pipefail

dirs=("${@:-examples}")

find "${dirs[@]}" \( -name "*.cpp" -o -name "*.hpp" \) -print0 |
    perl -0 -ne '
    use strict;
    use warnings;

    our $status;

    # Returns the text from the start of $text up to the point where brace
    # depth first falls below zero, starting from $depth. With $depth == 1 that
    # is the body of a brace initialiser already opened; with $depth == 0 it is
    # the remainder of the enclosing block, which is the scope a declared local
    # is actually visible in.
    #
    # Braces inside string or character literals would confuse this, which is
    # accepted: a `{` in a literal inside a LogEntry-building block is not a
    # shape this repository writes, and the failure mode is a diagnostic that
    # is too eager, not one that is silently blind.
    sub scan_block {
        my ($text, $start) = @_;
        my $depth = $start;
        my $body  = "";
        for my $ch (split //, $text) {
            if ($ch eq "{") {
                $depth++;
            } elsif ($ch eq "}") {
                $depth--;
                # Stop only once the depth drops *below* where the scan began:
                # a balanced `std::string{}` on the way is not the end of the
                # scope, and treating it as one is how this check would go
                # quietly blind.
                last if $depth < $start;
            }
            $body .= $ch;
        }
        return $body;
    }

    chomp(my $file = $_);
    # A tests/ component anywhere in the path: fixtures that reproduce what a
    # previous build wrote are unstamped on purpose.
    next if $file =~ m{(?:\A|/)tests/};

    open(my $fh, "<", $file) or die "cannot open $file: $!\n";
    my $src = do { local $/; <$fh> };
    close $fh;

    # An optionally-qualified LogEntry, then the object name, then either `;`
    # (default-constructed, populated field by field) or `{` (brace init).
    while ($src =~ /(?:::)?(?:morph::)?journal::LogEntry\s+(\w+)\s*([;{])/g) {
        my ($name, $opener) = ($1, $2);
        my $before = substr($src, 0, $-[0]);
        my $line   = 1 + ($before =~ tr/\n//);

        # A declaration-shaped token inside a comment is prose. Detected by the
        # start of its own physical line, the same way
        # scripts/check_deprecated_markers.sh does it, and for the same reason:
        # comments cannot be parsed reliably without also tracking string and
        # character literals. A real declaration never begins a comment line.
        my $linestart = ($before =~ /([^\n]*)\z/) ? $1 : "";
        next if $linestart =~ m{\A\s*(?://|\*|/\*)};

        # Opt-out marker anywhere in the comment block immediately above the
        # declaration. Scoped to that block rather than to a fixed number of
        # lines because the reason is prose and wraps: a marker that only
        # counted lines would silently stop applying as soon as someone
        # explained themselves at length.
        my @prev = split(/\n/, $before, -1);
        pop @prev;  # drop the declaration line itself
        my @comment;
        while (@prev && $prev[-1] =~ m{\A\s*(?://|\*|/\*)}) {
            unshift @comment, pop @prev;
        }
        next if join("\n", @comment) =~ /journal-stamp-exempt/;

        if ($opener eq "{") {
            # Brace initialisation: the designator has to be in the braces.
            # Scan forward, tracking depth, so a nested brace does not end the
            # initialiser early.
            my $body = scan_block(substr($src, $+[0]), 1);
            next if $body =~ /\.\s*schema\s*=/;
            print STDERR "error: $file:$line: journal::LogEntry \`$name\` is brace-initialised "
                       . "without a .schema designator, so every entry it produces is unstamped "
                       . "and replays unverified (docs/spec/journal/journal.md, \"Payload schema "
                       . "fingerprint\"). Stamp it with "
                       . "::morph::model::detail::actionPayloadSchema<Action>(), or write "
                       . "\`// journal-stamp-exempt: <reason>\` above the declaration.\n";
            $status = 1;
            next;
        }

        # Default-constructed: some later statement in the same block must
        # assign its schema. Scoped to the enclosing block rather than to the
        # rest of the file, so a *different* function that happens to name its
        # own entry `entry` and does stamp it cannot vouch for this one.
        my $scope = scan_block(substr($src, $+[0]), 0);
        next if $scope =~ /\b\Q$name\E\s*\.\s*schema\s*=/;
        print STDERR "error: $file:$line: journal::LogEntry \`$name\` is never stamped "
                   . "(\`$name.schema = ...\` is missing), so every entry it produces is "
                   . "unstamped and replays unverified (docs/spec/journal/journal.md, "
                   . "\"Payload schema fingerprint\"). Stamp it with "
                   . "::morph::model::detail::actionPayloadSchema<Action>(), or write "
                   . "\`// journal-stamp-exempt: <reason>\` above the declaration.\n";
        $status = 1;
    }

    END { exit($status ? 1 : 0) }
'
