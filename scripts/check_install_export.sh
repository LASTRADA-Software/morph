#!/usr/bin/env bash
# Usage: bash scripts/check_install_export.sh [--skip-header-set-verification] [REPO_ROOT]
#
# Fails unless a consumer project outside the tree can actually be built
# against `cmake --install`ed morph, via `find_package(morph CONFIG REQUIRED)`
# and `target_link_libraries(... morph::morph)`.
#
# Why this gate exists (morph#232): `cmake --install` on a morph build used to
# **exit 0** and install hundreds of Glaze headers plus a working
# `glazeConfig.cmake` -- Glaze carries its own install/export rules and gets
# them for free through `FetchContent` -- while installing zero morph headers
# and no `morphConfig.cmake`. Nothing failed. A consumer running the standard
# CMake workflow got a prefix silently holding someone else's dependency and
# none of the library they meant to install, and the only way to find out was
# to try to use it. That is this repository's recurring shape: a control that
# reports success while measuring nothing.
#
# So this check measures the thing itself. It installs to a scratch prefix and
# then *compiles and runs* a consumer, rather than grepping the tree for
# `install(` or the prefix for filenames. Compiling is the part that matters:
# three separate defects found while writing the install rules produced a
# prefix that `find_package` accepted --
#
#   1. installing only the declared `FILE_SET HEADERS` leaves out
#      `morph/detail/fixed_string.hpp` and `morph/detail/quantity_equation.hpp`,
#      which public headers include, so the install resolves in the build
#      directory and nowhere else;
#   2. `install(EXPORT ... NAMESPACE morph::)` prefixes the *target* name, so
#      `morph_net` arrives as `morph::morph_net` while every in-tree alias says
#      `morph::net`;
#   3. without `find_dependency(glaze)` in the package config, `morphTargets.cmake`
#      names an imported target nobody created.
#
# -- and each of the three is invisible to a check that stops at
# `find_package`. Every assertion below therefore has a consumer-visible
# consequence behind it.
#
# `--skip-header-set-verification` drops the one slow phase (compiling every
# header in morph's interface header sets standalone, ~44 translation units).
# It exists for scripts/test_check_install_export.sh, which needs the fast
# phases many times over and the slow one exactly once; CI runs the whole
# thing.
#
# POSIX host required: the check needs at least one optional component
# installed to have anything to say about exported target names, and
# `morph::net` is the one with no external dependencies. It is POSIX-only
# (`MORPH_BUILD_NET` warns and does nothing on Windows), and this script fails
# rather than passing vacuously if no component was installed.
set -euo pipefail

skip_header_set_verification=0
repo_root=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --skip-header-set-verification)
            skip_header_set_verification=1
            shift
            ;;
        -*)
            printf 'error: unknown option %s\n' "$1" >&2
            exit 2
            ;;
        *)
            repo_root="$1"
            shift
            ;;
    esac
done

if [ -z "$repo_root" ]; then
    repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
else
    repo_root="$(cd "$repo_root" && pwd)"
fi
readonly repo_root skip_header_set_verification

failures=0

note() { printf 'ok: %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

# A failing step's own output is the diagnostic -- "the consumer did not
# compile" without the compiler's reason is not actionable, and the self-test
# next door matches on those reasons to prove each check fires for the right
# cause.
run_step() {
    local description="$1"; shift
    local output
    if output="$("$@" 2>&1)"; then
        return 0
    fi
    fail "${description}"
    printf '%s\n' "$output" >&2
    return 1
}

workspace="$(mktemp -d)"
trap 'rm -rf "$workspace"' EXIT

readonly build_dir="${workspace}/build"
readonly prefix="${workspace}/prefix"
readonly consumer_dir="${workspace}/consumer"
readonly consumer_build="${workspace}/consumer-build"

# Bash 3.2 (still the system bash on macOS) treats "${array[@]}" on an empty
# array as an unbound variable under `set -u`, so both optional argument
# arrays are expanded through the `${arr[@]+...}` guard at their use sites.
generator_args=()
if command -v ninja >/dev/null 2>&1; then
    generator_args=(-G Ninja)
fi

# Share one FetchContent cache when the caller offers it. The self-test runs
# this script against half a dozen scratch trees, and re-cloning Glaze for
# each would dominate its runtime.
fetchcontent_args=()
if [ -n "${MORPH_CHECK_FETCHCONTENT_BASE_DIR:-}" ]; then
    mkdir -p "${MORPH_CHECK_FETCHCONTENT_BASE_DIR}"
    fetchcontent_args=(-DFETCHCONTENT_BASE_DIR="${MORPH_CHECK_FETCHCONTENT_BASE_DIR}")
fi

# ── 1. Configure and install the library ────────────────────────────────────
#
# MORPH_BUILD_NET is on so there is an optional component to check exported
# names against; MORPH_BUILD_OFFLINE_SQLITE is deliberately *off* so the
# "component that was not installed must not be found" case below is real.
if ! run_step "the library did not configure" \
    cmake -S "$repo_root" -B "$build_dir" ${generator_args[@]+"${generator_args[@]}"} \
        ${fetchcontent_args[@]+"${fetchcontent_args[@]}"} \
        -DCMAKE_BUILD_TYPE=Release \
        -DMORPH_BUILD_TESTS=OFF \
        -DMORPH_BUILD_EXAMPLES=OFF \
        -DMORPH_BUILD_NET=ON; then
    printf '\n%d install/export check(s) failed.\n' "$failures" >&2
    exit 1
fi
note "the library configures with install rules enabled"

# ── 2. Every header the install ships must compile standalone ───────────────
#
# Not part of `all`, so nothing else in CI builds it. It is also the only
# phase that can catch a regression in INTERFACE_HEADER_SETS_TO_VERIFY: the
# detail/ header set is installed but deliberately outside the verified set,
# because quantity_equation.hpp is included partway down quantity.hpp and is
# not self-contained. Widen the verified set back to "every interface header
# set" and this stops building.
if [ "$skip_header_set_verification" -eq 0 ]; then
    if run_step "morph's interface header sets do not all compile standalone" \
        cmake --build "$build_dir" --target morph_verify_interface_header_sets; then
        note "every verified interface header compiles standalone"
    fi
fi

# `cmake --install` exiting 0 is exactly what it did before morph#232, so its
# exit status is worth nothing on its own. It is checked anyway -- a *failing*
# install is still a failure -- and then the prefix is inspected.
run_step "cmake --install failed outright" \
    cmake --install "$build_dir" --prefix "$prefix" || true

# ── 3. The prefix must contain morph, not just something ────────────────────
for required in \
    "lib/cmake/morph/morphConfig.cmake" \
    "lib/cmake/morph/morphConfigVersion.cmake" \
    "lib/cmake/morph/morphTargets.cmake" \
    "include/morph/util/rational.hpp" \
    "include/morph/core/bridge.hpp"; do
    if [ ! -f "${prefix}/${required}" ]; then
        fail "cmake --install exited 0 but installed no ${required} -- this is the morph#232 shape"
    fi
done

readonly targets_file="${prefix}/lib/cmake/morph/morphTargets.cmake"
if [ -f "$targets_file" ]; then
    # NAMESPACE prefixes the target name, not the alias: without EXPORT_NAME,
    # morph_net is exported as morph::morph_net while every in-tree alias, the
    # README and the consumer below all say morph::net.
    if grep -q '^add_library(morph::morph_' "$targets_file"; then
        fail "an optional target is exported as '$(grep -o 'morph::morph_[a-z_]*' "$targets_file" | head -1)' rather than under its alias name -- install(EXPORT NAMESPACE morph::) prefixes the target name, so each optional target needs EXPORT_NAME"
    fi

    # Vacuity guard. With no optional component installed, the export-name
    # check above compared nothing and would pass on any regression.
    exported_components="$(grep -c '^add_library(morph::' "$targets_file" || true)"
    if [ "${exported_components:-0}" -lt 2 ]; then
        fail "no optional component was installed, so nothing here checked exported target names; this check needs a POSIX host where MORPH_BUILD_NET applies"
    fi
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%d install/export check(s) failed.\n' "$failures" >&2
    exit 1
fi
note "the prefix contains morph's headers and package config"

# ── 4. Build a consumer against the prefix ──────────────────────────────────
#
# The TU includes headers that reach the parts of the install a filename check
# cannot see: forms.hpp and quantity.hpp both include morph/detail/, and
# rational.hpp reaches Glaze.
mkdir -p "$consumer_dir"
cat > "${consumer_dir}/CMakeLists.txt" <<'CONSUMER_CMAKE'
cmake_minimum_required(VERSION 3.25)
project(morph_install_check_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(morph CONFIG REQUIRED COMPONENTS net)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE morph::morph morph::net)
CONSUMER_CMAKE

cat > "${consumer_dir}/main.cpp" <<'CONSUMER_MAIN'
// Compiled entirely outside the repository, against the install prefix alone.
#include <morph/core/bridge.hpp>
#include <morph/forms/forms.hpp>      // includes morph/detail/fixed_string.hpp
#include <morph/net/socket_backend.hpp>
#include <morph/util/quantity.hpp>    // includes morph/detail/quantity_equation.hpp
#include <morph/util/rational.hpp>    // reaches Glaze
#include <morph/version.hpp>

#include <cstdio>

int main() {
    const morph::math::Rational half{morph::math::Numerator{1}, morph::math::Denominator{2},
                                     morph::math::DecimalPlaces{2}};
    if (half.numerator != 1 || half.denominator != 2) {
        return 1;
    }
    std::printf("morph %d.%d.%d consumed from the install prefix\n", MORPH_VERSION_MAJOR,
                MORPH_VERSION_MINOR, MORPH_VERSION_PATCH);
    return 0;
}
CONSUMER_MAIN

if run_step "the consumer project could not configure against the install prefix" \
    cmake -S "$consumer_dir" -B "$consumer_build" ${generator_args[@]+"${generator_args[@]}"} \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$prefix"; then
    note "find_package(morph CONFIG REQUIRED COMPONENTS net) resolves"

    if run_step "the consumer project did not compile against the install prefix" \
        cmake --build "$consumer_build"; then
        note "a consumer TU including morph headers compiles and links"

        if run_step "the consumer binary did not run" "${consumer_build}/consumer"; then
            note "the consumer binary runs"
        fi
    fi
fi

# ── 5. A component that was not installed must not be found ─────────────────
#
# Otherwise the component gating is decorative: find_package would report
# success and the failure would surface as a missing target much later.
readonly missing_dir="${workspace}/consumer-missing"
readonly missing_build="${workspace}/consumer-missing-build"
mkdir -p "$missing_dir"
cat > "${missing_dir}/CMakeLists.txt" <<'MISSING_CMAKE'
cmake_minimum_required(VERSION 3.25)
project(morph_install_check_missing LANGUAGES CXX)
find_package(morph CONFIG REQUIRED COMPONENTS offline_sqlite)
MISSING_CMAKE

if cmake -S "$missing_dir" -B "$missing_build" ${generator_args[@]+"${generator_args[@]}"} \
        -DCMAKE_PREFIX_PATH="$prefix" >/dev/null 2>&1; then
    fail "find_package(morph COMPONENTS offline_sqlite REQUIRED) succeeded against an install built without MORPH_BUILD_OFFLINE_SQLITE"
else
    note "a component that was not installed fails at find_package time"
fi

# ── Verdict ─────────────────────────────────────────────────────────────────
if [ "$failures" -ne 0 ]; then
    printf '\n%d install/export check(s) failed.\n' "$failures" >&2
    exit 1
fi

printf '\nmorph installs, exports, and can be consumed via find_package(morph CONFIG).\n'
