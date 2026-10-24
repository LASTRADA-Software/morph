// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A date, or a date and a time, typed in the display zone as `yyyy-MM-dd` or `yyyy-MM-dd HH:mm`. C++ formats and
// parses it; an edit is reported once the user finishes it, and an empty field is a cleared value. A calendar beside
// the field produces the same text an edit would.
MorphField {
    id: root

    property bool withTime: false
    property bool edited: false
    // The display zone, minutes from UTC: the calendar's today is the zone's, not the system's.
    property int offsetMinutes: 0

    control: field
    accessibleItem: field
    defaultRole: Accessible.EditableText

    // The month the calendar shows, zero-based as `MonthGrid` takes it.
    property int pickerMonth: 0
    property int pickerYear: 0

    // The field's date as its `yyyy-MM-dd` prefix names it, or null when it names none.
    readonly property var chosenDate: {
        const match = /^(\d{4})-(\d{2})-(\d{2})/.exec(field.text)
        return match ? {
            year: Number(match[1]),
            month: Number(match[2]) - 1,
            day: Number(match[3])
        } : null
    }

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

    // The text a day chosen builds: `yyyy-MM-dd`, and the time of day already in the field for a date-time input.
    function textOf(year: int, month: int, day: int): string {
        let text = year.toString().padStart(4, "0") + "-" + (month + 1).toString().padStart(2, "0") + "-" +
                day.toString().padStart(2, "0")
        if (root.withTime) {
            const time = /[ T](\d{2}):(\d{2})/.exec(field.text)
            text += " " + (time !== null ? time[1] + ":" + time[2] : "00:00")
        }
        return text
    }

    function pickDay(year: int, month: int, day: int): void {
        const text = root.textOf(year, month, day)
        field.text = text
        // The pick reports its own change: a later blur must not report it again.
        root.edited = false
        root.fire("edit", text)
        picker.close()
    }

    TextField {
        font: Theme.bodyFont
        id: field
        width: root.controlWidth
        color: root.scheme.text
        placeholderTextColor: root.scheme.muted
        leftPadding: Theme.spaceOf(3)
        // The text stops before the calendar button.
        rightPadding: Theme.spaceOf(3) + (pickerButton.visible ? pickerButton.implicitWidth : 0)
        background: MorphInputBackground {
            scheme: root.scheme
            focused: field.activeFocus
        }
        readOnly: root.readOnly
        placeholderText: root.withTime ? "yyyy-mm-dd hh:mm" : "yyyy-mm-dd"
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
