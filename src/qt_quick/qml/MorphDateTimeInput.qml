// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A date, or a date and a time, typed in the display zone in the application's pattern, or as `yyyy-MM-dd` or
// `yyyy-MM-dd HH:mm` without one. C++ formats and parses it; an edit is reported once the user finishes it, and an
// empty field is a cleared value. A day chosen in the calendar beside the field is written by C++ too, in the same
// pattern, so the field's text is the one source of the value.
MorphField {
    id: root

    property bool withTime: false
    property bool edited: false
    // The display zone, minutes from UTC: the calendar's today is the zone's, not the system's.
    property int offsetMinutes: 0
    // The pattern the field shows and reads its value in; empty is ISO. Set once.
    property string pattern: ""
    // The field's day as C++ last read it, `{year, month (zero-based), day}`, or null when it names none.
    property var chosen: null

    control: field
    accessibleItem: field
    defaultRole: Accessible.EditableText

    // The month the calendar shows, zero-based as `MonthGrid` takes it.
    property int pickerMonth: 0
    property int pickerYear: 0

    // The field's date, which the calendar opens on and marks.
    readonly property var chosenDate: root.chosen

    // The zone's today, from UTC fields so the system time zone never moves it.
    function today(): var {
        const now = new Date(Date.now() + root.offsetMinutes * 60000)
        return {
            year: now.getUTCFullYear(),
            month: now.getUTCMonth(),
            day: now.getUTCDate()
        }
    }

    function openPicker(): void {
        const chosen = root.chosenDate
        const today = root.today()
        root.pickerYear = chosen !== null ? chosen.year : today.year
        root.pickerMonth = chosen !== null ? chosen.month : today.month
        picker.open()
    }

    function stepMonth(by: int): void {
        const month = root.pickerMonth + by
        if (month < 0) {
            root.pickerMonth = 11
            root.pickerYear -= 1
        } else if (month > 11) {
            root.pickerMonth = 0
            root.pickerYear += 1
        } else {
            root.pickerMonth = month
        }
    }

    // C++ writes the day, with the time of day already in the field, in the field's pattern, and reports it.
    function pickDay(year: int, month: int, day: int): void {
        // The pick reports its own change: a later blur must not report it again.
        root.edited = false
        root.fire("pick", [year, month, day])
        picker.close()
    }

    TextField {
        font: Theme.bodyFont
        id: field
        width: root.controlWidth
        // The widget's own chords come before the control's handling of the key.
        Keys.priority: Keys.BeforeItem
        Keys.onPressed: event => event.accepted = root.takeOwnChord(event)
        color: root.scheme.text
        placeholderTextColor: root.scheme.muted
        leftPadding: Theme.spaceOf(3)
        // The text stops before the calendar button.
        rightPadding: Theme.spaceOf(3) + (pickerButton.visible ? pickerButton.implicitWidth : 0)
        background: MorphInputBackground {
            tone: root.shownTone
            scheme: root.scheme
            focused: field.activeFocus
            readOnly: root.readOnly
        }
        readOnly: root.readOnly
        placeholderText: root.pattern !== "" ? root.pattern.toLowerCase().replace("xxx", "+hh:mm")
                                             : root.withTime ? "yyyy-mm-dd hh:mm" : "yyyy-mm-dd"
        // Declared, as the text input's field declares them.
        Accessible.role: root.accessibleRole
        Accessible.name: root.accessibleName
        Accessible.description: root.accessibleDescription
        onTextEdited: root.edited = true
        onEditingFinished: {
            if (!root.edited || root.readOnly)
                return
            root.edited = false
            root.fire("edit", text)
        }

        ToolButton {
            id: pickerButton
            objectName: "datePickerButton"
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            focusPolicy: Qt.NoFocus
            visible: !root.readOnly
            Accessible.name: "Pick a date"
            icon.source: Theme.iconSource("date.pick")
            icon.color: "transparent"
            text: icon.source.toString() !== "" ? "" : "▦"
            onClicked: root.openPicker()
        }
    }

    Popup {
        id: picker
        objectName: "datePickerPopup"
        // Placed in the field's coordinates: under it, its right edge on the field's.
        parent: field
        x: field.width - width
        y: field.height
        padding: Theme.spaceOf(2)
        // Focus, so Escape reaches the popup's close policy; the field reports no edit on losing focus alone.
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        // Focus returns to the field, wherever the calendar closes from.
        onClosed: field.forceActiveFocus()

        background: Rectangle {
            color: root.scheme.surface
            border.color: root.scheme.border
            radius: Theme.radiusCard
        }

        contentItem: Column {
            spacing: Theme.spaceOf(1)

            Row {
                width: grid.implicitWidth
                spacing: Theme.spaceOf(1)

                ToolButton {
                    id: previousButton
                    objectName: "previousMonth"
                    focusPolicy: Qt.NoFocus
                    text: "‹"
                    onClicked: root.stepMonth(-1)
                }

                Label {
                    textFormat: Text.PlainText
                    font: Theme.bodyFont
                    width: grid.implicitWidth - previousButton.implicitWidth - nextButton.implicitWidth -
                           2 * parent.spacing
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    color: root.scheme.text
                    text: Qt.locale().monthName(root.pickerMonth, Locale.LongFormat) + " " + root.pickerYear
                }

                ToolButton {
                    id: nextButton
                    objectName: "nextMonth"
                    focusPolicy: Qt.NoFocus
                    text: "›"
                    onClicked: root.stepMonth(1)
                }
            }

            DayOfWeekRow {
                width: grid.implicitWidth
                locale: grid.locale
                delegate: Text {
                    textFormat: Text.PlainText
                    font: Theme.bodyFont
                    color: root.scheme.muted
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: shortName
                    required property string shortName
                }
            }

            MonthGrid {
                id: grid
                objectName: "datePickerGrid"
                month: root.pickerMonth
                year: root.pickerYear
                delegate: Item {
                    id: dayCell
                    required property var model
                    objectName: "day_" + model.year + "-" + model.month + "-" + model.day
                    implicitWidth: Math.max(Theme.unitWidth * 2, Theme.unitHeight)
                    implicitHeight: Theme.unitHeight

                    readonly property bool today: {
                        const today = root.today()
                        return model.year === today.year && model.month === today.month && model.day === today.day
                    }
                    readonly property bool chosen: root.chosenDate !== null && model.year === root.chosenDate.year &&
                                                   model.month === root.chosenDate.month &&
                                                   model.day === root.chosenDate.day

                    Rectangle {
                        anchors.fill: parent
                        radius: Theme.radiusControl
                        color: dayCell.chosen ? root.scheme.accent : "transparent"
                        border.color: dayCell.today ? root.scheme.accent : "transparent"
                    }

                    Text {
                        textFormat: Text.PlainText
                        font: Theme.bodyFont
                        anchors.centerIn: parent
                        color: dayCell.chosen ? root.scheme.surface
                                              : dayCell.model.month === root.pickerMonth ? root.scheme.text
                                                                                         : root.scheme.muted
                        text: dayCell.model.day
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.pickDay(dayCell.model.year, dayCell.model.month, dayCell.model.day)
                    }
                }
            }
        }
    }
}
