#!/usr/bin/env bash
# Usage: bash scripts/check_nolint_directives.sh [DIR...]
#
# Fails if any NOLINTNEXTLINE directive cannot take effect -- see issue #627.
#
# NOLINTNEXTLINE suppresses diagnostics on the line *immediately* following it,
# counted in physical lines. If that next line is another comment, or is blank,
# or does not exist, the suppression lands on nothing:
#
#     // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward) -- the reason
#     // wrapped onto a second line
#     constexpr void forEachNamedMember(A&& action, Visitor&& visitor) {
#
# The directive above annotates the *comment* on the next line. The function is
# unannotated and its findings are still reported. clang-tidy says nothing about
# this: the directive parses, the file looks annotated, and the finding leaks.
#
# That is the failure mode AGENTS.md names first -- a control that reports
# success while measuring nothing -- one level below the gate: a suppression
# that reports success while suppressing nothing. It is also silent by
# construction, because the only symptom is a finding nobody expected to be
# suppressed in the first place, sitting in a file that looks like it has an
# opinion about it.
#
# include/morph/detail/fixed_string.hpp documents the hazard in prose and shows
# the fix, including the `// clang-format off` guard that stops the formatter
# re-wrapping a long directive. Nothing enforced it, so four other sites drifted
# into doing exactly what that comment warns about (#627). This gate is the
# enforcement.
#
# Remedies, either of which this gate accepts:
#
#   1. Put the reason *above* the directive, so the directive is the last
#      comment line before the code. This is what the four #627 sites now do.
#   2. Keep the reason on the directive's own line and add `// clang-format off`
#      / `// clang-format on` around it so the formatter cannot wrap it, as
#      fixed_string.hpp does for its four-check directive.
#
# WHAT THIS GATE DELIBERATELY DOES NOT MATCH
#
# A directive is recognised only when `NOLINTNEXTLINE` is the first token after
# the comment marker (`// NOLINTNEXTLINE...`, `/* NOLINTNEXTLINE...`). clang-tidy
# itself is looser -- it scans for the literal anywhere in a comment -- so prose
# that merely *names* the directive, as fixed_string.hpp's warning does:
#
#     // A NOLINTNEXTLINE directive must sit on ONE physical line to apply to the
#
# is a directive as far as clang-tidy is concerned (a vacuous one, annotating
# the next comment line, which is harmless) but is not scanned here. Anchoring
# is what lets the in-tree documentation of this hazard exist at all. The
# residual is a directive hidden mid-sentence in a comment; no such site exists
# in the tree, and writing one would be a stranger thing to do than the defect
# this gate catches.
#
# Exits 0 when every anchored NOLINTNEXTLINE is followed by a line of code.
# Exits 1 when one is not, printing a "file:line:" diagnostic and the two lines
# for each offender -- and also when no directive was found at all, because a
# gate that scanned nothing must not report success.
set -euo pipefail

if [ "$#" -eq 0 ]; then
    dirs=(include src tests examples)
else
    dirs=("$@")
fi

for dir in "${dirs[@]}"; do
    if [ ! -d "$dir" ]; then
        echo "error: not a directory: ${dir}" >&2
        exit 1
    fi
done

# find writes into a file rather than a process substitution so that its exit
# status is actually observed -- inside `< <(...)` it is discarded, and an
# unreadable tree would then reach the "no directives found" branch below and be
# reported as a tree that has none, which is a wrong diagnosis of a real
# failure. (Same reasoning as scripts/check_automoc_includes.sh.)
src_list="$(mktemp)"
trap 'rm -f "$src_list"' EXIT
# tests/lint holds this gate's own fixtures, three of which are deliberately
# inert directives; scanning them in the default whole-tree run would make the
# gate permanently red against its own test data. They are reached explicitly by
# scripts/test_check_nolint_directives.sh, which passes their directory as an
# argument -- so naming a path under tests/lint still scans it.
if ! find "${dirs[@]}" -type d \( -name build -o -name _deps -o -name .git \) -prune -o \
        -type d -path '*tests/lint' -prune -o \
        -type f \( -name '*.hpp' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' \
                   -o -name '*.cxx' -o -name '*.ipp' -o -name '*.c' \) -print0 \
        > "$src_list"; then
    echo "error: find failed while scanning for sources under: ${dirs[*]}" >&2
    exit 1
fi
mapfile -d '' -t src_files < "$src_list"

if [ "${#src_files[@]}" -eq 0 ]; then
    echo "error: no C/C++ sources found under: ${dirs[*]}" >&2
    exit 1
fi

total=0
offenders=""

for file in "${src_files[@]}"; do
    # awk holds the previous line so a directive can be judged against the one
    # that follows it. `found`/`bad` are printed on a trailing marker line so the
    # shell can accumulate both counts and the diagnostics in one pass.
    result="$(awk -v path="$file" '
        function trim(s) { sub(/^[[:space:]]+/, "", s); return s }
        {
            if (pending) {
                t = trim($0)
                # A directive is inert when the next physical line is another
                # comment, is blank, or is a continuation of a block comment.
                if (t == "" || t ~ /^\/\// || t ~ /^\/\*/ || t ~ /^\*/) {
                    printf "%s:%d: NOLINTNEXTLINE is followed by a comment or blank line, so it suppresses nothing\n", path, pendingline
                    printf "    %d | %s\n", pendingline, pendingtext
                    printf "    %d | %s\n", NR, $0
                    bad++
                }
                pending = 0
            }
            if (trim($0) ~ /^(\/\/|\/\*)[[:space:]]*NOLINTNEXTLINE([[:space:]]|\(|$)/) {
                found++
                pending = 1
                pendingline = NR
                pendingtext = $0
            }
        }
        END {
            # A directive on the final line of a file annotates nothing at all.
            if (pending) {
                printf "%s:%d: NOLINTNEXTLINE is the last line of the file, so it suppresses nothing\n", path, pendingline
                printf "    %d | %s\n", pendingline, pendingtext
                bad++
            }
            printf "@@ %d %d\n", found + 0, bad + 0
        }
    ' "$file")"

    marker="${result##*@@ }"
    body="${result%@@ *}"
    file_found="${marker%% *}"
    total=$((total + file_found))
    if [ -n "${body//[$'\n\t ']/}" ]; then
        offenders+="${body}"
    fi
done

if [ "$total" -eq 0 ]; then
    cat >&2 <<EMPTY
error: no NOLINTNEXTLINE directives found under: ${dirs[*]}
       This gate has nothing to check and must not report success. Either the
       scan's file set or its directive pattern has stopped matching the tree.
EMPTY
    exit 1
fi

if [ -n "$offenders" ]; then
    cat >&2 <<OFFENDERS

${offenders}
NOLINT directive lint failed: each directive above annotates the line that
follows it, and that line is a comment, is blank, or does not exist. The
suppression takes effect on nothing and the findings it names are still
reported -- silently, because clang-tidy does not diagnose a vacuous directive.

Fix it one of two ways:

  1. Move the reason ABOVE the directive, so the directive is the last comment
     line before the code it annotates. Preferred: it survives reformatting.

  2. Keep the reason on the directive's line and wrap the whole thing in
     "// clang-format off" / "// clang-format on", so the formatter cannot
     re-wrap it past the column limit. See include/morph/detail/fixed_string.hpp,
     which documents this hazard and demonstrates the guard.

Scanned ${total} NOLINTNEXTLINE directive(s) under: ${dirs[*]}
OFFENDERS
    exit 1
fi

echo "NOLINT directive lint OK: ${total} NOLINTNEXTLINE directive(s), all annotating code."
