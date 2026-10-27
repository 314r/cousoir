pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// The library's books, or those of the author, series, tag, language or
// publisher opened in a browse section: a table, or a grid of covers. A row's
// menu (right click, Menu or Shift+F10) opens it here or in a reader,
// converts it, sends it to a reader or moves it to the trash. Ctrl and Shift
// with a click or the moving keys, and Ctrl+A, select several books: the
// menu, Del, Ctrl+D and the conversions then act on all of them, and a pane
// beside the list edits them together.
Rectangle {
    id: page

    required property BookLibrary library
    property bool gridMode: false
    // The library's columns, the title stretching, those on the right
    // leaving first when the window is narrow.
    readonly property var columns: {
        const shown = page.library.availableColumns.filter(column => column.shown)
        return shown.map((column, index) => ({
            key: column.key,
            title: column.title,
            sort: column.key,
            chars: index === 0 ? 0 : page.columnChars(column),
            right: column.kind === "number",
            drop: shown.length - index
        }))
    }
    // The column whose menu is open, and its actions.
    property string menuColumn: ""
    readonly property var columnActions: {
        if (!page.menuColumn)
            return []
        const all = page.library.availableColumns
        const shown = all.filter(column => column.shown).map(column => column.key)
        const at = shown.indexOf(page.menuColumn)
        const actions = [
            { key: "sort:asc", title: qsTr("Sort A to Z, 1 to 9") },
            { key: "sort:desc", title: qsTr("Sort Z to A, 9 to 1") }
        ]
        if (at > 0) {
            if (at > 1)
                actions.push({ key: "left", title: qsTr("Move left") })
            if (at < shown.length - 1)
                actions.push({ key: "right", title: qsTr("Move right") })
            actions.push({ key: "hide", title: qsTr("Hide this column") })
        }
        for (const column of all.filter(column => !column.shown))
            actions.push({ key: "show:" + column.key, title: qsTr("Show “%1”").arg(column.title) })
        return actions
    }
    property ConversionService conversions: null
    property DeviceLibrary devices: null
    readonly property bool canConvert: library.writable && conversions !== null
    readonly property bool canSend: devices !== null && devices.writable
    readonly property int selectedCount: library.books.selectedCount
    // The menu's actions: on its book, or on every book selected when they
    // are several; both kept as the menu opens, should the list change
    // meanwhile.
    readonly property bool menuOnSelection: menuTargets.length > 1
    property int menuBookId: 0
    property var menuTargets: []
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

    // The books an action acts on: those selected, which a click on a row
    // outside them made the row alone; none when none is.
    function targetIds() {
        return page.library.books.selectedIds
    }

    function sortBy(key) {
        page.library.sortBy(key)
    }

    // How many caption characters a column of that kind takes.
    function columnChars(column) {
        const widths = { authors: 18, series: 16, formats: 12, publisher: 14, languages: 12, pubdate: 9, size: 9,
                         id: 6, pages: 6, tags: 18, identifiers: 22, path: 24 }
        if (widths[column.key] !== undefined)
            return widths[column.key]
        return { date: 10, rating: 7, number: 8, long: 20 }[column.kind] || 14
    }

    function runColumnAction(key) {
        const shown = page.library.columns.slice()
        const at = shown.indexOf(page.menuColumn)
        if (key === "sort:asc" || key === "sort:desc") {
            page.library.sortOption = page.menuColumn + (key === "sort:desc" ? "Desc" : "")
        } else if (key === "left" || key === "right") {
            const to = key === "left" ? at - 1 : at + 1
            shown.splice(at, 1)
            shown.splice(to, 0, page.menuColumn)
            page.library.columns = shown
        } else if (key === "hide") {
            shown.splice(at, 1)
            page.library.columns = shown
        } else if (key.startsWith("show:")) {
            // Beside the column asked about.
            shown.splice(at + 1, 0, key.slice(5))
            page.library.columns = shown
        }
        page.focusList()
    }

    // The current book's id, or 0.
    function currentBookId() {
        return page.library.books.idAt(page.currentIndex)
    }

    function openMenu(item, x, y) {
        page.menuBookId = page.library.books.idAt(page.currentIndex)
        page.menuTargets = page.targetIds()
        bookMenu.popup(item, x, y)
    }

    function runAction(key) {
        const bookId = page.menuBookId
        if (!bookId)
            return
        if (key === "open") {
            page.activate(page.library.books.indexOf(bookId))
            return
        }
        if (key === "trash") {
            page.trashRequested(page.menuTargets)
            return
        }
        if (key.startsWith("convert:")) {
            page.convertRequested(page.menuTargets, key.split(":")[1])
            return
        }
        if (key.startsWith("send:")) {
            page.sendRequested(page.menuTargets, key.slice(5))
            return
        }
        const error = key === "read" ? page.library.openFormat(bookId, "") : page.library.openFolder(bookId)
        if (error)
            page.failed(error)
    }

    // What stands in place of the books when there are none to show.
    function emptyTitle() {
        if (page.library.loaded && page.library.sectionError)
            return qsTr("Cousoir cannot run the saved search “%1”").arg(page.library.section.slice(7))
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
        if (page.library.loaded && page.library.sectionError)
            return page.library.sectionError
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
    // sort for instance; else it starts at the first, or at the first book
    // still selected among several. A single selection follows the current
    // book.
    Connections {
        target: page.library.books

        property int before: 0

        function onModelAboutToBeReset() {
            before = page.library.books.idAt(page.currentIndex)
        }

        function onModelReset() {
            const books = page.library.books
            const kept = books.indexOf(before)
            const selected = books.selectedCount > 1 ? books.indexOf(books.selectedIds[0]) : -1
            const index = kept >= 0 && (selected < 0 || books.isSelected(kept)) ? kept
                : selected >= 0 ? selected : books.count > 0 ? 0 : -1
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
                selectable: true
                Accessible.name: qsTr("Books")
                onActivated: function(index) { page.activate(index) }
                onSortRequested: function(sort) { page.sortBy(sort) }
                onColumnMenuRequested: function(key, header, x, y) {
                    page.menuColumn = key
                    columnMenu.popup(header, x, y)
                }
                onMenuRequested: function(index, row, x, y) { page.openMenu(row, x, y) }
                onSelectRequested: function(index, mode) { page.selectRow(index, mode) }
                onSelectAllRequested: page.library.books.selectAll()
            }

            CoverGrid {
                id: coverGrid

                anchors.fill: parent
                visible: page.gridMode
                name: "cover"
                rows: page.library.books
                selectable: true
                Accessible.name: qsTr("Book covers")
                onActivated: function(index) { page.activate(index) }
                onMenuRequested: function(index, cell, x, y) { page.openMenu(cell, x, y) }
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

        Divider {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            visible: notePane.visible
        }

        NotePanel {
            id: notePane
            objectName: "notePane"

            Layout.preferredWidth: Theme.paneWidth
            Layout.fillHeight: true
            visible: page.paneShown && !bulkPane.visible && wanted
            library: page.library
            onFailed: function(message) { page.failed(message) }
        }
    }

    ActionMenu {
        id: columnMenu
        objectName: "columnMenu"

        actions: page.columnActions
        onChosen: function(key) { page.runColumnAction(key) }
    }

    ActionMenu {
        id: bookMenu
        objectName: "bookMenu"

        actions: page.actions
        onChosen: function(key) { page.runAction(key) }
        // Back to the list, unless Open showed the book instead.
        onClosed: {
            if (page.visible)
                page.view.focusList()
        }
    }
}
