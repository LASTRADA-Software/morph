// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// An area files from the operating system can be dropped on, around its content. C++ decides whether it takes a
// drop: every file must have an accepted extension, and one that is not `multiple` refuses several files.
MorphWidget {
    id: root

    readonly property Item content: column

    // Offers the files at `urls` as a drop would; returns whether the zone took them.
    function offer(urls: var): bool {
        const list = []
        for (let i = 0; i < urls.length; ++i)
            list.push(urls[i].toString())
        if (!root.question("takes", list))
            return false
        root.fire("files", list)
        return true
    }

    implicitWidth: column.implicitWidth + 2 * Theme.unitWidth
    implicitHeight: column.implicitHeight + Theme.unitHeight

    Rectangle {
        anchors.fill: parent
        color: files.containsDrag ? Qt.alpha(Theme.info, 0.08) : "transparent"
        border.color: files.containsDrag ? Theme.info : Theme.border
        border.width: 1
        radius: 4
    }

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: Theme.unitWidth
    }

    DropArea {
        id: files
        anchors.fill: parent
        keys: ["text/uri-list"]
        onDropped: drop => {
            if (drop.hasUrls && root.offer(drop.urls))
                drop.accept()
        }
    }
}
