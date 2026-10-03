#!/usr/bin/env bash
# Usage:
#   bash scripts/check_tracy_capture.sh capture <tracy-capture> <tracy-csvexport> <out-dir> <zones> -- <program> [args...]
#   bash scripts/check_tracy_capture.sh assert <zones.csv> <zones>
#
# <zones> is a comma-separated list of zone names, e.g.
# "Bridge::executeVia,wire::decode".
#
# `capture` runs <program> under `tracy-capture`, exports the capture's zone
# statistics with `tracy-csvexport` to <out-dir>/<program-name>.csv, and then
# runs `assert` on that CSV. `assert` passes only when every named zone has a
# row in the CSV with a non-zero count.
#
# It fails closed. A capture that "succeeds" with no zones proves nothing, and
# every way of getting no zones is a failure here rather than a pass with an
# empty table: no CSV, a CSV with no rows, a header that is not the column
# layout this script reads, a named zone that is absent, a named zone whose
# count is zero, a program built without Tracy (the capture never connects
# and the wait below runs out), and a program or capture that does not exit.
#
# The program is expected to come from a build with MORPH_ENABLE_TRACY=ON and
# MORPH_TRACY_ON_DEMAND=OFF, and is run with TRACY_NO_EXIT=1: the client then
# records from the first instruction and, at exit, waits until the capture has
# drained it. With an on-demand client, whatever ran before `tracy-capture`
# connected would be missing, and whether a short program's zones appeared
# would depend on that race.
set -euo pipefail

readonly wait_seconds="${MORPH_TRACY_CAPTURE_WAIT:-600}"

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

# Waits for <pid> to exit, for at most <seconds>. Returns the process's own
# exit status, or kills it and returns 124 if it has not exited by then.
wait_bounded() {
    local pid="$1" seconds="$2" what="$3"
    local waited=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$waited" -ge "$seconds" ]; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            printf 'error: %s did not exit within %ss; killed it\n' "$what" "$seconds" >&2
            return 124
        fi
        sleep 1
        waited=$((waited + 1))
    done
    local status=0
    wait "$pid" || status=$?
    return "$status"
}

assert_zones() {
    local csv="$1" zones="$2"
    [ -f "$csv" ] || die "no CSV at ${csv}: the export produced nothing"
    [ -n "$zones" ] || die "no zone names given: an empty list would pass any capture"

    # The column layout `tracy-csvexport` writes without -u; `counts` is the
    # sixth column. A different header means this parser would read the wrong
    # column, so it is a failure, not something to guess around.
    local expected_header="name,src_file,src_line,total_ns,total_perc,counts"
    local header
    header="$(head -n 1 "$csv")"
    case "$header" in
        "${expected_header}"*) ;;
        *) die "unexpected CSV header in ${csv}: '${header}' (expected it to start with '${expected_header}')" ;;
    esac

    local rows
    rows="$(($(wc -l < "$csv") - 1))"
    [ "$rows" -gt 0 ] || die "${csv} has a header and no rows: the capture recorded no zones at all"
    printf 'ok: %s holds %d zone rows\n' "$csv" "$rows"

    local failures=0 zone count
    local IFS=','
    for zone in $zones; do
        # One name can have several rows (one per source location); a zone
        # counts as present when their counts sum to more than zero.
        count="$(awk -F',' -v zone="$zone" 'NR > 1 && $1 == zone { sum += $6 } END { print sum + 0 }' "$csv")"
        if [ "$count" -gt 0 ]; then
            printf 'ok: zone %-40s %s\n' "$zone" "$count"
        else
            printf 'error: zone %s has no events in %s\n' "$zone" "$csv" >&2
            failures=$((failures + 1))
        fi
    done
    [ "$failures" -eq 0 ] || die "${failures} named zone(s) missing from ${csv}"
}

capture() {
    [ "$#" -ge 6 ] || die "capture needs: <tracy-capture> <tracy-csvexport> <out-dir> <zones> -- <program> [args...]"
    local capture_bin="$1" csvexport_bin="$2" out_dir="$3" zones="$4"
    shift 4
    [ "$1" = "--" ] || die "expected '--' before the program"
    shift
    local program="$1"
    [ -x "$program" ] || die "program ${program} is not an executable file"

    mkdir -p "$out_dir"
    local name trace csv
    name="$(basename "$program")"
    trace="${out_dir}/${name}.tracy"
    csv="${out_dir}/${name}.csv"
    rm -f "$trace" "$csv"

    "$capture_bin" -o "$trace" -a 127.0.0.1 -f > "${out_dir}/${name}.capture.log" 2>&1 &
    local capture_pid=$!

    TRACY_NO_EXIT=1 "$@" > "${out_dir}/${name}.program.log" 2>&1 &
    local program_pid=$!

    local program_status=0
    wait_bounded "$program_pid" "$wait_seconds" "${name}" || program_status=$?
    local capture_status=0
    wait_bounded "$capture_pid" 120 "tracy-capture" || capture_status=$?

    printf '── %s (exit %d) ──\n' "$name" "$program_status"
    tail -n 20 "${out_dir}/${name}.program.log"
    printf '── tracy-capture (exit %d) ──\n' "$capture_status"
    tail -n 20 "${out_dir}/${name}.capture.log"

    [ "$program_status" -eq 0 ] || die "${name} exited ${program_status}"
    [ "$capture_status" -eq 0 ] || die "tracy-capture exited ${capture_status}"
    [ -s "$trace" ] || die "tracy-capture wrote no trace at ${trace}"

    "$csvexport_bin" "$trace" > "$csv" || die "tracy-csvexport failed on ${trace}"
    assert_zones "$csv" "$zones"
}

[ "$#" -ge 1 ] || die "usage: $0 capture|assert ..."
mode="$1"
shift
case "$mode" in
    capture) capture "$@" ;;
    assert)
        [ "$#" -eq 2 ] || die "assert needs: <zones.csv> <zones>"
        assert_zones "$1" "$2"
        ;;
    *) die "unknown mode '${mode}' (expected capture or assert)" ;;
esac
