#!/usr/bin/env bash
# Usage: bash scripts/test_runner_units.sh
#
# Self-test for .github/self-hosted-runner/install-runner-units.sh and the
# unit template it renders.
#
# The unit is the whole supervision story, and two of its directives are the
# difference between a visible failure and the six-day silent outage this
# change exists to prevent:
#
#   StartLimitBurst/StartLimitIntervalSec must land in [Unit], not [Service].
#   systemd moved them in v229 and silently ignores them in the wrong
#   section -- the unit would then retry forever exactly like the Docker
#   restart policy it replaces, and nothing would ever show as failed.
#
#   Restart=always is what brings a runner back after an OOM kill without
#   waiting for a reboot.
#
# Neither is observable without rendering the unit, so it is rendered into a
# scratch prefix and inspected, then handed to `systemd-analyze verify`.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly installer="${repo_root}/.github/self-hosted-runner/install-runner-units.sh"

failures=0
note() { printf 'ok: %s\n' "$*"; }
fail() {
    printf 'error: %s\n' "$*" >&2
    failures=$((failures + 1))
}

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

LASTRADA_RUNNER_PREFIX_DIR="$scratch" LASTRADA_RUNNER_NO_SYSTEMCTL=1 \
    bash "$installer" >/dev/null

unit="$(find "$scratch" -name 'lastrada-runner@.service' -print -quit)"
if [ -z "$unit" ]; then
    fail "no lastrada-runner@.service was rendered"
    printf '\n%d check(s) failed\n' "$failures" >&2
    exit 1
fi
note "unit rendered at ${unit#"$scratch"/}"

if grep -q '@[A-Z]*@' "$unit"; then
    fail "unsubstituted placeholders remain: $(grep -o '@[A-Z]*@' "$unit" | sort -u | tr '\n' ' ')"
else
    note "all placeholders substituted"
fi

# StartLimit* must be in [Unit]; systemd ignores them in [Service].
section=""
start_limit_section=""
while IFS= read -r line; do
    case "$line" in
        \[*\]) section="$line" ;;
        StartLimitBurst=*) start_limit_section="$section" ;;
    esac
done <"$unit"

if [ "$start_limit_section" = "[Unit]" ]; then
    note "StartLimitBurst is in [Unit]"
else
    fail "StartLimitBurst is in '${start_limit_section:-nowhere}', must be [Unit] or systemd ignores it"
fi

for directive in 'Restart=always' 'StartLimitBurst=5' 'StartLimitIntervalSec=600'; do
    if grep -qxF "$directive" "$unit"; then
        note "${directive} present"
    else
        fail "${directive} missing"
    fi
done

if grep -q 'ExecStart=.*run-runner.sh %i' "$unit"; then
    note "ExecStart calls run-runner.sh with the instance index"
else
    fail "ExecStart does not call run-runner.sh %i"
fi

config="$(find "$scratch" -name config -print -quit)"
if [ -n "$config" ] && grep -q '^RUNNER_CPUS=2$' "$config"; then
    note "config written with RUNNER_CPUS=2"
else
    fail "config missing or RUNNER_CPUS is not 2"
fi

if [ -n "$config" ] && grep -q 'morph-docker' "$config"; then
    fail "the retired morph-docker name/label is in the config"
else
    note "config carries no morph-docker name or label"
fi

wrapper="$(find "$scratch" -name run-runner.sh -print -quit)"
if [ -n "$wrapper" ] && [ -x "$wrapper" ]; then
    note "wrapper installed and executable"
else
    fail "wrapper not installed or not executable"
fi

# systemd's own opinion of the rendered unit.
if command -v systemd-analyze >/dev/null 2>&1; then
    staging="${scratch}/verify"
    mkdir -p "$staging"
    # Instantiate %i rather than verifying the template itself: systemd-analyze
    # cannot resolve %i in a bare foo@.service path, and would fail for that
    # reason rather than for any defect in the unit.
    sed 's/%i/1/g' "$unit" >"${staging}/lastrada-runner-verify.service"
    if out="$(systemd-analyze verify "${staging}/lastrada-runner-verify.service" 2>&1)"; then
        note "systemd-analyze verify accepts the unit"
    else
        # Missing ExecStart targets are expected in a scratch prefix; anything
        # else is a real defect in the unit.
        if printf '%s\n' "$out" | grep -vq 'Command .* is not executable'; then
            fail "systemd-analyze verify rejected the unit:"
            printf '%s\n' "$out" >&2
        else
            note "systemd-analyze verify clean apart from the scratch ExecStart path"
        fi
    fi
else
    note "systemd-analyze absent; skipping unit verification"
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%d check(s) failed\n' "$failures" >&2
    exit 1
fi
printf '\nall unit-installation checks passed\n'
