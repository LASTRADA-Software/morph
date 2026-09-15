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
#
# Also covers deregister-runner.sh, since it is installed by
# install-runner-units.sh and wired in by ExecStop=-@LIBEXEC@/deregister-runner.sh
# %i in the unit template: that its path is installed and its ExecStop=
# actually renders with the substituted path, and that its own GitHub
# id-resolution logic (against a stub gh) picks the right id, deletes
# nothing for an absent name, and never fails the stop even when gh does.
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

# StartLimit* must be in [Unit]; systemd ignores them in [Service]. Track
# both directives independently -- a mutation that moves only one of the two
# must still be caught.
section=""
start_limit_burst_section=""
start_limit_interval_section=""
while IFS= read -r line; do
    case "$line" in
        \[*\]) section="$line" ;;
        StartLimitBurst=*) start_limit_burst_section="$section" ;;
        StartLimitIntervalSec=*) start_limit_interval_section="$section" ;;
    esac
done <"$unit"

if [ "$start_limit_burst_section" = "[Unit]" ]; then
    note "StartLimitBurst is in [Unit]"
else
    fail "StartLimitBurst is in '${start_limit_burst_section:-nowhere}', must be [Unit] or systemd ignores it"
fi

if [ "$start_limit_interval_section" = "[Unit]" ]; then
    note "StartLimitIntervalSec is in [Unit]"
else
    fail "StartLimitIntervalSec is in '${start_limit_interval_section:-nowhere}', must be [Unit] or systemd ignores it"
fi

for directive in 'Restart=always' 'StartLimitBurst=12' 'StartLimitIntervalSec=900'; do
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

deregister_wrapper="$(find "$scratch" -name deregister-runner.sh -print -quit)"
if [ -n "$deregister_wrapper" ] && [ -x "$deregister_wrapper" ]; then
    note "deregister-runner.sh installed and executable"
else
    fail "deregister-runner.sh not installed or not executable"
fi

# ExecStop must render with the actual substituted @LIBEXEC@ path, not just
# be free of leftover @...@ markers in general (the generic placeholder scan
# above would not catch a forgotten ExecStop= line entirely, since a missing
# line leaves no placeholder behind to find).
if [ -n "$deregister_wrapper" ] && grep -qxF "ExecStop=-${deregister_wrapper} %i" "$unit"; then
    note "ExecStop renders with the substituted deregister-runner.sh path"
else
    fail "ExecStop missing or not correctly substituted (expected 'ExecStop=-${deregister_wrapper} %i')"
fi

# systemd's own opinion of the rendered unit.
if command -v systemd-analyze >/dev/null 2>&1; then
    staging="${scratch}/verify"
    mkdir -p "$staging"
    # Instantiate %i rather than verifying the template itself: systemd-analyze
    # cannot resolve %i in a bare foo@.service path, and would fail for that
    # reason rather than for any defect in the unit.
    sed 's/%i/1/g' "$unit" >"${staging}/lastrada-runner-verify.service"
    # Exit status alone does not gate this: a directive in the wrong section
    # (e.g. StartLimitIntervalSec under [Service]) makes systemd-analyze print
    # "Unknown key ... ignoring." to stderr and still exit 0. So the *output*
    # is the signal, checked regardless of exit status -- and the one kind of
    # noise this scratch prefix legitimately produces (ExecStart pointing at
    # a wrapper that was not actually installed there) is carved out by
    # matching only that specific message, so an "Unknown key" line -- or
    # anything else systemd-analyze complains about -- is never swallowed by
    # the carve-out.
    out="$(systemd-analyze verify "${staging}/lastrada-runner-verify.service" 2>&1)" || true
    unexpected="$(printf '%s\n' "$out" | grep -v 'Command .* is not executable' || true)"
    if [ -n "$unexpected" ]; then
        fail "systemd-analyze verify complained about the unit:"
        printf '%s\n' "$unexpected" >&2
    else
        note "systemd-analyze verify clean apart from the scratch ExecStart path"
    fi
else
    note "systemd-analyze absent; skipping unit verification"
fi

# The systemctl-driving code -- daemon-reload, the shrink loop, `enable --now`
# -- is never reached above: that run always sets LASTRADA_RUNNER_NO_SYSTEMCTL,
# which returns before any of it. Exercise it here against a stub systemctl
# placed first on PATH that only logs its argv and exits 0. This is the only
# systemctl the installer can reach in this invocation, so nothing here can
# touch the real system.
#
# Crucially, `list-unit-files 'lastrada-runner@*.service'` is NOT how the
# installer finds existing instances (see install-runner-units.sh for why:
# for a template unit, that command only ever shows the template itself,
# never the enabled instances). So the stub must not emit fabricated
# per-instance list-unit-files rows -- the real systemctl never does. It
# instead prints the *template's* row, exactly as verified live:
#
#   $ systemctl --user list-unit-files 'lastrada-runner@*.service' --no-legend --plain
#   lastrada-runner@.service indirect enabled
#
# The real fleet state instead lives as enablement symlinks under
# <unit_dir>/<wantedby>.wants/. So this test represents "a previously larger
# fleet" the same way: it pre-creates those symlinks in the scratch prefix
# before running the installer.
stub_bin="$(mktemp -d)"
scratch2="$(mktemp -d)"
trap 'rm -rf "$scratch" "$stub_bin" "$scratch2"' EXIT

systemctl_log="${stub_bin}/systemctl.log"
: >"$systemctl_log"
cat >"${stub_bin}/systemctl" <<STUBEOF
#!/usr/bin/env bash
printf '%s\n' "\$*" >>"${systemctl_log}"
args=("\$@")
if [ "\${args[0]:-}" = "--user" ]; then
    args=("\${args[@]:1}")
fi
if [ "\${args[0]:-}" = "list-unit-files" ]; then
    # This is what the real systemctl prints for a templated unit: only the
    # template's own row, never per-instance rows. If the installer were
    # still (mis)using this command to find instances to disable, it would
    # see nothing here and the shrink assertions below would fail.
    printf 'lastrada-runner@.service indirect enabled\n'
fi
exit 0
STUBEOF
chmod +x "${stub_bin}/systemctl"

# Pre-seed enablement symlinks representing a previously larger fleet:
# instances 1..5 within the config.example default RUNNER_COUNT=5 (must be
# left alone), plus 6 and 7 beyond it (must be disabled). These live under
# <unit_dir>/<wantedby>.wants/, which for a non-root run under
# LASTRADA_RUNNER_PREFIX_DIR is
# "${scratch2}${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/default.target.wants".
wants_dir="${scratch2}${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/default.target.wants"
mkdir -p "$wants_dir"
for i in 1 2 3 4 5 6 7; do
    ln -s "../lastrada-runner@.service" "${wants_dir}/lastrada-runner@${i}.service"
done

if ! PATH="${stub_bin}:${PATH}" LASTRADA_RUNNER_PREFIX_DIR="$scratch2" \
        bash "$installer" >/dev/null 2>"${stub_bin}/installer.stderr"; then
    fail "installer exited non-zero against the stubbed systemctl:"
    cat "${stub_bin}/installer.stderr" >&2
fi

# The stub logs "--user daemon-reload" for a non-root run and plain
# "daemon-reload" for a root one; normalize away the leading --user so the
# assertions below do not care which mode ran.
normalized_log="${stub_bin}/systemctl.normalized.log"
sed -e 's/^--user //' "$systemctl_log" >"$normalized_log"

if grep -qxF 'daemon-reload' "$normalized_log"; then
    note "daemon-reload was called"
else
    fail "daemon-reload was not called"
fi

enable_missing=0
for i in $(seq 1 5); do
    if ! grep -qxF "enable --now lastrada-runner@${i}.service" "$normalized_log"; then
        enable_missing=1
        fail "enable --now lastrada-runner@${i}.service was not called"
    fi
done
if [ "$enable_missing" -eq 0 ]; then
    note "enable --now called for lastrada-runner@1..5"
fi

shrink_missing=0
for i in 6 7; do
    if ! grep -qxF "disable --now lastrada-runner@${i}.service" "$normalized_log"; then
        shrink_missing=1
        fail "disable --now lastrada-runner@${i}.service (beyond RUNNER_COUNT) was not called"
    fi
done
if [ "$shrink_missing" -eq 0 ]; then
    note "disable --now called for the instances beyond RUNNER_COUNT (6, 7)"
fi

in_range_touched=0
for i in 1 2 3 4 5; do
    if grep -qxF "disable --now lastrada-runner@${i}.service" "$normalized_log"; then
        in_range_touched=1
        fail "disable --now was called for lastrada-runner@${i}.service, which is within RUNNER_COUNT"
    fi
done
if [ "$in_range_touched" -eq 0 ]; then
    note "the in-range instances (1..5) were left alone"
fi

# ── both install modes: user vs. system ──────────────────────────────────────
# Everything above ran the installer as whoever is running this script --
# normally a non-root developer or a CI job -- so it only ever exercised the
# non-root branch of install-runner-units.sh: ~/.config/systemd/user,
# default.target, `systemctl --user`. bootstrap-cloud-node.sh depends on the
# OTHER branch -- the root/system one taken when `id -u` is 0: /etc/systemd/
# system, /etc/lastrada-runner, /usr/local/libexec/lastrada-runner,
# multi-user.target, plain `systemctl` with no --user -- and nothing above
# ever rendered or asserted it. A change that silently broke that branch (the
# wrong WantedBy, or system mode reusing a user-mode path) would ship
# unnoticed with every check above still green.
#
# install-runner-units.sh honours LASTRADA_RUNNER_FORCE_SYSTEM=1 precisely so
# this can be exercised without actually running as root: it forces the
# root/system branch regardless of the real uid, defaulting to the ordinary
# UID-based detection when unset (as every check above relied on).

# First, confirm the render already done at the top of this script (no
# LASTRADA_RUNNER_FORCE_SYSTEM set, run as this non-root user) actually took
# the user-mode branch, not just that *a* unit came out the other end. This
# does not weaken or replace any assertion already made against $unit/$config
# above -- it adds mode-specific checks on the same render.
case "$unit" in
    "$scratch"*/systemd/user/lastrada-runner@.service) note "user mode: unit rendered under systemd/user" ;;
    *) fail "user mode: unit not rendered under systemd/user (got ${unit})" ;;
esac
case "$config" in
    "$scratch"*/lastrada-runner/config) note "user mode: config rendered under .../lastrada-runner, not /etc" ;;
    *) fail "user mode: config not rendered under .../lastrada-runner (got ${config})" ;;
esac
case "$wrapper" in
    "$scratch"*/lastrada-runner/run-runner.sh) note "user mode: wrapper rendered under a user data dir, not /usr/local/libexec" ;;
    *) fail "user mode: wrapper not rendered under a .../lastrada-runner data dir, or wrongly under /usr/local/libexec (got ${wrapper})" ;;
esac
if grep -qxF 'WantedBy=default.target' "$unit"; then
    note "user mode: WantedBy=default.target"
else
    fail "user mode: WantedBy=default.target missing (got $(grep '^WantedBy=' "$unit" || echo 'nothing'))"
fi

# Now render the system variant fresh, forced via LASTRADA_RUNNER_FORCE_SYSTEM
# so this does not need to actually run as root, into its own scratch prefix
# so it cannot be confused with the user-mode render above.
system_scratch="$(mktemp -d)"
trap 'rm -rf "$scratch" "$stub_bin" "$scratch2" "$system_scratch"' EXIT

LASTRADA_RUNNER_PREFIX_DIR="$system_scratch" LASTRADA_RUNNER_NO_SYSTEMCTL=1 \
    LASTRADA_RUNNER_FORCE_SYSTEM=1 \
    bash "$installer" >/dev/null

system_unit="$(find "$system_scratch" -name 'lastrada-runner@.service' -print -quit)"
if [ -z "$system_unit" ]; then
    fail "system mode: no lastrada-runner@.service was rendered"
else
    case "$system_unit" in
        "${system_scratch}/etc/systemd/system/lastrada-runner@.service") note "system mode: unit rendered under /etc/systemd/system" ;;
        *) fail "system mode: unit not rendered under /etc/systemd/system (got ${system_unit})" ;;
    esac
    if grep -qxF 'WantedBy=multi-user.target' "$system_unit"; then
        note "system mode: WantedBy=multi-user.target"
    else
        fail "system mode: WantedBy=multi-user.target missing (got $(grep '^WantedBy=' "$system_unit" || echo 'nothing'))"
    fi
fi

system_config="$(find "$system_scratch" -name config -print -quit)"
case "$system_config" in
    "${system_scratch}/etc/lastrada-runner/config") note "system mode: config rendered under /etc/lastrada-runner" ;;
    *) fail "system mode: config not rendered under /etc/lastrada-runner (got ${system_config:-nothing})" ;;
esac

system_wrapper="$(find "$system_scratch" -name run-runner.sh -print -quit)"
case "$system_wrapper" in
    "${system_scratch}/usr/local/libexec/lastrada-runner/run-runner.sh")
        if [ -x "$system_wrapper" ]; then
            note "system mode: wrapper rendered under /usr/local/libexec/lastrada-runner"
        else
            fail "system mode: wrapper at the expected path is not executable (${system_wrapper})"
        fi
        ;;
    *) fail "system mode: wrapper not rendered under /usr/local/libexec/lastrada-runner (got ${system_wrapper:-nothing})" ;;
esac

# Finally, confirm the systemctl invocation itself differs between the two
# modes: user mode drives `systemctl --user ...`, system mode drives plain
# `systemctl ...`. Re-exercise system mode against a stub systemctl on PATH
# (the only "systemctl" this invocation can reach), mirroring the user-mode
# exercise above -- that one's normalized log already proves user mode's raw
# invocations carried a leading "--user" (stripped before its assertions by
# `sed -e 's/^--user //'`); this proves system mode's raw invocations do not.
system_stub_bin="$(mktemp -d)"
system_stub_scratch="$(mktemp -d)"
trap 'rm -rf "$scratch" "$stub_bin" "$scratch2" "$system_scratch" "$system_stub_bin" "$system_stub_scratch"' EXIT

system_systemctl_log="${system_stub_bin}/systemctl.log"
: >"$system_systemctl_log"
cat >"${system_stub_bin}/systemctl" <<STUBEOF
#!/usr/bin/env bash
printf '%s\n' "\$*" >>"${system_systemctl_log}"
if [ "\${1:-}" = "list-unit-files" ]; then
    printf 'lastrada-runner@.service indirect enabled\n'
fi
exit 0
STUBEOF
chmod +x "${system_stub_bin}/systemctl"

if ! PATH="${system_stub_bin}:${PATH}" LASTRADA_RUNNER_PREFIX_DIR="$system_stub_scratch" \
        LASTRADA_RUNNER_FORCE_SYSTEM=1 \
        bash "$installer" >/dev/null 2>"${system_stub_bin}/installer.stderr"; then
    fail "installer exited non-zero in forced-system mode against the stubbed systemctl:"
    cat "${system_stub_bin}/installer.stderr" >&2
fi

if grep -qxF 'daemon-reload' "$system_systemctl_log"; then
    note "system mode: systemctl invoked as plain 'systemctl' (no --user)"
else
    fail "system mode: expected a plain 'daemon-reload' call (no --user) in the systemctl log: $(cat "$system_systemctl_log")"
fi
if grep -q -- '--user' "$system_systemctl_log"; then
    fail "system mode: systemctl was invoked with --user, which is the user-mode invocation, not system"
else
    note "system mode: no --user ever appeared in the systemctl invocations"
fi
if grep -qxF -- '--user daemon-reload' "$systemctl_log"; then
    note "user mode: systemctl invoked as 'systemctl --user' (raw log carried --user)"
else
    fail "user mode: expected the raw systemctl log to carry a leading --user (got: $(cat "$systemctl_log"))"
fi

# ── deregister-runner.sh: GitHub id resolution ───────────────────────────────
# ExecStop above runs deregister-runner.sh; exercise its id-resolution logic
# against a stub gh, the same way scripts/test_run_runner.sh exercises
# run-runner.sh's token minting. Reuses the config install-runner-units.sh
# wrote earlier in this script (GITHUB_ORG=LASTRADA-Software,
# RUNNER_NAME_PREFIX=lastrada-docker), so index 3 resolves to the name
# "lastrada-docker-3".
readonly deregister="${repo_root}/.github/self-hosted-runner/deregister-runner.sh"

dereg_scratch="$(mktemp -d)"
trap 'rm -rf "$scratch" "$stub_bin" "$scratch2" "$system_scratch" "$system_stub_bin" "$system_stub_scratch" "$dereg_scratch"' EXIT

cat >"${dereg_scratch}/gh" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"${DEREG_GH_LOG}"
if [ "${DEREG_GH_FAIL:-}" = "1" ]; then
    exit 1
fi
if [ "$1" = "api" ] && [ "${3:-}" = "--jq" ]; then
    if [ -n "${DEREG_RUNNER_NAME:-}" ] && printf '%s\n' "$4" | grep -qF "\"${DEREG_RUNNER_NAME}\""; then
        printf '%s\n' "${DEREG_RUNNER_ID:-9001}"
    fi
fi
exit 0
STUB
chmod +x "${dereg_scratch}/gh"

export DEREG_GH_LOG="${dereg_scratch}/gh.log"

# Resolves the right id for an existing name.
: >"$DEREG_GH_LOG"
out="$(DEREG_RUNNER_NAME=lastrada-docker-3 DEREG_RUNNER_ID=9001 \
    LASTRADA_RUNNER_CONFIG="$config" LASTRADA_RUNNER_GH="${dereg_scratch}/gh" \
    LASTRADA_RUNNER_DRY_RUN=1 \
    bash "$deregister" 3 2>&1)" && rc=0 || rc=$?
if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -q '9001'; then
    note "deregister-runner.sh resolves the id of an existing runner"
else
    fail "deregister-runner.sh did not resolve id 9001 for lastrada-docker-3 (exit ${rc}); got: ${out}"
fi

# Exits 0 and deletes nothing when the name is absent.
: >"$DEREG_GH_LOG"
out="$(DEREG_RUNNER_NAME=some-other-runner DEREG_RUNNER_ID=9002 \
    LASTRADA_RUNNER_CONFIG="$config" LASTRADA_RUNNER_GH="${dereg_scratch}/gh" \
    bash "$deregister" 3 2>&1)" && rc=0 || rc=$?
if [ "$rc" -ne 0 ]; then
    fail "deregister-runner.sh exited ${rc} for an absent runner, must exit 0"
elif grep -q -- '-X DELETE' "$DEREG_GH_LOG"; then
    fail "deregister-runner.sh issued a DELETE for a runner that was never resolved: $(cat "$DEREG_GH_LOG")"
else
    note "deregister-runner.sh exits 0 and deletes nothing when the name is absent"
fi

# Exits 0 even when gh fails outright.
: >"$DEREG_GH_LOG"
out="$(DEREG_GH_FAIL=1 \
    LASTRADA_RUNNER_CONFIG="$config" LASTRADA_RUNNER_GH="${dereg_scratch}/gh" \
    bash "$deregister" 3 2>&1)" && rc=0 || rc=$?
if [ "$rc" -eq 0 ]; then
    note "deregister-runner.sh exits 0 when gh fails"
else
    fail "deregister-runner.sh exited ${rc} when gh failed, must exit 0"
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%d check(s) failed\n' "$failures" >&2
    exit 1
fi
printf '\nall unit-installation checks passed\n'
