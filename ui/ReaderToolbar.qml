pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// The reader's toolbar: the contents, Back, the book's title, reading aloud,
// the bookmark, the text and page settings, full screen, and closing. In full
// screen it slides away while reading.
Rectangle {
    id: readerToolbar

    required property var readerWindow
    required property var settingsPopup
    // The mouse over the toolbar, which keeps it shown in full screen.
    readonly property bool hovered: barHover.hovered

    z: 2
    y: readerToolbar.readerWindow.chromeShown ? 0 : -height
    height: Theme.toolbarHeight
    color: readerToolbar.readerWindow.fullScreen ? Theme.canvas : Theme.surface

    Behavior on y { NumberAnimation { duration: 120 } }

    HoverHandler {
        id: barHover
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.gutter
        anchors.rightMargin: Theme.gutter
        spacing: Theme.gutter

        FlatButton {
            objectName: "readerTocButton"

            iconOnly: true
            text: qsTr("Table of contents (T)")
            icon.source: "assets/icons/list.svg"
            selected: readerToolbar.readerWindow.tocVisible
            onClicked: readerToolbar.readerWindow.tocVisible = !readerToolbar.readerWindow.tocVisible
        }

        FlatButton {
            objectName: "readerBackButton"

            visible: readerToolbar.readerWindow.backStack.length > 0
            variant: "outline"
            text: qsTr("Back")
            tip: qsTr("Back where you were (Alt+Left)")
            icon.source: "assets/icons/arrow-left.svg"
            onClicked: readerToolbar.readerWindow.goBack()
        }

        Label {
            objectName: "readerTitle"

            Layout.fillWidth: true
            text: readerToolbar.readerWindow.arrival || readerToolbar.readerWindow.reader.title
            elide: Text.ElideRight
            color: readerToolbar.readerWindow.arrival ? Theme.accent : Theme.textMuted
        }

        FlatButton {
            objectName: "readerSpeechButton"

            visible: readerToolbar.readerWindow.speech !== null && readerToolbar.readerWindow.speech.engines.length > 0
            enabled: readerToolbar.readerWindow.spineIndex >= 0
            iconOnly: true
            selected: readerToolbar.readerWindow.speaking
            text: readerToolbar.readerWindow.speaking ? qsTr("Stop reading aloud (R)") : qsTr("Read aloud from here (R)")
            icon.source: "assets/icons/speaker.svg"
            onClicked: readerToolbar.readerWindow.toggleSpeech()
        }

        FlatButton {
            objectName: "readerBookmarkButton"

            iconOnly: true
            enabled: readerToolbar.readerWindow.spineIndex >= 0
            text: readerToolbar.readerWindow.pageBookmarks.length ? qsTr("Remove the page's bookmark (Ctrl+D)")
                                                    : qsTr("Bookmark the page (Ctrl+D)")
            icon.source: readerToolbar.readerWindow.pageBookmarks.length ? "assets/icons/bookmark-filled.svg"
                                                           : "assets/icons/bookmark.svg"
            onClicked: readerToolbar.readerWindow.toggleBookmark()
        }

        FlatButton {
            id: settingsButton
            objectName: "readerSettingsButton"

            iconOnly: true
            text: qsTr("Text and page")
            icon.source: "assets/icons/type.svg"
            selected: readerToolbar.settingsPopup.opened
            onClicked: readerToolbar.settingsPopup.opened ? readerToolbar.settingsPopup.close() : readerToolbar.settingsPopup.open()
        }

        FlatButton {
            objectName: "readerFullScreenButton"

            iconOnly: true
            text: readerToolbar.readerWindow.fullScreen ? qsTr("Leave full screen (F11)") : qsTr("Full screen (F11)")
            icon.source: "assets/icons/fullscreen.svg"
            selected: readerToolbar.readerWindow.fullScreen
            onClicked: readerToolbar.readerWindow.toggleFullScreen()
        }

        FlatButton {
            objectName: "readerAddToLibrary"

            // A file of no library, which it can join.
            visible: readerToolbar.readerWindow.reader.looseFile !== "" && readerToolbar.readerWindow.library.writable
            variant: "outline"
            text: qsTr("Add to library")
            icon.source: "assets/icons/plus.svg"
            onClicked: {
                const error = readerToolbar.readerWindow.library.addBooks([readerToolbar.readerWindow.reader.looseFile])
                if (error)
                    readerToolbar.readerWindow.failed(error)
                else
                    readerToolbar.readerWindow.added(readerToolbar.readerWindow.reader.title)
            }
        }

        FlatButton {
            iconOnly: true
            text: qsTr("Close the book (Esc)")
            icon.source: "assets/icons/close.svg"
            onClicked: readerToolbar.readerWindow.close()
        }
    }

    Divider {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }
}
