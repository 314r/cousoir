pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// A browse section: the library's authors, series, tags, languages or
// publishers with their number of books. Opening one lists its books.
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
    readonly property var columns: [
        { key: "name", title: qsTr("Name"), sort: "name" },
        { key: "count", title: qsTr("Books"), sort: "count", chars: 6, right: true }
    ]

    signal itemActivated(int id)

    color: Theme.canvas

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
