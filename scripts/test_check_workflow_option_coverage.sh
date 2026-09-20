#!/usr/bin/env bash
# Usage: bash scripts/test_check_workflow_option_coverage.sh
#
# Self-test for scripts/check_workflow_option_coverage.py, the gate that keeps
# every declared `option(MORPH_BUILD_...)` built by some job in
# .github/workflows/.
#
# A lint gate nobody tests reports green whether or not it still detects
# anything, and this one is maximally exposed to that: the tree it guards is
# correct the moment the gate lands, so it passes on day one whether it is
# parsing anything at all. Its whole value is what it does on a day that has
# not happened yet -- the day someone declares option 17 and enables it
# nowhere. So the gate is checked in both directions: the unmodified tree must
# pass, and every drift it claims to catch is reintroduced into a scratch copy
# of the tree, one at a time, and must be caught for the stated reason.
#
# One mutation at a time matters: applied together, a single detection would
# mask every other.
#
# The checker enumerates CMake files with `git ls-files`, so each case runs
# against a throwaway git repository holding a copy of this one's tracked files
# rather than against a plain directory.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="scripts/check_workflow_option_coverage.py"

failures=0

note() { printf 'ok: %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

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
# is a substring the diagnostic must contain; without it a mutation that broke
# the tree some unrelated way -- a mangled sed, a file the mutator emptied --
# would count as a detection, and this self-test would report a gate that no
# longer detects anything as fully working.
expect_caught() {
    local description="$1" mutator="$2" expected="$3"
    local tree="${scratch}/case" output
    make_tree "$tree"
    if ! ( cd "$tree" && eval "$mutator" ); then
        fail "mutator failed to apply: ${description}"
        return
    fi
    if output="$( cd "$tree" && python3 "$checker" 2>&1 )"; then
        fail "NOT caught: ${description} -- the gate passed a tree it should reject"
        printf '%s\n' "$output" >&2
        return
    fi
    if printf '%s' "$output" | grep -qF "$expected"; then
        note "caught: ${description}"
    else
        fail "caught for the WRONG reason: ${description} -- no diagnostic containing '${expected}':"
        printf '%s\n' "$output" >&2
    fi
}

# The mirror, for false positives. A gate that rejected every tree would
# "catch" every case below while being worthless.
expect_accepted() {
    local description="$1" mutator="$2"
    local tree="${scratch}/case" output
    make_tree "$tree"
    if ! ( cd "$tree" && eval "$mutator" ); then
        fail "mutator failed to apply: ${description}"
        return
    fi
    if output="$( cd "$tree" && python3 "$checker" 2>&1 )"; then
        note "accepted: ${description}"
    else
        fail "FALSE POSITIVE: ${description} -- the gate rejected a tree it should accept:"
        printf '%s\n' "$output" >&2
    fi
}

# -- The unmodified tree must pass -------------------------------------------
make_tree "${scratch}/clean"
if output="$( cd "${scratch}/clean" && python3 "$checker" 2>&1 )"; then
    note "the unmodified tree passes"
else
    fail "the unmodified tree was rejected by the gate:"
    printf '%s\n' "$output" >&2
fi

# -- The defect this gate exists for -----------------------------------------
# The next MORPH_BUILD_* option, declared and enabled by nobody. This is the
# case morph#605 is about: not the option that was missing when the gate was
# written, but the one that will be missing after it.
expect_caught "a newly declared option that no job enables" \
    "printf '%s\n' 'option(MORPH_BUILD_TELEMETRY \"Build the telemetry sink\" OFF)' >> CMakeLists.txt" \
    "MORPH_BUILD_TELEMETRY is declared in this tree, defaults OFF"

# The original hole, reintroduced: drop the flag morph#605 added and the gate
# must go red again. This is the case that proves the gate is not vacuous for
# the very option that motivated it.
expect_caught "MORPH_BUILD_BANK_GUI dropped from ci.yml's all-features job" \
    "edit .github/workflows/ci.yml -e '/^ *-DMORPH_BUILD_BANK_GUI=ON /d'" \
    "MORPH_BUILD_BANK_GUI is declared in this tree, defaults OFF"

# ...and the WASM enablement must not paper over it. wasm-demo.yml has passed
# -DMORPH_BUILD_BANK_GUI=ON all along; under EMSCRIPTEN that builds gui_wasm/
# and never the native gui/ target morph#604 found broken. If this assertion
# ever stops holding, the gate has gone blind to its own founding case.
expect_caught "the Emscripten enablement does not count as native coverage" \
    "edit .github/workflows/ci.yml -e '/^ *-DMORPH_BUILD_BANK_GUI=ON /d'" \
    "but that is an Emscripten build"

# -- What must not count as coverage -----------------------------------------
# A flag mentioned in a comment. ci.yml's configure steps discuss flags they do
# not pass, so a checker that grepped the raw text would score them as covered.
expect_caught "the only enablement is commented out" \
    "edit .github/workflows/ci.yml -e 's|^\( *\)-DMORPH_BUILD_LOAD_TESTS=ON |\1# -DMORPH_BUILD_LOAD_TESTS=ON |'" \
    "MORPH_BUILD_LOAD_TESTS"

# A matrix-valued flag whose matrix has quietly gone all-OFF. The flag line is
# untouched and still reads `-DMORPH_BUILD_FUZZERS=${{ matrix.fuzzers }}`; only
# the values behind it change. A checker that accepted the expression on sight
# would report this covered. The literal `-DMORPH_BUILD_FUZZERS=ON` in the
# clang-tidy job goes too, or it would cover the option by itself and this case
# would prove nothing about the matrix resolution.
expect_caught "every matrix leg sets the fuzzers key OFF" \
    "edit .github/workflows/ci.yml -e '/-DMORPH_BUILD_FUZZERS=ON/d' \
        && edit .github/workflows/ci.yml -e \"s/^\( *\)fuzzers: 'ON'/\1fuzzers: 'OFF'/\"" \
    "MORPH_BUILD_FUZZERS"

# The other half of the same rule: with the literal gone, an ON matrix leg
# still counts. Without this case the rule above would be satisfied by a
# checker that simply never resolved a matrix expression at all.
expect_accepted "a matrix leg setting the key ON is the only enablement" \
    "edit .github/workflows/ci.yml -e '/-DMORPH_BUILD_FUZZERS=ON/d'"

# -- The exemption set must stay necessary -----------------------------------
# An exemption for an option that is in fact enabled. This is the shape the
# gate replaces: a hand-written record that is no longer true and that nothing
# reads back against reality.
expect_caught "an exemption for an option some job does enable" \
    "edit ${checker} -e 's|^    \"MORPH_BUILD_CLANG_TIDY\": |    \"MORPH_BUILD_NET\": \"stale\",\n    \"MORPH_BUILD_CLANG_TIDY\": |'" \
    "MORPH_BUILD_NET is exempted by this checker"

# An exemption for an option that no longer exists.
expect_caught "an exemption naming an option no CMake file declares" \
    "edit ${checker} -e 's|^    \"MORPH_BUILD_CLANG_TIDY\": |    \"MORPH_BUILD_GONE\": \"stale\",\n    \"MORPH_BUILD_CLANG_TIDY\": |'" \
    "is declared by no CMake file in the tree"

# -- The gate must not go blind ----------------------------------------------
# If the declaration syntax moves out from under the parser, the honest answer
# is failure. A gate with nothing left to check reports green exactly as
# loudly as one that checked everything -- morph#466's shape, one level up.
expect_caught "the option() declarations become unparseable" \
    "edit CMakeLists.txt -e 's/^option(MORPH_BUILD_/OPTION_DISABLED(MORPH_BUILD_/' \
        && edit examples/vetted_hmac/CMakeLists.txt -e 's/^option(MORPH_BUILD_/OPTION_DISABLED(MORPH_BUILD_/'" \
    "found no option(MORPH_BUILD_...) declarations at all"

# -- False positives ---------------------------------------------------------
# A default-ON option needs no workflow to name it: it is built by every job
# that does not turn it off.
expect_accepted "a newly declared option that defaults ON" \
    "printf '%s\n' 'option(MORPH_BUILD_TELEMETRY \"Build the telemetry sink\" ON)' >> CMakeLists.txt"

# Coverage moving between jobs is not drift. The option is still built; the
# gate must not care which leg does it.
expect_accepted "an option enabled by a different job than before" \
    "edit .github/workflows/ci.yml -e '/^ *-DMORPH_BUILD_LOAD_TESTS=ON /d' \
        && printf '%s\n' '            -DMORPH_BUILD_LOAD_TESTS=ON \\' >> .github/workflows/ci.yml"

if [ "$failures" -ne 0 ]; then
    printf '\n%d case(s) failed.\n' "$failures" >&2
    exit 1
fi

printf '\nall cases passed.\n'
