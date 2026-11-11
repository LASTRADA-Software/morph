#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Shoots lims's screens beside their mockups: each screen drawn by the example client on Qt Quick's offscreen platform
# at the expanded and the compact width over the mockups' scenario, and the mockup rendered by headless Firefox.
#
#   scripts/shoot_lims.sh OUT_DIR [CLIENT] [SCREEN...]
#
# CLIENT is the morph_lims_client binary (lims alone, without the other rungs), by default
# build/linux-everything/examples/client/morph_lims_client; pass `-` to take the default when naming screens. The
# script refuses a client older than any source under examples/lims or include/morph, since a grab from a stale client
# looks right and is not.
# SCREEN is a lims screen id; none names every screen of examples/lims/screens/grabs/*.tsv. Each row of those files is
# tab-separated: screen id, mockup file (without `.dc.html`), the scenario person who signs in, then the heights of the
# expanded grab, the compact grab and the mockup. Each shot runs in a fresh scratch directory with LIMS_SCENARIO=1. The
# script stops at the first shot that fails, naming it.

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $0 OUT_DIR [CLIENT] [SCREEN...]" >&2
    exit 2
fi
out_dir=$1
shift
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
client=$repo_root/build/linux-everything/examples/client/morph_lims_client
if [[ $# -ge 1 ]]; then
    if [[ $1 != - ]]; then
        client=$1
    fi
    shift
fi
if [[ ! -x $client ]]; then
    echo "$0: no client at $client; build morph_lims_client or name it" >&2
    exit 2
fi
newest=$(find "$repo_root/examples/lims" "$repo_root/include/morph" "$repo_root/examples/client" -type f \
    \( -name '*.cpp' -o -name '*.hpp' -o -name '*.json' -o -name '*.txt' -o -name '*.cmake' \) -newer "$client" \
    -print -quit)
if [[ -n $newest ]]; then
    echo "$0: $client is older than $newest; rebuild morph_lims_client" >&2
    exit 2
fi
mkdir -p "$out_dir"
out_dir=$(cd "$out_dir" && pwd)
mockups=$repo_root/examples/lims/design/mockups

# Rows are the grabs files' lines, in file order.
rows=()
for file in "$repo_root"/examples/lims/screens/grabs/*.tsv; do
    while IFS= read -r line || [[ -n $line ]]; do
        [[ -n $line ]] && rows+=("$line")
    done <"$file"
done

wanted=("$@")
for id in "${wanted[@]}"; do
    found=0
    for row in "${rows[@]}"; do
        [[ ${row%%$'\t'*} == "$id" ]] && found=1
    done
    if [[ $found -eq 0 ]]; then
        echo "$0: $id is in no grabs file" >&2
        exit 1
    fi
done
selected=0
for row in "${rows[@]}"; do
    IFS=$'\t' read -r screen mockup person expanded compact mockup_height <<<"$row"
    if [[ ${#wanted[@]} -gt 0 ]]; then
        match=0
        for id in "${wanted[@]}"; do
            [[ $id == "$screen" ]] && match=1
        done
        [[ $match -eq 1 ]] || continue
    fi
    selected=$((selected + 1))
    for width in expanded compact; do
        if [[ $width == expanded ]]; then
            size=1215x$expanded
        else
            size=390x$compact
        fi
        scratch=$(mktemp -d)
        if ! (cd "$scratch" && LIMS_SCENARIO=1 QT_QPA_PLATFORM=offscreen "$client" --ui=qt --app lims \
            --user "$person" --screen "$screen" --db "$scratch/app.db" --grab-size "$size" \
            --grab "$out_dir/$screen-$width.png"); then
            rm -rf "$scratch"
            echo "$0: the client failed on $screen at the $width width" >&2
            exit 1
        fi
        rm -rf "$scratch"
        echo "$out_dir/$screen-$width.png"
    done
    # A profile of its own: a Firefox that is already running owns the default one and refuses a second instance.
    profile=$(mktemp -d)
    rm -f "$out_dir/$mockup.png"
    firefox --headless --no-remote --profile "$profile" --window-size="1215,$mockup_height" \
        --screenshot "$out_dir/$mockup.png" "file://$mockups/$mockup.dc.html" >/dev/null 2>&1 || true
    rm -rf "$profile"
    if [[ ! -s $out_dir/$mockup.png ]]; then
        echo "$0: Firefox did not write the mockup $mockup" >&2
        exit 1
    fi
    echo "$out_dir/$mockup.png"
done
if [[ $selected -eq 0 ]]; then
    echo "$0: no screen of the grabs files matches ${wanted[*]:-(none)}" >&2
    exit 1
fi
