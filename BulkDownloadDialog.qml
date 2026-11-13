pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Controls.impl
import QtQuick.Layouts

import Cousoir

// Download metadata for the books selected: first whether covers come too,
// then each book as it is looked up, with what its best match changes and
// the cover found, ticked to take it. Apply saves the books ticked as one
// change, which Undo puts back. Space ticks the book it is on, Enter
// applies or presses the button it is on; Esc closes and stops the
// downloads.
Popup {
    id: dialog

    required property BulkMetadataDownloader downloader
    property var bookIds: []
    // "setup", then "books".
    property string page: "setup"
    property bool withCovers: true
    readonly property bool done: downloader.state === "done"

    signal failed(string message)
    signal saved(int count)

    function start(ids) {
        dialog.bookIds = ids
        dialog.page = "setup"
        open()
    }

    function download() {
        const error = dialog.downloader.start(dialog.bookIds, dialog.withCovers)
        if (error) {
            dialog.failed(error)
            close()
            return
        }
        dialog.page = "books"
        bookList.currentIndex = 0
        bookList.forceActiveFocus()
    }

    function apply() {
        if (!dialog.done || dialog.downloader.chosenCount === 0)
            return
        const count = dialog.downloader.chosenCount
        const error = dialog.downloader.apply()
        if (error) {
            dialog.failed(error)
            return
        }
        close()
        dialog.saved(count)
    }

    function pressEnter() {
        const item = dialog.contentItem.Window.activeFocusItem
        if (item && item.clicked !== undefined && item !== bookList)
            item.clicked()
        else if (dialog.page === "setup")
            dialog.download()
        else
            dialog.apply()
    }

    function stateText(row) {
        if (row.state === "waiting")
            return qsTr("Waiting")
        if (row.state === "searching")
            return qsTr("Looking it up…")
        if (row.state === "covers")
            return qsTr("Looking for covers…")
        if (row.state === "unchanged")
            return qsTr("Nothing new found")
        if (row.state === "failed")
            return row.error
        return ""
    }

    objectName: "bulkDownloadDialog"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - 2 * Theme.pagePadding : 0, Math.round(860 * Theme.spacingScale))
    height: Math.min(parent ? parent.height - 2 * Theme.pagePadding : 0, Math.round(620 * Theme.spacingScale))
    modal: true
    focus: true
    padding: Theme.panePadding
    closePolicy: Popup.CloseOnEscape
    onOpened: {
        if (dialog.page === "setup")
            downloadButton.forceActiveFocus()
    }
    onClosed: dialog.downloader.cancel()

    Overlay.modal: Rectangle {
        color: Qt.rgba(0, 0, 0, 0.35)
    }

    background: Rectangle {
        radius: Theme.radius
        color: Theme.canvas
        border.width: 1
        border.color: Theme.controlBorder
    }

    // A box ticked or not.
    component Tick: Rectangle {
        property bool checked: false

        radius: Theme.smallRadius
        implicitWidth: Math.round(Theme.iconSize * 1.1)
        implicitHeight: implicitWidth
        color: checked ? Theme.accent : "transparent"
        border.width: 1
        border.color: checked ? Theme.accent : Theme.controlBorder

        IconImage {
            anchors.centerIn: parent
            width: Theme.smallIconSize
            height: Theme.smallIconSize
            sourceSize: Qt.size(Theme.smallIconSize, Theme.smallIconSize)
            visible: parent.checked
            source: "assets/icons/check.svg"
            color: Theme.canvas
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.gutter

        Keys.onReturnPressed: dialog.pressEnter()
        Keys.onEnterPressed: dialog.pressEnter()

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gutter

            IconImage {
                Layout.preferredWidth: Theme.iconSize
                Layout.preferredHeight: Theme.iconSize
                sourceSize: Qt.size(Theme.iconSize, Theme.iconSize)
                source: "assets/icons/globe.svg"
                color: Theme.textBright
            }

            Label {
                objectName: "bulkDownloadTitle"

                Layout.fillWidth: true
                text: qsTr("Metadata for %n books", "", dialog.bookIds.length)
                color: Theme.textBright
                font.weight: Font.Bold
                elide: Text.ElideRight
            }

            Label {
                text: dialog.downloader.source === "openlibrary" ? qsTr("from Open Library")
                                                                  : qsTr("from Calibre's sources")
                color: Theme.textFaint
                font.pointSize: Theme.fontPointSizeCaption
            }
        }

        Divider {
            Layout.fillWidth: true
        }

        // What will happen, and whether covers come too.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: dialog.page === "setup"
            spacing: Theme.gutter

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Each book is looked up by its title, authors and identifiers, and its best match is taken, as Calibre's bulk download does: the fields that differ, but those Calibre is set to leave aside. You then see what changes for each book before anything is saved.")
            }

            RowLayout {
                spacing: Theme.gutter

                Tick {
                    objectName: "bulkDownloadCovers"

                    checked: dialog.withCovers
                    activeFocusOnTab: true
                    border.color: activeFocus ? Theme.accent : (checked ? Theme.accent : Theme.controlBorder)
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: qsTr("Covers too")
                    Accessible.checked: checked
                    Keys.onSpacePressed: dialog.withCovers = !dialog.withCovers

                    MouseArea {
                        anchors.fill: parent
                        onClicked: dialog.withCovers = !dialog.withCovers
                    }
                }

                Label {
                    text: qsTr("Covers too, the largest found")

                    MouseArea {
                        anchors.fill: parent
                        onClicked: dialog.withCovers = !dialog.withCovers
                    }
                }
            }

            Item { Layout.fillHeight: true }
        }

        // The books, as they are looked up.
        ListView {
            id: bookList
            objectName: "bulkDownloadBooks"

            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: dialog.page === "books"
            clip: true
            model: dialog.downloader.rows
            spacing: Theme.gutter
            boundsBehavior: Flickable.StopAtBounds
            highlightMoveDuration: 0
            keyNavigationEnabled: true
            activeFocusOnTab: true
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Books looked up")
            ScrollBar.vertical: ThinScrollBar {}
            Keys.onSpacePressed: {
                const row = dialog.downloader.rows[currentIndex]
                if (row)
                    dialog.downloader.setChosen(currentIndex, !row.chosen)
            }

            delegate: Rectangle {
                id: bookRow

                required property var modelData
                required property int index
                readonly property bool takeable: modelData.state === "found"

                objectName: "bulkDownloadBook" + index
                width: bookList.width
                implicitHeight: Math.max(rowColumn.implicitHeight, coverImage.visible ? coverImage.height : 0)
                    + 2 * Theme.gutter
                radius: Theme.smallRadius
                color: ListView.isCurrentItem && bookList.activeFocus ? Theme.selected
                    : rowMouse.containsMouse ? Theme.hover : "transparent"
                Accessible.role: Accessible.CheckBox
                Accessible.name: modelData.title
                Accessible.description: dialog.stateText(modelData)
                Accessible.checked: modelData.chosen

                MouseArea {
                    id: rowMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        bookList.currentIndex = bookRow.index
                        bookList.forceActiveFocus()
                        if (bookRow.takeable)
                            dialog.downloader.setChosen(bookRow.index, !bookRow.modelData.chosen)
                    }
                }

                Tick {
                    id: rowTick
                    objectName: "bulkDownloadTick" + bookRow.index

                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: Theme.gutter
                    checked: bookRow.modelData.chosen
                    opacity: bookRow.takeable ? 1 : 0.35
                }

                ColumnLayout {
                    id: rowColumn

                    anchors.left: rowTick.right
                    anchors.right: coverImage.visible ? coverImage.left : parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.gutter
                    spacing: 2

                    Label {
                        Layout.fillWidth: true
                        text: bookRow.modelData.title + (bookRow.modelData.authors ? "  ·  " + bookRow.modelData.authors : "")
                        font.weight: Font.Bold
                        elide: Text.ElideRight
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: dialog.stateText(bookRow.modelData)
                        color: bookRow.modelData.state === "failed" ? Theme.danger : Theme.textMuted
                        font.pointSize: Theme.fontPointSizeCaption
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                    }

                    Repeater {
                        model: bookRow.modelData.fields

                        delegate: Label {
                            required property var modelData

                            Layout.fillWidth: true
                            text: qsTr("%1: %2 → %3").arg(modelData.label)
                                .arg(modelData.current || "—").arg(modelData.found || "—")
                            color: Theme.textMuted
                            font.pointSize: Theme.fontPointSizeCaption
                            elide: Text.ElideRight
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: bookRow.modelData.cover !== ""
                        text: qsTr("Cover: the one found")
                        color: Theme.textMuted
                        font.pointSize: Theme.fontPointSizeCaption
                    }
                }

                Image {
                    id: coverImage

                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.gutter
                    visible: bookRow.modelData.cover !== ""
                    width: Math.round(Theme.iconSize * 3)
                    height: Math.round(width * 1.5)
                    source: bookRow.modelData.cover
                    sourceSize.width: width
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                }
            }
        }

        Divider {
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gutter

            Label {
                objectName: "bulkDownloadStatus"

                Layout.fillWidth: true
                visible: dialog.page === "books"
                text: dialog.done ? qsTr("%n books to update", "", dialog.downloader.chosenCount)
                                  : qsTr("Looked up: %1 of %2").arg(dialog.downloader.done).arg(dialog.downloader.total)
                color: Theme.textMuted
                font.pointSize: Theme.fontPointSizeCaption
                elide: Text.ElideRight
            }

            Item {
                Layout.fillWidth: true
                visible: dialog.page === "setup"
            }

            FlatButton {
                objectName: "bulkDownloadCancel"

                variant: "outline"
                text: qsTr("Cancel")
                onClicked: dialog.close()
            }

            FlatButton {
                id: downloadButton
                objectName: "bulkDownloadStart"

                visible: dialog.page === "setup"
                variant: "primary"
                text: qsTr("Download")
                onClicked: dialog.download()
            }

            FlatButton {
                objectName: "bulkDownloadApply"

                visible: dialog.page === "books"
                enabled: dialog.done && dialog.downloader.chosenCount > 0
                variant: "primary"
                text: qsTr("Apply")
                onClicked: dialog.apply()
            }
        }
    }
}
