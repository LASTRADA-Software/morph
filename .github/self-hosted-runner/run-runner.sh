#!/usr/bin/env bash
# Starts one self-hosted runner container in the foreground, with a
# registration token minted fresh for this start.
#
# Usage: run-runner.sh <index>
#
# systemd's lastrada-runner@<index>.service is the only intended caller.
#
# The token is minted here rather than baked into the container by whoever
# created it. A GitHub registration token expires after ~1 hour, so a token
# captured at `docker run` time is dead on every restart after that hour --
# which is exactly what left five containers restart-looping for six days
# (5,972 failed registrations) until this script existed. Minting per start
# means there is no stored credential that can go stale.
#
# LASTRADA_RUNNER_DRY_RUN=1 prints the docker argv and the stdin payload instead
# of executing, so scripts/test_run_runner.sh can assert the launch contract
# without Docker or GitHub.
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: run-runner.sh <index>" >&2
    exit 1
fi
readonly index="$1"

config="${LASTRADA_RUNNER_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/lastrada-runner/config}"
if [ ! -r "$config" ]; then
    echo "run-runner.sh: no readable config at ${config}" >&2
    exit 1
fi
# shellcheck disable=SC1090
. "$config"

: "${GITHUB_ORG:?config must set GITHUB_ORG}"
: "${RUNNER_NAME_PREFIX:?config must set RUNNER_NAME_PREFIX}"
: "${RUNNER_GROUP:?config must set RUNNER_GROUP}"
: "${RUNNER_LABELS:?config must set RUNNER_LABELS}"
: "${RUNNER_IMAGE:?config must set RUNNER_IMAGE}"
: "${RUNNER_CPUS:?config must set RUNNER_CPUS}"
: "${RUNNER_MEMORY:?config must set RUNNER_MEMORY}"

readonly name="${RUNNER_NAME_PREFIX}-${index}"
readonly gh="${LASTRADA_RUNNER_GH:-gh}"
readonly docker="${LASTRADA_RUNNER_DOCKER:-docker}"

# Organisation-level, not repo-level: one fleet serves morph, fastcached and
# Lightweight. Needs admin:org on whatever credential gh holds -- and that
# credential stays on the host, never entering a container.
token="$("$gh" api -X POST \
    "orgs/${GITHUB_ORG}/actions/runners/registration-token" --jq '.token // empty')"

# `--jq '.token // empty'` covers a missing field, but `gh` has printed the
# literal string "null" for one instead of empty output on some versions --
# and `[ -z "null" ]` is false, so that alone would slip a garbage token
# through to the container. Check both.
if [ -z "$token" ] || [ "$token" = "null" ]; then
    echo "run-runner.sh: minted an empty or null registration token" >&2
    exit 1
fi

# A container from a previous start can outlive its unit (SIGKILL, daemon
# crash) and still own the name; --rm only covers a clean exit.
"$docker" rm -f "$name" >/dev/null 2>&1 || true

argv=(
    "$docker" run --rm -i
    --name "$name"
    "--cpus=${RUNNER_CPUS}"
    "--memory=${RUNNER_MEMORY}"
    --add-host=host.docker.internal:host-gateway
    -e "RUNNER_NAME=${name}"
    -e "RUNNER_SCOPE_URL=https://github.com/${GITHUB_ORG}"
    -e "RUNNER_GROUP=${RUNNER_GROUP}"
    -e "RUNNER_LABELS=${RUNNER_LABELS}"
    -e "FASTCACHE_ADDR=${FASTCACHE_ADDR:-}"
    -e "CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
    "${RUNNER_IMAGE}"
)

if [ -n "${LASTRADA_RUNNER_DRY_RUN:-}" ]; then
    printf '%s\n' "${argv[@]}"
    printf 'STDIN:%s\n' "$token"
    exit 0
fi

# The token goes in on stdin, never in the environment. Every job on this
# container can read `env` and /proc/1/environ, and a live registration token
# would let contributor-authored code enrol its own machine into the runner
# group. `exec` so systemd tracks the docker client directly and its SIGTERM
# reaches the container rather than an intermediate shell.
exec "${argv[@]}" <<<"$token"
