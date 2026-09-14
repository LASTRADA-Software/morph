#!/usr/bin/env bash
# Installs the systemd units that supervise the self-hosted runner fleet.
#
# Two modes, chosen by who runs it:
#
#   as a normal user -> user units under ~/.config/systemd/user, controlled
#                       with `systemctl --user`. Needs lingering
#                       (`loginctl enable-linger $USER`) to start at boot
#                       without a login session. This is the mode used on the
#                       maintainer's workstation, where `gh` is already
#                       authenticated for that user.
#   as root          -> system units under /etc/systemd/system. This is what
#                       bootstrap-cloud-node.sh uses on a fresh VM.
#
# LASTRADA_RUNNER_PREFIX_DIR redirects every install path under one directory,
# and LASTRADA_RUNNER_NO_SYSTEMCTL=1 skips the systemctl calls, so
# scripts/test_runner_units.sh can render and inspect the unit without
# touching the real system.
set -euo pipefail

readonly here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$(id -u)" -eq 0 ]; then
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
fi

if [ -n "${LASTRADA_RUNNER_PREFIX_DIR:-}" ]; then
    unit_dir="${LASTRADA_RUNNER_PREFIX_DIR}${unit_dir}"
    config_dir="${LASTRADA_RUNNER_PREFIX_DIR}${config_dir}"
    libexec_dir="${LASTRADA_RUNNER_PREFIX_DIR}${libexec_dir}"
fi

mkdir -p "$unit_dir" "$config_dir" "$libexec_dir"

install -m 0755 "${here}/run-runner.sh" "${libexec_dir}/run-runner.sh"

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

sed \
    -e "s|@LIBEXEC@|${libexec_dir}|g" \
    -e "s|@CONFIG@|${config_dir}/config|g" \
    -e "s|@PREFIX@|${RUNNER_NAME_PREFIX}|g" \
    -e "s|@WANTEDBY@|${wantedby}|g" \
    "${here}/lastrada-runner@.service.in" >"${unit_dir}/lastrada-runner@.service"
echo "wrote ${unit_dir}/lastrada-runner@.service"

if [ -n "${LASTRADA_RUNNER_NO_SYSTEMCTL:-}" ]; then
    echo "LASTRADA_RUNNER_NO_SYSTEMCTL set; not touching systemd"
    exit 0
fi

"${systemctl[@]}" daemon-reload

# Disable instances left over from a previously larger fleet, so shrinking
# RUNNER_COUNT actually shrinks it.
for existing in $("${systemctl[@]}" list-unit-files 'lastrada-runner@*.service' \
                    --no-legend --plain 2>/dev/null | awk '{print $1}'); do
    idx="${existing#lastrada-runner@}"
    idx="${idx%.service}"
    if [ "$idx" -gt "$RUNNER_COUNT" ] 2>/dev/null; then
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
