pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// A note of the book, a footnote or an endnote, shown over the page.
Popup {
    id: noteBox
    objectName: "readerNoteBox"

    required property var readerWindow
    required property var readerSettings
    required property var view
    required property var pageArea

    property string href: ""
    property string html: ""
    // Where the link is, in the page's area.
    property var anchorRect: null

    // By the link: below it, else above it, within the page.
    function place() {
        const origin = noteBox.pageArea.mapToItem(null, 0, 0)
        if (!anchorRect) {
            x = Math.round((noteBox.readerWindow.width - width) / 2)
            y = Math.round((noteBox.readerWindow.height - height) / 3)
            return
        }
        x = Math.round(Math.max(origin.x + Theme.gutter,
                                Math.min(origin.x + noteBox.pageArea.width - width - Theme.gutter,
                                         origin.x + anchorRect.x + anchorRect.width / 2 - width / 2)))
        const below = origin.y + anchorRect.y + anchorRect.height + Theme.gutter
        y = Math.round(below + height < noteBox.readerWindow.height ? below
                                                             : Math.max(Theme.gutter, origin.y + anchorRect.y - height - Theme.gutter))
    }

    parent: Overlay.overlay
    width: Math.min(noteBox.pageArea.width - 2 * Theme.gutter, Math.round(52 * Theme.charWidth))
    padding: Theme.panePadding
    onClosed: noteBox.view.forceActiveFocus()

    background: Rectangle {
        radius: Theme.radius
        color: noteBox.readerWindow.pageBackground
        border.width: 1
        border.color: Theme.mix(noteBox.readerWindow.pageBackground, noteBox.readerWindow.pageText, 0.3)
    }

    contentItem: ColumnLayout {
        spacing: Theme.gutter

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(noteBoxText.implicitHeight, Math.round(noteBox.readerWindow.height * 0.4))
            contentWidth: availableWidth
            clip: true

            Label {
                id: noteBoxText
                objectName: "readerNoteBoxText"

                width: parent.width
                text: noteBox.html
                textFormat: Text.RichText
                wrapMode: Text.Wrap
                color: noteBox.readerWindow.pageText
                font.family: noteBox.readerSettings.fontFamily && noteBox.readerSettings.fontFamily !== "sans-serif"
                    ? noteBox.readerSettings.fontFamily : "serif"
                font.pointSize: Theme.fontPointSizeBody * 1.15 * noteBox.readerSettings.zoom
            }
        }

        RowLayout {
            Layout.fillWidth: true

            Item {
                Layout.fillWidth: true
            }

            FlatButton {
                objectName: "readerGoToNote"

                variant: "outline"
                text: qsTr("Go to the note")
                onClicked: {
                    noteBox.close()
                    noteBox.readerWindow.followLink(noteBox.href)
                }
            }
        }
    }
}
