# ── A shared source cache for FetchContent dependencies ──────────────────────
#
# Every configure in CI clones `glaze`, `Catch2`, `Lightweight` and
# `doxygen-awesome-css` again from github.com. One run configures more than a
# dozen times, so a single push produces dozens of anonymous clones from the
# self-hosted fleet's shared egress address -- and GitHub answers a throttled
# anonymous clone with 401, which makes git fall back to prompting for
# credentials and, with no TTY, fail as:
#
#     fatal: could not read Username for 'https://github.com': No such device or address
#
# which reads like an auth misconfiguration and is not one. Measured on
# 2026-09-16 within a single run: six self-hosted clones succeeded between
# 20:12 and 20:14, then every job starting 20:19-20:22 failed this way -- the
# same five runners and the same egress address, so not an outage but a
# threshold.
#
# `FETCHCONTENT_SOURCE_DIR_<NAME>` makes FetchContent use an existing tree and
# skip the download entirely. Pointing every configure at one cache directory
# therefore turns "a clone per configure" into "a clone per runner, once",
# which is the volume that trips the limit.
#
# Deliberately *not* `FETCHCONTENT_FULLY_DISCONNECTED`: a cache miss must fall
# back to cloning rather than fail the build. The cache is an optimisation, and
# an optimisation that can break a build is a liability.

# Where cached sources live. An explicit `MORPH_DEP_CACHE` wins; otherwise CI
# gets a default under the runner's home, which persists across jobs on a
# self-hosted runner. A local build gets nothing unless it opts in -- a
# developer's builds are not what exhausts a rate limit, and silently sharing
# sources between their checkouts would be a surprising thing to do.
if(DEFINED ENV{MORPH_DEP_CACHE})
    set(MORPH_DEP_CACHE_DIR "$ENV{MORPH_DEP_CACHE}")
elseif(DEFINED ENV{CI} AND DEFINED ENV{HOME})
    set(MORPH_DEP_CACHE_DIR "$ENV{HOME}/.cache/morph-dep-cache")
else()
    set(MORPH_DEP_CACHE_DIR "")
endif()

# morph_declare_dep below calls FetchContent_Declare, so this file no longer
# works only beside an `include(FetchContent)` the caller remembered to write.
# include() is idempotent; every call site already does this too.
include(FetchContent)

# ── Declaring and caching, split ─────────────────────────────────────────────
#
# `morph_declare_dep` is what call sites use; `morph_cache_dep` below is the
# caching half and is called only by it (and directly by
# a gate removed on 2026-09-23, which asserts that half's four properties on their
# own before asserting that declaring survives all four).
#
# The split is not stylistic. `morph_cache_dep` has three early returns -- no
# cache directory configured, no git, an explicit FETCHCONTENT_SOURCE_DIR_<NAME>
# override -- and the first of them is the *common* configuration: a local build
# opts out of the cache by default. So the caching half returns early on most
# machines, and folding `FetchContent_Declare` into it would leave the
# dependency undeclared on exactly those machines. Declaring is therefore
# unconditional and every early return is confined to the caching half.
#
# Why this wrapper exists at all: before it, every dependency wrote its revision
# twice -- once as `morph_cache_dep`'s `tag`, once as the `GIT_TAG` of the
# `FetchContent_Declare` beside it -- and the two could disagree. The divergence
# would be asymmetric in the worst way: a warm cache serves the first, an
# uncached configure fetches the second, both successfully and with no
# diagnostic anywhere. Comparing the two copies in a gate is possible; having
# only one copy leaves nothing to disagree.
#
# `GIT_REPOSITORY` and `GIT_TAG` are therefore refused in ARGN rather than
# forwarded: passing either would re-create the second copy inside the one call
# that was supposed to end it.
#
# Everything else in ARGN is forwarded to `FetchContent_Declare` verbatim.
# Today that is only `GIT_SHALLOW` -- TRUE for glaze, Catch2 and
# doxygen-awesome-css (tags, which a shallow clone resolves), FALSE for both
# Lightweight sites (a commit SHA, which it does not) -- but forwarding the
# rest of the argument list rather than one named option means a site that
# needs `SOURCE_SUBDIR` or `PATCH_COMMAND` next does not have to widen this
# function to get it.
function(morph_declare_dep name repository tag)
    foreach(_argument IN LISTS ARGN)
        if(_argument STREQUAL "GIT_REPOSITORY" OR _argument STREQUAL "GIT_TAG")
            message(FATAL_ERROR
                "morph_declare_dep(${name} ...) was passed ${_argument} as an extra "
                "argument. The repository and the tag are this call's own second and "
                "third arguments, and stating either of them twice is the divergence "
                "this function exists to make unwritable: the cache keys "
                "on what it is handed, FetchContent fetches what it is handed, and a "
                "warm cache would then build a different revision than a cold one, "
                "both successfully.")
        endif()
    endforeach()

    morph_cache_dep("${name}" "${repository}" "${tag}")

    FetchContent_Declare(
        ${name}
        GIT_REPOSITORY ${repository}
        GIT_TAG        ${tag}
        ${ARGN}
    )
endfunction()

# Points FetchContent at a cached checkout of @p name, populating the cache on
# first use. A no-op when no cache directory is configured, or when the caller
# already set FETCHCONTENT_SOURCE_DIR_<NAME> explicitly.
#
# `tag` is part of the directory name, so bumping a pin lands in a fresh
# directory instead of silently reusing the old revision -- the failure mode a
# cache keyed on name alone would have, and the one that is hardest to notice
# because everything still builds.
function(morph_cache_dep name repository tag)
    if(MORPH_DEP_CACHE_DIR STREQUAL "")
        return()
    endif()
    find_package(Git QUIET)
    if(NOT Git_FOUND)
        return()  # FetchContent needs git too; let it produce the diagnostic
    endif()
    string(TOUPPER "${name}" _upper)
    if(DEFINED FETCHCONTENT_SOURCE_DIR_${_upper} AND NOT FETCHCONTENT_SOURCE_DIR_${_upper} STREQUAL "")
        return()  # an explicit override wins, including the one CI may pass
    endif()

    string(SUBSTRING "${tag}" 0 16 _short_tag)
    string(MAKE_C_IDENTIFIER "${name}-${_short_tag}" _slug)
    set(_dir "${MORPH_DEP_CACHE_DIR}/${_slug}")
    # An explicit sentinel, written only after the clone *and* the checkout
    # succeeded, rather than probing for a file the dependency might not have.
    # The first version of this used `CMakeLists.txt`, which is not present in
    # every dependency -- `doxygen-awesome-css` is a stylesheet repository --
    # so that entry would have been re-cloned on every configure while looking
    # exactly like a working cache. It also distinguishes a complete entry from
    # a tree left behind by an interrupted populate.
    set(_stamp "${_dir}/.morph-dep-cache-ok")

    if(NOT EXISTS "${_stamp}")
        message(STATUS "morph: dep cache: populating ${name} (${tag}) at ${_dir}")
        file(MAKE_DIRECTORY "${MORPH_DEP_CACHE_DIR}")
        # Clone into a per-process staging path and rename into place, so two
        # configures racing on the same runner cannot leave a half-written tree
        # that later builds would treat as a valid cache entry. The rename is
        # atomic within one filesystem; whichever loses the race just discards
        # its own copy.
        string(RANDOM LENGTH 12 _stage_id)
        set(_staging "${_dir}.tmp.${_stage_id}")
        file(REMOVE_RECURSE "${_staging}")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} clone --quiet "${repository}" "${_staging}"
            RESULT_VARIABLE _clone_result
            ERROR_VARIABLE _clone_error)
        if(NOT _clone_result EQUAL 0)
            # Not fatal: FetchContent will do its own clone, exactly as before.
            message(STATUS "morph: dep cache: could not pre-clone ${name} (${_clone_error}); "
                           "leaving it to FetchContent")
            file(REMOVE_RECURSE "${_staging}")
            return()
        endif()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} -C "${_staging}" checkout --quiet "${tag}"
            RESULT_VARIABLE _checkout_result)
        if(NOT _checkout_result EQUAL 0)
            message(STATUS "morph: dep cache: ${tag} did not check out for ${name}; leaving it to FetchContent")
            file(REMOVE_RECURSE "${_staging}")
            return()
        endif()
        file(TOUCH "${_staging}/.morph-dep-cache-ok")
        if(NOT EXISTS "${_stamp}")
            file(REMOVE_RECURSE "${_dir}")
            file(RENAME "${_staging}" "${_dir}" RESULT _rename_result)
        endif()
        file(REMOVE_RECURSE "${_staging}")
    endif()

    if(EXISTS "${_stamp}")
        set(FETCHCONTENT_SOURCE_DIR_${_upper} "${_dir}" CACHE PATH
            "Cached ${name} source tree" FORCE)
        message(STATUS "morph: dep cache: ${name} from ${_dir}")
    endif()
endfunction()
