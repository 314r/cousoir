pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Reliure

// The library's trash, as Calibre keeps it: books removed whole, and files
// removed from books still there. Each comes back with Restore; Calibre
// deletes them for good after some days, which Reliure never does.
Rectangle {
    id: page

    required property BookLibrary library

    signal failed(string message)

    color: Theme.canvas

    function focusList() {
        trashList.forceActiveFocus()
    }

    function plural(count, one, many) {
        return (count === 1 ? one : many).arg(count)
    }

    function restore(entry) {
        const error = page.library.restoreFromTrash(entry.bookId, entry.kind === "formats" ? entry.formats : [])
        if (error)
            page.failed(error)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Label {
            objectName: "trashExpiry"

            Layout.fillWidth: true
            leftPadding: Theme.pagePadding
            rightPadding: Theme.pagePadding
            topPadding: Theme.gutter
            bottomPadding: Theme.gutter
            text: page.plural(page.library.trashExpiryDays, qsTr("Calibre deletes books from the trash after %1 day."),
                              qsTr("Calibre deletes books from the trash after %1 days."))
            color: Theme.textMuted
            wrapMode: Text.Wrap
        }

        Divider {
            Layout.fillWidth: true
        }

        ListView {
            id: trashList
            objectName: "trashList"

            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            focus: true
            keyNavigationEnabled: true
            model: page.library.trashEntries
            boundsBehavior: Flickable.StopAtBounds
            Accessible.name: qsTr("Trash")

            ScrollBar.vertical: ThinScrollBar { }

            // A row stays current as entries come and go.
            onCountChanged: {
                if (count > 0 && (currentIndex < 0 || currentIndex >= count))
                    currentIndex = Math.min(Math.max(currentIndex, 0), count - 1)
            }

            Keys.onReturnPressed: {
                if (currentItem && currentItem.restorable)
                    page.restore(page.library.trashEntries[currentIndex])
            }

            delegate: Rectangle {
                id: entry

                required property var modelData
                required property int index
                readonly property bool restorable: modelData.restorable
                readonly property int thumbWidth: Math.round(Theme.rowHeight * 1.4)

                objectName: "trash_" + modelData.key
                width: ListView.view.width
                implicitHeight: row.implicitHeight + 2 * Theme.gutter
                color: ListView.isCurrentItem && trashList.activeFocus ? Theme.selected
                    : hover.hovered ? Theme.hover : "transparent"

                HoverHandler {
                    id: hover
                }

                TapHandler {
                    onTapped: trashList.currentIndex = entry.index
                }

                RowLayout {
                    id: row

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.pagePadding
                    anchors.rightMargin: Theme.pagePadding
                    spacing: Theme.gutter

                    Rectangle {
                        Layout.preferredWidth: entry.thumbWidth
                        Layout.preferredHeight: Math.round(entry.thumbWidth * 1.5)
                        color: Theme.fill
                        border.width: 1
                        border.color: Theme.divider

                        Image {
                            anchors.fill: parent
                            anchors.margins: 1
                            visible: entry.modelData.cover.length > 0
                            source: entry.modelData.cover
                            sourceSize.width: entry.thumbWidth * 2
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            cache: false
                        }

                        Image {
                            anchors.centerIn: parent
                            visible: entry.modelData.cover.length === 0
                            width: Theme.iconSize
                            height: Theme.iconSize
                            source: entry.modelData.kind === "book" ? "assets/icons/book.svg" : "assets/icons/folder.svg"
                            sourceSize.width: Theme.iconSize
                            sourceSize.height: Theme.iconSize
                            opacity: 0.5
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            Layout.fillWidth: true
                            text: entry.modelData.title
                            font.weight: Font.Bold
                            elide: Text.ElideRight
                        }

                        Label {
                            Layout.fillWidth: true
                            text: entry.modelData.author
                            color: Theme.textMuted
                            elide: Text.ElideRight
                        }

                        Label {
                            Layout.fillWidth: true
                            text: (entry.modelData.kind === "formats"
                                    ? qsTr("%1 removed on %2").arg(entry.modelData.formats.join(", "))
                                        .arg(entry.modelData.removed)
                                    : qsTr("Removed on %1").arg(entry.modelData.removed))
                                + " · " + page.plural(entry.modelData.daysLeft, qsTr("%1 day left"), qsTr("%1 days left"))
                            color: Theme.textFaint
                            font.pointSize: Theme.fontPointSizeCaption
                            elide: Text.ElideRight
                        }
                    }

                    FlatButton {
                        objectName: "restore_" + entry.modelData.key

                        variant: "outline"
                        enabled: entry.restorable && page.library.writable
                        text: qsTr("Restore")
                        tip: !entry.restorable
                            ? (entry.modelData.kind === "book"
                                ? qsTr("The library already has a book with this one's number")
                                : qsTr("The book of these files is no longer in the library"))
                            : ""
                        onClicked: page.restore(entry.modelData)
                    }
                }
            }
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * Theme.panePadding, Math.round(420 * Theme.spacingScale))
        visible: page.library.trashEntries.length === 0
        spacing: Theme.gutter

        Label {
            objectName: "trashEmpty"

            Layout.fillWidth: true
            text: qsTr("The trash is empty")
            font.weight: Font.Bold
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Books and files you remove go here, in Reliure as in Calibre.")
            color: Theme.textMuted
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
    }
}
