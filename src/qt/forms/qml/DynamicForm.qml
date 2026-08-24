// SPDX-License-Identifier: Apache-2.0
//
// A form rendered purely from one action's JSON Schema:
//   x-order          -> field order          required  -> asterisk + submit gate
//   ExtUnits         -> unit suffix          minimum/maximum -> input hints
//   x-decimalPlaces  -> quantity fields (decimal input -> exact {num,den,dp})
//   x-optionsAction  -> combo box (options fetched by executing that action)
//   x-unitAlternatives -> unit selector with exact recalculation on switch
//   format date-time -> date-time input with a calendar/time picker
//   x-widget         -> control choice (textarea/slider/radio); unknown ids
//                       and a missing key both fall back to the type default
//   x-min/x-max/x-step -> slider track bounds + increment (Ranged fields)
//   type: "array"    -> comma-separated-with-validation control; encodes to
//                       a genuine JSON array literal, e.g. "a, b" -> ["a","b"]
//   x-submitMode: "explicit" -> suppresses auto-submit-on-validity; renders
//                       an explicit Submit button (enabled only while ready)
//                       instead -- see "Explicit submit mode" below
//
// Quantity payloads are assembled as JSON text from the typed digit string,
// so they are exact at any magnitude (same contract as the HTML renderer).
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

Frame {
    id: form

    property string actionType
    property var schema
    property var controller

    // Client-side theming/override registry (docs/spec/forms/forms.md,
    // "Theming / component-override registry"): null (the default) means no
    // registry installed -- every field renders its built-in control exactly
    // as it does today. See SlotRegistry.qml.
    property var slotRegistry: null

    property var fieldValues: ({})
    property var fieldOptions: ({})
    property var fieldUnits: ({})
    property int optionsRevision: 0
    property bool ready: false

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
    property var rules: schema["x-rules"] || []
    property int rulesRevision: 0

    // "x-submitMode": "explicit" (docs/spec/forms/forms.md, "Explicit submit
    // mode"): opts a side-effectful (non-query) action out of the default
    // auto-fire-on-validity behavior. When set, revalidate() still recomputes
    // `ready`/`previewLine` live but never calls submitIfValid() on its own;
    // an explicit submit Button (added to the layout below), enabled only
    // while `ready`, is the sole way to fire. Absent (the default) or any
    // other value keeps today's auto-submit-on-validity behavior unchanged.
    property bool explicitSubmitMode: schema["x-submitMode"] === "explicit"

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

    // Follow a $ref into $defs; attributes on the field win over the def's.
    function resolveRef(prop) {
        if (prop && prop["$ref"] !== undefined) {
            const defName = prop["$ref"].split("/").pop()
            const def = opt((schema["$defs"] || {})[defName], {})
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
        // parse_number_failure (morph#189). Resolve through the non-null branch
        // so the field is typed by T. `oneOf` is handled the same way; glaze
        // does not emit it today, but a hand-written or evolved schema may.
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
                if (p.type === undefined)
                    merged.type = branch.type
                return merged
            }
        }
        return p
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
        const hit2 = catalog.lookup(displayLocale, derivedKey)
        if (hit2 !== undefined && hit2 !== null)
            return hit2
        return literal
    }

    // Flat field descriptors, in declaration (x-order) order.
    property var fields: {
        const props = schema.properties || {}
        const required = schema.required || []
        return Object.keys(props)
            .sort(function (a, b) { return opt(props[a]["x-order"], 0) - opt(props[b]["x-order"], 0) })
            .map(function (name) {
                const raw = props[name]
                const p = resolveProp(raw)
                const types = Array.isArray(p.type) ? p.type : (p.type === undefined ? [] : [p.type])
                const dp = opt(raw["x-decimalPlaces"], p["x-decimalPlaces"])
                const optionsAction = opt(raw["x-optionsAction"], p["x-optionsAction"])
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
                return {
                    name: name,
                    title: literalTitle,
                    label: resolveText(i18nExplicitFieldKey(i18nOverride, "label"),
                                       i18nFieldKey(name, "label"), literalTitle),
                    description: resolveText(i18nExplicitFieldKey(i18nOverride, "help"),
                                              i18nFieldKey(name, "help"), literalHelp),
                    placeholder: resolveText(i18nExplicitFieldKey(i18nOverride, "placeholder"),
                                              i18nFieldKey(name, "placeholder"), literalPlaceholder),
                    readOnly: opt(raw["x-readonly"], opt(p["x-readonly"], false)),
                    hidden: opt(raw["x-hidden"], opt(p["x-hidden"], false)),
                    unit: unitText,
                    unitOptions: unitOptions,
                    canonDp: opt(dp, 0),
                    isChoice: optionsAction !== undefined,
                    optionsAction: opt(optionsAction, ""),
                    valueField: opt(opt(raw["x-optionValue"], p["x-optionValue"]), "id"),
                    labelField: opt(opt(raw["x-optionLabel"], p["x-optionLabel"]), "name"),
                    // Wire names of sibling fields whose current values
                    // parameterise this Choice's options action (cascading
                    // picklist); empty for an independent Choice.
                    dependsOn: opt(raw["x-optionsDependsOn"], opt(p["x-optionsDependsOn"], [])),
                    isDateTime: p.format === "date-time",
                    isQuantity: dp !== undefined,
                    decimals: opt(dp, 0),
                    isInteger: types.indexOf("integer") !== -1,
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
                    required: required.indexOf(name) !== -1,
                    minimum: p.minimum,
                    maximum: p.maximum,
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
                    unitAscii: opt(extUnits.unitAscii, ""),
                    jsonType: types.length > 0 ? types[0] : ""
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
        const groupDefs = (schema["x-layout"] || {}).groups || []
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
        if (meta && (meta.isQuantity || meta.isInteger))
            return parseFloat(text)
        return text
    }

    // Evaluates one condition node (`engaged` / `notEngaged` / `equals` / a
    // comparison kind reused as a boolean / the compound `and`/`or`/`not`
    // kinds, which recurse into `conditions`/`condition` to any depth). An
    // unrecognised `kind` fails closed (`false`) -- the renderer defers
    // enforcement to the server rather than passing an unknown validation
    // condition.
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
            for (let i = 0; i < nested.length; ++i) {
                if (!testCondition(nested[i]))
                    return false
            }
            return true
        }
        if (kind === "or") {
            const nested = cond.conditions || []
            for (let i = 0; i < nested.length; ++i) {
                if (testCondition(nested[i]))
                    return true
            }
            return false
        }
        if (kind === "not")
            return !testCondition(cond.condition)
        return false
    }

    // Evaluates one top-level x-rules entry. Presentation kinds
    // (visibleWhen/readonlyWhen) always return true -- they never gate
    // submission, only presentation (see fieldVisible/fieldReadonly below).
    // `and`/`or`/`not` are valid directly as a top-level rule (not only
    // nested inside a `when` clause) -- a single rule carrying a compound
    // condition tree -- so they delegate to testCondition exactly like the
    // comparison kinds already do. An unrecognised rule kind fails closed:
    // the renderer defers enforcement to the server rather than passing the
    // rule.
    function testRule(rule) {
        const kind = rule.kind
        const names = rule.fields || []
        if (kind === "requiredWhen") {
            if (!testCondition(rule.when))
                return true
            return fieldEngaged(names[0])
        }
        if (kind === "greater" || kind === "greaterOrEqual" || kind === "less" || kind === "lessOrEqual")
            return testCondition(rule)
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
            return testCondition(rule)
        return false
    }

    // Whether `name` is required right now because some requiredWhen rule's
    // condition currently holds for it (in addition to the schema's static
    // `required` array).
    function isDynamicallyRequired(name) {
        for (let i = 0; i < rules.length; ++i) {
            const rule = rules[i]
            if (rule.kind === "requiredWhen" && rule.fields[0] === name)
                return testCondition(rule.when)
        }
        return false
    }

    // Whether `name` should be shown. A field with no visibleWhen rule is
    // always visible (renderer fallback per forms.md).
    function fieldVisible(name) {
        for (let i = 0; i < rules.length; ++i) {
            const rule = rules[i]
            if (rule.kind === "visibleWhen" && rule.fields.indexOf(name) !== -1)
                return testCondition(rule.when)
        }
        return true
    }

    // Whether `name` should be read-only. A field with no readonlyWhen rule
    // is always editable (renderer fallback).
    function fieldReadonly(name) {
        for (let i = 0; i < rules.length; ++i) {
            const rule = rules[i]
            if (rule.kind === "readonlyWhen" && rule.fields.indexOf(name) !== -1)
                return testCondition(rule.when)
        }
        return false
    }

    // --- draft state --------------------------------------------------------

    // --- locale numeric formatting (mirrors morph::render::locale_format.hpp)
    // The payload's exact digit routines below stay entirely locale-free —
    // this is the one control-edge conversion step, applied once per entry.

    function normalizeLocaleNumber(text, decimalSeparator, groupSeparator) {
        let stripped = ""
        for (let i = 0; i < text.length; ++i) {
            if (groupSeparator !== "" && text[i] === groupSeparator)
                continue
            stripped += text[i]
        }
        let canonical = ""
        let sawDecimal = false
        for (let i = 0; i < stripped.length; ++i) {
            const ch = stripped[i]
            if (ch === decimalSeparator) {
                if (sawDecimal)
                    return null
                sawDecimal = true
                canonical += "."
            } else if (ch === "-") {
                if (i !== 0)
                    return null
                canonical += ch
            } else if (ch >= "0" && ch <= "9") {
                canonical += ch
            } else {
                return null
            }
        }
        if (canonical === "" || canonical === "-")
            return null
        return canonical
    }

    function formatCanonicalNumber(text, decimalSeparator, groupSeparator) {
        const neg = text.startsWith("-")
        const magnitude = neg ? text.slice(1) : text
        const dot = magnitude.indexOf(".")
        const wholePart = dot === -1 ? magnitude : magnitude.slice(0, dot)
        const fracPart = dot === -1 ? "" : magnitude.slice(dot + 1)
        let grouped = ""
        for (let i = 0; i < wholePart.length; ++i) {
            if (groupSeparator !== "" && i !== 0 && (wholePart.length - i) % 3 === 0)
                grouped += groupSeparator
            grouped += wholePart[i]
        }
        return (neg ? "-" : "") + grouped + (fracPart !== "" ? decimalSeparator + fracPart : "")
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
        const text = (opt(fieldValues[f.name], "")).trim()
        if (text === "")
            return null
        if (f.isArray) {
            return arrayJsonLiteral(text)
        }
        if (f.isChoice) {
            return text  // already a JSON literal (see the ComboBox's onActivated)
        }
        if (f.isDateTime) {
            const utcIso = zonedToUtcIso(text, displayOffsetMinutes)
            return utcIso === null ? null : JSON.stringify(utcIso)
        }
        if (f.isQuantity) {
            const canonicalText = normalizeLocaleNumber(text, qtLocale.decimalPoint, qtLocale.groupSeparator)
            if (canonicalText === null || !/^-?\d+(\.\d+)?$/.test(canonicalText))
                return null
            const unit = f.unitOptions[opt(fieldUnits[f.name], 0)]
            // Reject more decimals than the current unit's precision instead
            // of silently rounding them away.
            const fracLen = (canonicalText.split(".")[1] || "").length
            if (fracLen > unit.decimals)
                return null
            const value = parseFloat(canonicalText)
            // Bounds are declared against the canonical unit.
            if (opt(fieldUnits[f.name], 0) === 0) {
                if (f.minimum !== undefined && value < f.minimum)
                    return null
                if (f.maximum !== undefined && value > f.maximum)
                    return null
            }
            return rationalJson(canonicalText, unit, f.canonDp)
        }
        if (f.isInteger) {
            if (!/^-?\d+$/.test(text))
                return null
            const value = parseInt(text)
            if (f.minimum !== undefined && value < f.minimum)
                return null
            if (f.maximum !== undefined && value > f.maximum)
                return null
            // Normalise "007" -> "7": JSON forbids leading zeros in numbers.
            return text.replace(/^(-?)0+(?=\d)/, "$1")
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
        for (let i = 0; i < fields.length; ++i) {
            const f = fields[i]
            const text = (opt(fieldValues[f.name], "")).trim()
            const literal = fieldJsonLiteral(f)
            if (literal === null) {
                if (text !== "" || f.required || isDynamicallyRequired(f.name))
                    ok = false
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
                if (entry)
                    entry.text = ""
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

    Connections {
        target: form.controller
        function onReplyReceived(actionType, ok, payload) {
            if (actionType !== form.actionType)
                return
            form.resultOk = ok
            form.resultText = ok ? form.humanize(payload) : payload
        }
        function onOptionsReceived(optionsAction, ok, payload) {
            if (!ok)
                return
            let parsed
            try { parsed = JSON.parse(payload) } catch (ignored) { return }
            for (let i = 0; i < form.fields.length; ++i) {
                const f = form.fields[i]
                if (!f.isChoice || f.optionsAction !== optionsAction)
                    continue
                form.fieldOptions[f.name] = form.optionRows(parsed).map(function (row) {
                    return { label: String(row[f.labelField]), valueJson: JSON.stringify(row[f.valueField]) }
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

            RowLayout {
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

            Label {
                visible: fieldColumn.modelData.description !== ""
                text: fieldColumn.modelData.description
                opacity: 0.6
                font.pixelSize: 12
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
                                                 fieldColumn.modelData.jsonType)
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
                    onLoaded: {
                        item.field = fieldColumn.modelData
                        item.setValue = function (text) { form.setFieldValue(fieldColumn.modelData.name, text) }
                    }
                }

                ComboBox {
                    visible: overrideLoader.sourceComponent === null
                             && fieldColumn.modelData.isChoice && !fieldColumn.modelData.isRadioChoice
                    // A dependent Choice (x-optionsDependsOn) stays disabled
                    // until its parent(s) are engaged and a fetch has
                    // populated fieldOptions; an independent Choice is
                    // unaffected (dependsOn.length === 0 always short-circuits
                    // true here, exactly like before this feature existed).
                    enabled: !fieldColumn.modelData.readOnly
                             && (fieldColumn.modelData.dependsOn.length === 0
                                 || (form.fieldOptions[fieldColumn.modelData.name] || []).length > 0)
                    Layout.fillWidth: true
                    textRole: "label"
                    currentIndex: -1
                    displayText: currentIndex < 0 ? "— select —" : currentText
                    model: { form.optionsRevision; return form.fieldOptions[fieldColumn.modelData.name] || [] }
                    onActivated: form.setFieldValue(fieldColumn.modelData.name, model[currentIndex].valueJson)
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
                    objectName: (fieldColumn.modelData.isArray || fieldColumn.modelData.isBoolean)
                                ? "" : "field_" + fieldColumn.modelData.name
                    visible: overrideLoader.sourceComponent === null
                             && !fieldColumn.modelData.isChoice && !fieldColumn.modelData.isDateTime
                             && !fieldColumn.modelData.isMultiline && !fieldColumn.modelData.isSlider
                             && !fieldColumn.modelData.isArray && !fieldColumn.modelData.isBoolean
                    Layout.fillWidth: true
                    readOnly: fieldColumn.modelData.readOnly
                    placeholderText: fieldColumn.modelData.placeholder !== ""
                                     ? fieldColumn.modelData.placeholder
                                     : (fieldColumn.modelData.isQuantity
                                        ? "0." + "0".repeat(Math.max(1, fieldColumn.modelData.decimals))
                                        : (fieldColumn.modelData.isInteger ? "0" : ""))
                    inputMethodHints: (fieldColumn.modelData.isQuantity || fieldColumn.modelData.isInteger)
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
                // encoded the typed text as a JSON *string* ({"flag":"true"}),
                // and applied no validation at all, so "banana" was accepted
                // and sent; glaze rejected both with expected_true_or_false
                // (morph#189). A CheckBox can only produce the two valid
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
                            const canonicalText = form.normalizeLocaleNumber(
                                    entry.text.trim(), form.qtLocale.decimalPoint, form.qtLocale.groupSeparator)
                            const converted = canonicalText !== null
                                    ? form.convertText(canonicalText, fromUnit, toUnit) : ""
                            entry.text = converted !== ""
                                    ? form.formatCanonicalNumber(
                                          converted, form.qtLocale.decimalPoint, form.qtLocale.groupSeparator)
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

            RowLayout {
                visible: box.runData.section.title !== ""
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
                visible: !box.collapsed
                columns: box.runData.section.kind === "flat" ? 1 : 2

                Repeater {
                    model: box.runData.section.fields
                    delegate: fieldDelegate
                }
            }
        }
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

            TabBar {
                id: bar
                objectName: "tabBar"
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
                columns: 2

                Repeater {
                    model: tabsBox.runData.sections[tabsBox.currentTab].fields
                    delegate: fieldDelegate
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
            text: form.actionType
            font.bold: true
            font.pixelSize: 16
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
            text: {
                if (!form.ready)
                    return "fill the required (*) fields"
                return form.explicitSubmitMode ? "✓ ready -- press Submit" : "✓ executes automatically as you type"
            }
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
            sourceComponent: Button {
                id: submitButton
                objectName: "submitButton"
                enabled: form.ready
                text: "Submit"
                onClicked: form.submit()
            }
        }

        Label {
            visible: form.previewLine !== ""
            Layout.fillWidth: true
            text: form.previewLine
            wrapMode: Text.WrapAnywhere
            font.family: "monospace"
            font.pixelSize: 11
            opacity: 0.55
        }

        Label {
            visible: form.resultText !== ""
            Layout.fillWidth: true
            text: (form.resultOk ? "ok:  " : "err: ") + form.resultText
            wrapMode: Text.WrapAnywhere
            font.family: "monospace"
            font.pixelSize: 12
            color: form.resultOk ? palette.text : "#d33"
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
