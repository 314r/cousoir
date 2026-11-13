pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// A browse section: the library's authors, series, tags, languages or
// publishers with their number of books, and whether a note is kept on
// them. Opening one lists its books.
Rectangle {
    id: page

    required property BookLibrary library
    // What each section says when it has nothing to list.
    readonly property var nouns: ({
        "authors": { none: qsTr("No authors yet"), noMatch: qsTr("No author matches “%1”") },
        "series": { none: qsTr("No series yet"), noMatch: qsTr("No series matches “%1”") },
        "tags": { none: qsTr("No tags yet"), noMatch: qsTr("No tag matches “%1”") },
        "languages": { none: qsTr("No languages yet"), noMatch: qsTr("No language matches “%1”") },
        "publishers": { none: qsTr("No publishers yet"), noMatch: qsTr("No publisher matches “%1”") }
    })
    readonly property var noun: nouns[library.section] || nouns.authors
    readonly property var columns: {
        const columns = [
            { key: "name", title: qsTr("Name"), sort: "name" },
            { key: "count", title: qsTr("Books"), sort: "count", chars: 6, right: true }
        ]
        // Calibre 7's notes on them, which their books' page shows.
        if (library.facetRows.some(row => row.hasNote))
            columns.push({ key: "hasNote", title: qsTr("Note"), chars: 5, blank: true, format: row => row.hasNote ? "¶" : "" })
        return columns
    }

    signal itemActivated(int id)
    signal failed(string message)

    // What can be done to an item of the section: renamed, merged into
    // another by its name, or taken off its books. Not a language, whose
    // names are Calibre's.
    readonly property bool editable: library.writable && library.section !== "languages"
    property var menuRow: null
    readonly property var actions: [
        { key: "open", title: qsTr("Show its books") },
        { key: "rename", title: qsTr("Rename or merge…") }
    ].concat(library.section === "authors" ? [] : [{ key: "remove", title: qsTr("Take off its books…") }])
        .filter(action => action.key === "open" || page.editable)

    function openMenu(index, item, x, y) {
        page.menuRow = page.library.facetRows[index] || null
        if (page.menuRow)
            itemMenu.popup(item, x, y)
    }

    function runAction(key) {
        const row = page.menuRow
        if (!row)
            return
        if (key === "open") {
            page.itemActivated(row.id)
        } else if (key === "rename") {
            renameDialog.itemId = row.id
            renameDialog.title = qsTr("Rename “%1”").arg(row.name)
            renameDialog.ask(row.name)
        } else if (key === "remove") {
            removeDialog.itemId = row.id
            removeDialog.title = qsTr("Take “%1” off its %n books?", "", row.count).arg(row.name)
            removeDialog.ask()
        }
    }

    color: Theme.surface

    // Puts the cursor on the item with `id`, the one just left, if listed.
    function focusList(id) {
        const index = page.library.facetRows.findIndex(row => row.id === id)
        if (index >= 0)
            facetTable.currentIndex = index
        facetTable.focusList()
    }

    function activate(index) {
        const row = page.library.facetRows[index]
        if (!row)
            return
        facetTable.currentIndex = index
        page.itemActivated(row.id)
    }

    function sortBy(key) {
        const option = page.library.facetSortOption
        page.library.facetSortOption = option === key ? key + "Desc" : key
    }

    ListTable {
        id: facetTable

        anchors.fill: parent
        name: "facet"
        columns: page.columns
        rows: page.library.facetRows
        sortOption: page.library.facetSortOption
        minNameChars: 16
        Accessible.name: page.library.section
        onActivated: function(index) { page.activate(index) }
        onSortRequested: function(sort) { page.sortBy(sort) }
        onMenuRequested: function(index, row, x, y) { page.openMenu(index, row, x, y) }
    }

    ActionMenu {
        id: itemMenu
        objectName: "facetMenu"

        actions: page.actions
        onChosen: function(key) { page.runAction(key) }
    }

    RenameDialog {
        id: renameDialog
        objectName: "renameDialog"

        property int itemId: 0

        message: qsTr("A name another one has merges the two. Each book is changed as Calibre changes it.")
        onAccepted: function(name) {
            const error = page.library.renameItem(page.library.section, itemId, name)
            if (error)
                page.failed(error)
        }
    }

    ConfirmDialog {
        id: removeDialog
        objectName: "removeItemDialog"

        property int itemId: 0

        message: qsTr("The books stay; only this is taken off them.")
        acceptText: qsTr("Take Off")
        acceptVariant: "danger"
        onAccepted: {
            const error = page.library.renameItem(page.library.section, itemId, "")
            if (error)
                page.failed(error)
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * Theme.panePadding, Math.round(420 * Theme.spacingScale))
        visible: page.library.facetRows.length === 0
        spacing: Theme.gutter

        Label {
            objectName: "facetEmptyTitle"

            Layout.fillWidth: true
            text: page.library.facetCount === 0 ? page.noun.none : page.noun.noMatch.arg(page.library.searchText)
            font.weight: Font.Bold
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }

        Label {
            Layout.fillWidth: true
            visible: page.library.facetCount > 0
            text: qsTr("Esc clears the search.")
            color: Theme.textMuted
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
    }
}
