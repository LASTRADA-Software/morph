# SPDX-License-Identifier: Apache-2.0
#
# morph_demote_interface_includes_to_system(<target>)
#   Moves every `-I<dir>` out of <target>'s INTERFACE_COMPILE_OPTIONS and
#   re-adds the directories through target_include_directories(SYSTEM
#   INTERFACE), leaving every other option in place and in order.
#
# morph_demote_lightweight_odbc_includes()
#   The one caller that matters, applied to the fetched `Lightweight` target.
#
# ── Why this exists (morph#438) ──────────────────────────────────────────────
#
# The pinned Lightweight (bbb972a78e1962b968a2c6ad93f7dade736eaa01) resolves
# unixODBC with `pkg_check_modules(ODBC REQUIRED odbc)` and then propagates the
# result as a PUBLIC *compile option*:
#
#     target_compile_options(Lightweight PUBLIC ${ODBC_CFLAGS})
#
# so the ODBC include path lands in INTERFACE_COMPILE_OPTIONS rather than in
# INTERFACE_INCLUDE_DIRECTORIES. A plain `-I` header is coverage-instrumented;
# an `-isystem` one is not. On macOS with Homebrew's unixodbc, `odbc.pc` sets
# includedir to the versioned Cellar path (not the /opt/homebrew/include
# symlink), which is not on Xcode clang's implicit system search list, so
# `pkg-config --cflags odbc` emits a real `-I` -- and sql.h, sqlext.h and
# sqlucode.h are compiled with real coverage instrumentation into every
# coverage-instrumented morph target that links Lightweight::Lightweight
# PUBLIC. scripts/check_coverage_roots.sh then fails, correctly: three files in
# the mapping resolve outside the checkout.
#
# morph's three existing SYSTEM demotions (cmake/morph_add_rung.cmake for
# ladder_<rung>_gui_lib and ladder_<rung>_tests, examples/common/CMakeLists.txt
# for ladder_common_tests) all read INTERFACE_INCLUDE_DIRECTORIES and so cannot
# reach this flag; and ladder_<rung>_lib, the target that actually compiles the
# instrumented model code, has no demotion at all. Fixing it per-consumer would
# mean four more copies of the same block, so it is fixed once, on the
# dependency, at the point the target becomes available.
#
# ── The Linux blind spot, stated rather than discovered ──────────────────────
#
# On Linux, `pkg-config --cflags odbc` is empty: the same odbc.pc asks for the
# same `-I${includedir}`, but pkg-config strips it because /usr/include is on
# its system list. So on Linux CI, Lightweight's INTERFACE_COMPILE_OPTIONS
# contains no `-I` at all and this helper is a no-op -- which is precisely why
# the defect survived, and why "assert no -I remains in the interface" would be
# a check that passes identically whether or not this function has a body.
#
# The real check is therefore a synthetic fixture:
# tests/compile_checks/demote_interface_includes_selftest.cmake hands this
# function a hand-written option list covering every case and FATAL_ERRORs
# unless the resulting properties are exactly right. It runs on every configure
# that reaches tests/CMakeLists.txt, Linux included, and it fails if this
# function's body is removed.
#
# ── The long-term home ───────────────────────────────────────────────────────
#
# Upstream: `target_include_directories(Lightweight SYSTEM PUBLIC
# ${ODBC_INCLUDE_DIRS})` in place of the target_compile_options() call. That
# needs a pin bump to reach this repository, and this helper is harmless once
# it lands -- a target with no `-I` in its compile options is a no-op for it.

include_guard(GLOBAL)

# Moves `-I` include paths out of a target's INTERFACE_COMPILE_OPTIONS and
# re-adds them as SYSTEM interface include directories.
#
# Both spellings pkg_check_modules can produce are handled: the joined
# `-I/path` (one list element) and the separated `-I;/path` (two). Every
# non-`-I` option survives unchanged, in order -- including options that are
# merely spelled similarly, such as `-include`, which is a different flag and
# whose own argument must not be mistaken for an include directory.
#
# Idempotent by construction: a second call finds no `-I` left and returns
# without touching anything. Both FetchContent_MakeAvailable(Lightweight) sites
# call it, since whichever configures first is the one that defines the target.
function(morph_demote_interface_includes_to_system target)
    if(NOT TARGET "${target}")
        return()
    endif()

    # target_include_directories() cannot be called on an alias, so resolve one
    # to the real target. Reading through an alias is fine -- that is what the
    # three existing demotion sites do -- but writing is not.
    get_target_property(_morph_aliased "${target}" ALIASED_TARGET)
    if(_morph_aliased)
        set(target "${_morph_aliased}")
    endif()

    get_target_property(_morph_opts "${target}" INTERFACE_COMPILE_OPTIONS)
    if(NOT _morph_opts)
        return()
    endif()

    set(_morph_kept "")
    set(_morph_dirs "")
    set(_morph_want_dir OFF)
    foreach(_morph_opt IN LISTS _morph_opts)
        if(_morph_want_dir)
            # The element after a bare `-I` is its directory, whatever it looks
            # like -- a path beginning with a dash would still be one.
            list(APPEND _morph_dirs "${_morph_opt}")
            set(_morph_want_dir OFF)
        elseif(_morph_opt STREQUAL "-I")
            set(_morph_want_dir ON)
        elseif(_morph_opt MATCHES "^-I.")
            # MATCHES is case-sensitive, so `-include` (lowercase i) does not
            # reach here; the `.` requires at least one character of directory,
            # so a bare `-I` is handled by the branch above rather than
            # producing an empty include directory.
            string(SUBSTRING "${_morph_opt}" 2 -1 _morph_dir)
            list(APPEND _morph_dirs "${_morph_dir}")
        else()
            list(APPEND _morph_kept "${_morph_opt}")
        endif()
    endforeach()

    # A trailing bare `-I` with nothing after it names no directory. Put it
    # back untouched rather than inventing one or dropping the flag.
    if(_morph_want_dir)
        list(APPEND _morph_kept "-I")
    endif()

    if(NOT _morph_dirs)
        return()
    endif()

    if(_morph_kept)
        set_property(TARGET "${target}"
            PROPERTY INTERFACE_COMPILE_OPTIONS "${_morph_kept}")
    else()
        # No value at all, rather than one empty string element.
        set_property(TARGET "${target}" PROPERTY INTERFACE_COMPILE_OPTIONS)
    endif()
    target_include_directories("${target}" SYSTEM INTERFACE ${_morph_dirs})
endfunction()

# Call immediately after FetchContent_MakeAvailable(Lightweight), from every
# site that makes it available -- see this file's header for why.
function(morph_demote_lightweight_odbc_includes)
    morph_demote_interface_includes_to_system(Lightweight)
endfunction()
