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

# Checked explicitly, ahead of the find below: under `set -e`/`pipefail`,
# `find` on a directory that does not exist exits nonzero even with its own
# stderr redirected away, which would abort this script's `profiles=$(...)`
# assignment silently -- no output, no message, just exit 1 -- before the
# "No .profraw files found" diagnostic four lines down ever gets a chance to
# run. A missing BUILD_DIR and an empty one are different mistakes and
# deserve different messages.
if [ ! -d "$build_dir" ]; then
    echo "ERROR: $build_dir does not exist." >&2
    echo "Did you configure with cmake --preset clang-coverage first?" >&2
    exit 1
fi

profiles="$(find "$build_dir" -name '*.profraw' 2>/dev/null | tr '\n' ' ')"
if [ -z "${profiles// /}" ]; then
    echo "ERROR: No .profraw files found in $build_dir." >&2
    echo "Did you set LLVM_PROFILE_FILE and run ctest --preset clang-coverage?" >&2
    exit 1
fi

printf '%s' "$profiles"
