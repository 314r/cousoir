pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// The books' text searched, in the index Calibre keeps of it when its full
// text search is turned on: each book whose text has the words, with the
// passage around them. Enter reads the book, searching it for the words
// found.
Rectangle {
    id: page

    required property BookLibrary library
    property var rows: []
    property string error: ""
    property string query: ""
    property var index: ({})

    signal readRequested(int bookId, string query)

    color: Theme.surface

    function refresh() {
        page.index = page.library.textIndex()
    }

    function search(text) {
        page.query = text
        const found = page.library.searchBooksText(text)
        page.rows = found.rows
        page.error = found.error
    }

    function focusList() {
        if (page.rows.length > 0)
            results.forceActiveFocus()
        else
            field.forceActiveFocus()
    }

    function focusField() {
        field.forceActiveFocus()
        field.selectAll()
    }

    // The book read, searched for the words its passage has: "running"
    // for a search of "run", one word of "a OR b".
    function read(index) {
        const row = page.rows[index]
        if (!row)
            return
        const found = /\x1d([^\x1d]+)\x1d/.exec(row.snippet)
        page.readRequested(row.bookId, found ? found[1] : page.query)
    }

    // The words found in bold, the paragraphs' ends marked, the rest as it is.
    function marked(snippet) {
        const escaped = snippet.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
            .replace(/\s*\n\s*/g, " ¶ ")
        let open = false
        return escaped.replace(/\x1d/g, () => (open = !open) ? "<b>" : "</b>")
    }

    onVisibleChanged: {
        if (visible)
            refresh()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.pagePadding
        spacing: Theme.gutter

        SearchField {
            id: field
            objectName: "textSearchField"

            Layout.fillWidth: true
            Layout.maximumWidth: Math.round(640 * Theme.spacingScale)
            enabled: page.index.exists === true
            placeholderText: qsTr("Words in the books' text")
            Accessible.name: qsTr("Search the books' text")
            onTextChanged: delay.restart()
            Keys.onDownPressed: results.forceActiveFocus()
            Keys.onReturnPressed: page.focusList()

            Timer {
                id: delay
                interval: 300
                onTriggered: page.search(field.text)
            }
        }

        Label {
            objectName: "textIndexStatus"

            Layout.fillWidth: true
            text: page.index.exists !== true
                ? qsTr("Calibre has not indexed the text of this library's books. Turn full text searching on in Calibre, under Preferences > Searching, and its index is searched here.")
                : page.error ? page.error
                : page.index.pending > 0
                    ? qsTr("%1 Calibre still has %n formats to index.", "", page.index.pending).arg(qsTr("%n formats indexed.", "", page.index.indexed))
                    : qsTr("%n formats indexed.", "", page.index.indexed)
            color: page.error ? Theme.danger : Theme.textMuted
            font.pointSize: Theme.fontPointSizeCaption
            wrapMode: Text.Wrap
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: results
                objectName: "textResults"

                anchors.fill: parent
                clip: true
                model: page.rows
                spacing: Theme.gutter
                keyNavigationEnabled: true
                boundsBehavior: Flickable.StopAtBounds
                activeFocusOnTab: true
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Books found")
                ScrollBar.vertical: ThinScrollBar {}

                Keys.onReturnPressed: page.read(currentIndex)
                Keys.onEnterPressed: page.read(currentIndex)
                Keys.onUpPressed: function(event) {
                    if (currentIndex <= 0)
                        field.forceActiveFocus()
                    else
                        decrementCurrentIndex()
                }

                delegate: Rectangle {
                    id: hit

                    required property var modelData
                    required property int index

                    objectName: "textHit" + index
                    width: ListView.view.width
                    implicitHeight: texts.implicitHeight + 2 * Theme.gutter
                    radius: Theme.smallRadius
                    color: ListView.isCurrentItem && results.activeFocus ? Theme.selected
                        : hover.hovered ? Theme.hover : "transparent"
                    Accessible.role: Accessible.ListItem
                    Accessible.name: hit.modelData.title + ", " + hit.modelData.snippet.replace(/\x1d/g, "").replace(/\s+/g, " ")
                    Accessible.selected: ListView.isCurrentItem

                    HoverHandler {
                        id: hover
                    }

                    TapHandler {
                        onTapped: {
                            results.currentIndex = hit.index
                            results.forceActiveFocus()
                        }
                        onDoubleTapped: page.read(hit.index)
                    }

                    ColumnLayout {
                        id: texts

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: Theme.gutter
                        spacing: 2

                        Label {
                            Layout.fillWidth: true
                            text: hit.modelData.title + (hit.modelData.authors ? "  ·  " + hit.modelData.authors : "")
                                + "  ·  " + hit.modelData.format
                            font.weight: Font.Bold
                            elide: Text.ElideRight
                        }

                        Label {
                            Layout.fillWidth: true
                            text: page.marked(hit.modelData.snippet)
                            textFormat: Text.StyledText
                            color: Theme.textMuted
                            wrapMode: Text.Wrap
                            maximumLineCount: 3
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            Label {
                objectName: "textNothing"

                anchors.centerIn: parent
                width: parent.width
                visible: page.query.trim() !== "" && page.rows.length === 0 && !page.error && page.index.exists === true
                text: qsTr("No book's text has “%1”.").arg(page.query.trim())
                color: Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }
        }
    }
}
