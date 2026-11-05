// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls

// A single-line input's text drawn cut with an ellipsis, over the text box of the `editor` it is a child of, while the
// field shows its text from the start and the text is wider than the room: no focus, no selection, unmasked text.
// The editor stays the input and its text; this label only replaces how an overflowing, unfocused value is drawn, so
// the value, the accessible name and value, and every event are the field's own. The field draws its text in
// `transparent` while `shown`, and a label cut is `truncated`.
//
// With the focus the field follows the cursor, so the editor draws the text; a selection that outlives the focus (a
// context menu or a popup takes it, a window in the background loses it) is drawn by the editor as well, since a
// transparent text would leave the selection's highlight and glyphs over this label's. Both are intended: a window
// in the background shows its edited fields cut, and a field scrolled to its tail by an edit shows its start again
// once it loses the focus.
Label {
    id: root
    objectName: "elision"

    // The TextField whose text box this label shares.
    required property TextField editor
    // The colour the field's text is drawn in; this label draws the same, and the field's own is transparent while
    // this label is shown.
    required property color textColor

    // Whether the cut text is drawn in the editor's place.
    readonly property bool shown: !root.editor.activeFocus && root.editor.selectionStart === root.editor.selectionEnd &&
                                  root.editor.echoMode === TextInput.Normal && root.editor.text !== "" &&
                                  root.editor.contentWidth > root.width

    visible: root.shown
    x: root.editor.leftPadding
    y: root.editor.topPadding
    width: root.editor.width - root.editor.leftPadding - root.editor.rightPadding
    height: root.editor.height - root.editor.topPadding - root.editor.bottomPadding
    verticalAlignment: root.editor.verticalAlignment
    horizontalAlignment: root.editor.horizontalAlignment
    textFormat: Text.PlainText
    elide: Text.ElideRight
    text: root.editor.text
    font: root.editor.font
    color: root.textColor
    Accessible.ignored: true
}
