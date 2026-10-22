pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// The library's books, or those of the author, series, tag, language or
// publisher opened in a browse section: a table, or a grid of covers. A row's
// menu (right click, Menu or Shift+F10) opens it here or in a reader,
// converts it, sends it to a Kobo or moves it to the trash. Ctrl and Shift
// with a click or the moving keys, and Ctrl+A, select several books: the
// menu, Del, Ctrl+D and the conversions then act on all of them, and a pane
// beside the list edits them together.
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
    readonly property int selectedCount: library.books.selectedCount
    // The menu's actions: on its row, or on every book selected when its row
    // is one of several.
    readonly property bool menuOnSelection: selectedCount > 1 && library.books.isSelected(menuIndex)
    readonly property var actions: (menuOnSelection ? [] : [
        { key: "open", title: qsTr("Open") },
        { key: "read", title: qsTr("Open in reader") },
        { key: "folder", title: qsTr("Show in folder") }
    ]).concat([
        { key: "convert:KEPUB", title: menuOnSelection ? qsTr("Convert %1 books to KEPUB").arg(selectedCount)
                                                       : qsTr("Convert to KEPUB") },
        { key: "convert:AZW3", title: menuOnSelection ? qsTr("Convert %1 books to AZW3").arg(selectedCount)
                                                      : qsTr("Convert to AZW3") }
    ]).concat((page.canSend ? page.devices.devices : []).map(device => ({
        key: "send:" + device.root, title: menuOnSelection ? qsTr("Send %1 books to %2").arg(selectedCount).arg(device.name)
                                                           : qsTr("Send to %1").arg(device.name)
    }))).concat([
        { key: "trash", title: menuOnSelection ? qsTr("Move %1 books to Trash").arg(selectedCount) : qsTr("Move to Trash") }
    ]).filter(action => action.key === "trash" ? page.library.writable
                        : action.key.startsWith("convert:") ? page.canConvert : true)
    // The pane that edits the books selected together, beside the list.
    property bool paneShown: true
    readonly property var view: gridMode ? coverGrid : bookTable
    readonly property int currentIndex: view.currentIndex
    // The row whose menu is open.
    property int menuIndex: -1
    // A virtual library or a saved search narrows the books listed.
    readonly property bool narrowed: library.virtualLibrary !== "" || library.section.startsWith("search:")

    signal bookActivated(int bookId)
    // Move to Trash, chosen in a row's menu, for one book or several.
    signal trashRequested(var bookIds)
    // Convert to a format, chosen in a row's menu.
    signal convertRequested(var bookIds, string format)
    // Send to the reader on `root`, chosen in a row's menu.
    signal sendRequested(var bookIds, string root)
    signal reloadRequested()
    // Choose folder…, when the library cannot be read.
    signal libraryFolderRequested()
    // Opening a book's file failed; `message` says why.
    signal failed(string message)

    color: Theme.canvas

    // Makes the row current in both views, and selects it alone.
    function setCurrent(index) {
        bookTable.currentIndex = index
        coverGrid.currentIndex = index
        if (index >= 0)
            page.library.books.select(index, "only")
    }

    function focusList() {
        // Keep the book that was just closed under the cursor.
        const index = page.library.books.indexOf(page.library.currentBookId)
        if (index >= 0)
            page.setCurrent(index)
        page.view.focusList()
    }

    // Makes the book's row current, when it is shown.
    function select(bookId) {
        const index = page.library.books.indexOf(bookId)
        if (index < 0)
            return
        page.setCurrent(index)
        page.view.focusList()
    }

    function activate(index) {
        if (index < 0 || index >= page.library.books.count)
            return
        page.setCurrent(index)
        page.bookActivated(page.library.books.idAt(index))
    }

    // Selection from a view: both views then show the same current row.
    function selectRow(index, mode) {
        bookTable.currentIndex = index
        coverGrid.currentIndex = index
        page.library.books.select(index, mode)
    }

    // Back to the current book alone.
    function collapseSelection() {
        if (page.currentIndex >= 0)
            page.library.books.select(page.currentIndex, "only")
    }

    // The books an action on the row at `index` acts on: every book selected
    // when the row is one of them, else the row's.
    function targetIds(index) {
        const books = page.library.books
        if (books.selectedCount > 1 && books.isSelected(index))
            return books.selectedIds
        const id = books.idAt(index)
        return id ? [id] : []
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
            page.trashRequested(page.targetIds(index))
            return
        }
        if (key.startsWith("convert:")) {
            page.convertRequested(page.targetIds(index), key.split(":")[1])
            return
        }
        if (key.startsWith("send:")) {
            page.sendRequested(page.targetIds(index), key.slice(5))
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

    // A new list keeps the current book current when it still shows it, a
    // sort for instance; else it starts at the first. A single selection
    // follows the current book.
    Connections {
        target: page.library.books

        property int before: 0

        function onModelAboutToBeReset() {
            before = page.library.books.idAt(page.currentIndex)
        }

        function onModelReset() {
            const books = page.library.books
            const kept = books.indexOf(before)
            const index = kept >= 0 ? kept : books.count > 0 ? 0 : -1
            bookTable.currentIndex = index
            coverGrid.currentIndex = index
            if (index >= 0 && books.selectedCount <= 1)
                books.select(index, "only")
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

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListTable {
                id: bookTable

                anchors.fill: parent
                visible: !page.gridMode
                name: "book"
                columns: page.columns
                rows: page.library.books
                sortOption: page.library.sortOption
                selectionCount: page.selectedCount
                Accessible.name: qsTr("Books")
                onActivated: function(index) { page.activate(index) }
                onSortRequested: function(sort) { page.sortBy(sort) }
                onMenuRequested: function(index, row, x, y) {
                    page.menuIndex = index
                    bookMenu.popup(row, x, y)
                }
                onSelectRequested: function(index, mode) { page.selectRow(index, mode) }
                onSelectAllRequested: page.library.books.selectAll()
            }

            CoverGrid {
                id: coverGrid

                anchors.fill: parent
                visible: page.gridMode
                name: "cover"
                rows: page.library.books
                selectionCount: page.selectedCount
                Accessible.name: qsTr("Book covers")
                onActivated: function(index) { page.activate(index) }
                onMenuRequested: function(index, cell, x, y) {
                    page.menuIndex = index
                    bookMenu.popup(cell, x, y)
                }
                onSelectRequested: function(index, mode) { page.selectRow(index, mode) }
                onSelectAllRequested: page.library.books.selectAll()
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

        Divider {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            visible: bulkPane.visible
        }

        BulkEditPane {
            id: bulkPane
            objectName: "bulkPane"

            Layout.preferredWidth: Theme.paneWidth
            Layout.fillHeight: true
            visible: page.paneShown && page.selectedCount > 1
            library: page.library
            bookIds: visible ? page.library.books.selectedIds : []
            onFailed: function(message) { page.failed(message) }
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
