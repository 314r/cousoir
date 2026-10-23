pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// The books on a connected reader: those sent to it, those copied or bought,
// and whether each is one of the library's. A book in the library opens
// from here; one sent to the reader can be removed from it.
Rectangle {
    id: page

    required property DeviceLibrary devices
    readonly property var device: devices.current
    readonly property var columns: [
        { key: "title", title: qsTr("Title") },
        { key: "authors", title: qsTr("Authors"), chars: 18, drop: 3 },
        { key: "format", title: qsTr("Format"), chars: 6, drop: 1 },
        { key: "size", title: qsTr("Size"), chars: 9, right: true, drop: 2 },
        { key: "reading", title: qsTr("Read"), chars: 5, right: true, drop: 5 },
        { key: "notes", title: qsTr("Notes"), chars: 5, right: true, drop: 6,
          format: row => row.notes > 0 ? String(row.notes) : "" },
        { key: "library", title: qsTr("Library"), chars: 8, drop: 4,
          format: row => row.bookId > 0 ? qsTr("Yes") : row.purchased ? qsTr("Bought") : "" }
    ]
    readonly property var actions: {
        const row = page.devices.rows[page.menuIndex]
        if (!row)
            return []
        return [
            { key: "open", title: qsTr("Show in library"), enabled: row.bookId > 0 },
            { key: "remove", title: qsTr("Remove from %1").arg(page.device.name || ""),
              enabled: !row.purchased && page.devices.writable }
        ].filter(action => action.enabled)
    }
    property int menuIndex: -1

    // A book of the library, to open.
    signal bookRequested(int bookId)
    // Remove from the reader, asked for in a row's menu or with Delete.
    signal removeRequested(string lpath, string title)

    color: Theme.canvas

    function focusList() {
        deviceTable.focusList()
    }

    function activate(index) {
        const row = page.devices.rows[index]
        if (row && row.bookId > 0)
            page.bookRequested(row.bookId)
    }

    function remove(index) {
        const row = page.devices.rows[index]
        if (row && !row.purchased && page.devices.writable)
            page.removeRequested(row.lpath, row.title)
    }

    function runAction(key, index) {
        if (key === "open")
            page.activate(index)
        else if (key === "remove")
            page.remove(index)
    }

    ListTable {
        id: deviceTable

        anchors.fill: parent
        name: "device"
        columns: page.columns
        rows: page.devices.rows
        Accessible.name: qsTr("Books on %1").arg(page.device.name || "")
        onActivated: function(index) { page.activate(index) }
        onRemoveRequested: function(index) { page.remove(index) }
        onMenuRequested: function(index, row, x, y) {
            page.menuIndex = index
            if (page.actions.length > 0)
                deviceMenu.popup(row, x, y)
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * Theme.panePadding, Math.round(420 * Theme.spacingScale))
        visible: page.devices.rows.length === 0
        spacing: Theme.gutter

        Label {
            objectName: "deviceEmpty"

            Layout.fillWidth: true
            text: page.devices.loading ? qsTr("Reading the books on %1…").arg(page.device.name || "")
                : page.devices.lastError ? qsTr("Cannot read the books on %1").arg(page.device.name || "")
                : qsTr("No books on %1").arg(page.device.name || "")
            font.weight: Font.Bold
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }

        Label {
            Layout.fillWidth: true
            visible: !page.devices.loading
            text: page.devices.lastError || qsTr("Send books from the library with Ctrl+D or a book's Files.")
            color: Theme.textMuted
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
    }

    ActionMenu {
        id: deviceMenu
        objectName: "deviceMenu"

        actions: page.actions
        onChosen: function(key) { page.runAction(key, page.menuIndex) }
        onClosed: {
            if (page.visible)
                deviceTable.focusList()
        }
    }
}
