#!/usr/bin/env bash
# Usage: bash scripts/test_check_tracy_capture.sh
#
# Self-test for the assertion half of scripts/check_tracy_capture.sh, the gate
# the nightly Tracy capture job ends in. Every way a capture can carry no
# evidence -- no CSV, no rows, a different column layout, a named zone absent
# or counted zero, no zone named at all -- is fed to it from synthetic CSVs and
# must be refused for the stated reason, not merely with a nonzero exit; one
# CSV holding every named zone must pass. The capture half needs a profiled
# program and is exercised by the job itself.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly repo_root
readonly checker="${repo_root}/scripts/check_tracy_capture.sh"

failures=0
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

readonly header="name,src_file,src_line,total_ns,total_perc,counts,mean_ns,min_ns,max_ns,std_ns"

# expect_fail <case> <expected stderr fragment> <csv> <zones>
expect_fail() {
    local name="$1" fragment="$2" csv="$3" zones="$4"
    local out status=0
    out="$(bash "$checker" assert "$csv" "$zones" 2>&1)" || status=$?
    if [ "$status" -eq 0 ]; then
        printf 'error: %s: the checker passed\n%s\n' "$name" "$out" >&2
        failures=$((failures + 1))
    elif ! grep -qF -- "$fragment" <<<"$out"; then
        printf 'error: %s: failed, but not for "%s":\n%s\n' "$name" "$fragment" "$out" >&2
        failures=$((failures + 1))
    else
        printf 'ok: %s\n' "$name"
    fi
}

expect_pass() {
    local name="$1" csv="$2" zones="$3"
    local out
    if out="$(bash "$checker" assert "$csv" "$zones" 2>&1)"; then
        printf 'ok: %s\n' "$name"
    else
        printf 'error: %s: the checker refused a complete capture:\n%s\n' "$name" "$out" >&2
        failures=$((failures + 1))
    fi
}

complete="${scratch}/complete.csv"
cat > "$complete" <<EOF
${header}
Bridge::executeVia,bridge.hpp,1283,1000,1.0,42,23,1,90,4
wire::decode,wire.hpp,684,500,0.5,7,71,1,90,4
EOF

expect_pass "a CSV holding every named zone passes" "$complete" "Bridge::executeVia,wire::decode"

split="${scratch}/split.csv"
cat > "$split" <<EOF
${header}
Bridge::executeVia,bridge.hpp,1283,1000,1.0,0,0,0,0,0
Bridge::executeVia,bridge.hpp,1324,1000,1.0,3,23,1,90,4
EOF
expect_pass "counts of one name at several source locations are summed" "$split" "Bridge::executeVia"

expect_fail "no CSV" "no CSV at" "${scratch}/absent.csv" "Bridge::executeVia"

headerOnly="${scratch}/header_only.csv"
printf '%s\n' "$header" > "$headerOnly"
expect_fail "a header with no rows" "has a header and no rows" "$headerOnly" "Bridge::executeVia"

empty="${scratch}/empty.csv"
: > "$empty"
expect_fail "an empty file" "unexpected CSV header" "$empty" "Bridge::executeVia"

unwrapped="${scratch}/unwrapped.csv"
cat > "$unwrapped" <<EOF
name,src_file,src_line,ns_since_start,exec_time_ns,thread,value
Bridge::executeVia,bridge.hpp,1283,1000,10,1,
EOF
expect_fail "a different column layout" "unexpected CSV header" "$unwrapped" "Bridge::executeVia"

expect_fail "a named zone that is absent" "zone ModelStrands::task has no events" "$complete" \
    "Bridge::executeVia,ModelStrands::task"

zeroed="${scratch}/zeroed.csv"
cat > "$zeroed" <<EOF
${header}
Bridge::executeVia,bridge.hpp,1283,0,0.0,0,0,0,0,0
EOF
expect_fail "a named zone counted zero" "zone Bridge::executeVia has no events" "$zeroed" "Bridge::executeVia"

prefix="${scratch}/prefix.csv"
cat > "$prefix" <<EOF
${header}
Bridge::executeViaSomethingElse,bridge.hpp,1283,1000,1.0,42,23,1,90,4
EOF
expect_fail "a zone whose name only starts with the named one" "zone Bridge::executeVia has no events" \
    "$prefix" "Bridge::executeVia"

expect_fail "no zone named" "no zone names given" "$complete" ""

if [ "$failures" -ne 0 ]; then
    printf 'error: %d case(s) failed\n' "$failures" >&2
    exit 1
fi
printf 'ok: every case behaved\n'
