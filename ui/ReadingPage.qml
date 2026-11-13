pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// Continue reading: the books read, the last read first, each with how far
// it was read and when, from the places their readers keep in the library,
// Cousoir's, Calibre's viewer's and a Kobo's. Enter reads the book where it
// was left.
Rectangle {
    id: page

    required property BookLibrary library
    property var rows: []

    signal readRequested(int bookId)

    color: Theme.surface

    function refresh() {
        const current = list.currentIndex
        page.rows = page.library.readingRows()
        list.currentIndex = Math.min(Math.max(current, 0), page.rows.length - 1)
    }

    function focusList() {
        list.forceActiveFocus()
    }

    function read(index) {
        const row = page.rows[index]
        if (row)
            page.readRequested(row.bookId)
    }

    onVisibleChanged: {
        if (visible)
            refresh()
    }

    Connections {
        target: page.library

        function onLibraryChanged() {
            if (page.visible)
                page.refresh()
        }

        function onSaved() {
            if (page.visible)
                page.refresh()
        }
    }

    ListView {
        id: list
        objectName: "readingList"

        anchors.fill: parent
        anchors.margins: Theme.pagePadding
        clip: true
        model: page.rows
        spacing: Theme.gutter
        keyNavigationEnabled: true
        boundsBehavior: Flickable.StopAtBounds
        highlightMoveDuration: 0
        activeFocusOnTab: true
        Accessible.role: Accessible.List
        Accessible.name: qsTr("Books read")
        ScrollBar.vertical: ThinScrollBar {}

        Keys.onReturnPressed: page.read(currentIndex)
        Keys.onEnterPressed: page.read(currentIndex)

        delegate: ListRow {
            id: book

            required property var modelData
            required property int index

            objectName: "readingBook" + index
            implicitHeight: Math.max(cover.height, texts.implicitHeight) + 2 * Theme.gutter
            radius: Theme.smallRadius
            Accessible.name: book.modelData.title + ", " + book.statusText

            readonly property string statusText: (book.modelData.finished ? qsTr("Finished")
                                                                           : qsTr("%1 read").arg(book.modelData.progress))
                + " · " + book.modelData.when

            TapHandler {
                onTapped: {
                    list.currentIndex = book.index
                    list.forceActiveFocus()
                }
                onDoubleTapped: page.read(book.index)
            }

            Rectangle {
                id: cover

                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.gutter
                width: Math.round(Theme.iconSize * 2.6)
                height: Math.round(width * 1.5)
                radius: Theme.smallRadius
                color: Theme.fill
                clip: true

                Image {
                    anchors.fill: parent
                    visible: book.modelData.cover !== ""
                    source: book.modelData.cover
                    sourceSize.width: width
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }
            }

            ColumnLayout {
                id: texts

                anchors.left: cover.right
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.columnGap
                anchors.rightMargin: Theme.gutter
                spacing: 4

                Label {
                    Layout.fillWidth: true
                    text: book.modelData.title
                    font.weight: Font.Bold
                    elide: Text.ElideRight
                }

                Label {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: book.modelData.authors
                    color: Theme.textMuted
                    elide: Text.ElideRight
                }

                // How far, as a bar.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.maximumWidth: Math.round(320 * Theme.spacingScale)
                    Layout.topMargin: 2
                    implicitHeight: 4
                    radius: 2
                    color: Theme.divider

                    Rectangle {
                        width: Math.round(parent.width * book.modelData.fraction)
                        height: parent.height
                        radius: parent.radius
                        color: Theme.accent
                    }
                }

                Label {
                    objectName: "readingStatus" + book.index

                    Layout.fillWidth: true
                    text: book.statusText
                    color: Theme.textMuted
                    font.pointSize: Theme.fontPointSizeCaption
                    elide: Text.ElideRight
                }
            }
        }
    }

    Label {
        objectName: "readingEmpty"

        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * Theme.pagePadding, Math.round(520 * Theme.spacingScale))
        visible: page.rows.length === 0
        text: qsTr("No book has been read yet. A book read in Cousoir, in Calibre's viewer or on a Kobo shows here, where it was left.")
        color: Theme.textMuted
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
    }
}
