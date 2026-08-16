// SPDX-License-Identifier: Apache-2.0
//
// bookmarks' main screen: the signed-in user's collection, their tags, and
// the cross-user shared feed. Three panes' worth of behavior, none of it
// domain logic (examples/TESTING.md presenter rule 6, "QML is bindings-only"):
//
//   * every form here is the shipped MorphForms renderer (DynamicForm) driven
//     entirely by schemaJson<A>() — nothing in this file knows CreateBookmark
//     has a `visibility`, or that MergeTags takes two ids;
//   * every list and every detail line is a read-only display of
//     server-computed state relayed by the Task 17 presenters (via
//     gui_lib/bookmark_qml_bridges.hpp);
//   * every error string shown is the model's own `what()`;
//   * the one non-form input is the per-row selection checkbox, which types
//     nothing — it feeds BulkEdit's id list, and BulkEdit cannot be a
//     schema-driven form because its required `ids` member is a JSON array
//     the shipped renderer has no control for (README, known gaps).
//
// The three lists below are plain Qt Quick `ListView`s, not morph::forms'
// own `CollectionView`, and that is a deliberate choice rather than an
// oversight: `CollectionView` renders columns from a view schema —
// `morph::views::viewSchemaJson<V>()` — and this rung defines no such
// document for any of its three row types. Adding one purely to satisfy the
// list widget would be more schema surface than the three read-only lists
// here justify. Whoever adds view schemas to this rung should revisit it.
//
// Every controller property defaults to null so this same file also loads
// with nothing wired up, which is what the offscreen engine-load smoke test
// (tests/test_gui_qml_smoke.cpp) does.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MorphForms

Item {
    id: page

    property var formsController: null
    property var bookmarkController: null
    property var tagController: null
    property var feedController: null

    /// The whole `{actionType: schema}` document, parsed once by Main.qml.
    property var schemas: ({})

    property var rows: []
    property var tagRows: []
    property var feedRows: []
    property var currentBookmark: null
    property var selectedIds: []
    property bool includeArchived: false

    property string status: ""
    property bool statusIsError: false

    function report(message, isError) {
        page.status = message
        page.statusIsError = isError
    }

    function refreshBookmarks() {
        if (!page.bookmarkController)
            return
        if (page.includeArchived)
            page.bookmarkController.refreshIncludingArchived()
        else
            page.bookmarkController.refresh()
    }

    function refreshAll() {
        page.refreshBookmarks()
        if (page.tagController)
            page.tagController.refresh()
        if (page.feedController)
            page.feedController.refresh()
    }

    function isSelected(id) {
        return page.selectedIds.indexOf(id) !== -1
    }

    function setSelected(id, on) {
        const next = page.selectedIds.filter(function (each) { return each !== id })
        if (on)
            next.push(id)
        page.selectedIds = next
    }

    // The first listing cannot simply be requested once on completion. In
    // Remote mode AppContext::onReady() fires when the *socket* connects,
    // which is when gui/main.cpp builds the adapters — but a BridgeHandler's
    // registration is a round trip, and until its reply lands every dispatch
    // through it fails fast with "handler not bound" (morph/core/bridge.hpp).
    // `bound` (backed by `Bridge::whenBound()`) is each controller's own
    // settlement signal for that round trip — Local mode's handlers are
    // already bound by construction, so all three fire synchronously there.
    // This is the identical mitigation pastebin's own Main.qml carries, for
    // the identical reason.
    Connections {
        target: page.bookmarkController

        function onBound() {
            page.refreshBookmarks()
        }

        function onListed(rows) {
            page.rows = rows
            page.report("", false)
        }

        function onLoaded(bookmark) {
            page.currentBookmark = bookmark
            page.report("opened " + bookmark.url, false)
        }

        function onArchived() {
            page.report("archived", false)
            page.refreshBookmarks()
        }

        function onUnarchived() {
            page.report("unarchived", false)
            page.refreshBookmarks()
        }

        function onRemoved() {
            page.currentBookmark = null
            page.report("deleted", false)
            page.refreshBookmarks()
        }

        function onBulkEdited(affected) {
            page.report("bulk edit affected " + affected + " bookmark(s)", false)
            page.selectedIds = []
            page.refreshBookmarks()
        }

        function onFailed(message) {
            page.report(message, true)
        }
    }

    Connections {
        target: page.tagController

        function onBound() {
            page.tagController.refresh()
        }

        function onListed(rows) {
            page.tagRows = rows
        }

        function onFailed(message) {
            page.report(message, true)
        }
    }

    Connections {
        target: page.feedController

        function onBound() {
            page.feedController.refresh()
        }

        function onListed(rows) {
            page.feedRows = rows
        }

        function onFailed(message) {
            page.report(message, true)
        }
    }

    Connections {
        target: page.formsController

        // Every form on this screen submits through FormsBridge, so this —
        // not the presenters' own signals — is where a create/edit/rename/
        // merge/import outcome arrives.
        function onReplyReceived(actionType, ok, payload) {
            if (actionType === "Login")
                return
            if (!ok) {
                page.report(actionType + ": " + payload, true)
                return
            }
            page.report(actionType + " ok: " + payload, false)
            if (actionType === "CreateBookmark")
                createForm.resetFields()
            else if (actionType === "EditBookmark")
                editForm.resetFields()
            else if (actionType === "ImportBookmarks")
                importForm.resetFields()
            else if (actionType === "RenameTag")
                renameForm.resetFields()
            else if (actionType === "MergeTags")
                mergeForm.resetFields()
            page.refreshAll()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        Label {
            Layout.fillWidth: true
            visible: page.status !== ""
            wrapMode: Text.Wrap
            color: page.statusIsError ? "#d33" : palette.text
            text: page.status
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 8

            // ── Pane 1: create + the caller's own collection ───────────────
            ColumnLayout {
                Layout.preferredWidth: 400
                Layout.fillHeight: true
                spacing: 6

                DynamicForm {
                    id: createForm
                    Layout.fillWidth: true
                    actionType: "CreateBookmark"
                    schema: page.schemas["CreateBookmark"] || ({})
                    // Unbound on purpose — see LoginView.qml's identical note.
                    controller: null
                }

                Button {
                    Layout.fillWidth: true
                    text: "Create bookmark"
                    enabled: page.formsController !== null && createForm.ready
                    onClicked: page.formsController.submitIfValid("CreateBookmark", createForm.previewLine)
                }

                RowLayout {
                    Layout.fillWidth: true

                    Button {
                        text: "Refresh"
                        enabled: page.bookmarkController !== null
                        onClicked: page.refreshAll()
                    }

                    CheckBox {
                        text: "show archived"
                        checked: page.includeArchived
                        onToggled: {
                            page.includeArchived = checked
                            page.refreshBookmarks()
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        opacity: 0.7
                        horizontalAlignment: Text.AlignRight
                        text: page.rows.length + " bookmark(s)"
                    }
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: page.rows

                    delegate: RowLayout {
                        id: row
                        required property var modelData
                        width: ListView.view ? ListView.view.width : 0

                        CheckBox {
                            checked: page.isSelected(row.modelData.id)
                            onToggled: page.setSelected(row.modelData.id, checked)
                        }

                        ItemDelegate {
                            Layout.fillWidth: true
                            text: row.modelData.title !== ""
                                  ? row.modelData.title + "  ·  " + row.modelData.url
                                  : row.modelData.url
                            onClicked: {
                                if (page.bookmarkController)
                                    page.bookmarkController.open(row.modelData.id)
                            }
                        }

                        Label {
                            opacity: 0.6
                            text: row.modelData.visibility + " · " + row.modelData.archiveState
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true

                    Label {
                        opacity: 0.7
                        text: page.selectedIds.length + " selected"
                    }

                    Button {
                        text: "Bulk archive"
                        enabled: page.bookmarkController !== null && page.selectedIds.length > 0
                        onClicked: page.bookmarkController.bulkArchive(page.selectedIds, true)
                    }

                    Button {
                        text: "Bulk unarchive"
                        enabled: page.bookmarkController !== null && page.selectedIds.length > 0
                        onClicked: page.bookmarkController.bulkArchive(page.selectedIds, false)
                    }
                }
            }

            // ── Pane 2: the open bookmark, and the edit form for it ────────
            ColumnLayout {
                Layout.preferredWidth: 400
                Layout.fillHeight: true
                spacing: 6

                Label {
                    Layout.fillWidth: true
                    font.bold: true
                    elide: Text.ElideRight
                    text: page.currentBookmark
                          ? (page.currentBookmark.title !== "" ? page.currentBookmark.title
                                                               : page.currentBookmark.url)
                          : "no bookmark open — pick one from the list"
                }

                Repeater {
                    model: page.currentBookmark ? [
                        { key: "url", value: page.currentBookmark.url },
                        { key: "description", value: page.currentBookmark.description },
                        { key: "notes", value: page.currentBookmark.notes },
                        { key: "tags", value: page.currentBookmark.tags.join(", ") },
                        { key: "visibility", value: page.currentBookmark.visibility },
                        { key: "read", value: page.currentBookmark.readState },
                        { key: "archive", value: page.currentBookmark.archiveState },
                        { key: "created", value: page.currentBookmark.createdAt },
                        { key: "updated", value: page.currentBookmark.updatedAt }
                    ] : []

                    delegate: Label {
                        required property var modelData
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: modelData.key + ": " + modelData.value
                    }
                }

                RowLayout {
                    Layout.fillWidth: true

                    Button {
                        text: "Archive"
                        enabled: page.bookmarkController !== null && page.currentBookmark !== null
                        onClicked: page.bookmarkController.archive(page.currentBookmark.id)
                    }

                    Button {
                        text: "Unarchive"
                        enabled: page.bookmarkController !== null && page.currentBookmark !== null
                        onClicked: page.bookmarkController.unarchive(page.currentBookmark.id)
                    }

                    Button {
                        text: "Delete"
                        enabled: page.bookmarkController !== null && page.currentBookmark !== null
                        onClicked: page.bookmarkController.remove(page.currentBookmark.id)
                    }
                }

                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true

                    ColumnLayout {
                        width: parent.width

                        DynamicForm {
                            id: editForm
                            Layout.fillWidth: true
                            actionType: "EditBookmark"
                            schema: page.schemas["EditBookmark"] || ({})
                            controller: null
                        }

                        Button {
                            Layout.fillWidth: true
                            text: "Apply edit"
                            enabled: page.formsController !== null && editForm.ready
                            onClicked: page.formsController.submitIfValid("EditBookmark", editForm.previewLine)
                        }

                        DynamicForm {
                            id: importForm
                            Layout.fillWidth: true
                            actionType: "ImportBookmarks"
                            schema: page.schemas["ImportBookmarks"] || ({})
                            controller: null
                        }

                        Button {
                            Layout.fillWidth: true
                            text: "Import chunk"
                            enabled: page.formsController !== null && importForm.ready
                            onClicked: page.formsController.submitIfValid("ImportBookmarks", importForm.previewLine)
                        }
                    }
                }
            }

            // ── Pane 3: tags, and the cross-user shared feed ───────────────
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 6

                Label {
                    font.bold: true
                    text: "Tags (" + page.tagRows.length + ")"
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 120
                    clip: true
                    model: page.tagRows

                    delegate: Label {
                        required property var modelData
                        width: ListView.view ? ListView.view.width : 0
                        elide: Text.ElideRight
                        text: "#" + modelData.id + "  " + modelData.name + "  ·  "
                              + modelData.bookmarkCount + " bookmark(s)"
                    }
                }

                ScrollView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 260
                    clip: true

                    ColumnLayout {
                        width: parent.width

                        DynamicForm {
                            id: renameForm
                            Layout.fillWidth: true
                            actionType: "RenameTag"
                            schema: page.schemas["RenameTag"] || ({})
                            controller: null
                        }

                        Button {
                            Layout.fillWidth: true
                            text: "Rename tag"
                            enabled: page.formsController !== null && renameForm.ready
                            onClicked: page.formsController.submitIfValid("RenameTag", renameForm.previewLine)
                        }

                        DynamicForm {
                            id: mergeForm
                            Layout.fillWidth: true
                            actionType: "MergeTags"
                            schema: page.schemas["MergeTags"] || ({})
                            controller: null
                        }

                        Button {
                            Layout.fillWidth: true
                            text: "Merge tags"
                            enabled: page.formsController !== null && mergeForm.ready
                            onClicked: page.formsController.submitIfValid("MergeTags", mergeForm.previewLine)
                        }
                    }
                }

                Label {
                    font.bold: true
                    text: "Shared feed (" + page.feedRows.length + ")"
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: page.feedRows

                    delegate: Label {
                        required property var modelData
                        width: ListView.view ? ListView.view.width : 0
                        elide: Text.ElideRight
                        wrapMode: Text.NoWrap
                        text: (modelData.title !== "" ? modelData.title : modelData.url)
                              + "  ·  " + modelData.createdAt
                    }
                }
            }
        }
    }
}
