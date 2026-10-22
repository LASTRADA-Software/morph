# SPDX-License-Identifier: Apache-2.0
#
# Compiler-cache launcher selection.
#
# Three launchers are supported, in preference order:
#
#   1. pace-cc — the launcher from the Pace project. Its entries are
#      portable across checkout paths, so a CI runner and a developer working
#      from different directories share cache hits. It must already be installed
#      and on PATH. It is configured purely through the environment and caches
#      nothing unless PACE_ADDR / PACE_SOURCE_DIR / PACE_BINARY_DIR
#      are all set; the address defaults to pace-kv's own port,
#      127.0.0.1:6674, and the two roots are injected here via `cmake -E env`,
#      because CMake already knows them. Selecting it is conditional on a daemon
#      actually answering there — see the probe below.
#   2. sccache — the usual third-party launcher, used when pace-cc is
#      unavailable or unconfigured. Supports shared (Redis/S3/...) caches.
#      **Under MSVC or clang-cl, selecting it emits a warning**, and the warning
#      is the point: there sccache replays a hit's /showIncludes stream with the
#      absolute paths of the build that stored it, so two checkouts sharing one
#      cache stop rebuilding on a header change while the build stays green. GCC
#      and Clang are unaffected. It is a caveat rather than a `check` that skips
#      the row, because nothing here can tell whether the cache is shared and the
#      choice belongs to whoever is building. See the row below.
#   3. ccache — the classic local cache, used when neither of the above applies.
#
# Launchers are wired in as compiler launchers, so CPM-/FetchContent-fetched
# dependencies get cached too. If a launcher is already set (e.g. via the
# command line or a preset), it is left untouched.
#
# This file is self-contained on purpose — it uses nothing but stock CMake and
# names no target of the project it sits in — so it can be dropped into another
# repository verbatim. Two things follow from that and must be preserved: it
# cannot use CPM or FetchContent (a consuming project may have neither, and here
# it is included before CPM is even bootstrapped), and it must never fail a
# configure, since a build that has no cache should still be a build.
#
# When none of the three is installed, -DPACE_AUTO_INSTALL=ON fetches
# pace-cc from GitHub Releases rather than leaving the build uncached; see
# the block below. That alone still leaves a genuinely clean machine uncached,
# because a launcher with no daemon to talk to caches nothing —
# -DPACE_AUTO_START=ON additionally stages and starts a pace-kv daemon
# in the background when none answers at PACE_ADDR; off by
# default, and independently of auto-install, since starting a background
# process is a bigger side effect than downloading a file and CI relies on no
# daemon answering by default. To disable everything: -DUSE_COMPILER_CACHE=OFF.

option(USE_COMPILER_CACHE
       "Use a compiler-cache launcher when one is available (pace-cc when a daemon answers, else sccache, else ccache) [default: ON]"
       ON)

# Respect a launcher provided externally (command line, preset, toolchain).
# Check both C and CXX: a toolchain may set only one of them, and we must not
# override either (nor silently set the other alongside it).
if(DEFINED CMAKE_CXX_COMPILER_LAUNCHER OR DEFINED CMAKE_C_COMPILER_LAUNCHER)
    message(STATUS "[cache] Compiler launcher already set externally "
                   "(C='${CMAKE_C_COMPILER_LAUNCHER}', CXX='${CMAKE_CXX_COMPILER_LAUNCHER}'); leaving it untouched.")
    # A build tree configured before this module existed carries the launcher of
    # the day in its cache, and would keep it forever without a word about why
    # the selection below never runs.
    #
    # -U before --fresh, because the guard above reads exactly two cache entries
    # and -U deletes exactly those two: the reconfigure then falls through to
    # the selection below without discarding the build directory. --fresh
    # reaches the same place for a -D or an older configure, but it deletes
    # CMakeFiles/ and, unless the generator is re-passed, drops CMAKE_GENERATOR
    # back to the platform default on the way -- a `cmake -S . -B build --fresh`
    # on a tree configured `-G Ninja` comes back a Makefile tree with a stale
    # build.ninja still sitting in it.
    #
    # What -U saves is the configure state -- the populated _deps tree, the
    # generator, generated sources, cached find_ results -- and not, note, the
    # compiling: a launcher is part of every C/C++ compile command, so the next
    # build recompiles those TUs whichever route got here. That is the honest
    # size of it.
    #
    # The case that makes the ordering matter rather than merely cheaper is the
    # "a preset" one this very message names, where --fresh does not work at
    # all: the preset re-applies its cacheVariables on the reconfigure that
    # --fresh triggers, so the launcher comes back pinned and the tree is gone
    # for nothing. -U clears it there too, until the preset is next run.
    # All of the above measured on an isolated harness.
    if(DEFINED CACHE{CMAKE_CXX_COMPILER_LAUNCHER} OR DEFINED CACHE{CMAKE_C_COMPILER_LAUNCHER})
        message(STATUS "[cache] That value comes from the CMake cache (a -D, a preset, or an older configure); "
                       "to let this module choose instead, reconfigure the same build directory with "
                       "-UCMAKE_C_COMPILER_LAUNCHER -UCMAKE_CXX_COMPILER_LAUNCHER, which clears exactly those "
                       "two entries and leaves the build directory standing. (--fresh clears them too when they "
                       "came from a -D or an older configure, at the price of the whole build directory; when "
                       "they come from a preset it clears nothing, because the preset re-applies them on the "
                       "same reconfigure.)")
    endif()
    return()
endif()

find_program(PACE_CC pace-cc DOC "pace-cc tool path; needs a pace-kv daemon to be used")
find_program(SCCACHE sccache DOC "sccache tool path")
find_program(CCACHE ccache DOC "ccache tool path")

# find_program never revisits a cache entry it already filled, which is right for
# a PATH lookup and wrong for the auto-installed launcher below: that one lives in
# a per-user cache directory, and a cache cleaner emptying it would otherwise leave
# this pointing at a binary that is no longer there for the life of the build tree.
# Forget a path that has stopped existing so the lookup — and the fetch — can run
# again.
if(PACE_CC AND NOT EXISTS "${PACE_CC}")
    unset(PACE_CC CACHE)
    find_program(PACE_CC pace-cc DOC "pace-cc tool path; needs a pace-kv daemon to be used")
endif()

# Where the cache is: PACE_ADDR from the environment, else localhost's own
# port. That default reaches whichever of the two serves it -- a stock `pace-kv`
# (and the service the installers register) listens there, and so does a
# `pace-node`, whose --listen-cache defaults to the same address for
# exactly this reason. An empty -DPACE_ADDR= opts out of pace-cc entirely.
set(_pace_addr_env "$ENV{PACE_ADDR}")
if(_pace_addr_env STREQUAL "")
    set(_pace_addr_wanted "127.0.0.1:6674")
else()
    set(_pace_addr_wanted "${_pace_addr_env}")
endif()

# Ordinary cache semantics would freeze the address at whatever the first
# configure saw, so exporting PACE_ADDR to reach a remote daemon would do
# nothing until the build tree was wiped. Track the environment across
# configures instead and let a *change* to it retarget the cache entry — while
# leaving a -D from this very run alone, which is the one instruction more
# deliberate than the environment. The two are told apart by whether the cache
# still holds what this module last put there, which is also why the retarget
# needs a previous configure to compare against: on a first configure there is
# no bookkeeping yet, both tests hold vacuously, and a -DPACE_ADDR= meant
# to opt out would be overwritten by an address merely left in the environment.
if(NOT DEFINED CACHE{PACE_ADDR})
    set(PACE_ADDR "${_pace_addr_wanted}" CACHE STRING
        "host:port of the pace-kv compile-cache daemon, 127.0.0.1:6674 by default (empty disables the pace-cc launcher)")
elseif(DEFINED CACHE{_PACE_ADDR_APPLIED}
       AND NOT _pace_addr_env STREQUAL "${_PACE_ADDR_ENV_SEEN}"
       AND PACE_ADDR STREQUAL "${_PACE_ADDR_APPLIED}")
    message(STATUS "[cache] PACE_ADDR changed in the environment; retargeting to ${_pace_addr_wanted}")
    set(PACE_ADDR "${_pace_addr_wanted}" CACHE STRING
        "host:port of the pace-kv compile-cache daemon, 127.0.0.1:6674 by default (empty disables the pace-cc launcher)"
        FORCE)
endif()
set(_PACE_ADDR_ENV_SEEN "${_pace_addr_env}" CACHE INTERNAL
    "PACE_ADDR as the environment last presented it, to notice a change on reconfigure")
set(_PACE_ADDR_APPLIED "${PACE_ADDR}" CACHE INTERNAL
    "the address this module last applied, to tell its own value from one set externally")

# How pace-cc is configured, in one place: the probe below must test the
# very environment the build will use, or it would vouch for a configuration
# nothing else runs.
set(_pace_launcher_env
    "PACE_ADDR=${PACE_ADDR}"
    "PACE_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
    "PACE_BINARY_DIR=${CMAKE_BINARY_DIR}")

# Optional auto-install of pace-cc from this project's GitHub Releases.
#
# Vendored into another repository, everything above buys that repository
# nothing unless pace-cc is already on its PATH — a manual, out-of-band
# step on every new checkout and every fresh machine. With
# PACE_AUTO_INSTALL=ON this fetches the launcher itself: a prebuilt binary
# from the latest stable release, picked by host OS and architecture.
#
# Prebuilt rather than built from source, because building it would need a
# compiler, a build tree and several minutes at configure time — inside the very
# module whose job is to make compiling faster.
#
# Opt-in, because reaching out to the network during `cmake` is a meaningful
# change from "use what is already installed", and because it is what makes this
# module write outside the build tree for the first time. The binary is staged
# in a per-user directory shared by every repository and every build tree, so it
# is fetched once per machine and version rather than once per build.
#
# Nothing here can fail a configure. Every error path — no binary published for
# this platform, no network, a corrupt download, a binary that will not run —
# reports one status line and leaves PACE_CC empty, so selection falls
# through to sccache, then ccache, then no caching, exactly as it does today.
#
# That last property is why the download is deliberately *not* checked with
# `file(DOWNLOAD ... EXPECTED_HASH)`: a mismatch there is fatal even when STATUS
# is captured (measured), which is the one thing this must never be. The hash is
# compared by hand below instead — the same guarantee, minus the abort.

option(PACE_AUTO_INSTALL
       "Download pace-cc from GitHub Releases when no compiler-cache launcher is installed [default: OFF]"
       OFF)

set(PACE_AUTO_INSTALL_REPO "LASTRADA-Software/pace" CACHE STRING
    "owner/name of the GitHub repository to fetch prebuilt pace-cc binaries from")
set(PACE_AUTO_INSTALL_API "https://api.github.com" CACHE STRING
    "base URL of the GitHub API used to resolve the latest release")
set(PACE_AUTO_INSTALL_DOWNLOAD_BASE "https://github.com" CACHE STRING
    "base URL release archives are downloaded from (point at a mirror to install without reaching GitHub)")
set(PACE_AUTO_INSTALL_VERSION "" CACHE STRING
    "exact pace-cc version to install (empty resolves the latest stable release)")
set(PACE_AUTO_INSTALL_TTL_HOURS "24" CACHE STRING
    "how long a resolved latest release is reused before the GitHub API is asked again")

# Auto-installing the launcher alone leaves a genuinely clean machine exactly
# where it started: pace-cc caches nothing unless a pace-kv daemon
# answers at PACE_ADDR, and nothing installs one. PACE_AUTO_START is
# the separate opt-in that does — separate because starting a long-lived
# background process from a `cmake` configure is a materially bigger side
# effect than downloading a file, and a user who wants one but not the other
# must be able to say so.
#
# Defaulting OFF is what keeps CI's existing, relied-upon behaviour intact:
# several downstream projects run CI with no pace-kv daemon reachable on
# purpose, so a build transparently falls through to sccache. A default of ON
# would remove that boundary for everyone who has not opted out, rather than
# adding it only for those who opt in. No CI-environment sniffing backs this
# up — an explicit -DPACE_AUTO_START=ON is trusted at face value, the
# same way every other flag in this module is; guessing at "is this CI" from
# environment variables would just be a second, less legible way to be wrong.
option(PACE_AUTO_START
       "Start a fetched pace-kv daemon in the background when none answers at PACE_ADDR [default: OFF]"
       OFF)

# Where a fetched launcher is kept. Per user rather than per build tree, so the
# second build tree on a machine costs nothing, and per version, so a new release
# never overwrites a binary a configured tree still points at.
#
# The platform split mirrors the launcher's own choice of state directory
# (src/apps/pace-cc/Stats.cpp) — with the *cache* spellings rather than the
# state ones, since a re-downloadable binary is by definition cache and belongs
# where a cache cleaner may take it.
if(NOT DEFINED PACE_AUTO_INSTALL_DIR)
    if(DEFINED ENV{PACE_CACHE_DIR} AND NOT "$ENV{PACE_CACHE_DIR}" STREQUAL "")
        set(_pace_auto_dir_default "$ENV{PACE_CACHE_DIR}")
    elseif(CMAKE_HOST_WIN32 AND NOT "$ENV{LOCALAPPDATA}" STREQUAL "")
        set(_pace_auto_dir_default "$ENV{LOCALAPPDATA}/pace-cc")
    elseif(CMAKE_HOST_APPLE AND NOT "$ENV{HOME}" STREQUAL "")
        set(_pace_auto_dir_default "$ENV{HOME}/Library/Caches/pace-cc")
    elseif(NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
        set(_pace_auto_dir_default "$ENV{XDG_CACHE_HOME}/pace-cc")
    elseif(NOT "$ENV{HOME}" STREQUAL "")
        set(_pace_auto_dir_default "$ENV{HOME}/.cache/pace-cc")
    else()
        # No home to speak of (a bare container, a service account). Fall back to
        # the build tree: it still works, it just cannot be shared.
        set(_pace_auto_dir_default "${CMAKE_BINARY_DIR}/CMakeFiles/pace-cc")
    endif()
    get_filename_component(_pace_auto_dir_default "${_pace_auto_dir_default}" ABSOLUTE)
    set(PACE_AUTO_INSTALL_DIR "${_pace_auto_dir_default}" CACHE PATH
        "directory prebuilt pace-cc binaries are staged in, shared across repositories and build trees")
endif()

# Where an auto-started daemon keeps its cache. Persistent rather than
# in-memory, so the cache survives the daemon being restarted (a reboot, a
# manual kill) rather than starting cold every time — a subdirectory of the
# same per-user tree the staged binaries live in (PACE_AUTO_INSTALL_DIR),
# since both are per-machine, re-creatable state that a cache cleaner may take.
if(NOT DEFINED PACE_AUTO_START_STORAGE_DIR)
    set(PACE_AUTO_START_STORAGE_DIR "${PACE_AUTO_INSTALL_DIR}/daemon-storage" CACHE PATH
        "storage directory for a daemon PACE_AUTO_START starts, so its cache survives a restart")
endif()

# Published-asset table, one row per (host system, host architecture) this
# project releases a binary for. A host with no row is not an error — it simply
# falls back, which is what every platform does today. Publishing a new platform
# is a new row here and nothing below changes.
#
#   _pace_asset_<id>_system        CMAKE_HOST_SYSTEM_NAME this row serves
#   _pace_asset_<id>_arch          host processor spellings mapping to this row
#   _pace_asset_<id>_platform      asset-name infix, after "pace-<version>-"
#   _pace_asset_<id>_ext           archive extension
#   _pace_asset_<id>_member        path to the launcher inside the archive's
#                                top-level directory, which differs per
#                                platform because the packages install to
#                                different prefixes
#   _pace_asset_<id>_exe           staged launcher's filename
#   _pace_asset_<id>_daemon_member path to the daemon inside the same archive,
#                                staged only when PACE_AUTO_START asks
#                                for it — see _pace_auto_start_pace_kv below
#   _pace_asset_<id>_daemon_exe    staged daemon's filename
set(_pace_asset_rows linux_x86_64 darwin_arm64 windows_amd64)

set(_pace_asset_linux_x86_64_system "Linux")
set(_pace_asset_linux_x86_64_arch x86_64 amd64 AMD64)
set(_pace_asset_linux_x86_64_platform "Linux-x86_64")
set(_pace_asset_linux_x86_64_ext "tar.gz")
set(_pace_asset_linux_x86_64_member "usr/bin/pace-cc")
set(_pace_asset_linux_x86_64_exe "pace-cc")
set(_pace_asset_linux_x86_64_daemon_member "usr/bin/pace-kv")
set(_pace_asset_linux_x86_64_daemon_exe "pace-kv")

set(_pace_asset_darwin_arm64_system "Darwin")
set(_pace_asset_darwin_arm64_arch arm64 aarch64)
set(_pace_asset_darwin_arm64_platform "Darwin-arm64")
set(_pace_asset_darwin_arm64_ext "tar.gz")
set(_pace_asset_darwin_arm64_member "opt/pace/bin/pace-cc")
set(_pace_asset_darwin_arm64_exe "pace-cc")
set(_pace_asset_darwin_arm64_daemon_member "opt/pace/bin/pace-kv")
set(_pace_asset_darwin_arm64_daemon_exe "pace-kv")

# The Windows archive is a plain ZIP rather than the MSI beside it: an installer
# is not something this can open, and the launcher is one self-contained file
# with no VC++ redistributable behind it. Its interior differs from the two
# above because the Windows package is the only one not rooted at /, so its
# binaries sit at the conventional bin/ rather than usr/bin or opt/pace.
set(_pace_asset_windows_amd64_system "Windows")
set(_pace_asset_windows_amd64_arch AMD64 x86_64 x64)
set(_pace_asset_windows_amd64_platform "Windows-AMD64")
set(_pace_asset_windows_amd64_ext "zip")
set(_pace_asset_windows_amd64_member "bin/pace-cc.exe")
set(_pace_asset_windows_amd64_exe "pace-cc.exe")
set(_pace_asset_windows_amd64_daemon_member "bin/pace-kv.exe")
set(_pace_asset_windows_amd64_daemon_exe "pace-kv.exe")

# Pick the row serving this host.
# @param outVar Receives the row id, or empty when no binary is published for it.
function(_pace_auto_install_select_row outVar)
    set(${outVar} "" PARENT_SCOPE)
    foreach(_id IN LISTS _pace_asset_rows)
        if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "${_pace_asset_${_id}_system}")
            continue()
        endif()
        # Architecture spellings vary by OS and by how CMake was told about the
        # host, so a row lists every name that means it rather than one.
        foreach(_arch IN LISTS _pace_asset_${_id}_arch)
            if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "${_arch}")
                set(${outVar} "${_id}" PARENT_SCOPE)
                return()
            endif()
        endforeach()
    endforeach()
endfunction()

# Fetch (or reuse) the GitHub release metadata describing the latest stable
# release. GitHub's "latest" already excludes drafts and prereleases, so it is
# the stable release by definition.
#
# The response is cached on disk with a time-to-live for two reasons that both
# bite in CI: unauthenticated API access is limited to 60 requests per hour per
# IP, which a fleet of runners behind one egress address exhausts quickly, and a
# machine that has resolved once should keep working when the network is gone.
# A token in the environment raises that limit and is used when present.
#
# @param outVar Receives the release JSON.
# @param reasonVar Receives a short diagnostic when outVar is empty.
function(_pace_auto_install_release_json outVar reasonVar)
    set(${outVar} "" PARENT_SCOPE)

    # One cache file per repository: the staging directory is shared, and two
    # projects pointing at different forks must not read each other's answer.
    string(REPLACE "/" "_" _repoSlug "${PACE_AUTO_INSTALL_REPO}")
    set(_cacheFile "${PACE_AUTO_INSTALL_DIR}/${_repoSlug}-latest.json")

    if(EXISTS "${_cacheFile}")
        file(TIMESTAMP "${_cacheFile}" _stamp "%s")
        string(TIMESTAMP _now "%s" UTC)
        if(_stamp AND _now)
            math(EXPR _ageHours "(${_now} - ${_stamp}) / 3600")
            if(_ageHours LESS ${PACE_AUTO_INSTALL_TTL_HOURS})
                file(READ "${_cacheFile}" _cached)
                set(${outVar} "${_cached}" PARENT_SCOPE)
                set(${reasonVar} "" PARENT_SCOPE)
                return()
            endif()
        endif()
    endif()

    set(_headers HTTPHEADER "Accept: application/vnd.github+json")
    # Either spelling, because gh and actions/checkout each export their own.
    foreach(_tokenVar GITHUB_TOKEN GH_TOKEN)
        if(NOT "$ENV{${_tokenVar}}" STREQUAL "")
            list(APPEND _headers HTTPHEADER "Authorization: Bearer $ENV{${_tokenVar}}")
            break()
        endif()
    endforeach()

    set(_tmp "${CMAKE_BINARY_DIR}/CMakeFiles/pace-download/latest.json")
    file(DOWNLOAD
        "${PACE_AUTO_INSTALL_API}/repos/${PACE_AUTO_INSTALL_REPO}/releases/latest"
        "${_tmp}"
        ${_headers}
        STATUS _status
        TIMEOUT 30
        INACTIVITY_TIMEOUT 15)
    list(GET _status 0 _code)

    if(NOT _code EQUAL 0)
        list(GET _status 1 _message)
        # A stale answer beats no answer: the release we last saw is still a real
        # release, and a machine that is merely offline should keep building.
        if(EXISTS "${_cacheFile}")
            file(READ "${_cacheFile}" _cached)
            message(STATUS "[cache] GitHub API unreachable (${_message}); reusing the last known release")
            set(${outVar} "${_cached}" PARENT_SCOPE)
            set(${reasonVar} "" PARENT_SCOPE)
        else()
            set(${reasonVar} "cannot reach the GitHub API (${_message})" PARENT_SCOPE)
        endif()
        return()
    endif()

    file(READ "${_tmp}" _json)
    file(WRITE "${_cacheFile}" "${_json}")
    set(${outVar} "${_json}" PARENT_SCOPE)
    set(${reasonVar} "" PARENT_SCOPE)
endfunction()

# Resolve which release row, version and (when not pinned) release-metadata
# JSON an auto-install should use — the part of _pace_auto_install_pace_cc
# that has nothing to do with the launcher specifically. Split out so
# _pace_auto_start_pace_kv (below) resolves to the *same* release the launcher
# did rather than asking the GitHub API a second time and risking a different
# "latest" answering the two calls of one configure.
#
# @param rowVar Set to the asset-table row id for this host.
# @param versionVar Set to the resolved version, a bare numeric triple.
# @param jsonVar Set to the release metadata, or empty when the version was
#                pinned.
# @param reasonVar Set to a short diagnostic when resolution fails.
function(_pace_auto_install_resolve_release rowVar versionVar jsonVar reasonVar)
    set(${rowVar} "" PARENT_SCOPE)
    set(${versionVar} "" PARENT_SCOPE)
    set(${jsonVar} "" PARENT_SCOPE)

    _pace_auto_install_select_row(_row)
    if(NOT _row)
        set(${reasonVar}
            "no prebuilt binary is published for ${CMAKE_HOST_SYSTEM_NAME}-${CMAKE_HOST_SYSTEM_PROCESSOR}"
            PARENT_SCOPE)
        return()
    endif()

    # A pinned version is an answer in itself and costs no API call, which is
    # also what makes an auto-installing build reproducible.
    set(_json "")
    if(PACE_AUTO_INSTALL_VERSION)
        set(_version "${PACE_AUTO_INSTALL_VERSION}")
    else()
        _pace_auto_install_release_json(_json _why)
        if(NOT _json)
            set(${reasonVar} "${_why}" PARENT_SCOPE)
            return()
        endif()
        string(JSON _tag ERROR_VARIABLE _jsonErr GET "${_json}" tag_name)
        if(_jsonErr OR NOT _tag)
            set(${reasonVar} "the GitHub API answered without a release tag" PARENT_SCOPE)
            return()
        endif()
        set(_version "${_tag}")
    endif()

    # Asset names carry a bare numeric triple, so anything else cannot name a
    # file that exists — the same shape cmake/Version.cmake insists on.
    if(NOT _version MATCHES "^v?([0-9]+\\.[0-9]+\\.[0-9]+)$")
        set(${reasonVar} "'${_version}' is not a numeric X.Y.Z version" PARENT_SCOPE)
        return()
    endif()

    set(${rowVar} "${_row}" PARENT_SCOPE)
    set(${versionVar} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    set(${jsonVar} "${_json}" PARENT_SCOPE)
    set(${reasonVar} "" PARENT_SCOPE)
endfunction()

# Download and stage a prebuilt pace-cc for this host.
# @param outVar Set to the staged executable's path, or empty on any failure.
# @param reasonVar Set to a short diagnostic when outVar is empty.
function(_pace_auto_install_pace_cc outVar reasonVar)
    set(${outVar} "" PARENT_SCOPE)

    # An empty address is the documented way to opt out of pace-cc, so
    # fetching it would quietly override an instruction not to use it. Reported
    # rather than skipped: asking for the fetch and getting nothing, with no word
    # about why, is the silent fall-through the rest of this module avoids.
    if(NOT PACE_ADDR)
        set(${reasonVar} "PACE_ADDR is empty, which opts out of pace-cc" PARENT_SCOPE)
        return()
    endif()

    _pace_auto_install_resolve_release(_row _version _json _why)
    if(NOT _row)
        set(${reasonVar} "${_why}" PARENT_SCOPE)
        return()
    endif()

    # Cached for the lifetime of this configure so a later auto-start of the
    # daemon (from the very same release) resolves nothing a second time.
    set(_PACE_RESOLVED_ROW "${_row}" CACHE INTERNAL "")
    set(_PACE_RESOLVED_VERSION "${_version}" CACHE INTERNAL "")
    set(_PACE_RESOLVED_JSON "${_json}" CACHE INTERNAL "")

    # Keyed by platform as well as version. A home directory is not always local
    # to one machine — a shared or synchronised $HOME is normal — and two hosts of
    # different architectures reaching the same staging directory must not be
    # handed each other's binary.
    set(_stagedDir "${PACE_AUTO_INSTALL_DIR}/${_version}/${_pace_asset_${_row}_platform}")
    set(_staged "${_stagedDir}/${_pace_asset_${_row}_exe}")

    if(NOT EXISTS "${_staged}")
        _pace_auto_install_download("${_row}" "${_version}" "${_json}" "${_stagedDir}" _why)
        if(_why)
            set(${reasonVar} "${_why}" PARENT_SCOPE)
            return()
        endif()
    endif()

    # Asked of a binary just downloaded and of one staged by an earlier run
    # alike. A download can arrive intact and still be unusable here — too old a
    # libc, a truncated file no digest was published to catch — and a binary
    # staged last month can have been made unusable since, by the very sharing
    # the layout above accounts for. Finding out now costs one process; finding
    # out later costs every translation unit.
    execute_process(COMMAND "${_staged}" --version
                    TIMEOUT 10
                    RESULT_VARIABLE _rc
                    OUTPUT_QUIET
                    ERROR_QUIET)
    if(NOT _rc EQUAL 0)
        file(REMOVE "${_staged}")
        set(${reasonVar} "the downloaded launcher does not run here (${_rc})" PARENT_SCOPE)
        return()
    endif()

    set(${outVar} "${_staged}" PARENT_SCOPE)
    set(${reasonVar} "" PARENT_SCOPE)
endfunction()

# Fetch one published archive into the shared per-build-tree download cache and
# unpack it, without staging anything out of it yet. Split out of
# _pace_auto_install_download (below) so that staging the launcher and staging
# the daemon from the *same* release — the ordinary case, since both binaries
# live in one archive — costs one download and one unpack rather than two:
# fetching a several-megabyte archive twice in one configure for two files it
# already contains would be a needless round trip on every clean machine.
#
# @param row Asset-table row id describing this host.
# @param version Release version, a bare numeric triple.
# @param json Release metadata, or empty when the version was pinned.
# @param unpackDirVar Set to the directory the archive was unpacked into
#                      (its "${_stem}/..." layout preserved), on success.
# @param reasonVar Set to a short diagnostic on failure, empty on success.
function(_pace_auto_install_fetch_archive row version json unpackDirVar reasonVar)
    set(${reasonVar} "" PARENT_SCOPE)
    set(${unpackDirVar} "" PARENT_SCOPE)
    set(_row "${row}")
    set(_version "${version}")
    set(_json "${json}")

    set(_stem "pace-${_version}-${_pace_asset_${_row}_platform}")
    set(_assetName "${_stem}.${_pace_asset_${_row}_ext}")
    set(_url "")
    set(_digest "")

    # Prefer the URL and digest the API reported. A pinned version has no
    # metadata to consult, so its URL is composed from the documented layout —
    # which is also the path an internal mirror takes, since pinning a version
    # and pointing PACE_AUTO_INSTALL_DOWNLOAD_BASE elsewhere installs
    # without reaching GitHub at all.
    if(_json)
        string(JSON _assetCount ERROR_VARIABLE _jsonErr LENGTH "${_json}" assets)
        if(_jsonErr OR NOT _assetCount GREATER 0)
            set(${reasonVar} "release ${_version} publishes no assets" PARENT_SCOPE)
            return()
        endif()
        math(EXPR _lastAsset "${_assetCount} - 1")
        foreach(_i RANGE 0 ${_lastAsset})
            string(JSON _name ERROR_VARIABLE _e GET "${_json}" assets ${_i} name)
            if(_e OR NOT _name STREQUAL "${_assetName}")
                continue()
            endif()
            string(JSON _url ERROR_VARIABLE _e GET "${_json}" assets ${_i} browser_download_url)
            if(_e)
                set(_url "")
            endif()
            # Present on releases GitHub has digested; absent on older ones.
            string(JSON _digest ERROR_VARIABLE _e GET "${_json}" assets ${_i} digest)
            if(_e)
                set(_digest "")
            endif()
            break()
        endforeach()
        if(NOT _url)
            set(${reasonVar} "release ${_version} publishes no ${_assetName}" PARENT_SCOPE)
            return()
        endif()
    else()
        set(_url "${PACE_AUTO_INSTALL_DOWNLOAD_BASE}/${PACE_AUTO_INSTALL_REPO}/releases/download/v${_version}/${_assetName}")
    endif()

    message(STATUS "[cache] Fetching the Pace ${_version} release archive for ${_pace_asset_${_row}_platform}")

    set(_work "${CMAKE_BINARY_DIR}/CMakeFiles/pace-download")
    set(_archive "${_work}/${_assetName}")
    file(DOWNLOAD "${_url}" "${_archive}"
         STATUS _status
         TIMEOUT 120
         INACTIVITY_TIMEOUT 30)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
        list(GET _status 1 _message)
        file(REMOVE "${_archive}")
        set(${reasonVar} "downloading ${_assetName} failed (${_message})" PARENT_SCOPE)
        return()
    endif()

    if(_digest MATCHES "^sha256:([0-9a-fA-F]+)$")
        file(SHA256 "${_archive}" _actual)
        string(TOLOWER "${CMAKE_MATCH_1}" _expected)
        if(NOT _actual STREQUAL "${_expected}")
            file(REMOVE "${_archive}")
            set(${reasonVar} "${_assetName} failed its SHA256 check" PARENT_SCOPE)
            return()
        endif()
    endif()

    # Unpack beside the archive, inside the build tree, so two build trees
    # unpacking the same release at the same time cannot collide.
    set(_unpack "${_work}/unpack")
    file(REMOVE_RECURSE "${_unpack}")
    file(MAKE_DIRECTORY "${_unpack}")
    file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_unpack}")

    set(${unpackDirVar} "${_unpack}/${_stem}" PARENT_SCOPE)
endfunction()

# Copy one member out of an already-unpacked archive into a staging directory.
# Split out so the launcher and the daemon — two members of one archive — are
# each placed with the same race-safe move-into-place sequence, without
# duplicating it.
#
# @param unpackDir Directory _pace_auto_install_fetch_archive unpacked into.
# @param member Path to the executable inside unpackDir.
# @param exe Filename to give the staged copy.
# @param stagedDir Directory the executable is placed in.
# @param reasonVar Set to a short diagnostic on failure, empty on success.
function(_pace_auto_install_stage_member unpackDir member exe stagedDir reasonVar)
    set(${reasonVar} "" PARENT_SCOPE)

    set(_member "${unpackDir}/${member}")
    if(NOT EXISTS "${_member}")
        set(${reasonVar} "the archive does not contain ${member}" PARENT_SCOPE)
        return()
    endif()

    # Move into place rather than write in place: two configures racing on the
    # same shared staging directory must never expose a half-written binary to
    # the one that arrives second. The temporary name is derived from the build
    # tree, so the two racers cannot pick the same one either.
    string(SHA256 _treeHash "${CMAKE_BINARY_DIR}")
    string(SUBSTRING "${_treeHash}" 0 12 _treeHash)
    set(_pending "${stagedDir}/.staging-${_treeHash}-${exe}")
    file(MAKE_DIRECTORY "${stagedDir}")
    file(REMOVE "${_pending}")
    file(COPY_FILE "${_member}" "${_pending}" RESULT _copyFailed)
    if(_copyFailed)
        set(${reasonVar} "cannot write to ${stagedDir} (${_copyFailed})" PARENT_SCOPE)
        return()
    endif()
    file(CHMOD "${_pending}"
         PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    file(RENAME "${_pending}" "${stagedDir}/${exe}" RESULT _renameFailed)
    if(_renameFailed AND NOT EXISTS "${stagedDir}/${exe}")
        # Losing the race is success; failing to write at all is not.
        set(${reasonVar} "cannot stage ${exe} (${_renameFailed})" PARENT_SCOPE)
        return()
    endif()
    file(REMOVE "${_pending}")
endfunction()

# Fetch one published archive and stage the launcher out of it. Thin wrapper
# over the two functions above, kept so every existing caller (and the
# existing diagnostics they depend on) is unaffected by the split.
# @param row Asset-table row id describing this host.
# @param version Release version, a bare numeric triple.
# @param json Release metadata, or empty when the version was pinned.
# @param stagedDir Directory the launcher is placed in.
# @param reasonVar Set to a short diagnostic on failure, empty on success.
function(_pace_auto_install_download row version json stagedDir reasonVar)
    set(${reasonVar} "" PARENT_SCOPE)

    _pace_auto_install_fetch_archive("${row}" "${version}" "${json}" _unpackDir _why)
    if(_why)
        set(${reasonVar} "${_why}" PARENT_SCOPE)
        return()
    endif()

    _pace_auto_install_stage_member(
        "${_unpackDir}" "${_pace_asset_${row}_member}" "${_pace_asset_${row}_exe}"
        "${stagedDir}" _why)
    if(_why)
        set(${reasonVar} "${_why}" PARENT_SCOPE)
        return()
    endif()
endfunction()

# Split from the connect attempt below only by host/port parsing, itself
# already done once for PACE_ADDR — kept as its own function so the race
# check (does *anything* answer) and the future probe share no code by
# accident growing shared state.
#
# @param addr host:port to parse (PACE_ADDR's own shape).
# @param hostVar Set to the host part.
# @param portVar Set to the port part.
function(_pace_split_host_port addr hostVar portVar)
    # IPv6 in brackets ("[::1]:6674") is the one shape a naive last-colon split
    # gets wrong, so the bracket form is matched first.
    if(addr MATCHES "^\\[(.+)\\]:([0-9]+)$")
        set(${hostVar} "${CMAKE_MATCH_1}" PARENT_SCOPE)
        set(${portVar} "${CMAKE_MATCH_2}" PARENT_SCOPE)
    elseif(addr MATCHES "^(.+):([0-9]+)$")
        set(${hostVar} "${CMAKE_MATCH_1}" PARENT_SCOPE)
        set(${portVar} "${CMAKE_MATCH_2}" PARENT_SCOPE)
    else()
        set(${hostVar} "${addr}" PARENT_SCOPE)
        set(${portVar} "" PARENT_SCOPE)
    endif()
endfunction()

# Is anything at all answering at PACE_ADDR? Deliberately cheaper and
# less discerning than _pace_probe_pace_cc: that probe needs a real
# compiler invocation and answers "does the CACHE work", which is not the
# question here. This only answers "would starting a daemon collide with one
# already running" — ours from an earlier configure, or someone else's — so
# two configures racing on the same port must not both attempt a bind. Losing
# this race is deliberately not a failure case: whichever configure's spawn
# loses treats the winner's daemon as its own, which is exactly what
# _pace_probe_pace_cc then evaluates.
#
# CMake has no raw-socket primitive, so this shells out to the daemon binary
# itself with --version against the address — the one operation every
# pace-kv build already exposes for exactly this kind of check — rather
# than reaching for a platform-specific netcat/Test-NetConnection that may not
# be installed either. A future pace-kv could grow a lighter "are you
# there" ping; until then this costs one short-lived process.
#
# @param outVar Set to TRUE when something answers, FALSE otherwise.
function(_pace_daemon_answering addr outVar)
    set(${outVar} FALSE PARENT_SCOPE)
    _pace_split_host_port("${addr}" _host _port)
    if(NOT _port)
        return()
    endif()
    # A tiny C program would be a fifth language in a module that promises to
    # stay stock CMake; `cmake -E` has no TCP-connect verb either. The staged
    # pace-cc's own probe path already proves connectivity end-to-end at
    # negligible cost (it fails fast, ~10ms, against a closed port), so reuse
    # it here instead of inventing a second way to ask the same question.
    if(NOT PACE_CC)
        return()
    endif()
    set(_dir "${CMAKE_BINARY_DIR}/CMakeFiles/pace-probe")
    file(MAKE_DIRECTORY "${_dir}")
    set(_src "${_dir}/portcheck.cpp")
    if(NOT EXISTS "${_src}")
        file(WRITE "${_src}" "int pacePortCheck() { return 0; }\n")
    endif()
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(_args /nologo /c "${_src}" "/Fo${_dir}/portcheck.obj")
    else()
        set(_args -c "${_src}" -o "${_dir}/portcheck.o")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                "PACE_ADDR=${addr}"
                "PACE_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                "PACE_BINARY_DIR=${CMAKE_BINARY_DIR}"
                "PACE_VERBOSE=1"
                "PACE_NO_STATS=1"
                "PACE_TIMEOUT=500ms"
                "${PACE_CC}" "${CMAKE_CXX_COMPILER}" ${_args}
        WORKING_DIRECTORY "${_dir}"
        TIMEOUT 3
        RESULT_VARIABLE _rc
        OUTPUT_QUIET
        ERROR_VARIABLE _err)
    if(_err MATCHES "pace-cc: (HIT|MISS) key=")
        set(${outVar} TRUE PARENT_SCOPE)
    endif()
endfunction()

# Stage and start a pace-kv daemon in the background when PACE_ADDR is
# otherwise unreachable, so PACE_AUTO_INSTALL's launcher has something to
# talk to on a genuinely clean machine. Nothing here may fail a
# configure, exactly like _pace_auto_install_pace_cc: every failure is one
# status line and a fall-through to the probe finding no daemon, which is the
# behaviour without PACE_AUTO_START at all.
function(_pace_auto_start_pace_kv)
    _pace_daemon_answering("${PACE_ADDR}" _answering)
    if(_answering)
        # Ours from an earlier configure, or a pre-existing daemon started by
        # hand — either way there is nothing to start, and starting a second
        # one would only fight the first for the port.
        return()
    endif()

    _pace_split_host_port("${PACE_ADDR}" _bindHost _bindPort)
    if(NOT _bindPort)
        message(STATUS "[cache] Not starting a daemon: PACE_ADDR '${PACE_ADDR}' has no port")
        return()
    endif()

    # Reuse the launcher's own resolved release when this configure already
    # auto-installed one, rather than asking the GitHub API a second time —
    # which could, on a second call, resolve to a *different* "latest" than
    # the launcher just staged, if a release published in between the two
    # calls. Falling through to a fresh resolution below still works when the
    # launcher came from PATH and resolved nothing.
    if(DEFINED _PACE_RESOLVED_ROW AND _PACE_RESOLVED_ROW)
        set(_row "${_PACE_RESOLVED_ROW}")
        set(_version "${_PACE_RESOLVED_VERSION}")
        set(_json "${_PACE_RESOLVED_JSON}")
    else()
        _pace_auto_install_resolve_release(_row _version _json _why)
        if(NOT _row)
            message(STATUS "[cache] Not starting a daemon: ${_why}")
            return()
        endif()
    endif()

    if(NOT _pace_asset_${_row}_daemon_member)
        message(STATUS "[cache] Not starting a daemon: no pace-kv binary is published for ${_pace_asset_${_row}_platform}")
        return()
    endif()

    set(_stagedDir "${PACE_AUTO_INSTALL_DIR}/${_version}/${_pace_asset_${_row}_platform}")
    set(_staged "${_stagedDir}/${_pace_asset_${_row}_daemon_exe}")

    if(NOT EXISTS "${_staged}")
        _pace_auto_install_fetch_archive("${_row}" "${_version}" "${_json}" _unpackDir _why)
        if(NOT _unpackDir)
            message(STATUS "[cache] Not starting a daemon: ${_why}")
            return()
        endif()
        _pace_auto_install_stage_member(
            "${_unpackDir}" "${_pace_asset_${_row}_daemon_member}" "${_pace_asset_${_row}_daemon_exe}"
            "${_stagedDir}" _why)
        if(_why)
            message(STATUS "[cache] Not starting a daemon: ${_why}")
            return()
        endif()
    endif()

    file(MAKE_DIRECTORY "${PACE_AUTO_START_STORAGE_DIR}")

    # Bind whatever host PACE_ADDR actually named — including a LAN
    # address someone deliberately pointed a shared daemon at — and default
    # only when it named none, since PACE_ADDR's own default
    # (127.0.0.1:6674, set near the top of this file) is loopback-only and an
    # auto-started daemon should not be reachable from the network by
    # surprise.
    set(_bindArg "${_bindHost}")
    if(NOT _bindArg)
        set(_bindArg "127.0.0.1")
    endif()

    set(_daemonArgs
        "--bind=${_bindArg}"
        "--port=${_bindPort}"
        "--storage=${PACE_AUTO_START_STORAGE_DIR}")

    if(CMAKE_HOST_WIN32)
        # No double-fork equivalent outside the SCM path (see
        # .agent/rules/platform-service-and-config.md on --daemon and
        # launchd/SCM supervisors) — this is a plain background
        # process, not a registered service, so `cmake -E env` launches it
        # directly. execute_process has no detach flag of its own; started
        # this way the child is not joined to cmake's own console and outlives
        # the configure once cmake exits, which is standard Windows
        # child-process behaviour for a process that does not inherit the
        # parent's console. --pidfile is POSIX-daemon-mode only (ServiceControl
        # names it so explicitly) and is never written by the plain
        # ForegroundHost this runs under here, so it is omitted rather than
        # passed for a file that would never appear.
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env "${_staged}" ${_daemonArgs}
            RESULT_VARIABLE _rc
            TIMEOUT 5
            OUTPUT_QUIET ERROR_QUIET)
    else()
        # --daemon double-forks and the parent exits immediately once the
        # child has detached, so execute_process returns right away with the
        # daemon already backgrounded — this is the one place in the whole
        # module that flag is the right tool, as opposed to a *service*
        # registration, which --daemon must never be combined with (see
        # .agent/rules/platform-service-and-config.md: "the supervisor's launch
        # arguments must not pass --daemon"). A well-known --pidfile is what
        # turns "who started this"
        # into an answerable question — for an operator who wants to stop a
        # daemon this module left running (its own stated lifetime: until the
        # machine reboots or a human kills it), and for a test that needs to
        # clean one up without a PID execute_process never hands back, since
        # --daemon double-forks.
        set(_pidfile "${PACE_AUTO_START_STORAGE_DIR}/pace-kv.pid")
        execute_process(
            COMMAND "${_staged}" --daemon ${_daemonArgs} "--pidfile=${_pidfile}"
            RESULT_VARIABLE _rc
            TIMEOUT 5
            OUTPUT_QUIET ERROR_QUIET)
    endif()
    if(NOT _rc EQUAL 0)
        message(STATUS "[cache] Not starting a daemon: pace-kv exited immediately (${_rc})")
        return()
    endif()

    # The daemon forks/spawns and returns before it is necessarily listening
    # yet, so give it a short, bounded window to come up rather than letting
    # the very next thing that runs (the probe) be the first and only check —
    # a poll loop rather than a fixed sleep, since a fast machine should not
    # pay a worst-case wait. _pace_daemon_answering itself costs ~10ms once the
    # daemon is up, so ten attempts is a bounded ~1s beyond whatever the
    # daemon itself needed to bind.
    set(_upAttempt 0)
    set(_up FALSE)
    while(_upAttempt LESS 10 AND NOT _up)
        _pace_daemon_answering("${PACE_ADDR}" _up)
        if(NOT _up)
            execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.1)
        endif()
        math(EXPR _upAttempt "${_upAttempt} + 1")
    endwhile()

    if(_up)
        message(STATUS "[cache] Auto-started pace-kv (${_staged}) listening on ${PACE_ADDR}, storage=${PACE_AUTO_START_STORAGE_DIR}")
    else()
        message(STATUS "[cache] Started pace-kv (${_staged}) but it is not answering on ${PACE_ADDR} yet; the probe below will report the outcome")
    endif()
endfunction()

# Only when there is nothing else to use. A launcher the user installed is a
# decision already made, and preempting it would be the behaviour change this is
# careful not to be.
if(USE_COMPILER_CACHE
   AND PACE_AUTO_INSTALL
   AND NOT PACE_CC
   AND NOT SCCACHE
   AND NOT CCACHE)
    _pace_auto_install_pace_cc(_pace_auto_installed _pace_auto_why_not)
    if(_pace_auto_installed)
        message(STATUS "[cache] Auto-installed pace-cc (${_pace_auto_installed})")
        set(PACE_CC "${_pace_auto_installed}" CACHE FILEPATH
            "pace-cc tool path; needs a pace-kv daemon to be used" FORCE)
    else()
        message(STATUS "[cache] Not auto-installing pace-cc: ${_pace_auto_why_not}")
    endif()
endif()

# Belongs after auto-install rather than inside it: starting a daemon is
# useful whether pace-cc came from PATH or was just staged above, and
# either way PACE_CC has to be a real path before _pace_daemon_answering
# can shell out to it to test the port.
if(USE_COMPILER_CACHE
   AND PACE_AUTO_START
   AND PACE_CC
   AND PACE_ADDR)
    _pace_auto_start_pace_kv()
endif()

# Ask pace-cc itself whether the cache works, by compiling one tiny
# translation unit through it with PACE_VERBOSE=1 and requiring a reported
# cache outcome. A launcher that cannot reach its daemon still compiles fine —
# it just runs the real compiler — so nothing but an end-to-end exchange tells
# "the cache works" apart from "every TU will silently pay a failed connect,
# with precompiled headers disabled for nothing and ccache passed over".
#
# The match is positive (HIT/MISS only): should the launcher's diagnostics ever
# be reworded, this reports unusable and the build falls back to the next
# launcher, which is the harmless direction to be wrong in.
#
# @param outVar Set to TRUE when the cache served the probe, FALSE otherwise.
# @param reasonVar Set to a short diagnostic when outVar is FALSE.
function(_pace_probe_pace_cc outVar reasonVar)
    set(${outVar} FALSE PARENT_SCOPE)

    set(_dir "${CMAKE_BINARY_DIR}/CMakeFiles/pace-probe")
    set(_src "${_dir}/probe.cpp")
    # Both the file and its content are fixed, so the probe itself is a cache
    # hit from the second configure onwards — which exercises FETCH rather than
    # just STORE, and costs less than the first run.
    file(WRITE "${_src}" "int paceProbe() { return 0; }\n")

    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(_args /nologo /c "${_src}" "/Fo${_dir}/probe.obj")
    else()
        set(_args -c "${_src}" -o "${_dir}/probe.o")
    endif()

    # A probe that answers takes ~0.1s locally and little more over a LAN, so ten
    # seconds is generous for a working daemon and a bounded wait for a broken
    # one. The cap has to live here: PACE_TIMEOUT bounds the launcher's
    # send/recv but not its connect(), so an address that drops packets rather
    # than refusing them — a firewall, a downed VPN, a host that is simply gone —
    # stalls on the TCP connect timeout instead (measured: 2m30s), and every
    # configure would pay it.
    set(_timeoutSeconds 10)

    # NO_STATS keeps the probe out of `pace-cc --show-stats`, where it would
    # read as a build that never hits. TIMEOUT_MS bounds a daemon that accepts
    # the connection and then stalls; builds keep the launcher's own default.
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                ${_pace_launcher_env}
                "PACE_VERBOSE=1"
                "PACE_NO_STATS=1"
                "PACE_TIMEOUT=2s"
                "${PACE_CC}" "${CMAKE_CXX_COMPILER}" ${_args}
        WORKING_DIRECTORY "${_dir}"
        TIMEOUT ${_timeoutSeconds}
        RESULT_VARIABLE _rc
        OUTPUT_QUIET
        ERROR_VARIABLE _err)

    if(_rc MATCHES "[Tt]imeout")
        set(${reasonVar} "no answer within ${_timeoutSeconds}s" PARENT_SCOPE)
    elseif(NOT _rc EQUAL 0)
        set(${reasonVar} "probe compile failed (${_rc})" PARENT_SCOPE)
    elseif(_err MATCHES "pace-cc: (HIT|MISS) key=")
        set(${outVar} TRUE PARENT_SCOPE)
        set(${reasonVar} "" PARENT_SCOPE)
    elseif(_err MATCHES "pace-cc: (cache unavailable|not caching) \\(([^)]*)\\)")
        # Two lead-ins, because the launcher distinguishes "the cache let us down"
        # from "this compile is deliberately not cacheable" and says which. Both
        # end in a probe that did not cache, which is all this has to report.
        set(${reasonVar} "${CMAKE_MATCH_2}" PARENT_SCOPE)
    else()
        set(${reasonVar} "no cache outcome reported" PARENT_SCOPE)
    endif()
endfunction()

# Candidate table, most-preferred first. Each row <id> is described by:
#   _pace_cache_<id>_label     human-readable name for the status message
#   _pace_cache_<id>_program   the found program (empty when not installed)
#   _pace_cache_<id>_requires  extra condition; the row is skipped when falsy
#   _pace_cache_<id>_env       NAME=VALUE pairs to inject around the invocation
#   _pace_cache_<id>_check     function deciding usability at configure time
#                            (empty when being installed is enough); called as
#                            <fn>(<outVar> <reasonVar>) and only for a row that
#                            already passed program and requires
#   _pace_cache_<id>_detail    extra words for the status message (empty for none)
#   _pace_cache_<id>_caveat    a correctness hazard this launcher carries, warned
#                            about when it is the row that WINS (empty for none)
# Supporting a fourth launcher is adding an id here plus its seven variables.
set(_pace_cache_candidates pace_cc sccache ccache)

# Render "<label>[ <detail>]" for a row, so a launcher and where it points are
# named the same way whether it won or was passed over. Diagnosing a daemon that
# did not answer starts with knowing which address was tried.
# @param id Row id from _pace_cache_candidates.
# @param outVar Receives the rendered text.
function(_pace_cache_describe id outVar)
    set(_text "${_pace_cache_${id}_label}")
    if(_pace_cache_${id}_detail)
        string(APPEND _text " ${_pace_cache_${id}_detail}")
    endif()
    set(${outVar} "${_text}" PARENT_SCOPE)
endfunction()

set(_pace_cache_pace_cc_label "pace-cc")
set(_pace_cache_pace_cc_program "${PACE_CC}")
set(_pace_cache_pace_cc_requires "${PACE_ADDR}")
set(_pace_cache_pace_cc_env ${_pace_launcher_env})
set(_pace_cache_pace_cc_check _pace_probe_pace_cc)
set(_pace_cache_pace_cc_detail "at ${PACE_ADDR}")
set(_pace_cache_pace_cc_caveat "")

set(_pace_cache_sccache_label "sccache")
set(_pace_cache_sccache_program "${SCCACHE}")
set(_pace_cache_sccache_requires ON)
set(_pace_cache_sccache_env "")
set(_pace_cache_sccache_check "")
set(_pace_cache_sccache_detail "")
# Not a `check`, because the row stays usable: this is a hazard a developer has to
# be able to weigh, not a condition this module can evaluate. Nothing here can
# tell whether the cache about to be used is shared with another checkout.
#
# Carried only where the hazard exists, which is a property of the COMPILER
# rather than of sccache alone. Measured both ways, because warning where it
# cannot happen is how a warning becomes one people learn to skip:
#
#   * MSVC and clang-cl are exposed. sccache preprocesses them with `/EP`, which
#     emits no `#line` markers, so the text it hashes carries no paths at all and
#     two checkouts hash identically. `/showIncludes` then reports ABSOLUTE paths,
#     so the dependency stream replayed into the second checkout names the first.
#     Measured on Pace: two worktrees at one commit, 137 cross-worktree
#     hits, 1097 recorded dependency edges pointing into the wrong tree and none
#     into its own, and `ninja: no work to do` after a real edit to a header.
#
#   * GCC and Clang are not. Their preprocessed output carries `# n "path"` line
#     markers, so with the absolute include paths CMake generates the hashed text
#     differs between checkouts and there is no hit to replay (measured on Ubuntu
#     24.04: 0 hits, 2 misses). Spell the same compile with relative paths and
#     there IS a hit -- but then the depfile is relative too and resolves inside
#     the consuming tree, which is correct. Self-consistent either way.
#
# Both languages are tested, not just CXX: this module wires
# CMAKE_C_COMPILER_LAUNCHER as well, so a `project(x LANGUAGES C)` on MSVC is
# exposed exactly as much and would otherwise be the one configuration that gets
# sccache with no word about it.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"
   OR CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    set(_pace_cache_sccache_caveat
        "Under MSVC and clang-cl, sccache replays a cache hit's /showIncludes stream verbatim -- the ABSOLUTE paths spelled by the build that STORED it -- while the text it hashes to find that hit carries no paths at all, because it preprocesses with /EP and /EP emits no line markers. Two checkouts therefore share entries and then record each other's headers as their dependencies. Editing a header in the checkout you are building rebuilds nothing: the build stays green and the objects are stale.

It bites an INCREMENTAL build across two checkouts sharing one sccache cache. A clean build has no dependency graph to corrupt, and checkouts that all sit at the same absolute path replay paths that are correct -- CI is normally both, and unaffected. GCC and Clang are unaffected everywhere: their preprocessed output carries the paths, so the two checkouts do not share entries in the first place.

To avoid it: install pace-cc and run a pace-kv daemon, which rewrites a hit's paths into the consuming checkout and so does not have this failure mode; or pass -DSCCACHE= to fall through to ccache, which is unaffected; or configure with -DUSE_COMPILER_CACHE=OFF.")
else()
    set(_pace_cache_sccache_caveat "")
endif()

set(_pace_cache_ccache_label "ccache")
set(_pace_cache_ccache_program "${CCACHE}")
set(_pace_cache_ccache_requires ON)
set(_pace_cache_ccache_env "")
set(_pace_cache_ccache_check "")
set(_pace_cache_ccache_detail "")
# ccache's default `base_dir` is empty, which is documented to mean it does not
# rewrite absolute paths and so does not share entries between checkouts -- the
# precondition the sccache hazard above needs. Setting `base_dir` deliberately
# opts into that sharing, which is a choice its own documentation covers.
set(_pace_cache_ccache_caveat "")

set(_pace_cache_chosen "")
set(_pace_cache_rejected "")
if(USE_COMPILER_CACHE)
    foreach(_id IN LISTS _pace_cache_candidates)
        if(NOT _pace_cache_${_id}_program OR NOT _pace_cache_${_id}_requires)
            continue()
        endif()
        if(_pace_cache_${_id}_check)
            cmake_language(CALL ${_pace_cache_${_id}_check} _pace_cache_usable _pace_cache_why_not)
            if(NOT _pace_cache_usable)
                # Remember why, so a fall-through to a slower launcher explains
                # itself rather than looking like the faster one was never there.
                _pace_cache_describe("${_id}" _pace_cache_desc)
                list(APPEND _pace_cache_rejected "${_pace_cache_desc}: ${_pace_cache_why_not}")
                continue()
            endif()
        endif()
        set(_pace_cache_chosen "${_id}")
        break()
    endforeach()
endif()

# Say why a preferred launcher was passed over, whatever the outcome: falling
# through in silence looks exactly like it never being installed.
foreach(_rejection IN LISTS _pace_cache_rejected)
    message(STATUS "[cache] Not using ${_rejection}")
endforeach()

if(_pace_cache_chosen)
    set(_pace_cache_program "${_pace_cache_${_pace_cache_chosen}_program}")

    # `cmake -E env NAME=VALUE ... <program>` is the only way to attach
    # environment to a compiler launcher; rows without env invoke the program
    # directly so they pay no extra process.
    if(_pace_cache_${_pace_cache_chosen}_env)
        set(_pace_cache_launcher
            "${CMAKE_COMMAND}" -E env
            ${_pace_cache_${_pace_cache_chosen}_env}
            "${_pace_cache_program}")
    else()
        set(_pace_cache_launcher "${_pace_cache_program}")
    endif()

    _pace_cache_describe("${_pace_cache_chosen}" _pace_cache_desc)
    message(STATUS "[cache] Enabling ${_pace_cache_desc} (${_pace_cache_program}) for C/C++ compilation")

    # A launcher that can silently produce a WRONG build says so, at the moment a
    # build is opted into it. A warning rather than a status line because the
    # symptom arrives hours later and somewhere else entirely -- a stale object,
    # a green build, and a crash in code nobody touched -- so the one line naming
    # it has to still be findable in the log afterwards.
    #
    # Never fatal, and never a `check` that skips the row: which launcher to run
    # is the developer's call, and a module vendored into other repositories does
    # not get to make it for them. It only has to make it an informed one.
    if(_pace_cache_${_pace_cache_chosen}_caveat)
        message(WARNING "[cache] ${_pace_cache_${_pace_cache_chosen}_caveat}")
    endif()

    set(CMAKE_C_COMPILER_LAUNCHER ${_pace_cache_launcher})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${_pace_cache_launcher})

    # None of the launchers reproduces anything but the object file on a cache
    # hit, so a precompiled header (a second, separately produced artefact)
    # cannot be served from cache.
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)

    # CMake's C++20 module support puts scanning flags on every compile line
    # (-fmodules-ts -fmodule-mapper=<per-object modmap> on GCC). A preprocess-only
    # run with those flags fails, so a launcher that derives its key by
    # preprocessing falls back on *every* translation unit. This project has no
    # module units, so the scan is pure overhead; turn it off while a launcher is
    # in use.
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

    # For the same reason none of them supports /Zi (shared PDB). Force MSVC to
    # embed debug info in .obj files (/Z7) via the modern CMake knob (CMP0141),
    # and also fix up any legacy /Zi already present in FLAGS_DEBUG /
    # FLAGS_RELWITHDEBINFO.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        set(CMAKE_POLICY_DEFAULT_CMP0141 NEW)
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
        foreach(_var
                CMAKE_CXX_FLAGS_DEBUG
                CMAKE_C_FLAGS_DEBUG
                CMAKE_CXX_FLAGS_RELWITHDEBINFO
                CMAKE_C_FLAGS_RELWITHDEBINFO)
            string(REGEX REPLACE "([-/])Zi" "\\1Z7" ${_var} "${${_var}}")
        endforeach()
    endif()
else()
    # Define the launchers as empty rather than leaving them unset. Fetched
    # dependencies bring their own cache modules that auto-enable ccache when the
    # launcher is merely *undefined* (libunicode's cmake/EnableCcache.cmake does
    # exactly that), which would quietly re-enable caching for their targets.
    # An empty definition is inert for us and keeps USE_COMPILER_CACHE=OFF honest.
    set(CMAKE_C_COMPILER_LAUNCHER "")
    set(CMAKE_CXX_COMPILER_LAUNCHER "")

    if(NOT USE_COMPILER_CACHE)
        message(STATUS "[cache] Compiler caching disabled by USE_COMPILER_CACHE=OFF")
    elseif(_pace_cache_rejected)
        message(STATUS "[cache] No other compiler-cache launcher found (sccache, ccache); caching disabled "
                       "(start a daemon with `pace-kv` to cache through pace-cc)")
    else()
        message(STATUS "[cache] No compiler-cache launcher found (pace-cc, sccache, ccache); caching disabled")
    endif()
endif()
