// SPDX-License-Identifier: Apache-2.0
//
// A form rendered purely from one action's JSON Schema:
//   x-order          -> field order          required  -> asterisk + submit gate
//   ExtUnits         -> unit suffix          minimum/maximum -> input hints
//   x-decimalPlaces  -> quantity fields (decimal input -> exact {num,den,dp})
//   x-optionsAction  -> combo box (options fetched by executing that action)
//   x-unitAlternatives -> unit selector with exact recalculation on switch
//   format date-time -> date-time input with a calendar/time picker
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

    property var fieldValues: ({})
    property var fieldOptions: ({})
    property var fieldUnits: ({})
    property int optionsRevision: 0
    property bool ready: false
    property string previewLine: ""
    property string resultText: ""
    property bool resultOk: true

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
                return {
                    name: name,
                    title: opt(raw["title"], opt(p.title, name)),
                    description: opt(p.description, ""),
                    placeholder: opt(raw["x-placeholder"], opt(p["x-placeholder"], "")),
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
                    colspan: opt(opt(raw["x-colspan"], p["x-colspan"]), 1)
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
                if (!/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}(:\d{2})?$/.test(text)) { ok = false; continue }
                // The demo treats the entered time as UTC.
                parts.push(JSON.stringify(f.name) + ":"
                           + JSON.stringify((text.length === 16 ? text + ":00" : text) + "Z"))
            } else if (f.isQuantity) {
                if (!/^-?\d+(\.\d+)?$/.test(text)) { ok = false; continue }
                const unit = f.unitOptions[opt(fieldUnits[f.name], 0)]
                // Reject more decimals than the current unit's precision
                // instead of silently rounding them away.
                const fracLen = (text.split(".")[1] || "").length
                if (fracLen > unit.decimals) { ok = false; continue }
                const value = parseFloat(text)
                // Bounds are declared against the canonical unit.
                if (opt(fieldUnits[f.name], 0) === 0) {
                    if (f.minimum !== undefined && value < f.minimum) { ok = false; continue }
                    if (f.maximum !== undefined && value > f.maximum) { ok = false; continue }
                }
                parts.push(JSON.stringify(f.name) + ":" + rationalJson(text, unit, f.canonDp))
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
                    text: fieldColumn.modelData.title
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
                Layout.fillWidth: true

                ComboBox {
                    visible: fieldColumn.modelData.isChoice
                    enabled: !fieldColumn.modelData.readOnly
                    Layout.fillWidth: true
                    textRole: "label"
                    currentIndex: -1
                    displayText: currentIndex < 0 ? "— select —" : currentText
                    model: { form.optionsRevision; return form.fieldOptions[fieldColumn.modelData.name] || [] }
                    onActivated: form.setFieldValue(fieldColumn.modelData.name, model[currentIndex].valueJson)
                }

                DateTimePicker {
                    visible: fieldColumn.modelData.isDateTime
                    enabled: !fieldColumn.modelData.readOnly
                    Layout.fillWidth: true
                    onEdited: text => form.setFieldValue(fieldColumn.modelData.name, text)
                }

                TextField {
                    id: entry
                    objectName: "field_" + fieldColumn.modelData.name
                    visible: !fieldColumn.modelData.isChoice && !fieldColumn.modelData.isDateTime
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
                }

                // Unit selector when the unit system declares convertible
                // alternatives: switching recalculates the entry exactly.
                ComboBox {
                    visible: fieldColumn.modelData.isQuantity
                             && fieldColumn.modelData.unitOptions.length > 1
                    enabled: !fieldColumn.modelData.readOnly
                    implicitWidth: 92
                    textRole: "display"
                    model: fieldColumn.modelData.unitOptions
                    onActivated: {
                        const name = fieldColumn.modelData.name
                        const fromUnit = fieldColumn.modelData.unitOptions[form.opt(form.fieldUnits[name], 0)]
                        const toUnit = fieldColumn.modelData.unitOptions[currentIndex]
                        form.fieldUnits[name] = currentIndex
                        if (entry.text.trim() !== "")
                            entry.text = form.convertText(entry.text.trim(), fromUnit, toUnit)
                        else
                            form.revalidate()
                    }
                }

                Label {
                    visible: fieldColumn.modelData.unit !== ""
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
