# Schema-driven forms demo

Actions describe themselves as JSON Schemas; clients render forms from those
schemas at runtime and submit exact, unit-tagged values back to the model.
This example runs the whole loop locally with **two renderers driven by the
same generated schemas** — nothing about the forms is hardcoded in either
client.

```
lab_units.hpp     the application's unit system: enum + UnitTraits metadata
                  + consteval algebra (kg / m3 -> kg_per_m3 at compile time)
lab_model.hpp     actions (Quantity fields, optionalFields opt-out, validate())
                  + LabModel + glz::json_schema descriptions + registration
lab_wizard.hpp    IntakeWizard (a two-step flow over lab_model.hpp's actions)
                  + LabApp, the app-shell descriptor (menu -> screens)
lab_schemas.hpp   {actionType: schema}, {wizardId: schema}, the app-* document,
                  and SamplesView (a list/master-detail view over ListSamples)
                  -- every client-facing schema the demo serves, in one place
main.cpp          console demo: --schemas | --emit-html | REPL
screens/          the same actions, wizard and view as server-defined screens
```

Suggested reading order: `lab_units.hpp` → `lab_model.hpp` → `lab_wizard.hpp` →
`lab_schemas.hpp` — each only names types the previous file already declared.

## Build

```sh
cmake -B build -G Ninja -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON
ninja -C build morph_forms_demo morph_examples_client
```

## 1. Look at the schemas

```sh
./build/examples/forms/morph_forms_demo --schemas
```

Every property carries what a renderer needs: `description` and bounds (from
`glz::json_schema<A>`), `ExtUnits` and `x-decimalPlaces` (from the `Quantity`
types), `x-order` (declaration order), and the object carries a derived
`required` array — `moisture` is missing from it because the action lists it
in `optionalFields`, `note` because it is `std::optional`:

```json
"density": {
    "type": ["object", "null"],
    "properties": { "num": …, "den": …, "dp": … },
    "description": "Dry density",
    "ExtUnits": { "unitAscii": "kg_per_m3", "unitUnicode": "kg/m³" },
    "x-order": 1,
    "x-decimalPlaces": 1
},
…
"required": ["sampleId", "measuredAt", "density"]
```

## 2. HTML renderer + REPL round trip

```sh
./build/examples/forms/morph_forms_demo --emit-html   # writes forms_demo.html
./build/examples/forms/morph_forms_demo               # REPL
```

Open `forms_demo.html` in a browser. The page contains no knowledge of the
actions — a vanilla-JS renderer builds each form from the embedded schema:
field order, labels, unit suffixes, decimal steps, min/max, required markers,
a date-time input for the `format: "date-time"` field, a combo box for the
`x-optionsAction` field (its options were resolved at emit time by executing
`ListSamples`; the screens fetch them live instead), and a submit gate.
Filling a form produces a line like

```
ComputeDryDensity {"massDry":{"num":26505,"den":10,"dp":1},"volume":{"num":1,"den":1,"dp":3}}
```

(decimal input is converted to an exact rational client-side). Paste it into
the REPL, which dispatches through the same type-erased `ActionDispatcher`
seam `RemoteServer` uses:

```
ok:  {"num":5301,"den":2,"dp":3}          # 2650.5 kg/m³, exact
```

Error paths are part of the demo: submit `RecordMeasurement {"sampleId":7}`
(required `density` missing) and the model rejects it with the same
`validate()` predicate the GUI gates on — the dispatcher itself does not run
validators, so models guard their own preconditions.

## 3. Server-defined screens

[`screens/forms_screens.cpp`](screens/forms_screens.cpp) declares the demo's screens as UI documents
(spec 4) with the C++ builders:
- **`lab.density`**: the `ComputeDryDensity` form. The model divides the two quantities, and the screen
  shows the reply.
- **`lab.measure`**: the `RecordMeasurement` form, with its sample choice.
- **`lab.intake`**: `IntakeWizard` as a `wizard` node. The registered sample's id is bound forward
  into the measurement step.
- **`lab.samples`**: `SamplesView` as a `collection` node, with its row and collection actions.

`collection<SamplesView>` and `wizard<IntakeWizard>` (`morph/ui/author_forms.hpp`) carry the view's
and the wizard's documents in the bundle's catalog, so the generic client builds both without knowing
the types. `morph_examples_client --app forms` runs them in process. The model keeps its samples in
memory, so each screen has an instance of its own.

## Tests

The demo ships with its own test vectors (they run as part of `ctest`):

- `forms_html_math` (needs Node) — emits the page and drives the *shipped*
  JS math verbatim: exact payload assembly (including beyond 2^53), unit
  conversion with half-up rounding, schema helpers, embedded options.
- `forms_repl_roundtrip` (Unix) — pipes canonical and converted-unit
  submits, a tab-separated line, a malformed datetime, and a missing
  required field through the real REPL and checks every reply.
- The screens' tests and render smoke (`screens/`) — every screen on the
  screen harness, and mounted on Qt Quick offscreen.

## Notes

- Units never appear in payloads — they live in the C++ types and the
  schemas, so a client cannot submit a mismatched unit.
- `ComputeDryDensity.volume` demonstrates a field-level declared-precision
  override (`Quantity<Unit::m3, 4>` vs the unit default of 3): the schema
  advertises `x-decimalPlaces: 4` and both renderers adapt automatically.
- `RecordMeasurement.sampleId` is a `Choice<std::int64_t, "ListSamples">`:
  the combo box options are served by the `ListSamples` action and referenced
  from the schema (`x-optionsAction`), never baked into the form definition.
- `RecordMeasurement.measuredAt` is a `morph::time::Timestamp`: ISO-8601 UTC
  on the wire, `format: "date-time"` in the schema, and a malformed string is
  rejected as a wire error (try it in the REPL). Both clients provide a
  picker: the browser's native datetime-local control (with a *now* button)
  and the forms engine's date-time control.
- Mass and volume fields carry `x-unitAlternatives` (g/t for kg, L for m³
  with exact ratios): pick a different unit and the entered value
  recalculates exactly; the submitted payload is always in the canonical
  unit — try entering 2650500 g and watch the model reply in kg/m³.
- Quantity JSON is the nullable exact rational `{"num","den","dp"}`;
  non-canonical or hostile payloads are canonicalised/clamped on read.
- Both clients assemble the rational JSON from the typed digit string itself
  (no float round-trip), so payloads are exact at any magnitude, and input
  with more decimals than the field's `x-decimalPlaces` is rejected rather
  than silently rounded.
