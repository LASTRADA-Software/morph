// SPDX-License-Identifier: Apache-2.0
//
// A date-time input with a proper picker: manual ISO-8601 entry stays
// possible, and the calendar button opens a MonthGrid + time-spinbox popup.
// All times are UTC, matching the Timestamp wire contract.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    id: picker

    property alias text: manual.text
    signal edited(string text)

    function pad(value, width) {
        return String(value).padStart(width, "0")
    }

    TextField {
        id: manual
        Layout.fillWidth: true
        placeholderText: "YYYY-MM-DDTHH:MM:SS"
        onTextChanged: picker.edited(text)
    }

    Button {
        id: pickButton
        text: "📅"  // calendar emoji
        flat: true
        implicitWidth: implicitHeight
        onClicked: {
            popup.initFromText(manual.text)
            popup.open()
        }

        Popup {
            id: popup
            modal: true
            padding: 12
            // Anchored to the button (its visual parent), opening below and
            // right-aligned — Popup is not layout-managed, so plain x/y here
            // is the intended positioning mechanism.
            x: pickButton.width - width
            y: pickButton.height + 4

        property int selYear: 2026
        property int selMonth: 0  // 0-based, as MonthGrid expects
        property int selDay: 1

        // Seed the controls from the field text when it parses, else from the
        // current UTC time.
        function initFromText(text) {
            const parsed = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})(?::(\d{2}))?/.exec(text)
            const now = new Date()
            selYear = parsed ? parseInt(parsed[1]) : now.getUTCFullYear()
            selMonth = parsed ? parseInt(parsed[2]) - 1 : now.getUTCMonth()
            selDay = parsed ? parseInt(parsed[3]) : now.getUTCDate()
            hourBox.value = parsed ? parseInt(parsed[4]) : now.getUTCHours()
            minuteBox.value = parsed ? parseInt(parsed[5]) : now.getUTCMinutes()
            secondBox.value = parsed ? (parsed[6] === undefined ? 0 : parseInt(parsed[6])) : now.getUTCSeconds()
        }

        function setToNow() {
            const now = new Date()
            selYear = now.getUTCFullYear()
            selMonth = now.getUTCMonth()
            selDay = now.getUTCDate()
            hourBox.value = now.getUTCHours()
            minuteBox.value = now.getUTCMinutes()
            secondBox.value = now.getUTCSeconds()
        }

        function apply() {
            manual.text = picker.pad(selYear, 4) + "-" + picker.pad(selMonth + 1, 2) + "-"
                    + picker.pad(selDay, 2) + "T" + picker.pad(hourBox.value, 2) + ":"
                    + picker.pad(minuteBox.value, 2) + ":" + picker.pad(secondBox.value, 2)
            popup.close()
        }

        contentItem: ColumnLayout {
            spacing: 8

            RowLayout {
                Button {
                    text: "‹"  // single left angle
                    flat: true
                    implicitWidth: implicitHeight
                    onClicked: {
                        if (popup.selMonth === 0) {
                            popup.selMonth = 11
                            popup.selYear--
                        } else {
                            popup.selMonth--
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    font.bold: true
                    text: grid.locale.monthName(popup.selMonth) + " " + popup.selYear
                }
                Button {
                    text: "›"  // single right angle
                    flat: true
                    implicitWidth: implicitHeight
                    onClicked: {
                        if (popup.selMonth === 11) {
                            popup.selMonth = 0
                            popup.selYear++
                        } else {
                            popup.selMonth++
                        }
                    }
                }
            }

            DayOfWeekRow {
                locale: grid.locale
                Layout.fillWidth: true
                delegate: Label {
                    required property var model
                    text: model.shortName
                    horizontalAlignment: Text.AlignHCenter
                    opacity: 0.6
                    font.pixelSize: 11
                }
            }

            MonthGrid {
                id: grid
                Layout.fillWidth: true
                month: popup.selMonth
                year: popup.selYear
                delegate: Rectangle {
                    id: cell
                    required property var model
                    readonly property bool inMonth: model.month === grid.month
                    readonly property bool selected: inMonth && model.day === popup.selDay
                    implicitWidth: 30
                    implicitHeight: 26
                    radius: 4
                    color: selected ? grid.palette.highlight : "transparent"

                    Label {
                        anchors.centerIn: parent
                        text: cell.model.day
                        opacity: cell.inMonth ? 1 : 0.3
                        font.underline: cell.model.today
                        color: cell.selected ? grid.palette.highlightedText : grid.palette.text
                    }

                    TapHandler {
                        onTapped: {
                            if (cell.inMonth)
                                popup.selDay = cell.model.day
                        }
                    }
                }
            }

            RowLayout {
                Layout.alignment: Qt.AlignHCenter

                component TimeBox: SpinBox {
                    editable: true
                    wrap: true
                    implicitWidth: 86
                    textFromValue: function (value) { return picker.pad(value, 2) }
                    valueFromText: function (text) { return parseInt(text) || 0 }
                }

                TimeBox { id: hourBox; from: 0; to: 23 }
                Label { text: ":" }
                TimeBox { id: minuteBox; from: 0; to: 59 }
                Label { text: ":" }
                TimeBox { id: secondBox; from: 0; to: 59 }
            }

            RowLayout {
                Button {
                    text: "Now"
                    flat: true
                    onClicked: popup.setToNow()
                }
                Button {
                    text: "Clear"
                    flat: true
                    onClicked: {
                        manual.text = ""
                        popup.close()
                    }
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: "OK"
                    onClicked: popup.apply()
                }
            }
        }
        }
    }
}
