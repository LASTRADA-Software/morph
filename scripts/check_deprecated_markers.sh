#!/usr/bin/env bash
# Usage: bash scripts/check_deprecated_markers.sh [DIR...]
#
# Enforces docs/spec/VERSIONING.md's deprecation-window format: every
# `[[deprecated("...")]]` attribute in a scanned header must name both a
# target removal version and a replacement, in the exact shape
#
#   [[deprecated("removed in <major>.<minor>[.<patch>]; use <replacement> instead")]]
#
# e.g. [[deprecated("removed in 2.0.0; use morph::bridge::NewThing instead")]]
#
# Scans DIR (default: include/morph) recursively for *.hpp files. Exits 0 if
# every marker found matches the required shape (zero markers found is a
# pass -- nothing is deprecated yet at 0.1.0). Exits 1 and prints
# "file:line: <message>" for each offending marker on stderr otherwise.
#
# Requires GNU grep (-P, PCRE lookaround) -- this runs on the ubuntu-24.04 CI
# runner; not verified against BSD/macOS grep.
set -euo pipefail

readonly PATTERN='removed in [0-9]+\.[0-9]+(\.[0-9]+)?; use .+ instead'
dirs=("${@:-include/morph}")

status=0
while IFS= read -r -d '' file; do
    while IFS=: read -r lineno message; do
        [ -z "${message:-}" ] && continue
        if ! grep -Eq "$PATTERN" <<< "$message"; then
            echo "error: $file:$lineno: [[deprecated]] message must match 'removed in X.Y[.Z]; use <replacement> instead', got: $message" >&2
            status=1
        fi
    done < <(grep -noP '(?<=\[\[deprecated\(")[^"]*(?="\)\]\])' "$file" || true)
done < <(find "${dirs[@]}" -name '*.hpp' -print0)

exit "$status"
