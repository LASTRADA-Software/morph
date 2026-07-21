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
//
// Quantity payloads are assembled as JSON text from the typed digit string,
// so they are exact at any magnitude (same contract as the HTML renderer).

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
    property string previewLine: ""
    property string resultText: ""
    property bool resultOk: true

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
    function resolveProp(prop) {
        if (prop && prop["$ref"] !== undefined) {
            const defName = prop["$ref"].split("/").pop()
            const def = opt((schema["$defs"] || {})[defName], {})
            return Object.assign({}, def, prop)
        }
        return opt(prop, {})
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
                    isDateTime: p.format === "date-time",
                    isQuantity: dp !== undefined,
                    decimals: opt(dp, 0),
                    isInteger: types.indexOf("integer") !== -1,
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

    function revalidate() {
        // Assembled as JSON text (not JSON.stringify) so rational digits and
        // int64-sized integers stay exact.
        const parts = []
        let ok = true
        for (let i = 0; i < fields.length; ++i) {
            const f = fields[i]
            const text = (opt(fieldValues[f.name], "")).trim()
            if (text === "") {
                if (f.required)
                    ok = false
                continue
            }
            if (f.isChoice) {
                parts.push(JSON.stringify(f.name) + ":" + text)  // stored as a JSON literal
            } else if (f.isDateTime) {
                const utcIso = zonedToUtcIso(text, displayOffsetMinutes)
                if (utcIso === null) { ok = false; continue }
                parts.push(JSON.stringify(f.name) + ":" + JSON.stringify(utcIso))
            } else if (f.isQuantity) {
                const canonicalText = normalizeLocaleNumber(text, qtLocale.decimalPoint, qtLocale.groupSeparator)
                if (canonicalText === null || !/^-?\d+(\.\d+)?$/.test(canonicalText)) { ok = false; continue }
                const unit = f.unitOptions[opt(fieldUnits[f.name], 0)]
                // Reject more decimals than the current unit's precision
                // instead of silently rounding them away.
                const fracLen = (canonicalText.split(".")[1] || "").length
                if (fracLen > unit.decimals) { ok = false; continue }
                const value = parseFloat(canonicalText)
                // Bounds are declared against the canonical unit.
                if (opt(fieldUnits[f.name], 0) === 0) {
                    if (f.minimum !== undefined && value < f.minimum) { ok = false; continue }
                    if (f.maximum !== undefined && value > f.maximum) { ok = false; continue }
                }
                parts.push(JSON.stringify(f.name) + ":" + rationalJson(canonicalText, unit, f.canonDp))
            } else if (f.isInteger) {
                if (!/^-?\d+$/.test(text)) { ok = false; continue }
                const value = parseInt(text)
                if (f.minimum !== undefined && value < f.minimum) { ok = false; continue }
                if (f.maximum !== undefined && value > f.maximum) { ok = false; continue }
                // Normalise "007" -> "7": JSON forbids leading zeros in numbers.
                parts.push(JSON.stringify(f.name) + ":" + text.replace(/^(-?)0+(?=\d)/, "$1"))
            } else {
                parts.push(JSON.stringify(f.name) + ":" + JSON.stringify(text))
            }
        }
        ready = ok
        previewLine = ok ? "{" + parts.join(",") + "}" : ""
        if (ready && form.controller)
            form.controller.submitIfValid(form.actionType, form.previewLine)
    }

    function setFieldValue(name, text) {
        fieldValues[name] = text
        revalidate()
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
            }
            form.optionsRevision++
        }
    }

    Component {
        id: fieldDelegate

        ColumnLayout {
            id: fieldColumn
            required property var modelData
            Layout.fillWidth: true
            Layout.columnSpan: fieldColumn.modelData.colspan
            visible: !fieldColumn.modelData.hidden
            spacing: 2

            RowLayout {
                Label {
                    text: fieldColumn.modelData.label
                    font.bold: true
                }
                Label {
                    visible: fieldColumn.modelData.required
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
                    enabled: !fieldColumn.modelData.readOnly
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
                    objectName: "field_" + fieldColumn.modelData.name
                    visible: overrideLoader.sourceComponent === null
                             && !fieldColumn.modelData.isChoice && !fieldColumn.modelData.isDateTime
                             && !fieldColumn.modelData.isMultiline && !fieldColumn.modelData.isSlider
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
                    Accessible.role: Accessible.EditableText
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
            property var runData
            Layout.fillWidth: true
            property int currentTab: 0

            TabBar {
                id: bar
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
            text: form.ready ? "✓ executes automatically as you type" : "fill the required (*) fields"
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
            if (fields[i].isChoice)
                controller.fetchOptions(fields[i].optionsAction)
        }
    }
}
