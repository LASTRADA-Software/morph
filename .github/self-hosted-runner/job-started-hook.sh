#!/usr/bin/env bash
# ACTIONS_RUNNER_HOOK_JOB_STARTED -- runs before every job on this container.
#
# Removes sccache and ccache from the PATH directories a job can install into.
# CompileCache.cmake's FASTCACHE_AUTO_INSTALL path fires only when none of
# fastcache-cc/sccache/ccache is already on PATH, so a compile-cache binary
# left behind by a previous job silently costs the next one its connection to
# the host fastcached daemon -- the build still succeeds, just uncached.
#
# This has to run between jobs, not at container start: one fleet now serves
# morph, fastcached and Lightweight, and the contamination happens when a
# Lightweight job (hendrikmuhs/ccache-action apt-installs ccache) is followed
# by a morph job on the same container.
#
# fastcache-cc is deliberately NOT removed. When it is present, auto-install
# is unnecessary and CompileCache.cmake simply uses it, which is the outcome
# we want; removing it would force a re-download on every job.
#
# A target can be package-managed (ccache-action apt-installs ccache) or not
# (sccache is an unpackaged tarball). `rm -f`-ing a package-managed binary
# leaves dpkg still recording the package as installed, so the *next* job's
# `apt-get install ccache` reports "already the newest version" and installs
# nothing -- the binary never comes back, breaking every later ccache job on
# that container. So ownership is checked at runtime with `dpkg -S`: an owned
# target is removed with `apt-get remove`, so dpkg's state stays consistent
# and a later install genuinely reinstalls it; an unowned target is `rm -f`'d
# as before. LASTRADA_RUNNER_DPKG / LASTRADA_RUNNER_APT let tests substitute
# stub binaries, the same way run-runner.sh's LASTRADA_RUNNER_GH/DOCKER do.
#
# Never fails the job: a hook that exits non-zero aborts the run it was meant
# to help. Missing directory, missing tool, unremovable file, and a missing
# or failing dpkg/apt-get all fall through to "leave it, continue".
set -euo pipefail

dirs="${LASTRADA_RUNNER_HOOK_DIRS:-/usr/local/bin /usr/bin /home/runner/.cargo/bin}"
dpkg_bin="${LASTRADA_RUNNER_DPKG:-dpkg}"
apt_bin="${LASTRADA_RUNNER_APT:-apt-get}"

# Prints the name of the package owning $1 on stdout and returns 0, or prints
# nothing and returns 0 if the target is unowned, dpkg has no opinion, or
# dpkg itself is missing/erroring -- any of which sends the caller to rm.
owning_package() {
    local target="$1" line
    line="$("$dpkg_bin" -S "$target" 2>/dev/null)" || return 0
    printf '%s\n' "${line%%:*}"
}

remove_via_apt() {
    local pkg="$1"
    "$apt_bin" remove -y -qq "$pkg" >/dev/null 2>&1 ||
        sudo "$apt_bin" remove -y -qq "$pkg" >/dev/null 2>&1
}

for dir in $dirs; do
    [ -d "$dir" ] || continue
    for tool in sccache ccache; do
        target="${dir}/${tool}"
        [ -e "$target" ] || continue

        pkg="$(owning_package "$target")"
        if [ -n "$pkg" ]; then
            if remove_via_apt "$pkg"; then
                echo "job-started-hook: removed package ${pkg} owning ${target}"
            else
                echo "job-started-hook: could not remove package ${pkg} owning ${target}, continuing" >&2
            fi
        elif rm -f "$target" 2>/dev/null || sudo rm -f "$target" 2>/dev/null; then
            echo "job-started-hook: removed ${target}"
        else
            echo "job-started-hook: could not remove ${target}, continuing" >&2
        fi
    done
done

exit 0
