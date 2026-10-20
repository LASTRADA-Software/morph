#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Shoots the screens the layout vocabulary was checked against: the bank, lims and bookmarks sign-ins, bank.move and
# bookmarks.main, each drawn by the example client on Qt Quick's offscreen platform and saved with `--grab`.
#
#   scripts/shoot_screens.sh OUT_DIR [CLIENT]
#
# CLIENT is the morph_examples_client binary, by default build/clang-debug/examples/client/morph_examples_client. Each
# shot runs in a fresh scratch directory, so a local application's database starts empty. The script stops at the
# first shot the client fails, naming it.

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 OUT_DIR [CLIENT]" >&2
    exit 2
fi
out_dir=$1
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
client=${2:-$repo_root/build/clang-debug/examples/client/morph_examples_client}
if [[ ! -x $client ]]; then
    echo "$0: no client at $client; build morph_examples_client or name it" >&2
    exit 2
fi
mkdir -p "$out_dir"
out_dir=$(cd "$out_dir" && pwd)

# name, then the client's arguments.
shots=(
    "bank-signin|--app bank"
    "lims-signin|--app lims"
    "bookmarks-signin|--app bookmarks"
    "bank-move|--app bank --user alice --screen bank.move"
    "bookmarks-main|--app bookmarks --user alice --screen bookmarks.main"
)

for shot in "${shots[@]}"; do
    name=${shot%%|*}
    read -r -a arguments <<<"${shot#*|}"
    scratch=$(mktemp -d)
    if ! (cd "$scratch" && QT_QPA_PLATFORM=offscreen "$client" --ui=qt "${arguments[@]}" \
        --db "$scratch/app.db" --grab "$out_dir/$name.png"); then
        rm -rf "$scratch"
        echo "$0: the client failed on $name (${arguments[*]})" >&2
        exit 1
    fi
    rm -rf "$scratch"
    echo "$out_dir/$name.png"
done
