#!/usr/bin/env bash
# Usage: bash scripts/check_coverage_profiles.sh BUILD_DIR
#
# Prints the space-separated list of .profraw files under BUILD_DIR on
# stdout, and fails if there are none. This is the profile-discovery half of
# morph#430: naming it here, in one place, is what lets scripts/coverage.sh
# delete exactly this list once it has merged them, so a later run's find can
# never inherit a stale file this run already accounted for.
#
# On its own this script still does a recursive find with no time bound --
# that half of the fix is not "search narrower", it is "nothing is left for
# the search to find by the time it next runs". See scripts/coverage.sh's own
# comment at its PROFILES assignment for why the bound lives at the trailing
# edge (delete after merge) rather than the leading one (clean before ctest):
# this script does not control when ctest runs, so it cannot itself guarantee
# what exists before ctest writes into BUILD_DIR, only account fully for what
# is there when it is asked.
#
# A gate on its own if it fails: no .profraw means no profile data, and no
# amount of merging or reporting after this point can produce a coverage
# figure from nothing.
set -euo pipefail

build_dir="${1:?usage: check_coverage_profiles.sh BUILD_DIR}"
readonly build_dir

profiles="$(find "$build_dir" -name '*.profraw' 2>/dev/null | tr '\n' ' ')"
if [ -z "${profiles// /}" ]; then
    echo "ERROR: No .profraw files found in $build_dir." >&2
    echo "Did you set LLVM_PROFILE_FILE and run ctest --preset clang-coverage?" >&2
    exit 1
fi

printf '%s' "$profiles"
