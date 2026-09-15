#!/usr/bin/env bash
# Deregisters one runner from the GitHub organisation. Called from the host
# by systemd's ExecStop=-@LIBEXEC@/deregister-runner.sh %i (see
# lastrada-runner@.service.in).
#
# Usage: deregister-runner.sh <index>
#
# Why the host does this and not the container: entrypoint.sh's own EXIT trap
# deregisters with `config.sh remove --token`, but the only token it has is
# the REGISTRATION token it started with, and GitHub expires those after
# ~1 hour. Stopping a container that has been up longer than that makes the
# trap's removal fail, stranding both the registration and its live session
# -- the replacement then sits in "A session for this runner already exists
# ... Conflict. Retrying until reconnected" for a couple of minutes. The
# host's `gh` credential has no such expiry, so this script -- not the
# container -- owns deregistration under systemd; run-runner.sh passes the
# container `-e RUNNER_SELF_DEREGISTER=0` so it stands down and the two
# never race each other or double-remove.
#
# Must never fail the stop: a missing runner, a `gh` API error, or an absent
# `gh` binary are all reported and this still exits 0. Systemd runs ExecStop
# on every ordinary restart, not just a shutdown; a script that could fail
# here would turn routine restarts into red units. The leading `-` on the
# ExecStop= line is a second, belt-and-braces guard for the same thing.
#
# LASTRADA_RUNNER_DRY_RUN=1 prints what would be deleted instead of deleting,
# so scripts/test_runner_units.sh can assert the id-resolution logic without
# calling the GitHub API.
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: deregister-runner.sh <index>" >&2
    exit 1
fi
readonly index="$1"

config="${LASTRADA_RUNNER_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/lastrada-runner/config}"
if [ ! -r "$config" ]; then
    echo "deregister-runner.sh: no readable config at ${config}; nothing to deregister" >&2
    exit 0
fi
# shellcheck disable=SC1090
. "$config"

if [ -z "${GITHUB_ORG:-}" ] || [ -z "${RUNNER_NAME_PREFIX:-}" ]; then
    echo "deregister-runner.sh: config missing GITHUB_ORG or RUNNER_NAME_PREFIX; nothing to deregister" >&2
    exit 0
fi

readonly name="${RUNNER_NAME_PREFIX}-${index}"
readonly gh="${LASTRADA_RUNNER_GH:-gh}"

if ! command -v "$gh" >/dev/null 2>&1; then
    echo "deregister-runner.sh: '${gh}' not found on PATH; cannot deregister ${name}" >&2
    exit 0
fi

runner_id="$("$gh" api "orgs/${GITHUB_ORG}/actions/runners" \
    --jq ".runners[] | select(.name == \"${name}\") | .id" 2>/dev/null)" || {
    echo "deregister-runner.sh: failed to query runners for ${name}; leaving it as-is" >&2
    exit 0
}

if [ -z "$runner_id" ]; then
    echo "deregister-runner.sh: no registration found for ${name}; nothing to remove"
    exit 0
fi

if [ -n "${LASTRADA_RUNNER_DRY_RUN:-}" ]; then
    echo "deregister-runner.sh: would delete ${name} (id ${runner_id})"
    exit 0
fi

if "$gh" api -X DELETE "orgs/${GITHUB_ORG}/actions/runners/${runner_id}" >/dev/null 2>&1; then
    echo "deregister-runner.sh: removed ${name} (id ${runner_id})"
else
    echo "deregister-runner.sh: failed to remove ${name} (id ${runner_id}); it may reappear as stale" >&2
fi
exit 0
