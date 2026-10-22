pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// The library's books, or those of the author, series, tag, language or
// publisher opened in a browse section: a table, or a grid of covers. A row's
// menu (right click, Menu or Shift+F10) opens it here or in a reader,
// converts it, sends it to a Kobo or moves it to the trash.
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
    property ConversionService conversions: null
    property DeviceLibrary devices: null
    readonly property bool canConvert: library.writable && conversions !== null && conversions.available
    readonly property bool canSend: devices !== null && devices.available && devices.writable
    readonly property var actions: [
        { key: "open", title: qsTr("Open") },
        { key: "read", title: qsTr("Open in reader") },
        { key: "folder", title: qsTr("Show in folder") },
        { key: "convert:KEPUB", title: qsTr("Convert to KEPUB") },
        { key: "convert:AZW3", title: qsTr("Convert to AZW3") }
    ].concat((page.canSend ? page.devices.devices : []).map(device => ({
        key: "send:" + device.root, title: qsTr("Send to %1").arg(device.name)
    }))).concat([
        { key: "trash", title: qsTr("Move to Trash") }
    ]).filter(action => action.key === "trash" ? page.library.writable
                        : action.key.startsWith("convert:") ? page.canConvert : true)
    readonly property var view: gridMode ? coverGrid : bookTable
    readonly property int currentIndex: view.currentIndex
    // The row whose menu is open.
    property int menuIndex: -1
    // A virtual library or a saved search narrows the books listed.
    readonly property bool narrowed: library.virtualLibrary !== "" || library.section.startsWith("search:")

    signal bookActivated(int bookId)
    // Move to Trash, chosen in a row's menu.
    signal trashRequested(int bookId)
    // Convert to a format, chosen in a row's menu.
    signal convertRequested(int bookId, string format)
    // Send to the Kobo mounted on `root`, chosen in a row's menu.
    signal sendRequested(int bookId, string root)
    signal reloadRequested()
    // Choose folder…, when the library cannot be read.
    signal libraryFolderRequested()
    // Opening a book's file failed; `message` says why.
    signal failed(string message)

    color: Theme.canvas

    function focusList() {
        // Keep the book that was just closed under the cursor.
        const index = page.library.books.indexOf(page.library.currentBookId)
        if (index >= 0) {
            bookTable.currentIndex = index
            coverGrid.currentIndex = index
        }
        page.view.focusList()
    }

    // Makes the book's row current, when it is shown.
    function select(bookId) {
        const index = page.library.books.indexOf(bookId)
        if (index < 0)
            return
        bookTable.currentIndex = index
        coverGrid.currentIndex = index
        page.view.focusList()
    }

    function activate(index) {
        if (index < 0 || index >= page.library.books.count)
            return
        bookTable.currentIndex = index
        coverGrid.currentIndex = index
        page.bookActivated(page.library.books.idAt(index))
    }

    function sortBy(key) {
        const option = page.library.sortOption
        page.library.sortOption = option === key ? key + "Desc" : key
    }

    // The current book's id, or 0.
    function currentBookId() {
        return page.library.books.idAt(page.currentIndex)
    }

    function runAction(key, index) {
        const row = page.library.books.get(index)
        if (!row.id)
            return
        if (key === "open") {
            page.activate(index)
            return
        }
        if (key === "trash") {
            page.trashRequested(row.id)
            return
        }
        if (key.startsWith("convert:")) {
            page.convertRequested(row.id, key.split(":")[1])
            return
        }
        if (key.startsWith("send:")) {
            page.sendRequested(row.id, key.slice(5))
            return
        }
        const error = key === "read" ? page.library.openFormat(row.id, "") : page.library.openFolder(row.id)
        if (error)
            page.failed(error)
    }

    // What stands in place of the books when there are none to show.
    function emptyTitle() {
        if (page.library.loaded && page.library.searchError)
            return qsTr("Calibre's search cannot read “%1”").arg(page.library.searchText)
        if (page.library.loaded && page.library.scopeCount === 0 && page.narrowed)
            return qsTr("No books here")
        if (page.library.loaded)
            return page.library.scopeCount === 0 ? qsTr("No books yet")
                                                 : qsTr("No book matches “%1”").arg(page.library.searchText)
        if (page.library.loading)
            return qsTr("Reading the library…")
        return page.library.libraryPath ? qsTr("Cannot read the library") : qsTr("Welcome to Cousoir")
    }

    function emptyMessage() {
        if (page.library.loaded && page.library.searchError)
            return page.library.searchError
        if (page.library.loaded && page.library.scopeCount === 0 && page.narrowed)
            return page.library.virtualLibrary
                ? qsTr("The virtual library “%1” has none of them.").arg(page.library.virtualLibrary) : ""
        if (page.library.loaded)
            return page.library.scopeCount === 0 ? qsTr("Add books with Ctrl+N, or drop their files here.")
                                                 : qsTr("Esc clears the search.")
        if (page.library.loading)
            return ""
        return page.library.libraryPath
            ? page.library.lastError
            : qsTr("Cousoir opens the library Calibre opens on this computer, and Calibre has none yet. Choose a library's folder, the one holding metadata.db, or make one in Calibre and press Reload.")
    }

    // A new list keeps the current row where it was, or starts at the first,
    // in both views.
    Connections {
        target: page.library.books

        property int before: -1

        function onModelAboutToBeReset() {
            before = page.currentIndex
        }

        function onModelReset() {
            const count = page.library.books.count
            const index = before >= 0 && before < count ? before : count > 0 ? 0 : -1
            bookTable.currentIndex = index
            coverGrid.currentIndex = index
        }
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
            rows: page.library.books
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
            rows: page.library.books
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
            visible: page.library.books.count === 0
            spacing: Theme.gutter

            Label {
                objectName: "emptyTitle"

                Layout.fillWidth: true
                text: page.emptyTitle()
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }

            Label {
                objectName: "emptyMessage"

                Layout.fillWidth: true
                visible: text.length > 0
                text: page.emptyMessage()
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

                    // The one way forward when Calibre has no library.
                    variant: page.library.libraryPath ? "outline" : "primary"
                    text: qsTr("Choose folder…")
                    icon.source: "assets/icons/folder.svg"
                    onClicked: page.libraryFolderRequested()
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
