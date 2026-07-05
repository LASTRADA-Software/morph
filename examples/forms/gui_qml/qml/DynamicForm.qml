// SPDX-License-Identifier: Apache-2.0
//
// A form rendered purely from one action's JSON Schema:
//   x-order          -> field order          required  -> asterisk + submit gate
//   ExtUnits         -> unit suffix          minimum/maximum -> input hints
//   x-decimalPlaces  -> quantity fields (decimal input -> exact {num,den,dp})
//
// Quantity payloads are built with Math.round(value * 10^dp) — exact for demo
// magnitudes (< 2^53 minor units); a production client would use BigInt.

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
                const extUnits = opt(p.ExtUnits, {})
                return {
                    name: name,
                    description: opt(p.description, ""),
                    unit: opt(extUnits.unitUnicode, opt(extUnits.unitAscii, "")),
                    isQuantity: dp !== undefined,
                    decimals: opt(dp, 0),
                    isInteger: types.indexOf("integer") !== -1,
                    required: required.indexOf(name) !== -1,
                    minimum: p.minimum,
                    maximum: p.maximum
                }
            })
    }

    // --- draft state --------------------------------------------------------

    function toRational(text, dp) {
        const scale = Math.pow(10, dp)
        return { num: Math.round(parseFloat(text) * scale), den: scale, dp: dp }
    }

    function revalidate() {
        const body = {}
        let ok = true
        for (let i = 0; i < fields.length; ++i) {
            const f = fields[i]
            const text = (opt(fieldValues[f.name], "")).trim()
            if (text === "") {
                if (f.required)
                    ok = false
                continue
            }
            if (f.isQuantity) {
                if (!/^-?\d+(\.\d+)?$/.test(text)) { ok = false; continue }
                const value = parseFloat(text)
                if (f.minimum !== undefined && value < f.minimum) { ok = false; continue }
                if (f.maximum !== undefined && value > f.maximum) { ok = false; continue }
                body[f.name] = toRational(text, f.decimals)
            } else if (f.isInteger) {
                if (!/^-?\d+$/.test(text)) { ok = false; continue }
                const value = parseInt(text)
                if (f.minimum !== undefined && value < f.minimum) { ok = false; continue }
                if (f.maximum !== undefined && value > f.maximum) { ok = false; continue }
                body[f.name] = value
            } else {
                body[f.name] = text
            }
        }
        ready = ok
        previewLine = ok ? JSON.stringify(body) : ""
    }

    function setFieldValue(name, text) {
        fieldValues[name] = text
        revalidate()
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
            model: form.fields

            ColumnLayout {
                id: fieldColumn
                required property var modelData
                Layout.fillWidth: true
                spacing: 2

                RowLayout {
                    Label {
                        text: fieldColumn.modelData.name
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

                    TextField {
                        Layout.fillWidth: true
                        placeholderText: fieldColumn.modelData.isQuantity
                                         ? "0." + "0".repeat(Math.max(1, fieldColumn.modelData.decimals))
                                         : (fieldColumn.modelData.isInteger ? "0" : "")
                        inputMethodHints: (fieldColumn.modelData.isQuantity || fieldColumn.modelData.isInteger)
                                          ? Qt.ImhFormattedNumbersOnly : Qt.ImhNone
                        onTextChanged: form.setFieldValue(fieldColumn.modelData.name, text)
                    }

                    Label {
                        visible: fieldColumn.modelData.unit !== ""
                        text: fieldColumn.modelData.unit
                        opacity: 0.6
                    }
                }
            }
        }

        Button {
            Layout.topMargin: 8
            text: "execute"
            enabled: form.ready
            onClicked: form.controller.submit(form.actionType, form.previewLine)
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

    Component.onCompleted: revalidate()
}
