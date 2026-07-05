#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end REPL round trip against the real dispatcher: valid submits (in
# canonical and converted units), tab-separated input, the strict datetime
# codec rejecting garbage, and the model guarding missing required fields.
# Usage: test_repl.sh <path-to-morph_forms_demo>

set -eu
demo="$1"
out=$(printf '%s\n%s\n%s\t%s\n%s\n%s\n' \
    'ComputeDryDensity {"massDry":{"num":26505,"den":10,"dp":1},"volume":{"num":1,"den":1,"dp":3}}' \
    'ComputeDryDensity {"massDry":{"num":26505000,"den":10000,"dp":3},"volume":{"num":10000,"den":10000,"dp":4}}' \
    'RecordMeasurement' '{"sampleId":7,"measuredAt":"2026-07-05T14:30:00Z","density":{"num":5301,"den":2,"dp":1}}' \
    'RecordMeasurement {"sampleId":7,"measuredAt":"garbage","density":{"num":1,"den":1,"dp":1}}' \
    'RecordMeasurement {"sampleId":7}' \
    | "$demo")

echo "$out" | grep -q 'ok:  {"num":5301,"den":2,"dp":3}' || { echo "FAIL: canonical submit"; exit 1; }
echo "$out" | grep -q 'ok:  {"num":5301,"den":2,"dp":4}' || { echo "FAIL: converted-units submit"; exit 1; }
echo "$out" | grep -q 'sample 7 at 2026-07-05T14:30:00.000Z' || { echo "FAIL: tab-separated line"; exit 1; }
echo "$out" | grep -q 'err: .*syntax_error' || { echo "FAIL: malformed datetime not rejected"; exit 1; }
echo "$out" | grep -q 'err: RecordMeasurement: sampleId, measuredAt and density are required' \
    || { echo "FAIL: missing required not rejected"; exit 1; }
echo "repl round trip: all assertions passed"
