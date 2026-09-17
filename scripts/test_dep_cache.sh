#!/usr/bin/env bash
# Usage: bash scripts/test_dep_cache.sh
#
# Self-test for cmake/DepCache.cmake (morph#552). A dependency cache that
# silently stops caching looks exactly like one that is working -- the build
# still succeeds, it just clones again -- so the four properties it promises are
# asserted rather than assumed:
#
#   1. Unconfigured, it is a complete no-op (a developer's local build is
#      unchanged, and does not start sharing source trees between checkouts).
#   2. Cold, it populates the cache and FetchContent performs no clone.
#   3. Warm, it reuses the tree without re-populating, from a different build
#      directory and a different build type.
#   4. When the pre-clone fails, the configure survives and leaves
#      FETCHCONTENT_SOURCE_DIR_<NAME> unset, so FetchContent still gets its
#      chance. The cache is an optimisation; an optimisation that can break a
#      build is a liability.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
failures=0

fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

# A standalone project, so the assertions are about DepCache.cmake rather than
# about whatever else morph's own configure happens to do.
mkdir -p "${work}/proj"
cat > "${work}/proj/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.25)
project(morph_dep_cache_selftest NONE)
include("${MORPH_DEPCACHE}")
morph_cache_dep("${DEP_NAME}" "${DEP_REPO}" "${DEP_TAG}")
string(TOUPPER "${DEP_NAME}" _upper)
if(DEFINED FETCHCONTENT_SOURCE_DIR_${_upper} AND NOT FETCHCONTENT_SOURCE_DIR_${_upper} STREQUAL "")
    message(STATUS "selftest: pointed at ${FETCHCONTENT_SOURCE_DIR_${_upper}}")
else()
    message(STATUS "selftest: not pointed anywhere")
endif()
CMAKE

configure_once() {
    local build="$1" cache="$2" name="$3" repo="$4" tag="$5"
    rm -rf "${build}"
    local env_args=()
    if [ -n "${cache}" ]; then
        env_args=(env "MORPH_DEP_CACHE=${cache}")
    else
        # Also clear CI, or the helper's CI default would kick in and this
        # would no longer be the "unconfigured" case it claims to test.
        env_args=(env -u MORPH_DEP_CACHE -u CI)
    fi
    "${env_args[@]}" cmake -S "${work}/proj" -B "${build}" \
        -DMORPH_DEPCACHE="${repo_root}/cmake/DepCache.cmake" \
        -DDEP_NAME="${name}" -DDEP_REPO="${repo}" -DDEP_TAG="${tag}" 2>&1
}

# ── 1. Unconfigured: a complete no-op ───────────────────────────────────────
out="$(configure_once "${work}/b1" "" selftestdep https://example.invalid/x.git v1)"
if grep -q "dep cache" <<< "${out}"; then
    fail "with no cache configured the helper still acted: it must be inert for local builds"
fi
if ! grep -q "selftest: not pointed anywhere" <<< "${out}"; then
    fail "with no cache configured FETCHCONTENT_SOURCE_DIR was set anyway"
fi

# ── 2/3. Cold populate, then warm reuse ─────────────────────────────────────
# A tiny public repository, so the self-test does not clone something large.
readonly probe_repo="https://github.com/jothepro/doxygen-awesome-css.git"
readonly probe_tag="v2.3.4"
cache="${work}/cache"
mkdir -p "${cache}"

cold="$(configure_once "${work}/b2" "${cache}" doxygen-awesome-css "${probe_repo}" "${probe_tag}")"
if ! grep -q "dep cache: populating" <<< "${cold}"; then
    fail "a cold cache did not populate"
fi
if ! grep -q "selftest: pointed at" <<< "${cold}"; then
    fail "after populating, FETCHCONTENT_SOURCE_DIR was not set"
fi

warm="$(configure_once "${work}/b3" "${cache}" doxygen-awesome-css "${probe_repo}" "${probe_tag}")"
if grep -q "dep cache: populating" <<< "${warm}"; then
    fail "a warm cache populated again -- it is not being reused"
fi
if ! grep -q "selftest: pointed at" <<< "${warm}"; then
    fail "a warm cache did not point FetchContent at the cached tree"
fi

# ── 4. A failed pre-clone degrades instead of breaking ──────────────────────
if broken="$(configure_once "${work}/b4" "${cache}" nosuchdep \
        https://github.com/LASTRADA-Software/definitely-not-a-real-repo-552.git v1.0.0)"; then
    if ! grep -q "could not pre-clone" <<< "${broken}"; then
        fail "an unreachable repository did not report a failed pre-clone"
    fi
    if ! grep -q "selftest: not pointed anywhere" <<< "${broken}"; then
        fail "after a failed pre-clone FETCHCONTENT_SOURCE_DIR was set to a tree that was never populated"
    fi
else
    fail "an unreachable repository aborted the configure -- the cache must never be able to break a build"
fi

if [ "${failures}" -ne 0 ]; then
    echo "dep-cache self-test: ${failures} assertion(s) failed"
    exit 1
fi
echo "dep-cache self-test: inert when unconfigured, populates cold, reuses warm, degrades on failure."
