#!/usr/bin/env bash
# Installs the systemd units that supervise the self-hosted runner fleet.
#
# Two modes, chosen by who runs it (or by LASTRADA_RUNNER_FORCE_SYSTEM, below):
#
#   as a normal user -> user units under ~/.config/systemd/user, controlled
#                       with `systemctl --user`. Needs lingering
#                       (`loginctl enable-linger $USER`) to start at boot
#                       without a login session -- this script checks that
#                       and enables it itself if it's off. This is the mode
#                       used on the maintainer's workstation, where `gh` is
#                       already authenticated for that user.
#   as root          -> system units under /etc/systemd/system. This is what
#                       bootstrap-cloud-node.sh uses on a fresh VM.
#
# LASTRADA_RUNNER_PREFIX_DIR redirects every install path under one directory,
# and LASTRADA_RUNNER_NO_SYSTEMCTL=1 skips the systemctl calls (and, see
# below, the lingering check too), so scripts/test_runner_units.sh can render
# and inspect the unit without touching the real system.
#
# LASTRADA_RUNNER_FORCE_SYSTEM=1 selects the root/system path regardless of
# the running user's actual uid, so scripts/test_runner_units.sh can render
# and assert the system-unit variant -- the one bootstrap-cloud-node.sh
# depends on -- without actually running as root.
set -euo pipefail

readonly here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$(id -u)" -eq 0 ] || [ -n "${LASTRADA_RUNNER_FORCE_SYSTEM:-}" ]; then
    unit_dir="/etc/systemd/system"
    config_dir="/etc/lastrada-runner"
    libexec_dir="/usr/local/libexec/lastrada-runner"
    wantedby="multi-user.target"
    systemctl=(systemctl)
else
    unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
    config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/lastrada-runner"
    libexec_dir="${XDG_DATA_HOME:-$HOME/.local/share}/lastrada-runner"
    wantedby="default.target"
    systemctl=(systemctl --user)

    # User units only start at boot without a login session if lingering is
    # enabled for this user; otherwise the whole fleet simply never comes
    # back after a reboot, silently. Irrelevant to the root/system path
    # above, where units are system-wide and lingering doesn't apply. Uses
    # `loginctl` rather than touching systemd state directly so this honours
    # a stubbed `loginctl` on PATH the same way the systemctl calls below
    # honour a stubbed `systemctl` -- see scripts/test_runner_units.sh.
    #
    # Skipped entirely under LASTRADA_RUNNER_NO_SYSTEMCTL: that flag's whole
    # contract is "render files, don't touch the real system", and
    # `loginctl` mutates real login-manager state just as much as `systemctl`
    # does. Checked *before* anything below tries to call `loginctl`, not
    # after -- this used to run unconditionally, ahead of the
    # LASTRADA_RUNNER_NO_SYSTEMCTL check further down, so isolation-mode
    # callers (including this script's own self-test) were mutating real
    # lingering state despite the flag, and an environment with no usable
    # login session (an ordinary GitHub Actions runner, for instance) could
    # fail `loginctl` outright and abort the install before any file was
    # rendered.
    #
    # Non-fatal even when it does run: a missing or failing `loginctl` must
    # not abort an otherwise-successful install, it should just warn loudly
    # that the fleet won't survive a reboot and say exactly what to run.
    if [ -n "${LASTRADA_RUNNER_NO_SYSTEMCTL:-}" ]; then
        echo "LASTRADA_RUNNER_NO_SYSTEMCTL set; skipping the lingering check (this mode must not touch loginctl either)"
    elif ! command -v loginctl >/dev/null 2>&1; then
        echo "warning: loginctl not found on PATH; cannot check or enable lingering for ${USER} -- this fleet will NOT survive a reboot until you run: loginctl enable-linger ${USER}" >&2
    else
        linger_state="$(loginctl show-user "$USER" --property=Linger 2>/dev/null || true)"
        if [ "$linger_state" != "Linger=yes" ]; then
            echo "lingering is off for ${USER}; enabling it (loginctl enable-linger ${USER}) so this fleet survives a reboot without a login session"
            if ! loginctl enable-linger "$USER" 2>/dev/null; then
                echo "warning: 'loginctl enable-linger ${USER}' failed -- this fleet will NOT survive a reboot until you run: loginctl enable-linger ${USER}" >&2
            fi
        else
            echo "lingering already enabled for ${USER}"
        fi
    fi
fi

if [ -n "${LASTRADA_RUNNER_PREFIX_DIR:-}" ]; then
    unit_dir="${LASTRADA_RUNNER_PREFIX_DIR}${unit_dir}"
    config_dir="${LASTRADA_RUNNER_PREFIX_DIR}${config_dir}"
    libexec_dir="${LASTRADA_RUNNER_PREFIX_DIR}${libexec_dir}"
fi

mkdir -p "$unit_dir" "$config_dir" "$libexec_dir"

install -m 0755 "${here}/run-runner.sh" "${libexec_dir}/run-runner.sh"
install -m 0755 "${here}/deregister-runner.sh" "${libexec_dir}/deregister-runner.sh"

# Never clobber a config an operator has tuned; config.example is the
# reference, the installed file is theirs.
if [ ! -e "${config_dir}/config" ]; then
    install -m 0600 "${here}/config.example" "${config_dir}/config"
    echo "wrote ${config_dir}/config"
else
    echo "kept existing ${config_dir}/config"
fi

# shellcheck disable=SC1091
. "${config_dir}/config"
: "${RUNNER_COUNT:?config must set RUNNER_COUNT}"
: "${RUNNER_NAME_PREFIX:?config must set RUNNER_NAME_PREFIX}"

# A non-numeric RUNNER_COUNT is not caught by the check above (":?" only
# rejects empty/unset), and feeding it to `seq` below is not caught by `set
# -e` either: a failed command substitution inside a `for ... in $(...)`
# word-list does not trip -e, so the enable loop below would silently run
# zero times while the script still reaches its success banner and exits 0 --
# a fleet of zero runners, reported as a successful install. Fail loudly here
# instead.
case "$RUNNER_COUNT" in
    ''|*[!0-9]*)
        echo "install-runner-units.sh: RUNNER_COUNT must be a positive integer, got '${RUNNER_COUNT}'" >&2
        exit 1
        ;;
esac
if [ "$RUNNER_COUNT" -eq 0 ]; then
    echo "install-runner-units.sh: RUNNER_COUNT must be a positive integer, got '${RUNNER_COUNT}'" >&2
    exit 1
fi

docker_bin="$(command -v docker || true)"
if [ -z "$docker_bin" ]; then
    echo "install-runner-units.sh: docker not found on PATH; cannot resolve @DOCKER@ for ExecStopPost" >&2
    exit 1
fi

sed \
    -e "s|@LIBEXEC@|${libexec_dir}|g" \
    -e "s|@CONFIG@|${config_dir}/config|g" \
    -e "s|@PREFIX@|${RUNNER_NAME_PREFIX}|g" \
    -e "s|@WANTEDBY@|${wantedby}|g" \
    -e "s|@DOCKER@|${docker_bin}|g" \
    "${here}/lastrada-runner@.service.in" >"${unit_dir}/lastrada-runner@.service"
echo "wrote ${unit_dir}/lastrada-runner@.service"

if [ -n "${LASTRADA_RUNNER_NO_SYSTEMCTL:-}" ]; then
    echo "LASTRADA_RUNNER_NO_SYSTEMCTL set; not touching systemd"
    exit 0
fi

"${systemctl[@]}" daemon-reload

# Disable instances left over from a previously larger fleet, so shrinking
# RUNNER_COUNT actually shrinks it.
#
# `systemctl list-unit-files 'lastrada-runner@*.service'` does NOT work for
# this: for a template unit, list-unit-files only ever shows the template
# itself (`lastrada-runner@.service`), never the instances enabled from it.
# Verified live against this exact fleet:
#
#   $ systemctl --user list-unit-files 'lastrada-runner@*.service' --no-legend --plain
#   lastrada-runner@.service indirect enabled
#
# ...while `~/.config/systemd/user/default.target.wants/` actually holds the
# five enablement symlinks lastrada-runner@1.service .. @5.service. The same
# is true of any templated unit (e.g. getty@.service vs. getty@tty1.service).
# `systemctl list-units --all` is not a substitute either -- it only shows
# *loaded* units, not everything enabled. So enumerate the enablement
# symlinks directly.
for existing_path in "${unit_dir}/${wantedby}.wants/lastrada-runner@"*.service; do
    [ -e "$existing_path" ] || continue
    existing="$(basename "$existing_path")"
    idx="${existing#lastrada-runner@}"
    idx="${idx%.service}"
    # The redirect below used to be the only guard against a non-numeric idx
    # ([ -gt ] on a non-integer would abort the script under `set -e`), but
    # this loop is inside `if`, which is already `set -e`-exempt -- so it did
    # not do what it appeared to. Skip non-numeric idx values explicitly
    # instead of relying on that.
    case "$idx" in
        ''|*[!0-9]*)
            echo "skipping ${existing}: index '${idx}' is not numeric" >&2
            continue
            ;;
    esac
    if [ "$idx" -gt "$RUNNER_COUNT" ]; then
        echo "disabling ${existing} (beyond RUNNER_COUNT=${RUNNER_COUNT})"
        "${systemctl[@]}" disable --now "$existing" || true
    fi
done

for i in $(seq 1 "$RUNNER_COUNT"); do
    "${systemctl[@]}" enable --now "lastrada-runner@${i}.service"
    echo "enabled lastrada-runner@${i}"
done

echo
echo "status:  ${systemctl[*]} status 'lastrada-runner@*'"
echo "logs:    journalctl${systemctl[1]:+ --user} -u 'lastrada-runner@1' -f"
