---
id: 006
title: A forms schema is a pure function of the compiled action type, so per-instance data (a versioned analysis definition's precision and bounds) cannot reach the keys the framework itself enforces
subsystem: forms
severity: major
source: lims rung 6, build order §4 (review D4)
disposition: open
test: examples/lims/tests/test_schema_versioning.cpp
---

`morph::forms::schemaJson<A>()` is memoised per *type* and derives every key
from `A`'s compiled members: `x-decimalPlaces` from
`Quantity<U, D>::declaredDecimals`, `required` from the required-ness rule,
`x-rules` from `A::formRules`. There is no seam that lets a model serve a
schema whose framework-meaningful keys depend on the *row* the form is
about.

That is fine for an application whose forms are fixed at compile time. It is
not fine for one whose form definitions are data — which is precisely what a
versioned analysis catalogue is, and what `examples/lims/README.md` build
order §4 requires ("clients render the version the result was captured
with").

## Measured

`examples/lims/tests/test_schema_versioning.cpp`, case *"Only the version's
data varies; the framework-enforced parts are identical"*:

- Analysis version 1 declares 3 decimal places; version 2 declares 1.
- Both served forms carry `"x-decimalPlaces":3` — the value of the compiled
  `Quantity<LimsUnit::mg_per_L, 3>`'s template parameter.
- `x-decimalPlaces` is not decorative: `docs/spec/forms/forms.md`
  ("Advertised precision is enforced on dispatch") makes it a contract the
  framework applies to an incoming payload. So the key a lab operator's
  renderer honours says 3 for an analysis the lab defined at 1.

The rung works around this by serving a *second*, app-defined key
(`x-versionDecimalPlaces`) beside it and re-implementing the per-version
check as a hand-written model check against the version row
(`SampleModel::execute(CaptureConcentration)`). Overwriting `x-decimalPlaces`
in the DOM was rejected as a workaround: it would advertise a promise no
code in the framework keeps.

The same gap applies to the definition's spec range and detection limits.
They are served as app-defined `x-specLow`/`x-specHigh`/
`x-limitOfDetection`/`x-upperDetectionLimit` extras, and case *"The spec
range a served schema advertises is enforced by nobody"* pins the
consequence: a payload outside the served bound passes `validate()` (the
compiled rule vocabulary cannot name a bound that lives in a row) and is
stored unflagged.

A second, structural half of the same gap: because the schema is a function
of the type, there is **one compiled result-entry action per unit family,
not per analysis**. `CaptureConcentration` serves every mg/L analysis; an
analysis denominated in amps has no form at all, and
`AnalysisCatalogModel::execute(GetAnalysisSchema)` refuses rather than
serving a plausible-looking wrong one. A version that wanted a different
*shape* — one more field, a different rule — could not be served without
recompiling.

## What should happen

Some seam that lets a model supply per-instance schema data at serve time,
and that the dispatch-side enforcement then honours for that payload. The
cheapest shape that would close the precision half: let a `Quantity` field's
advertised and enforced decimals come from a value the model supplies
alongside the schema (an overlay merged by `mergeSchemaExtras`, keyed by
wire field name), rather than only from the template parameter — with the
dispatch-time precision check reading the same overlay. The bounds half
needs the rule vocabulary to be able to name a *served* literal rather than
only a compile-time one.

If the answer is instead "schemas are compiled, by design", that belongs in
`docs/spec/forms/forms.md`'s Limitations section explicitly, because rung 7
("runtime custom fields") is planned on the opposite assumption and
`examples/lims/README.md` already names this as the bridge toward it.
