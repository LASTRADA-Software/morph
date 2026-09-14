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
# Never fails the job: a hook that exits non-zero aborts the run it was meant
# to help.
set -euo pipefail

dirs="${LASTRADA_RUNNER_HOOK_DIRS:-/usr/local/bin /usr/bin /home/runner/.cargo/bin}"

for dir in $dirs; do
    [ -d "$dir" ] || continue
    for tool in sccache ccache; do
        target="${dir}/${tool}"
        [ -e "$target" ] || continue
        if rm -f "$target" 2>/dev/null || sudo rm -f "$target" 2>/dev/null; then
            echo "job-started-hook: removed ${target}"
        else
            echo "job-started-hook: could not remove ${target}, continuing" >&2
        fi
    done
done

exit 0
