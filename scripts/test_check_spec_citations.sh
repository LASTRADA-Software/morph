#!/usr/bin/env bash
# Usage: bash scripts/test_check_spec_citations.sh
#
# Self-test for the two gates in scripts/check_spec_citations.sh that were
# themselves added to close a citation family nothing could fail for: the
# section-citation check (check 5, morph#316), which keeps a citation naming a
# *section* of a markdown file pointing at a section that is really there, and
# pointing at an entry that still exists and is named by its slug.
#
# A lint gate nobody tests reports green whether or not it still detects
# anything -- and this gate exists precisely because the surrounding script had
# a check that could not fail: check 3 verified that a cited *path* resolved and
# stopped there, so a citation naming a heading that had been renamed, or never
# written, linted green for as long as the file it named existed. A
# section-citation check with a regex that quietly stopped matching would
# reproduce that bug inside its own fix, so the gate is checked in both
# directions: the unmodified tree must pass, and every drift it claims to catch
# is reintroduced into a scratch copy of the tree, one at a time, and must be
# caught -- including the case where the scan itself goes blind.
#
# One mutation at a time matters: applied together, a single detection would
# mask every other.
#
# The checker takes no arguments and reads the tree through `git ls-files`, so
# each case runs against a throwaway git repository holding a copy of this
# one's tracked files rather than against a directory argument.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="scripts/check_spec_citations.sh"

failures=0

note() { printf 'ok: %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# One pristine copy of every tracked file, in its own git repository (the
# checker resolves its own root with `git rev-parse` and enumerates the tree
# with `git ls-files`, so a plain directory would not do). Built once; each
# case gets a directory copy of it, which is far cheaper than re-copying a
# thousand files per mutation.
readonly pristine="${scratch}/pristine"
mkdir -p "$pristine"
while IFS= read -r -d '' tracked; do
    mkdir -p "${pristine}/$(dirname "$tracked")"
    cp "${repo_root}/${tracked}" "${pristine}/${tracked}"
done < <(cd "$repo_root" && git ls-files -z)
git -C "$pristine" init -q
git -C "$pristine" add -A

make_tree() {
    local dest="$1"
    rm -rf "$dest"
    mkdir -p "$dest"
    cp -R "${pristine}/." "$dest"
}

# `sed -i` is not portable between GNU and BSD sed; edit through a temp file.
edit() {
    local file="$1"; shift
    sed "$@" "$file" > "${file}.new"
    mv "${file}.new" "$file"
}

# Each mutation must be caught, and caught *for the stated reason*. `$expected`
# is a substring the resulting diagnostic must contain; without it a mutation
# that broke the tree in some unrelated way -- a mangled sed, a file the mutator
# emptied -- would count as a detection, and this self-test would report a gate
# that no longer detects anything as fully working.
expect_caught() {
    local description="$1" mutator="$2" expected="$3"
    local tree="${scratch}/case" output
    make_tree "$tree"
    if ! ( cd "$tree" && eval "$mutator" ); then
        fail "mutator failed to apply: ${description}"
        return
    fi
    if output="$( cd "$tree" && bash "$checker" 2>&1 )"; then
        fail "NOT caught: ${description} -- the gate passed a tree it should reject"
        return
    fi
    if printf '%s' "$output" | grep -qF "$expected"; then
        note "caught: ${description}"
    else
        fail "caught for the WRONG reason: ${description} -- no diagnostic containing '${expected}':"
        printf '%s\n' "$output" >&2
    fi
}

# The mirror of expect_caught, for the false positives this check must not
# manufacture. A heading match that were merely "the cited text is a heading,
# spelled exactly" would reject citations that are correct today.
expect_accepted() {
    local description="$1" mutator="$2"
    local tree="${scratch}/case" output
    make_tree "$tree"
    if ! ( cd "$tree" && eval "$mutator" ); then
        fail "mutator failed to apply: ${description}"
        return
    fi
    if output="$( cd "$tree" && bash "$checker" 2>&1 )"; then
        note "accepted: ${description}"
    else
        fail "FALSE POSITIVE: ${description} -- the gate rejected a tree it should accept:"
        printf '%s\n' "$output" >&2
    fi
}

# ── The unmodified tree must pass ────────────────────────────────────────────
make_tree "${scratch}/clean"
if output="$( cd "${scratch}/clean" && bash "$checker" 2>&1 )"; then
    note "the unmodified tree passes"
else
    fail "the unmodified tree was rejected by the lint:"
    printf '%s\n' "$output" >&2
fi

# ── Each drift the section-citation check claims to catch ────────────────────

# The plain shape: one citation, one quoted section, all on one line.
expect_caught "a citation naming a section that does not exist" \
    "printf '%s\n' '/// See \`docs/spec/core/logger.md\`, \"No Such Section Here\".' >> include/morph/core/logger.hpp" \
    'docs/spec/core/logger.md has no section "No Such Section Here"'

# The shape morph#316 actually reported: a real section, a fabricated
# subsection, and the whole citation Doxygen-wrapped across two comment lines
# so that neither line contains it in full.
expect_caught "a wrapped citation whose subsection does not exist" \
    "printf '%s\n%s\n' '/// See \`docs/spec/core/logger.md\`, \"Failure modes\",' '/// \"A Subsection Never Written\".' >> include/morph/core/logger.hpp" \
    'docs/spec/core/logger.md has no section "A Subsection Never Written"'

# The check must require a *heading*, not merely that the quoted text occurs
# somewhere in the file. "model not found" is real prose in backend.md -- a
# substring grep would wave this through.
expect_caught "a citation naming prose that is not a heading" \
    "printf '%s\n' '/// See \`docs/spec/core/backend.md\`, \"model not found\".' >> include/morph/core/logger.hpp" \
    'docs/spec/core/backend.md has no section "model not found"'

# A cited file outside docs/spec/ that does not exist: check 3's dangling-path
# scan only walks docs/spec/**.md and morph/**.hpp, so this one is check 5's to
# catch or nobody's.
expect_caught "a citation naming a markdown file that does not resolve" \
    "printf '%s\n' '/// See \`examples/NO_SUCH_DOC.md\`, \"Failure modes\".' >> include/morph/core/logger.hpp" \
    'section citation names examples/NO_SUCH_DOC.md, which does not resolve'

# Proof the check is behavioural rather than a self-consistency check on the
# citing site: nothing textual changes in any file that cites this heading. The
# *spec* renames the section, exactly as a spec edit would, and every citation
# of it must go dangling.
expect_caught "a spec renaming a section its code comments cite" \
    "edit docs/spec/core/backend.md -e 's/^## Asynchronous registration .*\$/## Async registration — \`registerModelAsync\`/'" \
    'has no section "Asynchronous registration"'

# Vacuity guard on the scan itself. A stale glob here would leave the check
# reporting green having verified nothing -- the exact failure mode that let
# the dangling citations accumulate under a job named for checking them. The
# mutation goes into the *third* occurrence of that file list, which is the
# section-citation scan's; the first is check 3's dangling-path scan and the
# second its findings scan, each with a floor of its own. Blinding the wrong
# one trips a different floor, whose diagnostic does not contain the string
# expected below -- so a stale index here fails loudly rather than silently
# testing the wrong scan.
break_section_scan() {
    awk '
        /git ls-files -z .\*\.md. .\*\.hpp/ && ++n == 2 {
            sub(/-z .*\.qml./, "-z NO_SUCH_PATTERN")
        }
        { print }
    ' "$checker" > mutated && mv mutated "$checker"
}

expect_caught "the section-citation scan's own file glob going stale" \
    break_section_scan \
    'section-citation check only scanned 0 cited sections'

# ── The false positives it must not manufacture ──────────────────────────────
# Headings across this tree carry an appended qualifier that citations of them
# routinely drop -- `## Foo — bar` and `## Foo (bar)` are both cited as "Foo".
# A checker that demanded the full heading text would reject citations that are
# correct, so both qualifier shapes are asserted to keep matching when only the
# qualifier changes and the title does not.
expect_accepted "a heading whose em-dash qualifier changed but whose title did not" \
    "edit docs/spec/core/backend.md -e 's/^## Asynchronous registration .*\$/## Asynchronous registration — a reworded qualifier/'"

expect_accepted "a heading whose qualifier became a parenthetical" \
    "edit docs/spec/core/backend.md -e 's/^## Asynchronous registration .*\$/## Asynchronous registration (a reworded qualifier)/'"

# ── Verdict ─────────────────────────────────────────────────────────────────
if [ "$failures" -ne 0 ]; then
    printf '\n%d self-test check(s) failed.\n' "$failures" >&2
    exit 1
fi

printf '\nscripts/check_spec_citations.sh detects every section-citation drift it claims to.\n'
