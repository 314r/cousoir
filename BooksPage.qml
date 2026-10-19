pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Reliure

// The library's books, or those of the author, series, tag, language or
// publisher opened in a browse section: a table, or a grid of covers. A row's
// menu (right click, Menu or Shift+F10) opens it here or in a reader.
Rectangle {
    id: page

    required property BookLibrary library
    property bool gridMode: false
    readonly property var columns: [
        { key: "title", title: qsTr("Title"), sort: "title" },
        { key: "authors", title: qsTr("Authors"), sort: "authors", chars: 18, drop: 5 },
        { key: "series", title: qsTr("Series"), sort: "series", chars: 16, drop: 2 },
        { key: "formats", title: qsTr("Formats"), chars: 12, drop: 1 },
        { key: "added", title: qsTr("Added"), sort: "added", chars: 10, drop: 4 },
        { key: "rating", title: qsTr("Rating"), sort: "rating", chars: 6, drop: 3 }
    ]
    readonly property var actions: [
        { key: "open", title: qsTr("Open") },
        { key: "read", title: qsTr("Open in reader") },
        { key: "folder", title: qsTr("Show in folder") }
    ]
    readonly property var view: gridMode ? coverGrid : bookTable
    readonly property int currentIndex: view.currentIndex
    // The row whose menu is open.
    property int menuIndex: -1

    signal bookActivated(int bookId)
    signal reloadRequested()
    signal settingsRequested()
    // Opening a book's file failed; `message` says why.
    signal failed(string message)

    color: Theme.canvas

    function focusList() {
        // Keep the book that was just closed under the cursor.
        const rows = page.library.rows
        const index = rows.findIndex(row => row.id === page.library.currentBookId)
        if (index >= 0) {
            bookTable.currentIndex = index
            coverGrid.currentIndex = index
        }
        page.view.focusList()
    }

    function moveCurrent(delta) {
        page.view.moveCurrent(delta)
    }

    function activate(index) {
        if (index < 0 || index >= page.library.rows.length)
            return
        bookTable.currentIndex = index
        coverGrid.currentIndex = index
        page.bookActivated(page.library.rows[index].id)
    }

    function sortBy(key) {
        const option = page.library.sortOption
        page.library.sortOption = option === key ? key + "Desc" : key
    }

    // The current book's id, or 0.
    function currentBookId() {
        const row = page.library.rows[page.currentIndex]
        return row ? row.id : 0
    }

    function runAction(key, index) {
        const row = page.library.rows[index]
        if (!row)
            return
        if (key === "open") {
            page.activate(index)
            return
        }
        const error = key === "read" ? page.library.openFormat(row.id, "") : page.library.openFolder(row.id)
        if (error)
            page.failed(error)
    }

    // Both views follow the same row, so switching keeps the selection.
    onGridModeChanged: {
        if (gridMode)
            coverGrid.currentIndex = bookTable.currentIndex
        else
            bookTable.currentIndex = coverGrid.currentIndex
        // Once the other view shows: a hidden item cannot take the focus.
        if (visible)
            Qt.callLater(() => page.view.focusList())
    }

    Item {
        anchors.fill: parent

        ListTable {
            id: bookTable

            anchors.fill: parent
            visible: !page.gridMode
            name: "book"
            columns: page.columns
            rows: page.library.rows
            sortOption: page.library.sortOption
            Accessible.name: qsTr("Books")
            onActivated: function(index) { page.activate(index) }
            onSortRequested: function(sort) { page.sortBy(sort) }
            onMenuRequested: function(index, row, x, y) {
                page.menuIndex = index
                bookMenu.popup(row, x, y)
            }
        }

        CoverGrid {
            id: coverGrid

            anchors.fill: parent
            visible: page.gridMode
            name: "cover"
            rows: page.library.rows
            Accessible.name: qsTr("Book covers")
            onActivated: function(index) { page.activate(index) }
            onMenuRequested: function(index, cell, x, y) {
                page.menuIndex = index
                bookMenu.popup(cell, x, y)
            }
        }

        ColumnLayout {
            anchors.centerIn: parent
            width: Math.min(parent.width - 2 * Theme.panePadding, Math.round(420 * Theme.spacingScale))
            visible: page.library.rows.length === 0
            spacing: Theme.gutter

            Label {
                objectName: "emptyTitle"

                Layout.fillWidth: true
                text: !page.library.loaded
                    ? (page.library.loading ? qsTr("Reading the library…") : qsTr("Cannot read the library"))
                    : page.library.scopeCount === 0
                        ? qsTr("No books yet")
                        : qsTr("No book matches “%1”").arg(page.library.searchText)
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }

            Label {
                objectName: "emptyMessage"

                Layout.fillWidth: true
                visible: text.length > 0
                text: !page.library.loaded
                    ? (page.library.loading ? "" : page.library.lastError)
                    : page.library.scopeCount === 0
                        ? qsTr("Books you add to the library in Calibre appear here.")
                        : qsTr("Esc clears the search.")
                color: Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: Theme.gutter
                visible: !page.library.loaded && !page.library.loading
                spacing: Theme.gutter

                FlatButton {
                    objectName: "reloadButton"

                    variant: "outline"
                    text: qsTr("Reload")
                    onClicked: page.reloadRequested()
                }

                FlatButton {
                    objectName: "chooseLibraryButton"

                    variant: "outline"
                    text: qsTr("Choose library…")
                    onClicked: page.settingsRequested()
                }
            }
        }
    }

    ActionMenu {
        id: bookMenu
        objectName: "bookMenu"

        actions: page.actions
        onChosen: function(key) { page.runAction(key, page.menuIndex) }
        // Back to the list, unless Open showed the book instead.
        onClosed: {
            if (page.visible)
                page.view.focusList()
        }
    }
}
