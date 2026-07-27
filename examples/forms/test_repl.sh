#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end REPL round trip against the real dispatcher: valid submits (in
# canonical and converted units), tab-separated input, the strict datetime
# codec rejecting garbage, the model guarding missing required fields, a
# dependent Choice's options action receiving the parent's value as its
# body, and the SamplesView CRUD action set (list/edit/delete/create, plus
# their validate()-guarded rejection paths).
# Usage: test_repl.sh <path-to-morph_forms_demo>

set -eu
demo="$1"
out=$(printf '%s\n%s\n%s\t%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n' \
    'ComputeDryDensity {"massDry":{"num":26505,"den":10,"dp":1},"volume":{"num":1,"den":1,"dp":3}}' \
    'ComputeDryDensity {"massDry":{"num":26505000,"den":10000,"dp":3},"volume":{"num":10000,"den":10000,"dp":4}}' \
    'RecordMeasurement' '{"sampleId":7,"measuredAt":"2026-07-05T14:30:00Z","density":{"num":5301,"den":2,"dp":1}}' \
    'RecordMeasurement {"sampleId":7,"measuredAt":"garbage","density":{"num":1,"den":1,"dp":1}}' \
    'RecordMeasurement {"sampleId":7}' \
    'ListCountries {}' \
    'ListCities {"country":1}' \
    'ShippingAddress {"country":1,"city":10}' \
    'EditSample {"id":1,"name":"Proctor A2"}' \
    'ListSamples {}' \
    'DeleteSample {"id":2}' \
    'CreateSample {}' \
    'EditSample {"id":999,"name":"Ghost"}' \
    'DeleteSample {"id":0}' \
    | "$demo")

# The first two inputs are the same measurement expressed in different units
# and decimal places, so the property under test is that they reduce to the
# *same* exact canonical result -- which means counting the matching lines, not
# grepping for one twice. (Two identical `grep -q` calls, which is what stood
# here, are satisfied by a single occurrence: the second asserted nothing, and
# the converted-units submit could have vanished entirely without failing.)
canonical_hits=$(echo "$out" | grep -c 'ok:  {"num":5301,"den":2,"dp":4}' || true)
[ "$canonical_hits" -eq 2 ] || {
    echo "FAIL: expected the canonical and the converted-units submit to yield the same exact result twice, got $canonical_hits matching line(s)"
    exit 1
}
echo "$out" | grep -q 'sample 7 at 2026-07-05T14:30:00.000Z' || { echo "FAIL: tab-separated line"; exit 1; }
echo "$out" | grep -q 'err: .*syntax_error' || { echo "FAIL: malformed datetime not rejected"; exit 1; }
echo "$out" | grep -q 'err: action failed validation: LabModel/RecordMeasurement' \
    || { echo "FAIL: missing required not rejected"; exit 1; }
echo "$out" | grep -q '"countries":\[{"id":1,"name":"Wonderland"},{"id":2,"name":"Narnia"}\]' \
    || { echo "FAIL: ListCountries (independent Choice options)"; exit 1; }
echo "$out" | grep -q '"cities":\[{"id":10,"name":"Looking-Glass City"},{"id":11,"name":"Tulgey Wood"}\]' \
    || { echo "FAIL: ListCities filtered by the parent country in its body"; exit 1; }
echo "$out" | grep -q '"summary":"ship to city 10 in country 1"' \
    || { echo "FAIL: ShippingAddress dispatch"; exit 1; }

echo "$out" | grep -q 'ok:  {"id":1,"name":"Proctor A2"}' || { echo "FAIL: EditSample did not rename"; exit 1; }
echo "$out" | grep -q '"samples":\[{"id":1,"name":"Proctor A2"}' \
    || { echo "FAIL: ListSamples did not reflect the rename (persistence, not just the edit's own reply)"; exit 1; }
echo "$out" | grep -q 'ok:  {"id":2}' || { echo "FAIL: DeleteSample did not ack the removed id"; exit 1; }
echo "$out" | grep -q '"id":100,"name":"New sample"' || { echo "FAIL: CreateSample did not append id 100"; exit 1; }
echo "$out" | grep -q 'err: EditSample: no sample with id 999' || { echo "FAIL: EditSample unknown id not rejected"; exit 1; }
echo "$out" | grep -q 'err: action failed validation: LabModel/DeleteSample' || { echo "FAIL: DeleteSample zero id not rejected"; exit 1; }

schemas=$("$demo" --schemas)
echo "$schemas" | grep -q 'x-optionsDependsOn' \
    || { echo "FAIL: x-optionsDependsOn not emitted for ShippingAddress.city"; exit 1; }

echo "repl round trip: all assertions passed"
