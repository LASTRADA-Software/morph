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
lab_schemas.hpp   the {actionType: schema} object every client consumes
main.cpp          console demo: --schemas | --emit-html | REPL
gui_qml/          Qt Quick renderer of the same schemas (MORPH_BUILD_FORMS_QML=ON)
```

## Build

```sh
cmake -B build -G Ninja -DMORPH_BUILD_FORMS_QML=ON   # omit the flag to skip the QML app
ninja -C build morph_forms_demo morph_forms_qml
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
"required": ["sampleId", "density"]
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
`ListSamples`; the QML client fetches them live instead), and a submit gate.
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

## 3. QML renderer

```sh
./build/examples/forms/gui_qml/morph_forms_qml
```

`qml/DynamicForm.qml` renders any action schema at runtime (same rules as the
HTML renderer). Submits travel as real `morph::wire::Envelope`s through an
in-process `RemoteServer` — the QML GUI is a JSON-speaking wire client, so
pointing it at a networked server later means swapping the transport inside
`FormsController`, not touching the QML.

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
  rejected as a wire error (try it in the REPL).
- Quantity JSON is the nullable exact rational `{"num","den","dp"}`;
  non-canonical or hostile payloads are canonicalised/clamped on read.
- Both clients assemble the rational JSON from the typed digit string itself
  (no float round-trip), so payloads are exact at any magnitude, and input
  with more decimals than the field's `x-decimalPlaces` is rejected rather
  than silently rounded.
