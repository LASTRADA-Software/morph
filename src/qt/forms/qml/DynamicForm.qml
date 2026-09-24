// SPDX-License-Identifier: Apache-2.0
//
// A form rendered purely from one action's JSON Schema:
//   x-order          -> field order          required  -> asterisk + submit gate
//   ExtUnits         -> unit suffix          minimum/maximum -> input hints
//   multipleOf       -> value must be an exact multiple (1 = "whole number")
//   x-decimalPlaces  -> quantity fields (decimal input -> exact {num,den,dp})
//   x-optionsAction  -> combo box (options fetched by executing that action)
//   x-unitAlternatives -> unit selector with exact recalculation on switch
//   format date-time -> date-time input with a calendar/time picker
//   x-widget         -> control choice (textarea/slider/radio); unknown ids
//                       and a missing key both fall back to the type default
//   x-min/x-max/x-step -> slider track bounds + increment (Ranged fields)
//   type: "array"    -> comma-separated-with-validation control; encodes to
//                       a genuine JSON array literal, e.g. "a, b" -> ["a","b"]
//   type: "number"   -> plain text field encoding a JSON number, for a member
//                       with no x-decimalPlaces (a bare double/float)
//   x-displayDecimals -> a plain number's display/entry precision: at most
//                       that many fraction digits are accepted, and the
//                       JSON-number encoding is kept (FieldMeta::decimals)
//   x-submitMode: "explicit" -> suppresses auto-submit-on-validity; renders
//                       an explicit Submit button (enabled only while ready)
//                       instead -- see "Explicit submit mode" below
//
// A member this vocabulary has no control for -- an object-typed member, or a
// collection whose items are objects -- is *unrepresentable*: no typed text
// encodes to the shape the schema asks for. Such a member is named in its
// field descriptor's `unrepresentable` and keeps the form short of `ready`
// (docs/spec/forms/forms.md, "What `ready` claims"). The one exception is a
// collection of objects a host slot claims (SlotRegistry): the slot edits the
// rows as cell texts and this form encodes each cell with the same encoders
// its own controls use -- see "Collections of objects" in forms.md.
//
// Quantity payloads are assembled as JSON text from the typed digit string,
// so they are exact at any magnitude (same contract as the HTML renderer).
//
// `ready` is a claim about the *payload*: true only when the assembled body
// satisfies the schema the form was generated from, not merely when every
// control the renderer happened to draw is filled. `unrepresentableReason`
// carries the live reason when a member no input can satisfy is what blocks
// submission.
//
// By default, the form calls controller.submitIfValid(...) automatically
// the instant every field/rule is satisfied (safe for a read-only query
// action). A schema for a side-effectful action should set the top-level
// "x-submitMode": "explicit" key: this suppresses that auto-call and instead
// requires the user to press the rendered Submit button, which is disabled
// until the form is ready.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "JsonExact.js" as JsonExact

Frame {
    id: form

    property string actionType
    property var schema
    property var controller

    // The schema as ordinary JSON data, however it was supplied.
    // **Every read of the schema below goes through this, never through
    // `schema` itself.**
    //
    // A *bound* `schema` -- what every shipped app writes, and what the
    // renderer contract describes -- arrives as a genuine JS object. A schema
    // *assigned* instead (an initial property, a `setProperty` from C++,
    // `createTemporaryObject(c, p, {schema: ...})`) round-trips through
    // QVariant, and each of its arrays comes back as a QVariantList. A
    // QVariantList carries `.length`, `.indexOf` and `.map`, so most of this
    // file never noticed -- but `Array.isArray` on one is **false**, and the
    // three places that ask that question therefore took the wrong branch,
    // silently, on a form that still reported `ready`:
    //
    //   - `type: ["integer","null"]` (what schemaJson emits for a rule-3
    //     strong id under `$defs`) was wrapped rather than unpacked, leaving
    //     `isInteger` false, so the id was submitted as a quoted JSON
    //     *string* and the server answered parse_number_failure;
    //   - the `anyOf`-over-`$ref` collapse never runs, so a nullable `$ref`
    //     member falls back to that same encoding;
    //   - the closed-set recognition never runs, so an `enum` draws a
    //     free-text box over what the schema states is a closed set.
    //
    // Normalising once, here, is what stops that being a standing trap for
    // the next `Array.isArray` anyone writes against schema data.
    readonly property var schemaData: normalizeSchema(schema)

    // Client-side theming/override registry (docs/spec/forms/forms.md,
    // "Theming / component-override registry"): null (the default) means no
    // registry installed -- every field renders its built-in control exactly
    // as it does today. See SlotRegistry.qml.
    property var slotRegistry: null

    // The chrome Component registered for `role`, or null for the built-in
    // (docs/spec/forms/forms.md, "Chrome slots").
    function chrome(role) {
        return slotRegistry ? slotRegistry.resolveChrome(role) : null
    }

    // Assigns each of `values` to the same-named property of a chrome item --
    // only where the item declares it, so a chrome Component declares just the
    // members it uses. A value may be a Qt.binding. `form` is always offered.
    function bindChrome(item, values) {
        if (!item)
            return
        if ("form" in item)
            item.form = form
        for (const key in values) {
            if (key in item)
                item[key] = values[key]
        }
    }

    // Whether the text held for `name` is typed but does not encode -- the
    // "this field is wrong" state a host's label chrome may show. Blank is not
    // invalid: that is the required gate's business.
    function fieldInvalid(name) {
        const f = fieldByName[name]
        if (!f)
            return false
        return opt(fieldValues[name], "").trim() !== "" && fieldJsonLiteral(f) === null
    }

    // The status line's text, shared by the built-in label and a status chrome.
    readonly property string statusText: {
        if (!ready) {
            // Filling fields in is the usual remedy, but it is not the
            // remedy for a member this renderer cannot represent, and
            // telling the user to fill something that would not help
            // is the worse half of the same lie a `ready` of true
            // would be. Name the member instead.
            if (unrepresentableReason !== "")
                return "cannot be submitted -- " + unrepresentableReason
            return "fill the required (*) fields"
        }
        return explicitSubmitMode ? "✓ ready -- press Submit" : "✓ executes automatically as you type"
    }

    property var fieldValues: ({})
    property var fieldOptions: ({})
    property var fieldUnits: ({})
    property int optionsRevision: 0
    property bool ready: false

    // Why no input can make this form ready, or "" when none applies: the
    // first member the renderer cannot represent that submission currently
    // waits on (`"<wire name>: <reason>"`). Written by revalidate() from the
    // same pass that writes `ready`, so the two cannot disagree.
    //
    // Empty while the form is ready, and also empty while it is merely
    // unfilled -- a blank required field or an unsatisfied rule is the
    // ordinary submit gate, which the user can act on, and the status label
    // below says so. This property exists for the case the user cannot act
    // on, which is otherwise indistinguishable from it.
    property string unrepresentableReason: ""

    // Non-zero while values are being written programmatically rather than
    // edited by a user -- restoring a control the layout just recreated, or
    // clearing the form between rows. revalidate() keeps recomputing validity
    // and the preview during such a window but does *not* auto-submit, because
    // repopulating a form is not a user action and must never fire one. A
    // counter, not a flag, so nested writes (a reset that itself triggers
    // refreshDependents) cannot re-enable submission early.
    property int programmaticEdit: 0
    property string previewLine: ""
    property string resultText: ""
    property bool resultOk: true

    // Cross-field rules (docs/spec/forms/forms.md's x-rules): absent when
    // the action declares no formRules, in which case every helper below is
    // a no-op and behavior is byte-identical to a renderer with no rule
    // support (the fallback the spec requires).
    property var rules: schemaData["x-rules"] || []
    property int rulesRevision: 0

    // "x-submitMode": "explicit" (docs/spec/forms/forms.md, "Explicit submit
    // mode"): opts a side-effectful (non-query) action out of the default
    // auto-fire-on-validity behavior. When set, revalidate() still recomputes
    // `ready`/`previewLine` live but never calls submitIfValid() on its own;
    // an explicit submit Button (added to the layout below), enabled only
    // while `ready`, is the sole way to fire. Absent (the default) or any
    // other value keeps today's auto-submit-on-validity behavior unchanged.
    property bool explicitSubmitMode: schemaData["x-submitMode"] === "explicit"

    // i18n: a host-supplied translation catalog (see I18nCatalog.hpp) and the
    // BCP-47 locale to resolve against. `catalog: null` (the default) means
    // "no catalog installed" — every label/help/placeholder falls back to
    // its schema literal, exactly as today.
    property var catalog: null
    property string displayLocale: "C"
    property var qtLocale: Qt.locale(displayLocale)

    // The display zone for Timestamp entry/editing, in minutes east of UTC
    // (e.g. 120 for UTC+2). 0 (the default) is the identity transform —
    // today's "entered time is UTC" behavior.
    property int displayOffsetMinutes: 0

    // --- schema helpers -----------------------------------------------------

    function opt(value, fallback) {
        return value === undefined ? fallback : value
    }

    // Re-reads a schema as plain JSON data, for `schemaData` above. JSON is
    // the only shape a schema has, so a round trip through it is a no-op in
    // meaning for a value that was already plain.
    //
    // Anything that is not an object -- including the `undefined` of a form
    // whose schema has not been set yet -- passes through untouched, so an
    // unconfigured form behaves exactly as it did before.
    function normalizeSchema(value) {
        if (value === null || typeof value !== "object")
            return value
        try {
            return JSON.parse(JSON.stringify(value))
        } catch (ignored) {
            // JSON.stringify throws on a reference cycle, which a parsed
            // schema cannot contain but a hand-written QML object literal
            // could. Such a schema renders today (nothing here follows a
            // `$ref` by identity), so hand it back untouched rather than
            // taking the whole form out with a broken binding.
            return value
        }
    }

    // A `{num,den,dp}` Rational node (the wire shape `x-minimum`/`x-maximum`
    // take on a schema decorated by morph::forms::InstanceConstraints) read as
    // a JS number, or `undefined` when the key is absent or malformed.
    //
    // The quotient is a double, like the compiled `minimum`/`maximum` this
    // sits beside: the live client gate is an approximation, and the exact
    // check is the model's own `InstanceConstraints::checkValue`, which
    // compares the same declaration against an exact `Rational`. No client
    // rounding can let a value past the model (see forms.md, "Per-instance
    // constraints").
    function boundValue(node) {
        if (node === undefined || node === null || typeof node !== "object")
            return undefined
        const num = node.num
        const den = node.den
        if (typeof num !== "number" || typeof den !== "number" || den === 0)
            return undefined
        return num / den
    }

    // A declared scalar bound as a finite number, or `undefined` for "no bound".
    //
    // JSON has no `Infinity`, so `schemaData`'s round trip turns a non-finite
    // bound -- reachable only from a hand-authored QML object literal, never
    // from `schemaJson<A>()` -- into `null`. A null bound is not `undefined`,
    // so every gate below would read it as the bound **0**: `maximum: Infinity`
    // would go from "no effective ceiling" to rejecting every positive value.
    // JSON Schema gives a null numeric keyword no meaning either, so both
    // spellings land on the same answer: not a declared bound.
    function numericBound(value) {
        return typeof value === "number" && isFinite(value) ? value : undefined
    }

    // Whether `value` breaks a declared `multipleOf`. An absent or
    // non-positive step is no constraint at all, matching JSON Schema, which
    // requires `multipleOf` to be strictly positive.
    //
    // The comparison is a double quotient with a tolerance, like every other
    // numeric gate in this file: the bound itself arrived through JSON.parse,
    // so exactness was already lost before this function saw it. The exact
    // check is the model's `allFieldBoundsSatisfied`, which divides the same
    // two values as exact `Rational`s (see forms.md, "Per-field scalar
    // bounds"). The tolerance is relative so a large quotient is judged on the
    // same terms as a small one.
    function violatesMultipleOf(value, step) {
        if (step === undefined || !(step > 0))
            return false
        const quotient = value / step
        const nearest = Math.round(quotient)
        return Math.abs(quotient - nearest) > 1e-9 * Math.max(1, Math.abs(quotient))
    }

    // Three-way compare of two integers held as decimal strings: -1, 0, 1.
    // Needed because a JS number cannot hold an int64 bound exactly, so the
    // comparison has to happen on digits. Inputs are already
    // /^-?\d+$/-validated by the caller.
    function compareIntText(left, right) {
        const leftNeg = left.charAt(0) === "-"
        const rightNeg = right.charAt(0) === "-"
        if (leftNeg !== rightNeg)
            return leftNeg ? -1 : 1
        // Strip sign and leading zeros so "007" and "7" compare equal.
        const leftDigits = left.replace(/^-?0*/, "") || "0"
        const rightDigits = right.replace(/^-?0*/, "") || "0"
        let cmp = 0
        if (leftDigits.length !== rightDigits.length)
            cmp = leftDigits.length < rightDigits.length ? -1 : 1
        else if (leftDigits < rightDigits)
            cmp = -1
        else if (leftDigits > rightDigits)
            cmp = 1
        // Both negative reverses the magnitude ordering.
        return leftNeg ? -cmp : cmp
    }

    // Follow a $ref into $defs; attributes on the field win over the def's.
    function resolveRef(prop) {
        if (prop && prop["$ref"] !== undefined) {
            const defName = prop["$ref"].split("/").pop()
            const def = opt((schemaData["$defs"] || {})[defName], {})
            return Object.assign({}, def, prop)
        }
        return opt(prop, {})
    }

    function resolveProp(prop) {
        const p = resolveRef(prop)
        // A nullable member whose type is itself a $ref (e.g. a bare
        // std::optional<std::int64_t>, or std::optional<TagId>) emits
        // {"anyOf": [{"$ref": ...}, {"type": "null"}]} with **no top-level
        // "type" key**. Resolving only the top-level $ref left every kind flag
        // below false, so the value fell through to the plain-text encoding and
        // went out as a quoted JSON *string* that the server then rejected with
        // parse_number_failure. Resolve through the non-null branch
        // so the field is typed by T. `oneOf` is handled the same way — glaze
        // emits it for every `glz::enumerate`d `enum class`, and a
        // hand-written or evolved schema may use it for nullability too.
        const branches = Array.isArray(p.anyOf) ? p.anyOf : (Array.isArray(p.oneOf) ? p.oneOf : null)
        if (branches !== null) {
            for (let i = 0; i < branches.length; ++i) {
                const branch = resolveRef(branches[i])
                if (branch.type === "null")
                    continue
                // Outer keys win over the branch's (an x-* extension declared
                // beside the anyOf is the more specific statement), except that
                // the outer object is precisely the one with no "type".
                const merged = Object.assign({}, branch, p)
                delete merged.anyOf
                delete merged.oneOf
                // A closed set of alternatives is not the nullability shape
                // this collapse exists for: its branches differ in *value*,
                // so the first one's `const` is not the field's own and must
                // not be left masquerading as it. The set itself
                // survives via enumChoices() below, which reads the branches
                // rather than this collapsed node.
                delete merged["const"]
                if (p.type === undefined)
                    merged.type = branch.type
                return merged
            }
        }
        return p
    }

    // A resolved property's declared JSON types, as an array however the
    // schema spells it: one string, a list (`["integer","null"]`), or no
    // `type` key at all, which is no declaration rather than a type.
    function jsonTypes(p) {
        return Array.isArray(p.type) ? p.type : (p.type === undefined ? [] : [p.type])
    }

    // Why this renderer cannot represent `p`, or "" when it can.
    //
    // The form draws flat fields. A member whose value is an *object* reaches
    // one scalar control, and a collection whose items are objects reaches the
    // comma-separated array control, so whatever a user types encodes as a
    // JSON string (or an array of strings) where the schema asks for an object
    // or an array of them. No input closes that gap: the member is
    // unrepresentable, not merely unfilled.
    //
    // Naming it is what keeps `ready` a claim about the payload rather than
    // about which controls happen to be filled. A form that reported ready
    // here would assemble a body the action must reject, and the rejection
    // would surface at the action boundary instead of in the form, where the
    // user could see it. Which of drawing a sub-form, declining the schema or
    // going on flattening it is right is a separate question; the readiness
    // answer is the same under all three.
    //
    // `typed` says one of the kind flags already claims the property for a
    // control that encodes a shape of its own. That is what keeps a Quantity
    // and a Choice -- both `"type": "object"` in the schema, both with an
    // encoder that produces the right shape -- out of this.
    function unrepresentableMemberReason(p, types, typed) {
        if (!typed && types.indexOf("object") !== -1)
            return "a nested object member, which this renderer does not draw"
        if (types.indexOf("array") !== -1) {
            const itemTypes = jsonTypes(resolveProp(p.items))
            if (itemTypes.indexOf("object") !== -1 || itemTypes.indexOf("array") !== -1)
                return "a collection of nested objects, which this renderer does not draw"
        }
        return ""
    }

    // The renderer kind of a resolved property: the control it gets, which
    // the JSON type alone does not say (a Quantity and a nested object are both
    // "object", a Choice is "integer", an enum and a date-time are "string").
    // Asked in the order fieldJsonLiteral encodes in, so the kind names the
    // encoder the field actually gets.
    function fieldKind(p, types, dp, optionsAction, isEnum) {
        if (types.indexOf("array") !== -1) {
            const itemTypes = jsonTypes(resolveProp(p.items))
            return itemTypes.indexOf("object") !== -1 ? "objectArray" : "array"
        }
        if (isEnum)
            return "enum"
        if (optionsAction !== undefined)
            return "choice"
        if (p.format === "date-time")
            return "datetime"
        if (p.format === "date")
            return "date"
        if (dp !== undefined)
            return "quantity"
        if (types.indexOf("integer") !== -1)
            return "integer"
        if (types.indexOf("boolean") !== -1)
            return "boolean"
        if (types.indexOf("number") !== -1)
            return "number"
        if (types.indexOf("object") !== -1)
            return "object"
        return "string"
    }

    // Value/label pairs for a property that states a **closed set of values**
    // outright, or [] for one that does not. Two spellings, both handled:
    //
    //   1. A `oneOf`/`anyOf` in which every branch bar `{"type":"null"}`
    //      pins one value with `const` and names it with `title` -- what
    //      glaze emits for a C++ `enum class` that declares a `glz::meta`
    //      with `glz::enumerate` (the same declaration that makes it travel
    //      as its enumerator name rather than its underlying integer):
    //
    //        "role": {"type": "string",
    //                 "oneOf": [{"title": "Viewer",  "const": "Viewer"},
    //                           {"title": "Member",  "const": "Member"},
    //                           {"title": "Manager", "const": "Manager"}]}
    //
    //   2. The bare JSON Schema `enum` keyword, which glaze does not emit but
    //      a hand-written or evolved schema may: `{"enum": ["a", "b"]}`.
    //
    // This is distinguishable from the nullable-`$ref` shape resolveProp
    // collapses, whose branches carry no `const` at all: **one**
    // branch without a `const` and the property is not a closed set, so the
    // whole thing falls back rather than offering a partial list.
    //
    // `valueJson` is the JSON literal of the value (a string alternative
    // therefore arrives quoted), matching the convention a server-fetched
    // Choice's rows already use, so both feed the same combo box and the same
    // fieldJsonLiteral path.
    function enumChoices(prop) {
        const p = resolveRef(prop)
        const literals = p["enum"]
        if (Array.isArray(literals)) {
            const rows = []
            for (let i = 0; i < literals.length; ++i) {
                // A `null` member spells nullability, not a choosable value —
                // the same thing the "null" branch means in shape 1. Leaving
                // the field blank is how the user declines it.
                if (literals[i] === null)
                    continue
                rows.push({ label: String(literals[i]), valueJson: JSON.stringify(literals[i]) })
            }
            return rows
        }
        const branches = Array.isArray(p.anyOf) ? p.anyOf : (Array.isArray(p.oneOf) ? p.oneOf : null)
        if (branches === null)
            return []
        const rows = []
        for (let i = 0; i < branches.length; ++i) {
            const branch = resolveRef(branches[i])
            if (branch.type === "null")
                continue
            if (branch["const"] === undefined)
                return []
            rows.push({ label: String(opt(branch.title, branch["const"])),
                        valueJson: JSON.stringify(branch["const"]) })
        }
        return rows
    }

    // --- i18n: field-slot key derivation and resolution ------------------
    // Mirrors morph::forms::i18n::fieldKey/explicitFieldKey (forms/i18n.hpp)
    // and morph::render::resolveText (render/i18n.hpp) for the one slot this
    // renderer consumes (field label/help/placeholder) — group/rule/wizard/
    // app keys have no consumer here yet.

    function i18nFieldKey(field, slot) {
        return actionType + "." + field + "." + slot
    }

    // A field's x-i18nKey (when declared) replaces the derived
    // "<actionType>.<field>" stem; the slot suffix still applies.
    function i18nExplicitFieldKey(i18nKeyOverride, slot) {
        return i18nKeyOverride ? (i18nKeyOverride + "." + slot) : undefined
    }

    // Resolution: explicit key, then derived key, then the schema literal.
    // `explicitKey` may be undefined (no x-i18nKey declared). No catalog
    // installed (or a full miss) resolves to `literal` unchanged.
    function resolveText(explicitKey, derivedKey, literal) {
        if (!catalog)
            return literal
        // Reading catalog.revision (bumped on every addTranslation) makes
        // `fields` (which calls this) a dependency of the catalog's
        // contents, not just its identity — otherwise a translation seeded
        // after `fields` first evaluates (e.g. from the catalog's own
        // Component.onCompleted, which runs after the initial binding pass)
        // would go unnoticed. Same cache-invalidation idiom as
        // `optionsRevision` below, for `fieldOptions`.
        catalog.revision
        if (explicitKey !== undefined) {
            const hit = catalog.lookup(displayLocale, explicitKey)
            if (hit !== undefined && hit !== null)
                return hit
        }
        if (derivedKey === undefined)
            return literal
        const hit2 = catalog.lookup(displayLocale, derivedKey)
        if (hit2 !== undefined && hit2 !== null)
            return hit2
        return literal
    }

    // Flat field descriptors, in declaration (x-order) order.
    //
    // Reading the registry's revision makes a slot registered after the first
    // evaluation re-describe the member it claims: whether a collection of
    // objects is representable depends on it (see describeObject).
    property var fields: {
        if (slotRegistry)
            slotRegistry.revision
        return describeObject(schemaData, 0)
    }

    // Whether a host slot claims the member `name` -- the same resolution the
    // field delegate performs, so the two cannot disagree.
    function slotClaims(name, xWidget, unitAscii, jsonType, kind) {
        return slotRegistry !== null && slotRegistry !== undefined
               && slotRegistry.resolve(actionType, name, xWidget, unitAscii, jsonType, kind) !== null
    }

    // Field descriptors for one object schema's properties, in x-order order.
    // `depth` is 0 for the action itself and 1 for the element of a top-level
    // collection of objects (`itemFields` below); element members are
    // described once and never recursed into further, so a self-referential
    // row type cannot loop. At depth 1 a label resolves through an explicit
    // x-i18nKey or the literal only: the derived "<action>.<field>" key names
    // top-level members.
    function describeObject(objectSchema, depth) {
        const props = (objectSchema && objectSchema.properties) || {}
        const required = (objectSchema && objectSchema.required) || []
        return Object.keys(props)
            .sort(function (a, b) { return opt(props[a]["x-order"], 0) - opt(props[b]["x-order"], 0) })
            .map(function (name) {
                const raw = props[name]
                const p = resolveProp(raw)
                const types = jsonTypes(p)
                const dp = opt(raw["x-decimalPlaces"], p["x-decimalPlaces"])
                // A plain number's display precision (FieldMeta::decimals).
                // Only a "number" with no x-decimalPlaces reads it: the
                // declared precision of a Quantity is the one that encodes.
                const displayDp = (dp === undefined && types.indexOf("number") !== -1)
                        ? opt(raw["x-displayDecimals"], p["x-displayDecimals"]) : undefined
                const optionsAction = opt(raw["x-optionsAction"], p["x-optionsAction"])
                // A closed set stated by the schema itself. Read
                // from `raw`, not the collapsed `p`: resolveProp keeps only
                // one branch, and the set is the point. A field that also
                // declares x-optionsAction is a server-fetched Choice and
                // stays one -- the two are kept mutually exclusive so every
                // `isChoice` test below (the options fetch, the dependent
                // refresh) keeps meaning exactly what it meant.
                const enumOptionRows = optionsAction !== undefined ? [] : enumChoices(raw)
                const extUnits = opt(p.ExtUnits, {})
                const unitText = opt(extUnits.unitUnicode, opt(extUnits.unitAscii, ""))
                // Canonical unit first, then declared convertible alternatives.
                const unitOptions = [{ display: unitText, decimals: opt(dp, 0), num: 1, den: 1 }]
                const alternatives = opt(raw["x-unitAlternatives"], opt(p["x-unitAlternatives"], []))
                for (let a = 0; a < alternatives.length; ++a) {
                    const alt = alternatives[a]
                    unitOptions.push({ display: opt(alt.display, alt.id), decimals: alt.decimals,
                                       num: alt.num, den: alt.den })
                }
                // x-widget chooses the control; a missing key or an id this
                // renderer does not recognise both fall back to the type
                // default (plain text field / combo box), since none of the
                // flags below are set in that case.
                const widget = opt(raw["x-widget"], p["x-widget"])
                const sliderMin = opt(raw["x-min"], p["x-min"])
                const sliderMax = opt(raw["x-max"], p["x-max"])
                const sliderStep = opt(raw["x-step"], p["x-step"])
                // x-i18nKey (FieldMeta::i18nKey, when declared) replaces the
                // derived "<actionType>.<field>" stem for all three of this
                // field's text slots; a field with no override falls
                // through to the derived key, then to the schema literal.
                const i18nOverride = opt(raw["x-i18nKey"], opt(p["x-i18nKey"], ""))
                const literalTitle = opt(raw["title"], opt(p.title, name))
                const literalHelp = opt(p.description, "")
                const literalPlaceholder = opt(raw["x-placeholder"], opt(p["x-placeholder"], ""))
                // The kind flags below that hand the property to a control
                // with an encoder of its own, restated here because
                // unrepresentableMemberReason has to know whether anything
                // claimed the property before it judges its declared type.
                // `isArray` is deliberately absent: the array control claims
                // the property but encodes its items as strings, so an array
                // of objects is unrepresentable even though a control drew it.
                // `isNumber` is absent for the same reason: a member declared
                // both "number" and "object" is satisfied by neither reading,
                // and the object one is the half no typed text can close.
                const typedControl = dp !== undefined || optionsAction !== undefined
                        || enumOptionRows.length > 0 || p.format === "date-time"
                        || types.indexOf("integer") !== -1 || types.indexOf("boolean") !== -1
                // A collection whose element is an object schema with members
                // of its own (glaze's std::vector<Sub>): the shape a host
                // grid slot edits row by row.
                const itemSchema = types.indexOf("array") !== -1 ? resolveProp(p.items) : {}
                const isObjectArray = jsonTypes(itemSchema).indexOf("object") !== -1
                        && itemSchema.properties !== undefined
                const jsonType = types.length > 0 ? types[0] : ""
                const kind = fieldKind(p, types, dp, optionsAction, enumOptionRows.length > 0)
                // Only a top-level collection is handed to a slot: its rows
                // are stored in the collection's own fieldValues entry, which
                // a member one level down does not have.
                const claimedBySlot = depth === 0 && isObjectArray
                        && slotClaims(name, opt(widget, ""), opt(extUnits.unitAscii, ""), jsonType, kind)
                const derivedKey = function (slot) { return depth === 0 ? i18nFieldKey(name, slot) : undefined }
                return {
                    name: name,
                    title: literalTitle,
                    label: resolveText(i18nExplicitFieldKey(i18nOverride, "label"),
                                       derivedKey("label"), literalTitle),
                    description: resolveText(i18nExplicitFieldKey(i18nOverride, "help"),
                                              derivedKey("help"), literalHelp),
                    placeholder: resolveText(i18nExplicitFieldKey(i18nOverride, "placeholder"),
                                              derivedKey("placeholder"), literalPlaceholder),
                    readOnly: opt(raw["x-readonly"], opt(p["x-readonly"], false)),
                    hidden: opt(raw["x-hidden"], opt(p["x-hidden"], false)),
                    unit: unitText,
                    unitOptions: unitOptions,
                    canonDp: opt(dp, 0),
                    isChoice: optionsAction !== undefined,
                    // A closed set the schema enumerates: drawn with the same
                    // combo box `isChoice` draws, but the options are already
                    // here, so there is no options action and no fetch. The
                    // rows are `{label, valueJson}`, identical in shape to a
                    // fetched Choice's, so both share fieldJsonLiteral and
                    // the delegate below.
                    isEnum: enumOptionRows.length > 0,
                    enumOptions: enumOptionRows,
                    optionsAction: opt(optionsAction, ""),
                    valueField: opt(opt(raw["x-optionValue"], p["x-optionValue"]), "id"),
                    labelField: opt(opt(raw["x-optionLabel"], p["x-optionLabel"]), "name"),
                    // Wire names of sibling fields whose current values
                    // parameterise this Choice's options action (cascading
                    // picklist); empty for an independent Choice.
                    dependsOn: opt(raw["x-optionsDependsOn"], opt(p["x-optionsDependsOn"], [])),
                    isDateTime: p.format === "date-time",
                    isQuantity: dp !== undefined,
                    // Fraction digits: a Quantity's declared precision, else a
                    // plain number's x-displayDecimals, else 0.
                    // `decimalsDeclared` tells a slot which of "0" and "none
                    // declared" it is looking at.
                    decimals: opt(dp, opt(displayDp, 0)),
                    decimalsDeclared: dp !== undefined || displayDp !== undefined,
                    displayDecimals: displayDp,
                    isInteger: types.indexOf("integer") !== -1,
                    // "number" -- a bare `double`/`float`. The plain text
                    // field draws it, but the JSON *number* encoding is its
                    // own: the fall-through at the end of fieldJsonLiteral
                    // would quote the digits. This flag says only what the
                    // schema's type is, so it is also true for a *precise*
                    // field spelled "number" with an x-decimalPlaces beside
                    // it; fieldJsonLiteral asks `isQuantity` first, which is
                    // what keeps such a field on the exact {num,den,dp}
                    // encoding.
                    isNumber: types.indexOf("number") !== -1,
                    // "boolean" -- a CheckBox, not the plain text field's
                    // fall-through (which wrapped the typed text as a JSON
                    // *string*: {"flag":"true"}, or {"flag":"banana"} for
                    // anything at all, since a TextField applies no validation.
                    // glaze rejects both with expected_true_or_false).
                    isBoolean: types.indexOf("boolean") !== -1,
                    // "array" (glaze's std::vector<T> schema shape: {"type":
                    // "array", "items": {...}}) -- a comma-separated-with-
                    // validation control, not the plain text field's
                    // fall-through (which would wrap the typed text as a
                    // JSON *string*, not an array). Scoped to array-of-string
                    // today; any other item type still renders this control
                    // but each entry is encoded as a JSON string, same as an
                    // array of strings, rather than silently misencoding.
                    isArray: types.indexOf("array") !== -1,
                    // A collection of objects, and -- for a top-level one --
                    // its element's member descriptors, in x-order order: the
                    // columns a grid slot draws (label, unit, decimals,
                    // readOnly, required, and the kind flags that say how a
                    // cell's text is encoded). Empty for any other member.
                    isObjectArray: isObjectArray,
                    itemFields: (isObjectArray && depth === 0) ? describeObject(itemSchema, depth + 1) : [],
                    // True when a registered slot draws this collection, which
                    // is what makes it representable (see `unrepresentable`).
                    claimedBySlot: claimedBySlot,
                    // Why no control here can collect what the schema asks
                    // for, or "" for every member this renderer represents --
                    // which is every member of a flat action. A non-empty
                    // reason makes the member unencodable, so the form reports
                    // ready only for a payload that legitimately omits it.
                    //
                    // A top-level collection of objects a slot claims is the
                    // exception: the slot collects each row's cell texts and
                    // encodeObjectArray encodes them, so an encoding exists.
                    unrepresentable: claimedBySlot ? "" : unrepresentableMemberReason(p, types, typedControl),
                    required: required.indexOf(name) !== -1,
                    // `resolveRef` merges the property node *over* the `$def`
                    // it points at, so these three read a per-field bound
                    // declared through `FieldMeta` as readily as
                    // one glaze stamped on the shared type definition -- which
                    // is what makes a bound on one `Quantity` member leave a
                    // sibling of the same type alone.
                    minimum: numericBound(p.minimum),
                    maximum: numericBound(p.maximum),
                    // `multipleOf = 1` is how "whole number" is spelled: the
                    // constraint a `Quantity` cannot carry in its type, since
                    // `Quantity<U, Dec>` requires `Dec >= 1` and therefore
                    // always represents tenths exactly.
                    multipleOf: p.multipleOf,
                    // Exact decimal-string companions for a bound a double
                    // cannot hold. `p.minimum`/`p.maximum` reached this object
                    // through JSON.parse (every app does
                    // `JSON.parse(controller.schemasJson)`), so an int64 bound
                    // is already rounded by the time it gets here -- INT64_MAX
                    // arrives as 9223372036854775808. These strings are not
                    // Undefined for any bound a double holds
                    // exactly, which is the overwhelmingly common case.
                    exactMinimum: p["x-exactMinimum"],
                    exactMaximum: p["x-exactMaximum"],
                    // Per-*instance* bounds a model wrote into the served
                    // schema from data (morph::forms::InstanceConstraints).
                    // `{num,den,dp}` Rational nodes, never plain
                    // numbers, and never emitted by schemaJson<A>() itself.
                    // Without these the renderer honours only the compiled
                    // `minimum`/`maximum` and an instance's own range is
                    // decorative -- exactly the "two values for one concept,
                    // and the renderer believes the compiled one" outcome the
                    // decoration seam exists to remove.
                    instanceMinimum: boundValue(opt(raw["x-minimum"], p["x-minimum"])),
                    instanceMaximum: boundValue(opt(raw["x-maximum"], p["x-maximum"])),
                    section: opt(raw["x-section"], p["x-section"]),
                    colspan: opt(opt(raw["x-colspan"], p["x-colspan"]), 1),
                    isMultiline: widget === "textarea",
                    isSlider: widget === "slider" && sliderMin !== undefined && sliderMax !== undefined,
                    isRadioChoice: (optionsAction !== undefined) && widget === "radio",
                    sliderMin: opt(sliderMin, 0),
                    sliderMax: opt(sliderMax, 100),
                    sliderStep: opt(sliderStep, 1),
                    // Renderer-toolkit override-slot keys (docs/spec/forms/
                    // forms.md, "Theming / component-override registry"):
                    // xWidget is advisory and additive -- absent, it resolves
                    // to "" and SlotRegistry.resolve()'s byWidget tier never
                    // matches.
                    xWidget: opt(widget, ""),
                    // The control this renderer would draw, named for
                    // SlotRegistry.byKind (see fieldKind).
                    kind: kind,
                    unitAscii: opt(extUnits.unitAscii, ""),
                    jsonType: jsonType
                }
            })
    }

    // name -> field descriptor, for parent/child lookups by wire name.
    property var fieldByName: {
        const map = {}
        for (let i = 0; i < fields.length; ++i)
            map[fields[i].name] = fields[i]
        return map
    }

    // Reverse of x-optionsDependsOn: parent field name -> [dependent child names].
    property var dependents: {
        const map = {}
        for (let i = 0; i < fields.length; ++i) {
            const f = fields[i]
            for (let j = 0; j < f.dependsOn.length; ++j) {
                const parentName = f.dependsOn[j]
                if (map[parentName] === undefined)
                    map[parentName] = []
                map[parentName].push(f.name)
            }
        }
        return map
    }

    // Field descriptors bucketed into x-layout's declared groups (in
    // x-layout order), with every field absent from every group collected
    // into one implicit trailing group — never dropped, per
    // docs/spec/forms/forms.md, "Layout & grouping". When the schema
    // declares no x-layout at all, this is one implicit group holding every
    // field: the pre-grouping flat form, unchanged.
    property var sections: {
        const groupDefs = (schemaData["x-layout"] || {}).groups || []
        if (groupDefs.length === 0)
            return [{ title: "", kind: "flat", fields: fields }]

        const buckets = groupDefs.map(function (g) {
            return { title: g.title, kind: g.kind, fields: [] }
        })
        const trailing = { title: "", kind: "flat", fields: [] }
        for (let i = 0; i < fields.length; ++i) {
            const f = fields[i]
            if (f.section !== undefined && f.section >= 0 && f.section < buckets.length)
                buckets[f.section].fields.push(f)
            else
                trailing.fields.push(f)
        }
        return trailing.fields.length > 0 ? buckets.concat([trailing]) : buckets
    }

    // Consecutive "tab" sections share one tab bar; every other section
    // (including the implicit "flat" one) renders as its own run.
    property var renderRuns: {
        const runs = []
        let i = 0
        while (i < sections.length) {
            if (sections[i].kind === "tab") {
                const tabRun = []
                while (i < sections.length && sections[i].kind === "tab") {
                    tabRun.push(sections[i])
                    ++i
                }
                runs.push({ type: "tabset", sections: tabRun })
            } else {
                runs.push({ type: "single", section: sections[i] })
                ++i
            }
        }
        return runs
    }

    // --- cross-field rules (x-rules): condition/rule evaluation ------------

    function fieldMeta(name) {
        for (let i = 0; i < fields.length; ++i) {
            if (fields[i].name === name)
                return fields[i]
        }
        return null
    }

    function fieldEngaged(name) {
        return (opt(fieldValues[name], "")).trim() !== ""
    }

    // A JS-comparable reading of a field's current text, or `undefined` when
    // unengaged. Quantity and integer fields compare numerically; everything
    // else (dates, choices, plain strings) compares as text -- ISO-8601
    // date-time text sorts lexicographically in chronological order, so this
    // is exact for a "greater(checkOut, checkIn)" style comparison without a
    // full date parser here. This is an approximation for the live client
    // gate only: the server re-validates the same rule exactly, over the
    // declared-precision Rational (see forms.md), so no client rounding can
    // let an invalid action through -- the correctness floor never depends
    // on this function.
    function comparableValue(name) {
        const text = (opt(fieldValues[name], "")).trim()
        if (text === "")
            return undefined
        const meta = fieldMeta(name)
        // A boolean field's stored value is the text "true"/"false" (the
        // CheckBox writes those), but `equals` against a `bool` emits a JSON
        // *boolean* literal. Comparing the two as text made `"true" === true`
        // false, so a requiredWhen keyed on a boolean never fired on the
        // client while the compiled evaluator fires it.
        if (meta && meta.isBoolean)
            return text === "true"
        if (meta && (meta.isQuantity || meta.isInteger))
            return parseFloat(text)
        return text
    }

    // Evaluates one condition node (`engaged` / `notEngaged` / `equals` / a
    // comparison kind reused as a boolean / the compound `and`/`or`/`not`
    // kinds, which recurse into `conditions`/`condition` to any depth).
    //
    // **Three-valued.** `true` / `false` / `undefined`, where `undefined` is
    // "this renderer cannot evaluate this node" -- an unrecognised `kind`.
    // That is a distinct answer from `false`, and collapsing the two makes
    // this renderer contradict every other client of the same spec sentence:
    // a `not` wrapping an unknown child comes out *true*, so a requiredWhen
    // keyed on it demands a field for a reason the renderer has just admitted
    // it cannot judge.
    //
    // `undefined` propagates. `and` is `false` if any child is false, and
    // `undefined` if none is false but some is unevaluable. `or` is `true` if
    // any child is true, and `undefined` if none is true but some is
    // unevaluable. `not` of `undefined` is `undefined`. Each caller then
    // decides what "unevaluable" means for its own question -- see testRule
    // (defer), fieldVisible (show) and fieldReadonly (leave editable).
    function testCondition(cond) {
        const kind = cond.kind
        const names = cond.fields || []
        if (kind === "engaged")
            return fieldEngaged(names[0])
        if (kind === "notEngaged")
            return !fieldEngaged(names[0])
        if (kind === "equals") {
            if (!fieldEngaged(names[0]))
                return false
            // An integral literal beyond 2^53 arrives here already rounded by
            // JSON.parse, so comparing it as a number collapses values the
            // schema kept distinct. `valueText` carries the exact digits when
            // the emitter judged the number unsafe; compare on digits then.
            if (cond.valueText !== undefined) {
                const text = (opt(fieldValues[names[0]], "")).trim()
                if (!/^-?\d+$/.test(text))
                    return false
                return compareIntText(text, cond.valueText) === 0
            }
            const literal = (cond.value && cond.value.num !== undefined)
                             ? (cond.value.num / cond.value.den) : cond.value
            return comparableValue(names[0]) === literal
        }
        if (kind === "greater" || kind === "greaterOrEqual" || kind === "less" || kind === "lessOrEqual") {
            const lv = comparableValue(names[0])
            const rv = comparableValue(names[1])
            if (lv === undefined || rv === undefined)
                return true  // vacuously satisfied while an operand is unengaged
            if (kind === "greater") return lv > rv
            if (kind === "greaterOrEqual") return lv >= rv
            if (kind === "less") return lv < rv
            return lv <= rv
        }
        if (kind === "and") {
            const nested = cond.conditions || []
            let unknown = false
            for (let i = 0; i < nested.length; ++i) {
                const child = testCondition(nested[i])
                if (child === false)
                    return false        // one false child settles `and` outright
                if (child === undefined)
                    unknown = true
            }
            return unknown ? undefined : true
        }
        if (kind === "or") {
            const nested = cond.conditions || []
            let unknown = false
            for (let i = 0; i < nested.length; ++i) {
                const child = testCondition(nested[i])
                if (child === true)
                    return true         // one true child settles `or` outright
                if (child === undefined)
                    unknown = true
            }
            return unknown ? undefined : false
        }
        if (kind === "not") {
            const child = testCondition(cond.condition)
            return child === undefined ? undefined : !child
        }
        return undefined                // unrecognised kind: cannot evaluate
    }

    // Evaluates one top-level x-rules entry: `true` means "this rule does not
    // block submission". Presentation kinds (visibleWhen/readonlyWhen) always
    // return true -- they never gate submission, only presentation (see
    // fieldVisible/fieldReadonly below). `and`/`or`/`not` are valid directly
    // as a top-level rule (not only nested inside a `when` clause) -- a single
    // rule carrying a compound condition tree -- so they delegate to
    // testCondition exactly like the comparison kinds already do.
    //
    // **An unevaluable rule does not block** -- an unrecognised `kind`, or a
    // recognised one whose condition tree contains an unrecognised node. The
    // renderer hands it to the server, which runs the compiled rule list and
    // has no "unrecognised kind" case at all (forms.md, "Renderer fallback").
    // Blocking instead would make every additive extension of the closed rule
    // vocabulary a breaking change for every deployed renderer: an older
    // client would refuse to submit *anything* against a newer server, and
    // the user would see a permanently disabled form with no way to satisfy
    // it. The correctness floor is unaffected -- it never depended on the
    // client understanding the key.
    function testRule(rule) {
        const kind = rule.kind
        const names = rule.fields || []
        if (kind === "requiredWhen") {
            // Only a definitely-true condition makes the field required;
            // `false` (vacuous) and `undefined` (unevaluable) both pass.
            if (testCondition(rule.when) !== true)
                return true
            return fieldEngaged(names[0])
        }
        if (kind === "greater" || kind === "greaterOrEqual" || kind === "less" || kind === "lessOrEqual")
            return testCondition(rule) !== false
        if (kind === "exactlyOneOf" || kind === "atLeastOneOf" || kind === "mutuallyExclusive") {
            let count = 0
            for (let i = 0; i < names.length; ++i) {
                if (fieldEngaged(names[i]))
                    count++
            }
            if (kind === "exactlyOneOf") return count === 1
            if (kind === "atLeastOneOf") return count >= 1
            return count <= 1
        }
        if (kind === "visibleWhen" || kind === "readonlyWhen")
            return true
        if (kind === "and" || kind === "or" || kind === "not")
            return testCondition(rule) !== false
        return true                     // unrecognised kind: defer to the server
    }

    // Whether `name` is required right now because some requiredWhen rule's
    // condition currently holds for it (in addition to the schema's static
    // `required` array).
    function isDynamicallyRequired(name) {
        for (let i = 0; i < rules.length; ++i) {
            const rule = rules[i]
            if (rule.kind === "requiredWhen" && rule.fields[0] === name)
                return testCondition(rule.when) === true
        }
        return false
    }

    // Whether `name` should be shown. A field with no visibleWhen rule is
    // always visible (renderer fallback per forms.md), and so is one whose
    // condition this renderer cannot evaluate -- hiding a field over an
    // unrecognised `kind` would remove the user's only way to fill in a form
    // the server may well accept.
    function fieldVisible(name) {
        for (let i = 0; i < rules.length; ++i) {
            const rule = rules[i]
            if (rule.kind === "visibleWhen" && rule.fields.indexOf(name) !== -1)
                return testCondition(rule.when) !== false
        }
        return true
    }

    // Whether `name` should be read-only. A field with no readonlyWhen rule
    // is always editable (renderer fallback), and so is one whose condition
    // cannot be evaluated -- same reasoning as fieldVisible.
    function fieldReadonly(name) {
        for (let i = 0; i < rules.length; ++i) {
            const rule = rules[i]
            if (rule.kind === "readonlyWhen" && rule.fields.indexOf(name) !== -1)
                return testCondition(rule.when) === true
        }
        return false
    }

    // --- draft state --------------------------------------------------------

    // --- locale numeric formatting (mirrors morph::render::locale_format.hpp)
    // The payload's exact digit routines below stay entirely locale-free —
    // this is the one control-edge conversion step, applied once per entry.

    // Grouping is *validated*, not stripped. A group separator is
    // only dropped where one can legally be -- preceded by one to three digits,
    // followed by exactly three more, never after the decimal separator.
    // Stripping it unconditionally turns a de-DE user's US-style "1.5" into 15:
    // a valid number, ten times too large, that nothing downstream can
    // recognise as wrong. The C++ edge validates on the same rule, and the two
    // are checked against each other on the same inputs.
    // The negative sign is matched as a whole string, not as one code unit.
    // 77 of the 711 locales Qt 6.11.2 knows spell it as something
    // other than a bare ASCII "-": 23 use U+2212, and 54 prefix it with a bidi
    // control mark (U+061C, U+200E, U+200F), making it two or three code units
    // -- ar_DZ does so even though its sign *is* the ordinary hyphen. `ch ===
    // "-"` matches none of them, and formatCanonicalNumber would emit a sign this
    // function then rejected. A bare "-" stays accepted alongside the locale's
    // own spelling: U+2212 and the bidi marks are on no keyboard, so matching
    // only the locale spelling would leave those users no way to type a
    // negative number at all. An omitted or empty negativeSign reads as "-",
    // not as "no sign" -- there is no locale without one.
    // A leading positive sign is accepted and *dropped*: canonical
    // text is -?[0-9]+(\.[0-9]+)?, which has no "+" in it, so "+5" yields "5".
    // 54 of the 711 locales spell the positive sign with a bidi control mark
    // before the "+" (U+061C, U+200E, U+200F), and unlike the negative side
    // there is no U+2212 analogue -- every non-ASCII spelling here is two or
    // three code units, so whole-string matching is the only thing that matches
    // any of them. formatCanonicalNumber below takes no positiveSign and never
    // emits one: a positive displays unsigned in every locale, and emitting the
    // sign would turn every positive number in every form from "5" into "+5".
    // The pair is therefore deliberately not inverse across a positive sign.
    // The digits are locale data too, carried as a *base*: a
    // Unicode decimal digit set is ten contiguous code points by definition
    // (UAX #44), so one zeroDigit is enough and a ten-element table is not
    // needed. 76 of the 711 locales Qt 6.11.2 knows report a zeroDigit other
    // than ASCII "0", across eleven distinct sets -- two of them astral
    // (U+11136 Chakma, U+1E950 Adlam), which is why this scans code *points*
    // via codePointAt and steps two units for one digit when it has to. Entry
    // accepts a digit in [zeroDigit, zeroDigit+9] or in ["0","9"]; display
    // emits only the locale's. That asymmetry is the positive-sign rule applied
    // to digits: the locale's own digits are on the user's keyboard only if their
    // keyboard has them, and accepting an extra spelling cannot change a value
    // because the canonical output always spells digits in ASCII. Mixing the
    // two families in one entry is malformed -- see below.
    // All five facts travel as one object rather than as five positional
    // arguments, mirroring the C++ NumericLocale aggregate: on that side a row
    // of interchangeable string_views would need a clang-tidy suppression for
    // bugprone-easily-swappable-parameters. Here the gain is the one a reader
    // gets -- a call names each fact -- and it keeps the two mirrors
    // structurally identical.
    // The two *separators* are matched as whole strings for the same reason the
    // signs are, and uniformly with them: `text.startsWith(sep, i)`, not
    // `ch === sep` over one UTF-16 code unit. No locale reaches the difference
    // -- measured over the 711 locales Qt 6.11.2 reports, *every* decimalPoint
    // and *every* groupSeparator is exactly one code unit, against 54
    // multi-unit spellings for each sign -- so this is for the reader, and for
    // docs/spec/forms/forms.md, "Both edges, or neither": the C++ edge matches
    // separators whole (`rest.starts_with(...)`), and a divergence between the
    // two is a divergence in what the product accepts.
    // The digit-set base of a NumericLocale-shaped object: the code point of
    // its zeroDigit, or ASCII "0" when it is absent or empty. Empty reads as
    // the default for the reason an empty negativeSign does -- there is no
    // locale without digits, so it cannot mean "this entry has none".
    function localeDigitBase(loc) {
        const zero = (loc && loc.zeroDigit) ? loc.zeroDigit : "0"
        return zero.codePointAt(0)
    }

    // A digit at index i, in the locale's set or in ASCII, or null. Returns the
    // canonical ASCII spelling, how many UTF-16 units it occupied (two for an
    // astral digit) and which family it came from. When base is "0" the two
    // families are the same set, so `native` is always true and nothing can mix.
    function leadingDigit(text, i, base) {
        const cp = text.codePointAt(i)
        if (cp === undefined)
            return null
        const units = cp > 0xFFFF ? 2 : 1
        if (cp >= base && cp < base + 10)
            return { canonical: String(cp - base), units: units, native: true }
        if (cp >= 0x30 && cp <= 0x39)
            return { canonical: String(cp - 0x30), units: units, native: false }
        return null
    }

    function normalizeLocaleNumber(text, locale) {
        const loc = locale ? locale : {}
        const decimalSeparator = loc.decimalSeparator !== undefined ? loc.decimalSeparator : "."
        const groupSeparator = loc.groupSeparator !== undefined ? loc.groupSeparator : ""
        // One string cannot play both roles: there is no reading of "1.5" this
        // function could defend, so it reports rather than guesses.
        if (groupSeparator !== "" && groupSeparator === decimalSeparator)
            return null

        const sign = loc.negativeSign ? loc.negativeSign : "-"
        const plus = loc.positiveSign ? loc.positiveSign : "+"
        const base = localeDigitBase(loc)
        const groupSize = 3
        let canonical = ""
        let sawDecimal = false
        let sawAnyOutput = false
        let digitsInGroup = 0
        let sawGroup = false
        let sawNativeDigit = false
        let sawAsciiDigit = false
        for (let i = 0; i < text.length; ++i) {
            const ch = text[i]
            if (groupSeparator !== "" && text.startsWith(groupSeparator, i)) {
                if (sawDecimal)
                    return null          // grouping belongs to the integer part only
                // The first group is one to three digits; every later one is
                // exactly three.
                const wellPlaced = sawGroup ? digitsInGroup === groupSize
                                            : (digitsInGroup >= 1 && digitsInGroup <= groupSize)
                if (!wellPlaced)
                    return null
                sawGroup = true
                digitsInGroup = 0
                i += groupSeparator.length - 1 // the loop's ++i consumes the last unit
                continue
            }
            if (decimalSeparator !== "" && text.startsWith(decimalSeparator, i)) {
                if (sawDecimal)
                    return null
                if (sawGroup && digitsInGroup !== groupSize)
                    return null          // the last group is short: "1.5" in de-DE
                sawDecimal = true
                canonical += "."
                // The decimal point is output, so a sign straight after it is
                // not leading.
                sawAnyOutput = true
                i += decimalSeparator.length - 1 // the loop's ++i consumes the last unit
                continue
            }
            if (text.startsWith(sign, i)) {
                if (sawAnyOutput)
                    return null
                canonical += "-"     // the canonical spelling, whatever the locale's is
                sawAnyOutput = true
                i += sign.length - 1 // the loop's ++i consumes the last unit
                continue
            }
            if (text.startsWith(plus, i)) {
                if (sawAnyOutput)
                    return null
                // Dropped, never carried into the output -- but sawAnyOutput is
                // still set, so "+-5" and "+1+2" stay malformed.
                sawAnyOutput = true
                i += plus.length - 1 // the loop's ++i consumes the last unit
                continue
            }
            if (ch === "-") {
                if (sawAnyOutput)
                    return null
                canonical += ch
            } else if (ch === "+") {
                // The bare ASCII spelling, accepted in every locale even when
                // the locale's own is a bidi-prefixed form. Dropped, as above.
                if (sawAnyOutput)
                    return null
            } else {
                const digit = leadingDigit(text, i, base)
                if (digit === null)
                    return null
                // Which family the digit came from is recorded and judged once,
                // after the loop: an entry that mixes them is malformed
                // wherever the second family appears.
                if (digit.native)
                    sawNativeDigit = true
                else
                    sawAsciiDigit = true
                canonical += digit.canonical
                ++digitsInGroup
                i += digit.units - 1 // the loop's ++i consumes the last unit
            }
            sawAnyOutput = true
        }
        // A grouped integer part has to end on a group boundary too.
        if (sawGroup && !sawDecimal && digitsInGroup !== groupSize)
            return null
        // Two digit families in one entry is malformed, not "55": neither a
        // keyboard nor a display edge produces an interleaving, and rejecting
        // it matches the rule that a sign anywhere but the leading position is
        // malformed. Pinned by a test on both edges, because accepting it would
        // have been equally implementable.
        if (sawNativeDigit && sawAsciiDigit)
            return null
        if (canonical === "" || canonical === "-")
            return null
        return canonical
    }

    // One canonical character as the locale would display it: an ASCII digit
    // becomes the code point that far above the digit base, anything else is
    // copied through. The pass-through arm is what keeps this byte-identical
    // for text that does not honour the canonical shape, which this function
    // has always passed out unchanged.
    function displayDigit(ch, base) {
        const cp = ch.codePointAt(0)
        return (cp >= 0x30 && cp <= 0x39) ? String.fromCodePoint(base + (cp - 0x30)) : ch
    }

    // The locale's positiveSign is deliberately not read here: a positive
    // number displays unsigned in every locale, so the entry edge above accepts
    // a leading "+" that this edge never produces. The digits, by contrast,
    // *are* emitted in the locale's set -- this edge has to match the entry
    // edge or the pair is not inverse, which is the round trip
    // docs/spec/forms/forms.md requires.
    function formatCanonicalNumber(text, locale) {
        const loc = locale ? locale : {}
        const decimalSeparator = loc.decimalSeparator !== undefined ? loc.decimalSeparator : "."
        const groupSeparator = loc.groupSeparator !== undefined ? loc.groupSeparator : ""
        // Empty reads as "-", not as "no sign": formatting a negative to no
        // sign at all would be a silently wrong value, not a rejected one.
        const sign = loc.negativeSign ? loc.negativeSign : "-"
        const base = localeDigitBase(loc)
        const neg = text.startsWith("-")
        const magnitude = neg ? text.slice(1) : text
        const dot = magnitude.indexOf(".")
        const wholePart = dot === -1 ? magnitude : magnitude.slice(0, dot)
        const fracPart = dot === -1 ? "" : magnitude.slice(dot + 1)
        let grouped = ""
        for (let i = 0; i < wholePart.length; ++i) {
            if (groupSeparator !== "" && i !== 0 && (wholePart.length - i) % 3 === 0)
                grouped += groupSeparator
            grouped += displayDigit(wholePart[i], base)
        }
        let fraction = ""
        for (let k = 0; k < fracPart.length; ++k)
            fraction += displayDigit(fracPart[k], base)
        return (neg ? sign : "") + grouped + (fracPart !== "" ? decimalSeparator + fraction : "")
    }

    // --- zoned Timestamp entry --------------------------------------------

    // Converts a "YYYY-MM-DDTHH:MM[:SS]" wall-clock reading in the display
    // zone (offsetMinutes minutes east of UTC) to the canonical UTC
    // ISO-8601 wire string. offsetMinutes === 0 is the identity transform.
    function zonedToUtcIso(text, offsetMinutes) {
        const m = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})(?::(\d{2}))?$/.exec(text)
        if (!m)
            return null
        const asUtcMillis = Date.UTC(parseInt(m[1]), parseInt(m[2]) - 1, parseInt(m[3]),
                                      parseInt(m[4]), parseInt(m[5]), m[6] === undefined ? 0 : parseInt(m[6]))
        const utcMillis = asUtcMillis - offsetMinutes * 60000
        const d = new Date(utcMillis)
        const pad = (v, w) => String(v).padStart(w, "0")
        return pad(d.getUTCFullYear(), 4) + "-" + pad(d.getUTCMonth() + 1, 2) + "-" + pad(d.getUTCDate(), 2)
               + "T" + pad(d.getUTCHours(), 2) + ":" + pad(d.getUTCMinutes(), 2) + ":" + pad(d.getUTCSeconds(), 2) + "Z"
    }

    // --- exact digit-string arithmetic (QML JS has no reliable BigInt) -----

    // digits * factor, both non-negative; factor stays well under 2^26 so the
    // per-digit products fit in doubles exactly.
    function mulDigits(digits, factor) {
        let carry = 0
        let out = ""
        for (let i = digits.length - 1; i >= 0; --i) {
            const prod = (digits.charCodeAt(i) - 48) * factor + carry
            out = String(prod % 10) + out
            carry = Math.floor(prod / 10)
        }
        while (carry > 0) {
            out = String(carry % 10) + out
            carry = Math.floor(carry / 10)
        }
        return out.replace(/^0+(?=\d)/, "")
    }

    function incDigits(digits) {
        const out = digits.split("")
        for (let i = out.length - 1; i >= 0; --i) {
            if (out[i] === "9") {
                out[i] = "0"
            } else {
                out[i] = String(+out[i] + 1)
                return out.join("")
            }
        }
        return "1" + out.join("")
    }

    // digits / divisor with half-up rounding; both non-negative.
    function divRoundDigits(digits, divisor) {
        let out = ""
        let rem = 0
        for (let i = 0; i < digits.length; ++i) {
            const cur = (rem * 10) + (digits.charCodeAt(i) - 48)
            out += String(Math.floor(cur / divisor))
            rem = cur % divisor
        }
        if (rem * 2 >= divisor)
            out = incDigits(out)
        return out.replace(/^0+(?=\d)/, "")
    }

    // The typed decimal as scaled digits (value * 10^dp), sign separate.
    function scaledDigits(text, dp) {
        const neg = text.startsWith("-")
        const pieces = (neg ? text.slice(1) : text).split(".")
        const frac = ((pieces[1] || "") + "0".repeat(dp)).slice(0, dp)
        const digits = ((pieces[0] || "0") + frac).replace(/^0+(?=\d)/, "")
        return { neg: neg && digits !== "0", digits: digits }
    }

    // Exact rational JSON for a value typed in `unit` (the exact
    // unit-to-canonical ratio rides along; payloads stay canonical).
    function rationalJson(text, unit, canonDp) {
        const scaled = scaledDigits(text, unit.decimals)
        const num = mulDigits(scaled.digits, unit.num)
        const den = mulDigits("1" + "0".repeat(unit.decimals), unit.den)
        return '{"num":' + (scaled.neg ? "-" : "") + num + ',"den":' + den + ',"dp":' + canonDp + "}"
    }

    // Recalculate a decimal string from one unit into another, exactly,
    // rounded half-up to the target unit's decimals.
    function convertText(text, from, to) {
        if (!/^-?\d+(\.\d+)?$/.test(text))
            return ""
        const divisor = from.den * to.num * Math.pow(10, from.decimals)
        if (divisor > 1e12)
            return ""  // out of the demo's exact long-division range
        const scaled = scaledDigits(text, from.decimals)
        let digits = mulDigits(scaled.digits, from.num)
        digits = mulDigits(digits, to.den)
        digits = digits + "0".repeat(to.decimals)
        digits = divRoundDigits(digits, divisor)
        if (to.decimals === 0)
            return (scaled.neg ? "-" : "") + digits
        const padded = digits.padStart(to.decimals + 1, "0")
        return (scaled.neg ? "-" : "") + padded.slice(0, -to.decimals) + "." + padded.slice(-to.decimals)
    }

    // Encodes an "array"-typed field's comma-separated entry text as a
    // genuine JSON array literal of strings -- e.g. "red, green, blue" ->
    // ["red","green","blue"] -- never the JSON *string* the generic
    // fallback (`JSON.stringify(text)`) would have produced. Splits on
    // comma, trims surrounding whitespace off each entry, and drops empty
    // entries (so "red,, green," -> ["red","green"], not ["red","","green",""]).
    // An entry list that is blank or entirely empty after trimming (","," ,")
    // returns "[]" -- a genuinely empty array is still a valid array
    // literal, distinct from the field itself being unengaged (handled by
    // fieldJsonLiteral's blank-text check before this is ever called).
    // A collection-of-objects field's rows, as the JS array a slot wrote with
    // setRows (JSON text in fieldValues), or [] when there are none yet or the
    // text is not an array.
    function objectArrayRows(text) {
        if (text === undefined || text === null || String(text).trim() === "")
            return []
        try {
            const parsed = JSON.parse(text)
            return Array.isArray(parsed) ? parsed : []
        } catch (ignored) {
            return []
        }
    }

    // Encodes a collection of objects from its rows' cell texts: `text` is a
    // JSON array of `{member: cellText}` objects, one per row, where each cell
    // text is what the built-in control for that member would hold (a digit
    // string for a number or Quantity, "true"/"false" for a boolean, the
    // option's `valueJson` for a closed set). Each cell goes through
    // encodeFieldText with the element's own member descriptor, so the same
    // syntax, locale, precision and bound rules apply as at the top level; a
    // blank optional cell is omitted from its row object. Returns null -- no
    // literal, so the form is not ready -- when the text is not an array of
    // objects, a cell does not encode, or a required member is blank.
    function encodeObjectArray(f, text) {
        let rows
        try {
            rows = JSON.parse(text)
        } catch (ignored) {
            return null
        }
        if (!Array.isArray(rows))
            return null
        const encodedRows = []
        for (let r = 0; r < rows.length; ++r) {
            const row = rows[r]
            if (row === null || typeof row !== "object" || Array.isArray(row))
                return null
            const parts = []
            for (let m = 0; m < f.itemFields.length; ++m) {
                const member = f.itemFields[m]
                const cell = row[member.name]
                const cellText = (cell === undefined || cell === null) ? "" : String(cell)
                const literal = encodeFieldText(member, cellText, 0)
                if (literal === null) {
                    if (cellText.trim() !== "" || member.required)
                        return null
                    continue
                }
                parts.push(JSON.stringify(member.name) + ":" + literal)
            }
            encodedRows.push("{" + parts.join(",") + "}")
        }
        return "[" + encodedRows.join(",") + "]"
    }

    function arrayJsonLiteral(text) {
        const items = text.split(",")
            .map(function (item) { return item.trim() })
            .filter(function (item) { return item !== "" })
        return JSON.stringify(items)
    }

    // Encodes one field's current input text as the JSON literal morph
    // expects on the wire, applying the same per-kind syntax and bounds
    // checks as submission. Returns null when the field is blank or its
    // typed text does not currently encode to a valid literal. Shared by
    // revalidate() (the submit body) and optionsRequestBody() (a dependent
    // Choice's parent values).
    function fieldJsonLiteral(f) {
        return encodeFieldText(f, opt(fieldValues[f.name], ""), opt(fieldUnits[f.name], 0))
    }

    // The encoder behind fieldJsonLiteral, over an explicit text and unit
    // selection instead of the form's own draft -- so a collection's cells,
    // which have no entry in fieldValues, go through exactly the rules a
    // top-level control's text does. `unitIndex` selects from f.unitOptions
    // (0 is the canonical unit).
    function encodeFieldText(f, rawText, unitIndex) {
        const text = String(rawText === undefined || rawText === null ? "" : rawText).trim()
        if (text === "")
            return null
        // A member no control can collect has no literal, whatever was typed:
        // every encoding below would produce a value of the wrong JSON type,
        // and a wrong literal is worse than none, because it is the one that
        // makes the form report ready.
        if (f.unrepresentable !== "")
            return null
        if (f.isObjectArray) {
            return encodeObjectArray(f, text)
        }
        if (f.isArray) {
            return arrayJsonLiteral(text)
        }
        if (f.isEnum) {
            // Also already a JSON literal — but here the whole set is in the
            // schema, so membership is decidable *on the client*, and a value
            // outside it is invalid rather than merely "the server will say
            // no". Without this the form reports ready for role="Emperor"
            // and assembles a body for it, which is the opposite
            // of what a submit gate is for. Same reason isBoolean refuses
            // anything but true/false. A server-fetched Choice below is
            // deliberately not checked this way: its option list is a
            // snapshot that may already be stale (choice.md, "Validation &
            // staleness"), so the server owns that verdict.
            for (let i = 0; i < f.enumOptions.length; ++i) {
                if (f.enumOptions[i].valueJson === text)
                    return text
            }
            return null
        }
        if (f.isChoice) {
            return text  // already a JSON literal (see the ComboBox's onActivated)
        }
        if (f.isDateTime) {
            const utcIso = zonedToUtcIso(text, displayOffsetMinutes)
            return utcIso === null ? null : JSON.stringify(utcIso)
        }
        if (f.isQuantity) {
            const canonicalText = normalizeLocaleNumber(text, {
                                                            decimalSeparator: qtLocale.decimalPoint,
                                                            groupSeparator: qtLocale.groupSeparator,
                                                            negativeSign: qtLocale.negativeSign,
                                                            positiveSign: qtLocale.positiveSign,
                                                            zeroDigit: qtLocale.zeroDigit
                                                        })
            if (canonicalText === null || !/^-?\d+(\.\d+)?$/.test(canonicalText))
                return null
            const unit = f.unitOptions[unitIndex]
            // Reject more decimals than the current unit's precision instead
            // of silently rounding them away.
            const fracLen = (canonicalText.split(".")[1] || "").length
            if (fracLen > unit.decimals)
                return null
            const value = parseFloat(canonicalText)
            // Bounds are declared against the canonical unit.
            if (unitIndex === 0) {
                if (f.minimum !== undefined && value < f.minimum)
                    return null
                if (f.maximum !== undefined && value > f.maximum)
                    return null
                // A decorated schema's per-instance range. Narrows
                // the compiled bound; it never widens it, because both are
                // checked. Quantity fields only, matching what
                // InstanceConstraints::checkAction checks server-side -- a
                // client that gated a key the model does not check would be a
                // new divergence, not a fix for one.
                if (f.instanceMinimum !== undefined && value < f.instanceMinimum)
                    return null
                if (f.instanceMaximum !== undefined && value > f.instanceMaximum)
                    return null
                if (violatesMultipleOf(value, f.multipleOf))
                    return null
            }
            return rationalJson(canonicalText, unit, f.canonDp)
        }
        if (f.isInteger) {
            if (!/^-?\d+$/.test(text))
                return null
            // Normalise "007" -> "7": JSON forbids leading zeros in numbers.
            const normalised = text.replace(/^(-?)0+(?=\d)/, "$1")
            // Prefer the exact string bound when the schema carries one: a
            // double-valued bound rounds at 2^53, and comparing INT64_MAX + 1
            // against a maximum rounded *up* to 9223372036854775808 judges it
            // "not greater" and lets it through the gate.
            const value = parseInt(text)
            if (f.exactMinimum !== undefined) {
                if (compareIntText(normalised, f.exactMinimum) < 0)
                    return null
            } else if (f.minimum !== undefined && value < f.minimum) {
                return null
            }
            if (f.exactMaximum !== undefined) {
                if (compareIntText(normalised, f.exactMaximum) > 0)
                    return null
            } else if (f.maximum !== undefined && value > f.maximum) {
                return null
            }
            if (violatesMultipleOf(value, f.multipleOf))
                return null
            return normalised
        }
        if (f.isNumber) {
            // A bare `double`/`float` member: `"type": "number"` with no
            // x-decimalPlaces and no Quantity wrapper, so none of the encoders
            // above claims it. It needs one of its own -- the generic
            // fall-through at the end of this function would wrap the typed
            // digits as a JSON *string*, and the schema asks for a number.
            // Declaring it unrepresentable instead would be worse than the
            // wrong value it replaces: a number is exactly what a text field
            // collects, so a form carrying one would never be submittable.
            //
            // Locale-normalised first, like a Quantity's entry: a decimal
            // field is typed with the locale's decimal separator and
            // grouping, and neither belongs in the JSON number.
            const canonicalNumber = normalizeLocaleNumber(text, {
                                                              decimalSeparator: qtLocale.decimalPoint,
                                                              groupSeparator: qtLocale.groupSeparator,
                                                              negativeSign: qtLocale.negativeSign,
                                                              positiveSign: qtLocale.positiveSign,
                                                              zeroDigit: qtLocale.zeroDigit
                                                          })
            // No exponent and no trailing separator: the grammar a text field
            // is expected to collect, and every spelling outside it (blank
            // fraction, stray sign, letters) is refused rather than encoded.
            if (canonicalNumber === null || !/^-?\d+(\.\d+)?$/.test(canonicalNumber))
                return null
            // A declared display precision is an entry limit, as a Quantity's
            // is: more fraction digits are refused rather than rounded away.
            if (f.displayDecimals !== undefined
                    && (canonicalNumber.split(".")[1] || "").length > f.displayDecimals)
                return null
            const numberValue = parseFloat(canonicalNumber)
            // The declared range, which for a plain member is the one glaze
            // stamps on the type itself: a `float` field carries ±3.4e38, so
            // this gate refuses a value the member cannot hold. `ready` is a
            // claim about the payload satisfying the schema, so a bound the
            // schema states is checked here whether or not the model's own
            // bound check covers this member kind.
            if (f.minimum !== undefined && numberValue < f.minimum)
                return null
            if (f.maximum !== undefined && numberValue > f.maximum)
                return null
            if (violatesMultipleOf(numberValue, f.multipleOf))
                return null
            // Digits carried through as typed, never through parseFloat: a
            // round-trip through a JS number re-spells what the user wrote
            // ("1e+41" for a long entry) and rounds at the seventeenth digit.
            // Only the leading-zero run has to go, because JSON forbids it.
            return canonicalNumber.replace(/^(-?)0+(?=\d)/, "$1")
        }
        if (f.isBoolean) {
            // Emitted bare, never quoted. The CheckBox only ever stores these
            // two spellings; any other retained value (a prefill from a stale
            // payload, say) is invalid rather than silently coerced.
            if (text === "true")
                return "true"
            if (text === "false")
                return "false"
            return null
        }
        return JSON.stringify(text)
    }

    function revalidate() {
        // Assembled as JSON text (not JSON.stringify) so rational digits and
        // int64-sized integers stay exact.
        const parts = []
        let ok = true
        let blocker = ""
        for (let i = 0; i < fields.length; ++i) {
            const f = fields[i]
            const text = (opt(fieldValues[f.name], "")).trim()
            const literal = fieldJsonLiteral(f)
            if (literal === null) {
                if (text !== "" || f.required || isDynamicallyRequired(f.name)) {
                    ok = false
                    // An unrepresentable member blocks submission only when
                    // the payload would have to carry it -- the schema
                    // requires it, or the user typed into it anyway. One left
                    // blank and optional is legitimately omitted, and a
                    // payload the schema accepts is not something to report.
                    // First one wins: the caller wants a reason, and the whole
                    // set is on the field descriptors.
                    if (blocker === "" && f.unrepresentable !== "")
                        blocker = f.name + ": " + f.unrepresentable
                }
                continue
            }
            parts.push(JSON.stringify(f.name) + ":" + literal)
        }
        // Cross-field rules (x-rules): evaluated after the per-field checks
        // above, over the same draft. Presentation kinds (visibleWhen /
        // readonlyWhen) never fail this loop -- testRule always returns
        // true for them.
        if (ok) {
            for (let r = 0; r < rules.length; ++r) {
                if (!testRule(rules[r])) {
                    ok = false
                    break
                }
            }
        }
        ready = ok
        unrepresentableReason = ok ? "" : blocker
        previewLine = ok ? "{" + parts.join(",") + "}" : ""
        rulesRevision++
        // In explicit-submit mode the renderer never fires on its own --
        // only submit() (wired to the explicit submit Button below) does.
        if (!form.explicitSubmitMode && ready && form.controller && form.programmaticEdit === 0)
            form.controller.submitIfValid(form.actionType, form.previewLine)
    }

    // Explicit submit mode's sole trigger: the submit Button's onClicked
    // calls this. A no-op unless the form is currently ready -- the button
    // is also disabled while !ready, so this guard is defense in depth, not
    // the only gate.
    function submit() {
        if (ready && form.controller)
            form.controller.submitIfValid(form.actionType, form.previewLine)
    }

    // Runs `body` with auto-submit suppressed (see programmaticEdit), then
    // revalidates once so `ready`/`previewLine` reflect the result.
    function withoutAutoSubmit(body) {
        form.programmaticEdit++
        try {
            body()
        } finally {
            form.programmaticEdit--
        }
        form.revalidate()
    }

    // Depth-first lookup of a control by objectName within this form.
    function findControl(item, name) {
        if (!item)
            return null
        if (item.objectName === name)
            return item
        const kids = item.children || []
        for (let i = 0; i < kids.length; ++i) {
            const found = form.findControl(kids[i], name)
            if (found)
                return found
        }
        return null
    }

    // Clears every field back to its unedited state: the value map, the unit
    // selections, and the visible controls.
    //
    // A form instance is reused across the rows it edits (CollectionView keeps
    // one modalForm and one detailForm for the whole collection), and prefill
    // only writes the fields named in v-rowAction's bind. Without an explicit
    // reset, everything else kept the previous row's value -- and because
    // revalidate() submits as soon as the form is `ready`, opening a second row
    // fired the action with that row's id and the *previous* row's field
    // values, writing data the user never entered and never saw.
    function resetFields() {
        form.withoutAutoSubmit(function() {
            form.fieldValues = ({})
            form.fieldUnits = ({})
            for (let i = 0; i < form.fields.length; ++i) {
                const name = form.fields[i].name
                const entry = form.findControl(form, "field_" + name)
                if (entry) {
                    // An enum's combo box claims this objectName
                    // and carries no writable `text` -- "no selection" is
                    // currentIndex -1, the state it is created in.
                    if (form.fields[i].isEnum)
                        entry.currentIndex = -1
                    else
                        entry.text = ""
                }
                const area = form.findControl(form, "multiline_" + name)
                if (area)
                    area.text = ""
                const slider = form.findControl(form, "slider_" + name)
                if (slider)
                    slider.value = slider.from
            }
        })
    }

    // The JSON body to send a Choice field's options action: {parentName:
    // value, ...} built from the current values of its declared parents
    // (x-optionsDependsOn). Returns null when any parent is not yet engaged
    // or valid — the caller must not fetch in that case (same null
    // convention as fieldJsonLiteral).
    function optionsRequestBody(field) {
        const parts = []
        for (let i = 0; i < field.dependsOn.length; ++i) {
            const parentName = field.dependsOn[i]
            const parent = form.fieldByName[parentName]
            const literal = parent ? form.fieldJsonLiteral(parent) : null
            if (literal === null)
                return null
            parts.push(JSON.stringify(parentName) + ":" + literal)
        }
        return "{" + parts.join(",") + "}"
    }

    // Re-fetches (or clears) every Choice field that depends on parentName,
    // called whenever parentName's value changes. A child whose parents are
    // not all currently engaged is not fetched — its options are cleared and
    // its stale selection (if any) is dropped instead.
    function refreshDependents(parentName) {
        const children = form.dependents[parentName] || []
        for (let i = 0; i < children.length; ++i) {
            const child = form.fieldByName[children[i]]
            const body = form.optionsRequestBody(child)
            if (body === null) {
                form.fieldOptions[child.name] = []
                form.optionsRevision++
                if ((opt(form.fieldValues[child.name], "")) !== "") {
                    form.fieldValues[child.name] = ""
                    form.revalidate()
                }
                continue
            }
            if (form.controller)
                form.controller.fetchOptions(child.optionsAction, body)
        }
    }

    function setFieldValue(name, text) {
        fieldValues[name] = text
        revalidate()
        if (form.dependents[name] !== undefined)
            form.refreshDependents(name)
    }

    // Extracts the option rows from an options action's result: the result
    // itself when it is an array, otherwise its first array-valued member.
    function optionRows(result) {
        if (Array.isArray(result))
            return result
        for (const key in result) {
            if (Array.isArray(result[key]))
                return result[key]
        }
        return []
    }

    // If the reply is a bare rational, append its decimal reading.
    function humanize(payload) {
        try {
            const parsed = JSON.parse(payload)
            if (parsed !== null && parsed.num !== undefined && parsed.den !== undefined)
                return payload + "  =  " + (parsed.num / parsed.den).toFixed(opt(parsed.dp, 3))
        } catch (ignored) {}
        return payload
    }

    // `replyReceived` is on every controller, so this block stays strict: a
    // handler here that matches no signal is a misspelling and must be loud.
    Connections {
        target: form.controller
        function onReplyReceived(actionType, ok, payload) {
            if (actionType !== form.actionType)
                return
            form.resultOk = ok
            form.resultText = ok ? form.humanize(payload) : payload
        }
    }

    // `optionsReceived` is not. It exists only on a controller that serves a
    // morph::forms::Choice field, and a controller that serves none declares
    // no stub for it -- the sanctioned shape (bookmarks' and pastebin's forms
    // controllers both document why). Unaccommodated, that shape makes the form
    // warn once per instance about the handler below.
    //
    // The accommodation is the gated target, not `ignoreUnknownSignals`. A
    // controller without the signal is never connected to, so there is nothing
    // to warn about; a controller that has it is connected strictly, so a
    // misspelling of the handler below is still reported. `ignoreUnknownSignals`
    // would silence both, and the second is not noise: a misspelled handler on a
    // Choice-serving controller drops every option, leaves the combo box empty
    // and the form short of `ready`, and would then do so with no diagnostic at
    // all.
    Connections {
        target: form.controller && form.controller.optionsReceived !== undefined
                    ? form.controller : null
        function onOptionsReceived(optionsAction, ok, payload) {
            if (!ok)
                return
            let parsed
            // Exact-int aware: an option id above 2^53 is rounded by a plain
            // JSON.parse, and re-stringifying the rounded number selects a
            // different row -- or, for a dense id range, makes two options
            // indistinguishable from each other.
            try { parsed = JsonExact.parse(payload) } catch (ignored) { return }
            for (let i = 0; i < form.fields.length; ++i) {
                const f = form.fields[i]
                if (!f.isChoice || f.optionsAction !== optionsAction)
                    continue
                form.fieldOptions[f.name] = form.optionRows(parsed).map(function (row) {
                    return { label: JsonExact.text(row[f.labelField]),
                             valueJson: JsonExact.literal(row[f.valueField]) }
                })
                // A parent change re-fetches; drop a selection the new list
                // no longer backs (closes the staleness noted in choice.md's
                // Failure modes). A no-op for an independent Choice: its
                // options rarely change underneath an already-made
                // selection, but the check is unconditional and harmless
                // either way.
                const current = form.fieldValues[f.name]
                if (current !== undefined && current !== ""
                    && !form.fieldOptions[f.name].some(function (row) { return row.valueJson === current })) {
                    form.fieldValues[f.name] = ""
                    form.revalidate()
                }
            }
            form.optionsRevision++
        }
    }

    Component {
        id: fieldDelegate

        ColumnLayout {
            id: fieldColumn
            objectName: "column_" + fieldColumn.modelData.name
            required property var modelData
            Layout.fillWidth: true
            Layout.columnSpan: fieldColumn.modelData.colspan
            visible: { form.rulesRevision; return !fieldColumn.modelData.hidden && form.fieldVisible(fieldColumn.modelData.name) }
            enabled: { form.rulesRevision; return !form.fieldReadonly(fieldColumn.modelData.name) }
            spacing: 2

            property var labelChrome: form.chrome("fieldLabel")
            property var helpChrome: form.chrome("fieldHelp")

            RowLayout {
                visible: fieldColumn.labelChrome === null
                Label {
                    text: fieldColumn.modelData.label
                    font.bold: true
                }
                Label {
                    visible: { form.rulesRevision; return fieldColumn.modelData.required || form.isDynamicallyRequired(fieldColumn.modelData.name) }
                    text: "*"
                    color: "#d33"
                }
            }

            // A host's label chrome replaces the row above: the caption, the
            // live required marker and the invalid state are its to draw.
            Loader {
                active: fieldColumn.labelChrome !== null
                visible: active
                Layout.fillWidth: true
                sourceComponent: fieldColumn.labelChrome
                onLoaded: form.bindChrome(item, {
                    field: fieldColumn.modelData,
                    text: fieldColumn.modelData.label,
                    required: Qt.binding(function () {
                        form.rulesRevision
                        return fieldColumn.modelData.required || form.isDynamicallyRequired(fieldColumn.modelData.name)
                    }),
                    invalid: Qt.binding(function () {
                        form.rulesRevision
                        return form.fieldInvalid(fieldColumn.modelData.name)
                    })
                })
            }

            Label {
                visible: fieldColumn.helpChrome === null && fieldColumn.modelData.description !== ""
                text: fieldColumn.modelData.description
                opacity: 0.6
                font.pixelSize: 12
            }

            Loader {
                active: fieldColumn.helpChrome !== null && fieldColumn.modelData.description !== ""
                visible: active
                Layout.fillWidth: true
                sourceComponent: fieldColumn.helpChrome
                onLoaded: form.bindChrome(item, {
                    field: fieldColumn.modelData,
                    text: fieldColumn.modelData.description
                })
            }

            RowLayout {
                id: controlsRow
                Layout.fillWidth: true

                // Resolution order: field -> x-widget -> unit -> type ->
                // null (built-in). Entirely client-side -- see
                // SlotRegistry.qml and docs/spec/forms/forms.md ("Theming /
                // component-override registry").
                property var overrideComponent: form.slotRegistry
                    ? form.slotRegistry.resolve(form.actionType, fieldColumn.modelData.name,
                                                 fieldColumn.modelData.xWidget,
                                                 fieldColumn.modelData.unitAscii,
                                                 fieldColumn.modelData.jsonType,
                                                 fieldColumn.modelData.kind)
                    : null

                Loader {
                    id: overrideLoader
                    Layout.fillWidth: true
                    sourceComponent: controlsRow.overrideComponent
                    // Contract every registered slot Component implements: a
                    // `field` property (the resolved, merged def+property
                    // descriptor) and a `setValue(text)` function -- the
                    // same set-value path the built-in controls use, so an
                    // override participates in the required-gate and
                    // auto-fire without special-casing.
                    //
                    // Optional, each assigned only when the slot declares it:
                    // `fieldText` (the retained text, kept current -- a prefill,
                    // a reset or a rebuilt tab reaches the slot through it),
                    // `rows` / `setRows(rows)` (a collection of objects as a JS
                    // array of {member: cellText}), and `form` (this form, for
                    // encodeFieldText and the rest of its public surface).
                    onLoaded: {
                        const name = fieldColumn.modelData.name
                        item.field = fieldColumn.modelData
                        item.setValue = function (text) { form.setFieldValue(name, text) }
                        // revalidate() bumps rulesRevision after every write to
                        // fieldValues, a plain object that notifies nothing.
                        if ("fieldText" in item)
                            item.fieldText = Qt.binding(function () {
                                form.rulesRevision
                                return form.opt(form.fieldValues[name], "")
                            })
                        if ("rows" in item)
                            item.rows = Qt.binding(function () {
                                form.rulesRevision
                                return form.objectArrayRows(form.fieldValues[name])
                            })
                        if ("setRows" in item)
                            item.setRows = function (rows) { form.setFieldValue(name, JSON.stringify(rows)) }
                        if ("form" in item)
                            item.form = form
                    }
                }

                // One combo box for both closed sets: the server-fetched
                // Choice (x-optionsAction) and the schema-stated enum
                // (a `oneOf` of `const`s, or a bare `enum`). They
                // differ only in where the rows come from -- an enum's are
                // already in the schema, so it never fetches -- and the rows
                // have the same {label, valueJson} shape either way.
                ComboBox {
                    id: choiceEntry
                    // An enum claims the field_ objectName from the plain
                    // TextField, which is hidden for it, exactly as the
                    // CheckBox does for a boolean. A fetched Choice keeps the
                    // pre-existing arrangement (the hidden TextField holds
                    // the name) so no existing caller's lookup changes.
                    objectName: fieldColumn.modelData.isEnum
                                ? "field_" + fieldColumn.modelData.name : ""
                    visible: overrideLoader.sourceComponent === null
                             && (fieldColumn.modelData.isChoice || fieldColumn.modelData.isEnum)
                             && !fieldColumn.modelData.isRadioChoice
                    // A dependent Choice (x-optionsDependsOn) stays disabled
                    // until its parent(s) are engaged and a fetch has
                    // populated fieldOptions; an independent Choice is
                    // unaffected (dependsOn.length === 0 always short-circuits
                    // true here, exactly like before this feature existed).
                    // An enum has no parents and needs no fetch, so it is only
                    // ever disabled by x-readonly.
                    enabled: !fieldColumn.modelData.readOnly
                             && (fieldColumn.modelData.isEnum
                                 || fieldColumn.modelData.dependsOn.length === 0
                                 || (form.fieldOptions[fieldColumn.modelData.name] || []).length > 0)
                    Layout.fillWidth: true
                    textRole: "label"
                    currentIndex: -1
                    displayText: currentIndex < 0 ? "— select —" : currentText
                    model: {
                        form.optionsRevision
                        return fieldColumn.modelData.isEnum
                               ? fieldColumn.modelData.enumOptions
                               : (form.fieldOptions[fieldColumn.modelData.name] || [])
                    }
                    onActivated: form.setFieldValue(fieldColumn.modelData.name, model[currentIndex].valueJson)
                    // Re-seed from the retained value whenever this delegate is
                    // (re)created -- see the plain TextField's comment below
                    // for why (a tab switch destroys and rebuilds every
                    // control, and `currentIndex` is otherwise write-only).
                    // Enum only: its rows are in the schema and so are present
                    // at creation, whereas a fetched Choice's arrive later and
                    // are re-selected by onOptionsReceived instead.
                    Component.onCompleted: {
                        if (!fieldColumn.modelData.isEnum)
                            return
                        const retained = form.opt(form.fieldValues[fieldColumn.modelData.name], "")
                        const rows = fieldColumn.modelData.enumOptions
                        for (let i = 0; i < rows.length; ++i) {
                            if (rows[i].valueJson === retained) {
                                choiceEntry.currentIndex = i
                                return
                            }
                        }
                    }
                    Accessible.role: Accessible.ComboBox
                    Accessible.name: fieldColumn.modelData.name
                    Accessible.description: (fieldColumn.modelData.required ? "Required. " : "")
                                             + fieldColumn.modelData.description
                }

                // x-widget: "radio" turns a Choice into a radio group instead
                // of a combo box; the options come from the same
                // fetchOptions() call either way.
                ColumnLayout {
                    id: radioGroup
                    objectName: "radio_" + fieldColumn.modelData.name
                    visible: overrideLoader.sourceComponent === null
                             && fieldColumn.modelData.isChoice && fieldColumn.modelData.isRadioChoice
                    Layout.fillWidth: true
                    spacing: 2
                    property int checkedIndex: -1
                    Accessible.role: Accessible.Grouping
                    Accessible.name: fieldColumn.modelData.name
                    Accessible.description: (fieldColumn.modelData.required ? "Required. " : "")
                                             + fieldColumn.modelData.description

                    ButtonGroup { id: radioButtons }

                    Repeater {
                        model: { form.optionsRevision; return form.fieldOptions[fieldColumn.modelData.name] || [] }
                        delegate: RadioButton {
                            required property var modelData
                            required property int index
                            text: modelData.label
                            enabled: !fieldColumn.modelData.readOnly
                            ButtonGroup.group: radioButtons
                            checked: radioGroup.checkedIndex === index
                            onToggled: {
                                radioGroup.checkedIndex = index
                                form.setFieldValue(fieldColumn.modelData.name, modelData.valueJson)
                            }
                        }
                    }
                }

                DateTimePicker {
                    visible: overrideLoader.sourceComponent === null && fieldColumn.modelData.isDateTime
                    enabled: !fieldColumn.modelData.readOnly
                    Layout.fillWidth: true
                    onEdited: text => form.setFieldValue(fieldColumn.modelData.name, text)
                    Accessible.role: Accessible.EditableText
                    Accessible.name: fieldColumn.modelData.name
                    Accessible.description: (fieldColumn.modelData.required ? "Required. " : "")
                                             + fieldColumn.modelData.description
                }

                TextField {
                    id: entry
                    objectName: (fieldColumn.modelData.isArray || fieldColumn.modelData.isBoolean
                                 || fieldColumn.modelData.isEnum)
                                ? "" : "field_" + fieldColumn.modelData.name
                    visible: overrideLoader.sourceComponent === null
                             && !fieldColumn.modelData.isChoice && !fieldColumn.modelData.isDateTime
                             && !fieldColumn.modelData.isMultiline && !fieldColumn.modelData.isSlider
                             && !fieldColumn.modelData.isArray && !fieldColumn.modelData.isBoolean
                             && !fieldColumn.modelData.isEnum
                    Layout.fillWidth: true
                    readOnly: fieldColumn.modelData.readOnly
                    placeholderText: fieldColumn.modelData.placeholder !== ""
                                     ? fieldColumn.modelData.placeholder
                                     : (fieldColumn.modelData.isQuantity
                                        ? "0." + "0".repeat(Math.max(1, fieldColumn.modelData.decimals))
                                        : (fieldColumn.modelData.isInteger ? "0"
                                           : (fieldColumn.modelData.displayDecimals !== undefined
                                              ? (fieldColumn.modelData.displayDecimals > 0
                                                 ? "0." + "0".repeat(fieldColumn.modelData.displayDecimals) : "0")
                                              : "")))
                    inputMethodHints: (fieldColumn.modelData.isQuantity || fieldColumn.modelData.isInteger
                                       || fieldColumn.modelData.isNumber)
                                      ? Qt.ImhFormattedNumbersOnly : Qt.ImhNone
                    onTextChanged: form.setFieldValue(fieldColumn.modelData.name, text)
                    // Re-seed from the retained value whenever this delegate is
                    // (re)created. The tabbed layout drives its Repeater off
                    // `sections[currentTab].fields`, so switching tabs destroys
                    // and rebuilds every control, and `text` is otherwise
                    // write-only -- it flows out via onTextChanged and never
                    // back in. Returning to a tab therefore showed empty
                    // controls while revalidate() went on auto-submitting the
                    // values still held in fieldValues: the form sent data the
                    // user could not see. Not a `text:` binding, because
                    // prefill() assigns text imperatively and would break it.
                    Component.onCompleted: form.withoutAutoSubmit(function() {
                        entry.text = form.opt(form.fieldValues[fieldColumn.modelData.name], "")
                    })
                    Accessible.role: Accessible.EditableText
                    Accessible.name: fieldColumn.modelData.name
                    Accessible.description: (fieldColumn.modelData.required ? "Required. " : "")
                                             + fieldColumn.modelData.description
                }

                // "array" (glaze's std::vector<T> schema shape) — a
                // comma-separated-with-validation control: the typed text is
                // split on comma, each entry trimmed, and encoded as a
                // genuine JSON array literal by fieldJsonLiteral/
                // arrayJsonLiteral, never wrapped as a JSON *string* the way
                // the plain TextField's fallback would. Reuses the plain
                // TextField's field_ objectName -- the two are mutually
                // exclusive per field (isArray), so exactly one claims it.
                TextField {
                    id: arrayEntry
                    objectName: fieldColumn.modelData.isArray ? "field_" + fieldColumn.modelData.name : ""
                    visible: overrideLoader.sourceComponent === null && fieldColumn.modelData.isArray
                    Layout.fillWidth: true
                    readOnly: fieldColumn.modelData.readOnly
                    placeholderText: fieldColumn.modelData.placeholder !== ""
                                     ? fieldColumn.modelData.placeholder
                                     : "comma-separated (e.g. red, green, blue)"
                    onTextChanged: form.setFieldValue(fieldColumn.modelData.name, text)
                    // Re-seed from the retained value whenever this delegate
                    // is (re)created — see the plain TextField's comment
                    // above for why (tab-switch destroys/rebuilds delegates).
                    Component.onCompleted: form.withoutAutoSubmit(function() {
                        arrayEntry.text = form.opt(form.fieldValues[fieldColumn.modelData.name], "")
                    })
                    Accessible.role: Accessible.EditableText
                    Accessible.name: fieldColumn.modelData.name
                    Accessible.description: (fieldColumn.modelData.required ? "Required. " : "")
                                             + fieldColumn.modelData.description
                                             + " Comma-separated list."
                }

                // "boolean" — a CheckBox. The plain TextField's fall-through
                // would encode the typed text as a JSON *string*
                // ({"flag":"true"}), and apply no validation at all, so
                // "banana" would be accepted and sent; glaze rejects both with
                // expected_true_or_false. A CheckBox can only produce the two valid
                // spellings. Reuses the plain TextField's field_ objectName —
                // the two are mutually exclusive per field (isBoolean), so
                // exactly one claims it.
                CheckBox {
                    id: boolEntry
                    objectName: fieldColumn.modelData.isBoolean ? "field_" + fieldColumn.modelData.name : ""
                    visible: overrideLoader.sourceComponent === null && fieldColumn.modelData.isBoolean
                    enabled: !fieldColumn.modelData.readOnly
                    onToggled: form.setFieldValue(fieldColumn.modelData.name, checked ? "true" : "false")
                    // Re-seed from the retained value whenever this delegate is
                    // (re)created — see the plain TextField's comment above for
                    // why (a tab switch destroys and rebuilds every control).
                    //
                    // A *required* boolean with no retained value is seeded
                    // "false" rather than left blank: a checkbox always shows a
                    // definite state, so an unchecked required box that blocked
                    // `ready` would be a form the user cannot see how to
                    // satisfy. An *optional* boolean is left unset and is
                    // omitted from the payload until the user touches it, which
                    // is what distinguishes "not answered" from an explicit
                    // false for a std::optional<bool> member.
                    Component.onCompleted: form.withoutAutoSubmit(function() {
                        // Every field's delegate instantiates this CheckBox and
                        // hides it unless the field is boolean, so this hook runs
                        // for fields of every type -- without this guard it seeded
                        // "false" into every *required* field, satisfying the
                        // required gate for text fields the user had not filled in.
                        if (!fieldColumn.modelData.isBoolean)
                            return
                        const retained = form.opt(form.fieldValues[fieldColumn.modelData.name], "")
                        if (retained === "" && fieldColumn.modelData.required) {
                            form.setFieldValue(fieldColumn.modelData.name, "false")
                            boolEntry.checked = false
                            return
                        }
                        boolEntry.checked = retained === "true"
                    })
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: fieldColumn.modelData.name
                    Accessible.description: (fieldColumn.modelData.required ? "Required. " : "")
                                             + fieldColumn.modelData.description
                }

                // x-widget: "textarea" (a Multiline field) — same wire string
                // as an ordinary TextField, just edited over multiple lines.
                TextArea {
                    id: notesArea
                    objectName: "multiline_" + fieldColumn.modelData.name
                    visible: overrideLoader.sourceComponent === null && fieldColumn.modelData.isMultiline
                    Layout.fillWidth: true
                    Layout.preferredHeight: 72
                    readOnly: fieldColumn.modelData.readOnly
                    wrapMode: TextArea.Wrap
                    onTextChanged: form.setFieldValue(fieldColumn.modelData.name, text)
                    // Same re-seed as the TextField above — see its comment.
                    Component.onCompleted: form.withoutAutoSubmit(function() {
                        notesArea.text = form.opt(form.fieldValues[fieldColumn.modelData.name], "")
                    })
                    Accessible.role: Accessible.EditableText
                    Accessible.name: fieldColumn.modelData.name
                    Accessible.description: (fieldColumn.modelData.required ? "Required. " : "")
                                             + fieldColumn.modelData.description
                }

                // x-widget: "slider" (a Ranged field) — track bounds and step
                // come from x-min/x-max/x-step, never from glaze's own
                // minimum/maximum (those stay validation-only).
                Slider {
                    id: levelSlider
                    objectName: "slider_" + fieldColumn.modelData.name
                    visible: overrideLoader.sourceComponent === null && fieldColumn.modelData.isSlider
                    enabled: !fieldColumn.modelData.readOnly
                    Layout.fillWidth: true
                    from: fieldColumn.modelData.sliderMin
                    to: fieldColumn.modelData.sliderMax
                    stepSize: fieldColumn.modelData.sliderStep
                    onMoved: form.setFieldValue(fieldColumn.modelData.name, String(Math.round(value)))
                    // Same re-seed as the TextField above — see its comment.
                    // `onMoved` (not onValueChanged) fires only for user drags,
                    // so restoring the position here cannot loop back.
                    Component.onCompleted: {
                        const retained = form.opt(form.fieldValues[fieldColumn.modelData.name], "")
                        if (retained !== "")
                            levelSlider.value = Number(retained)
                    }
                    Accessible.role: Accessible.Slider
                    Accessible.name: fieldColumn.modelData.name
                    Accessible.description: (fieldColumn.modelData.required ? "Required. " : "")
                                             + fieldColumn.modelData.description
                }

                Label {
                    visible: overrideLoader.sourceComponent === null && fieldColumn.modelData.isSlider
                    text: fieldColumn.modelData.isSlider ? String(Math.round(levelSlider.value)) : ""
                    opacity: 0.6
                }

                // Unit selector when the unit system declares convertible
                // alternatives: switching recalculates the entry exactly.
                ComboBox {
                    visible: overrideLoader.sourceComponent === null && fieldColumn.modelData.isQuantity
                             && fieldColumn.modelData.unitOptions.length > 1
                    enabled: !fieldColumn.modelData.readOnly
                    implicitWidth: 92
                    textRole: "display"
                    model: fieldColumn.modelData.unitOptions
                    Accessible.role: Accessible.ComboBox
                    Accessible.name: fieldColumn.modelData.name + " unit"
                    onActivated: {
                        const name = fieldColumn.modelData.name
                        const fromUnit = fieldColumn.modelData.unitOptions[form.opt(form.fieldUnits[name], 0)]
                        const toUnit = fieldColumn.modelData.unitOptions[currentIndex]
                        form.fieldUnits[name] = currentIndex
                        if (entry.text.trim() !== "") {
                            const canonicalText = form.normalizeLocaleNumber(entry.text.trim(), {
                                    decimalSeparator: form.qtLocale.decimalPoint,
                                    groupSeparator: form.qtLocale.groupSeparator,
                                    negativeSign: form.qtLocale.negativeSign,
                                    positiveSign: form.qtLocale.positiveSign,
                                    zeroDigit: form.qtLocale.zeroDigit
                                })
                            const converted = canonicalText !== null
                                    ? form.convertText(canonicalText, fromUnit, toUnit) : ""
                            entry.text = converted !== ""
                                    ? form.formatCanonicalNumber(converted, {
                                          decimalSeparator: form.qtLocale.decimalPoint,
                                          groupSeparator: form.qtLocale.groupSeparator,
                                          negativeSign: form.qtLocale.negativeSign,
                                          zeroDigit: form.qtLocale.zeroDigit
                                      })
                                    : ""
                        } else {
                            form.revalidate()
                        }
                    }
                }

                Label {
                    visible: overrideLoader.sourceComponent === null && fieldColumn.modelData.unit !== ""
                             && !(fieldColumn.modelData.isQuantity
                                  && fieldColumn.modelData.unitOptions.length > 1)
                    text: fieldColumn.modelData.unit
                    opacity: 0.6
                }
            }
        }
    }

    Component {
        id: sectionRun

        // A single section: "flat" (the implicit whole-form bucket used
        // when the schema declares no x-layout — one column, no chrome,
        // pixel-identical to the pre-grouping renderer), "section" (a
        // titled fieldset), or "accordion" (a collapsible panel). "tab"
        // groups never reach here — the renderer merges consecutive "tab"
        // sections into one tabsetRun instead.
        ColumnLayout {
            id: box
            property var runData
            Layout.fillWidth: true
            property bool collapsed: false
            // The implicit "flat" bucket has no chrome to replace.
            property var sectionChrome: (!box.runData || box.runData.section.kind === "flat")
                                        ? null : form.chrome(box.runData.section.kind)

            RowLayout {
                visible: box.sectionChrome === null && box.runData.section.title !== ""
                Layout.fillWidth: true

                Button {
                    visible: box.runData.section.kind === "accordion"
                    text: box.collapsed ? "▸" : "▾"
                    flat: true
                    onClicked: box.collapsed = !box.collapsed
                }
                Label {
                    text: box.runData.section.title
                    font.bold: true
                    font.pixelSize: 16
                }
            }

            GridLayout {
                Layout.fillWidth: true
                visible: box.sectionChrome === null && !box.collapsed
                columns: box.runData.section.kind === "flat" ? 1 : 2

                // Empty under a chrome: the fields are created inside it
                // instead, and one delegate per field is what keeps every
                // objectName unique.
                Repeater {
                    model: box.sectionChrome === null ? box.runData.section.fields : []
                    delegate: fieldDelegate
                }
            }

            // A host's section chrome (for "section", and for "accordion"
            // unless one is registered for it): the card, the heading and any
            // collapsing are its own; this form creates the field grid inside
            // the chrome's `contentItem`.
            Loader {
                active: box.sectionChrome !== null
                visible: active
                Layout.fillWidth: true
                sourceComponent: box.sectionChrome
                onLoaded: {
                    form.bindChrome(item, {
                        title: box.runData.section.title,
                        kind: box.runData.section.kind,
                        section: box.runData.section
                    })
                    form.createFieldGrid(item, 2, function () { return box.runData.section.fields })
                }
            }
        }
    }

    // The field grid a section or tab-set chrome hosts, created inside the
    // chrome's `contentItem` (expected to be a Layout, e.g. a ColumnLayout).
    Component {
        id: chromeFieldGrid

        GridLayout {
            id: chromeGrid
            property var gridFields: []
            Layout.fillWidth: true

            Repeater {
                model: chromeGrid.gridFields
                delegate: fieldDelegate
            }
        }
    }

    // `fieldsOf` is a function so the grid follows a binding (a tab-set
    // chrome's currentIndex) rather than a snapshot.
    function createFieldGrid(chromeItem, columns, fieldsOf) {
        if (!chromeItem || !chromeItem.contentItem) {
            console.warn("DynamicForm: a section/tabset chrome must declare `contentItem`; its fields are not shown")
            return null
        }
        return chromeFieldGrid.createObject(chromeItem.contentItem, {
            columns: columns,
            gridFields: Qt.binding(fieldsOf)
        })
    }

    Component {
        id: tabsetRun

        // Consecutive "tab" groups share one tab bar; the grid below shows
        // only the fields of whichever tab is currently selected.
        ColumnLayout {
            id: tabsBox
            objectName: "tabset"
            property var runData
            Layout.fillWidth: true
            property int currentTab: 0
            // Null until the run is assigned, so the chrome never loads without it.
            property var tabsetChrome: tabsBox.runData ? form.chrome("tabset") : null

            TabBar {
                id: bar
                objectName: "tabBar"
                visible: tabsBox.tabsetChrome === null
                Layout.fillWidth: true
                currentIndex: tabsBox.currentTab
                onCurrentIndexChanged: tabsBox.currentTab = currentIndex

                Repeater {
                    model: tabsBox.runData.sections
                    delegate: TabButton {
                        required property var modelData
                        text: modelData.title
                    }
                }
            }

            GridLayout {
                Layout.fillWidth: true
                visible: tabsBox.tabsetChrome === null
                columns: 2

                Repeater {
                    model: tabsBox.tabsetChrome === null ? tabsBox.runData.sections[tabsBox.currentTab].fields : []
                    delegate: fieldDelegate
                }
            }

            // A host's tab-set chrome: it draws the tabs from `tabs` and owns
            // `currentIndex`; this form shows the selected tab's fields inside
            // its `contentItem`, rebuilt on every switch exactly as the
            // built-in tab bar's grid is.
            Loader {
                active: tabsBox.tabsetChrome !== null
                visible: active
                Layout.fillWidth: true
                sourceComponent: tabsBox.tabsetChrome
                onLoaded: {
                    const chromeItem = item
                    form.bindChrome(chromeItem, {
                        tabs: tabsBox.runData.sections.map(function (section) { return { title: section.title } })
                    })
                    form.createFieldGrid(chromeItem, 2, function () {
                        const index = ("currentIndex" in chromeItem) ? chromeItem.currentIndex : 0
                        const section = tabsBox.runData.sections[index]
                        return section ? section.fields : []
                    })
                }
            }
        }
    }

    // --- layout ---------------------------------------------------------------

    ColumnLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 4

        Label {
            visible: form.chrome("header") === null
            text: form.actionType
            font.bold: true
            font.pixelSize: 16
        }

        Loader {
            active: form.chrome("header") !== null
            visible: active
            Layout.fillWidth: true
            sourceComponent: form.chrome("header")
            onLoaded: form.bindChrome(item, { text: Qt.binding(function () { return form.actionType }) })
        }

        Repeater {
            model: form.renderRuns

            delegate: Loader {
                id: runLoader
                required property var modelData
                Layout.fillWidth: true
                sourceComponent: runLoader.modelData.type === "tabset" ? tabsetRun : sectionRun
                onLoaded: item.runData = runLoader.modelData
            }
        }

        Label {
            Layout.topMargin: 8
            visible: form.chrome("status") === null
            text: form.statusText
            opacity: 0.6
            font.italic: true
            // A blocked submit is announced, not merely tinted (docs/spec/
            // forms/forms.md's accessibility slice): the same text shown
            // visually is exposed as this label's accessible description,
            // reactively, since `text` is itself reactive.
            Accessible.role: Accessible.StaticText
            Accessible.name: text
            Accessible.description: text
        }

        Loader {
            active: form.chrome("status") !== null
            visible: active
            Layout.fillWidth: true
            sourceComponent: form.chrome("status")
            onLoaded: form.bindChrome(item, {
                text: Qt.binding(function () { return form.statusText }),
                ready: Qt.binding(function () { return form.ready }),
                reason: Qt.binding(function () { return form.unrepresentableReason }),
                explicitSubmit: Qt.binding(function () { return form.explicitSubmitMode })
            })
        }

        // "x-submitMode": "explicit" (docs/spec/forms/forms.md, "Explicit
        // submit mode"): the sole trigger for a side-effectful action's
        // submission. Enabled only while `ready`, matching the required (*)
        // asterisk / submit-gate convention documented in this file's header
        // comment -- a disabled button communicates the same gate the
        // auto-submit label does for the default mode. Loaded only when the
        // schema opts in, so a default (auto-submit) schema has no such
        // control anywhere in the item tree, not merely a hidden one.
        Loader {
            active: form.explicitSubmitMode
            Layout.topMargin: 4
            sourceComponent: form.chrome("submitButton") !== null ? form.chrome("submitButton") : builtinSubmitButton
            // A host's submit chrome gets `ready` and `submit()`; the form's
            // own guard in submit() still applies to it.
            onLoaded: {
                if (sourceComponent !== builtinSubmitButton)
                    form.bindChrome(item, {
                        ready: Qt.binding(function () { return form.ready }),
                        submit: function () { form.submit() }
                    })
            }
        }

        Component {
            id: builtinSubmitButton
            Button {
                id: submitButton
                objectName: "submitButton"
                enabled: form.ready
                text: "Submit"
                onClicked: form.submit()
            }
        }

        Label {
            visible: form.chrome("preview") === null && form.previewLine !== ""
            Layout.fillWidth: true
            text: form.previewLine
            wrapMode: Text.WrapAnywhere
            font.family: "monospace"
            font.pixelSize: 11
            opacity: 0.55
        }

        Label {
            visible: form.chrome("result") === null && form.resultText !== ""
            Layout.fillWidth: true
            text: (form.resultOk ? "ok:  " : "err: ") + form.resultText
            wrapMode: Text.WrapAnywhere
            font.family: "monospace"
            font.pixelSize: 12
            color: form.resultOk ? palette.text : "#d33"
        }

        // Preview and result chrome are loaded whatever their text: a host
        // that wants neither registers an empty Item, and one that wants them
        // decides for itself when to show an empty one.
        Loader {
            active: form.chrome("preview") !== null
            visible: active
            Layout.fillWidth: true
            sourceComponent: form.chrome("preview")
            onLoaded: form.bindChrome(item, { text: Qt.binding(function () { return form.previewLine }) })
        }

        Loader {
            active: form.chrome("result") !== null
            visible: active
            Layout.fillWidth: true
            sourceComponent: form.chrome("result")
            onLoaded: form.bindChrome(item, {
                text: Qt.binding(function () { return form.resultText }),
                ok: Qt.binding(function () { return form.resultOk })
            })
        }
    }

    Component.onCompleted: {
        revalidate()
        // Tests instantiate the form without a controller; fetch only when wired.
        if (!controller)
            return
        for (let i = 0; i < fields.length; ++i) {
            // A dependent Choice is never fetched here — every field starts
            // blank, so its parent can't be engaged yet. refreshDependents
            // fetches it once setFieldValue engages that parent.
            if (fields[i].isChoice && fields[i].dependsOn.length === 0)
                controller.fetchOptions(fields[i].optionsAction, "{}")
        }
    }
}
