#!/usr/bin/env bash
# Usage: bash scripts/test_check_install_export.sh
#
# Self-test for scripts/check_install_export.sh, the gate that keeps
# `find_package(morph CONFIG)` working for a consumer outside the tree.
#
# A gate nobody tests reports green whether or not it still detects anything,
# and this one guards a defect whose entire character was reporting success:
# `cmake --install` exited 0 while installing none of morph (morph#232). A
# check for that which itself passed against a broken install would be worse
# than no check at all -- it would turn "nobody looked" into "something looked
# and said it was fine".
#
# So the gate is checked in both directions: the unmodified tree must pass,
# and each defect it claims to catch is reintroduced into a scratch copy of the
# tree, one at a time, and must be caught *for the stated reason*. The
# mutations below are not hypothetical: the first is morph#232 itself, three
# more were live bugs in these install rules that survived a reading of the
# CMake and were found only by installing to a prefix and building something
# against it, and the last is a vacuity guard on the check that caught one of
# them.
#
# One mutation at a time matters: applied together, a single detection would
# mask every other.
#
# Runtime: most cases run the checker with --skip-header-set-verification,
# which drops its one slow phase (~44 standalone header compilations). The
# clean-tree case and the one mutation that phase exists to catch run the whole
# thing. Expect a few minutes overall; this belongs in its own CI job rather
# than in front of the build matrix.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="scripts/check_install_export.sh"

failures=0

note() { printf 'ok: %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# Every scratch tree shares one FetchContent cache, so Glaze is cloned once
# for the whole self-test rather than once per mutation.
export MORPH_CHECK_FETCHCONTENT_BASE_DIR="${scratch}/deps"

# A scratch copy holding exactly the files the checker configures against:
# with tests and examples off, morph's build reads the root CMakeLists.txt,
# cmake/, and the headers.
make_tree() {
    local dest="$1"
    rm -rf "$dest"
    mkdir -p "$dest"
    cp "${repo_root}/CMakeLists.txt" "$dest/"
    cp -R "${repo_root}/cmake" "$dest/cmake"
    cp -R "${repo_root}/include" "$dest/include"
    mkdir -p "$dest/scripts"
    cp "${repo_root}/${checker}" "$dest/scripts/"
}

# `sed -i` is not portable between GNU and BSD sed; edit through a temp file.
edit() {
    local file="$1"; shift
    sed "$@" "$file" > "${file}.new"
    mv "${file}.new" "$file"
}

# Each mutation must be caught, and caught *for the stated reason*. `$tree` is
# a fresh copy the mutator edits; every remaining argument is a substring the
# resulting diagnostic must contain, and all of them must be present. Without
# that a mutation that broke the tree in some unrelated way -- a mangled sed, a
# file the mutator emptied, a compiler that could not find anything at all --
# would count as a detection, and this self-test would report a gate that no
# longer detects anything as fully working.
#
# **Expected substrings must be toolchain-independent.** Every one below is
# either a diagnostic the checker writes itself, or a bare identifier: a header
# path, a target name, a symbol. None quotes a compiler's or CMake's phrasing.
# That is not fussiness -- it is the trap this repository keeps walking into,
# where something developed against one toolchain behaves differently under
# another and nobody notices until CI. It bit this very file: the
# header-not-found case first asserted on clang's
# `'morph/detail/fixed_string.hpp' file not found`, which cannot match GCC's
# `morph/detail/fixed_string.hpp: No such file or directory`, so CI reported
# "caught for the WRONG reason" about a mutation the checker had detected
# perfectly. Pairing the checker's own wording (which pins *which* check fired)
# with a bare identifier (which pins the cause) is specific enough to rule out
# an unrelated failure, and both compilers agree on it.
#
# `$mode` is spelled out per case rather than defaulted. An earlier revision
# defaulted it, and the one case that needs the slow phase passed an empty
# string -- which `${4:-...}` helpfully replaced with the default again,
# silently skipping the phase that case exists to exercise while still
# reporting green.
expect_caught() {
    local description="$1" mutator="$2" mode="$3"
    shift 3
    local expected=("$@")
    local tree="${scratch}/case" output flag missing needle

    case "$mode" in
        fast) flag="--skip-header-set-verification" ;;
        full) flag="" ;;
        *) fail "unknown mode '${mode}' for: ${description}"; return ;;
    esac

    make_tree "$tree"
    if ! ( cd "$tree" && eval "$mutator" ); then
        fail "mutator failed to apply: ${description}"
        return
    fi
    if output="$(bash "${tree}/${checker}" $flag "$tree" 2>&1)"; then
        fail "NOT caught: ${description} -- the gate passed a tree it should reject"
        return
    fi

    missing=""
    for needle in "${expected[@]}"; do
        if ! printf '%s' "$output" | grep -qF -- "$needle"; then
            missing="${missing}${missing:+, }'${needle}'"
        fi
    done
    if [ -z "$missing" ]; then
        note "caught: ${description}"
    else
        fail "caught for the WRONG reason: ${description} -- diagnostic lacks ${missing}:"
        printf '%s\n' "$output" >&2
    fi
}

# ── The unmodified tree must pass ────────────────────────────────────────────
#
# Run in full, header-set verification included: the mutation for that phase
# below only proves the phase can fail, not that it passes on a good tree.
make_tree "${scratch}/clean"
if output="$(bash "${scratch}/clean/${checker}" "${scratch}/clean" 2>&1)"; then
    note "the unmodified tree passes"
else
    fail "the unmodified tree was rejected by the gate:"
    printf '%s\n' "$output" >&2
fi

# ── Each defect the gate claims to catch ─────────────────────────────────────

# The original defect: no install rules at all, so `cmake --install` exits 0
# having installed Glaze and nothing of morph.
expect_caught "morph's install rules not running at all" \
    "edit CMakeLists.txt -e 's@^option(MORPH_INSTALL \"Generate morph.s install and export rules\" .*\$@option(MORPH_INSTALL \"\" OFF)@'" \
    fast \
    "installed no lib/cmake/morph/morphConfig.cmake" \
    "installed no include/morph/util/rational.hpp"

# Bug 1: installing only the declared FILE_SET HEADERS. `find_package`
# succeeds, the prefix looks populated, and the consumer cannot compile,
# because public headers include morph/detail/ headers that belong to no
# header set. Only building the consumer finds this.
#
# The second needle is the bare header path: clang says "'...' file not found",
# GCC says "...: No such file or directory", and neither phrasing belongs in an
# assertion. The path itself is in both.
expect_caught "the detail/ header set dropped from the install" \
    "edit CMakeLists.txt -e '/^# The detail\/ headers that public headers include\./,/^set_target_properties(morph PROPERTIES INTERFACE_HEADER_SETS_TO_VERIFY HEADERS)$/d' -e '/FILE_SET morph_detail_headers DESTINATION/d'" \
    fast \
    "the consumer project did not compile against the install prefix" \
    "morph/detail/fixed_string.hpp"

# Bug 2: INTERFACE_HEADER_SETS_TO_VERIFY defaults to *every* interface header
# set, and quantity_equation.hpp is included partway down quantity.hpp and is
# not self-contained. This is the one case the slow phase exists for, so it is
# the one case that runs it.
#
# `formatOptionalDecimal` is the identifier the header uses before quantity.hpp
# has declared it. Both compilers name it -- clang as "use of undeclared
# identifier", GCC as "was not declared in this scope" -- and it appears
# nowhere else in the output, which the file name alone cannot promise (ninja
# prints that on the progress line for a unit that compiled fine).
expect_caught "the verified header sets widened back to include detail/" \
    "edit CMakeLists.txt -e '/INTERFACE_HEADER_SETS_TO_VERIFY HEADERS/d'" \
    full \
    "morph's interface header sets do not all compile standalone" \
    "morph/detail/quantity_equation.hpp" \
    "formatOptionalDecimal"

# Bug 3: install(EXPORT NAMESPACE morph::) prefixes the target name, so
# morph_net is exported as morph::morph_net while every in-tree alias, the
# README, and any consumer say morph::net.
expect_caught "EXPORT_NAME dropped from the optional targets" \
    "edit CMakeLists.txt -e '/EXPORT_NAME \${_morph_component})/d' -e 's@^            set_target_properties(morph_\${_morph_component} PROPERTIES\$@@'" \
    fast \
    "morph::morph_net"

# Bug 4: morph::morph names glaze::glaze in its interface, so morphTargets.cmake
# names it literally. Without find_dependency, find_package(morph) reports the
# package found and then fails on a target nobody created. `glaze::glaze` is
# the target name CMake echoes back, not CMake's phrasing around it.
expect_caught "find_dependency(glaze) dropped from the package config" \
    "edit cmake/morphConfig.cmake.in -e '/^find_dependency(glaze /d'" \
    fast \
    "the consumer project could not configure against the install prefix" \
    "glaze::glaze"

# Vacuity guard on the exported-name check. With no optional component
# installed there is no morph::<component> in morphTargets.cmake to inspect,
# so that check would compare nothing and pass on any regression.
expect_caught "no optional component installed, leaving the export-name check with nothing to read" \
    "edit CMakeLists.txt -e 's@foreach(_morph_component IN ITEMS offline_sqlite qt_forms qt net)@foreach(_morph_component IN ITEMS \"\")@'" \
    fast \
    "no optional component was installed"

# ── Verdict ─────────────────────────────────────────────────────────────────
if [ "$failures" -ne 0 ]; then
    printf '\n%d self-test check(s) failed.\n' "$failures" >&2
    exit 1
fi

printf '\nscripts/check_install_export.sh detects every install/export defect it claims to.\n'
