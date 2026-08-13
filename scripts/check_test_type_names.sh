#!/usr/bin/env bash
# Usage: bash scripts/check_test_type_names.sh [DIR...]
#
# Detects a same-name collision between two file-scope (external-linkage)
# struct/class declarations in different test files -- see issue #84.
#
# Two independently-written test files, each declaring `struct OrderModel {
# ... }` at file scope with no enclosing namespace, is an ODR violation: two
# external-linkage types with the same name and different definitions. Never
# diagnosed by the compiler or linker (each translation unit only ever sees
# its own definition); which one the linker keeps for a given call site is
# link-order dependent. This bit CI for real: see the "Fix applied" section
# of issue #84 -- test_remote_step_interleaving.cpp's bare `OrderModel` stub
# silently won over test_conflict_resolution.cpp's real one in some builds,
# so notifyBackendChanged() stopped calling onBackendChanged() and the
# offline queue never drained.
#
# A type declared inside a namespace -- anonymous (internal linkage, never
# visible to another TU) or named (linker-symbol-qualified, so a same-named
# type in a different namespace cannot collide) -- is safe from this bug
# class and is not scanned. Only struct/class declarations lexically at
# namespace depth 0 within the file are considered. Template specializations
# (`template <> struct morph::model::ModelTraits<Foo> { ... }`) are excluded:
# they specialize an existing template, not declare a new type, and the name
# already carries the qualifying namespace.
#
# Scans each given DIR recursively for *.cpp files (default: tests, which
# excludes tests/lint/ -- see below). Exits 0 if every file-scope type name
# is unique across the scanned files. Exits 1 and prints one "name declared
# in file:line and file:line" diagnostic per collision on stderr otherwise.
#
# tests/lint/ is never scanned by the default target: it holds this
# checker's own self-test fixtures (tests/lint/test_type_names/), which
# deliberately declare colliding names (see .../invalid/), so folding it
# into a real tests/**/*.cpp sweep would report those fixtures as collisions
# against each other and against genuine test files that happen to reuse a
# generic fixture name like OrderModel or Widget. Passing tests/lint (or a
# path under it) explicitly -- as scripts/test_check_test_type_names.sh does
# -- still scans it: only the *default* argument excludes it.
#
# Requires perl (whole-file scanning, brace-depth and namespace tracking --
# the same tool and technique check_deprecated_markers.sh uses).
set -euo pipefail

if [ "$#" -eq 0 ]; then
    dirs=("tests")
    prune=(-path "tests/lint" -prune -o)
else
    dirs=("$@")
    prune=()
fi

find "${dirs[@]}" "${prune[@]}" -name "*.cpp" -print0 | perl -0 -ne '
    use strict;
    use warnings;

    our %declaredAt;   # name -> "file:line" of first sighting
    our $status;

    chomp(my $file = $_);
    open(my $fh, "<", $file) or die "cannot open $file: $!\n";
    my $src = do { local $/; <$fh> };
    close $fh;
    my @lines = split /\n/, $src;

    my $depth = 0;          # brace depth, any block
    my @nsDepth;            # stack of depths at which a namespace block opened
    my $prevNonBlank = "";  # previous non-blank line, to spot `template <>`

    for (my $i = 0; $i < @lines; $i++) {
        my $line = $lines[$i];
        my $lineNo = $i + 1;

        # Only a file-scope (namespace depth 0) declaration is external-linkage
        # and thus scannable; skip everything while inside any namespace block.
        my $atFileScope = (scalar(@nsDepth) == 0);

        if ($atFileScope && $line =~ /^(?:struct|class)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:[:{;]|$)/) {
            my $name = $1;
            my $isSpecialization = ($prevNonBlank =~ /^template\s*<\s*>\s*$/);
            if (!$isSpecialization) {
                my $here = "$file:$lineNo";
                if (exists $declaredAt{$name} && $declaredAt{$name} !~ /^\Q$file:\E/) {
                    print STDERR "error: struct/class \"$name\" declared at file scope in both $declaredAt{$name} and $here (ODR collision risk -- see issue #84)\n";
                    $status = 1;
                } elsif (!exists $declaredAt{$name}) {
                    $declaredAt{$name} = $here;
                }
            }
        }

        # Track whether this line opens a namespace block, then update brace
        # depth for every brace on the line (namespace and otherwise).
        my $opensNamespace = ($line =~ /^namespace\b[^{]*\{\s*$/);

        my $opens = () = ($line =~ /\{/g);
        my $closes = () = ($line =~ /\}/g);

        if ($opensNamespace) {
            push @nsDepth, $depth;
        }
        $depth += $opens - $closes;
        while (@nsDepth && $depth <= $nsDepth[-1]) {
            pop @nsDepth;
        }

        $prevNonBlank = $line if $line !~ /^\s*$/;
    }

    END {
        exit(defined $status ? $status : 0);
    }
' -- 2>&1
status=$?

if [ "$status" -ne 0 ]; then
    echo ""
    echo "Test-type-name lint failed. Rename one of the colliding types (prefer a"
    echo "short, file/feature-specific prefix, e.g. StepILOrderModel over"
    echo "OrderModel) or move it into an anonymous namespace if it does not need"
    echo "external linkage for a BRIDGE_REGISTER_* macro."
    exit 1
fi

echo "Test-type-name lint OK: no file-scope struct/class name collisions found."
