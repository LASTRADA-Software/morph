#!/usr/bin/env bash
# Usage: scripts/add_apt_repository_retrying.sh <ppa> [attempts]
#
# `add-apt-repository` against a Launchpad PPA, retried with backoff.
#
# Why this exists: six CI steps across ci.yml and docs.yml need GCC 15 from
# ppa:ubuntu-toolchain-r/test, and each called add-apt-repository exactly once.
# Launchpad serves that call from a service that returns HTTP 500 often enough
# to matter -- on 2026-08-25 it took out roughly a dozen jobs across six pull
# requests in one afternoon, every one dying in the dependency-install step
# before a single file was compiled:
#
#     lazr.restfulclient.errors.ServerError: HTTP Error 500: Internal Server Error
#     b'GPGKeyTemporarilyNotFoundError'
#
# Such a failure carries no information about the change under test, but it
# looks exactly like a real one in the checks list, so each costs a human the
# time to open the log and rule it out. The outage is not ours to fix; treating
# a transient 500 as fatal is.
#
# It deliberately still fails after the last attempt rather than continuing
# without the PPA. The apt-get install that follows would then pull the distro's
# older GCC and the job would build and pass while testing a compiler the
# project does not target -- a green tick for the wrong thing, which is worse
# than a red one.
#
# MORPH_ADD_APT_REPOSITORY overrides the command, so the test suite can drive
# this against a stub instead of touching the machine's apt configuration.
set -euo pipefail

readonly PPA="${1:?usage: $0 <ppa> [attempts]}"
readonly ATTEMPTS="${2:-5}"
readonly ADD_APT="${MORPH_ADD_APT_REPOSITORY:-sudo add-apt-repository}"
# Overridable so the test does not spend its runtime asleep.
readonly BACKOFF_UNIT="${MORPH_RETRY_BACKOFF_SECONDS:-15}"

for attempt in $(seq 1 "$ATTEMPTS"); do
    if $ADD_APT -y "$PPA"; then
        if [ "$attempt" -gt 1 ]; then
            echo "add-apt-repository succeeded on attempt ${attempt}."
        fi
        exit 0
    fi
    if [ "$attempt" -eq "$ATTEMPTS" ]; then
        echo "::error::add-apt-repository ${PPA} failed ${ATTEMPTS} times. This is usually a" \
             "Launchpad outage rather than a fault in the change under test -- check" \
             "https://status.canonical.com before investigating further."
        exit 1
    fi
    delay=$((attempt * BACKOFF_UNIT))
    echo "add-apt-repository ${PPA} failed (attempt ${attempt}/${ATTEMPTS}); retrying in ${delay}s."
    sleep "${delay}"
done
