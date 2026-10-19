pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window

import Reliure

// A book: its cover, title, authors and description, beside the details
// pane with the rest of its metadata and its files. While the library may be
// written, an image dropped on the cover replaces it, and the Cover menu
// picks, pastes or removes one.
Rectangle {
    id: page

    required property BookLibrary library
    property var book: ({})
    property bool detailsVisible: true
    readonly property int coverWidth: Math.round(180 * Theme.spacingScale)
    // What the details pane would say, in one line while it is hidden.
    readonly property var facts: [
        book.published || "",
        (book.languages || []).join(", "),
        (book.formats || []).map(format => format.format).join(", ")
    ].filter(fact => fact.length > 0)

    // The toolbar asks before undoing every change since the book opened.
    property bool confirmingRevert: false
    readonly property bool editing: details.editing
    readonly property var coverActions: [
        { key: "choose", title: qsTr("Choose image…") },
        { key: "paste", title: qsTr("Paste image") },
        { key: "remove", title: qsTr("Remove cover") }
    ].filter(action => action.key !== "remove" || page.book.hasCover)

    signal failed(string message)

    color: Theme.canvas

    function setCover(url) {
        const error = page.library.setCover(page.book.id, url.toString())
        if (error)
            page.failed(error)
    }

    function pasteCover() {
        const error = page.library.pasteCover(page.book.id)
        if (error)
            page.failed(error)
    }

    function runCoverAction(key) {
        if (key === "choose") {
            coverDialog.open()
            return
        }
        const error = key === "paste" ? page.library.pasteCover(page.book.id) : page.library.removeCover(page.book.id)
        if (error)
            page.failed(error)
    }

    function finishEditing() {
        details.finishEditing()
    }

    function revertChanges() {
        page.confirmingRevert = false
        details.finishEditing()
        page.library.revertCurrentBook()
        page.forceActiveFocus()
    }

    onBookChanged: {
        if (!page.library.canRevert)
            confirmingRevert = false
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        ScrollView {
            id: bookScroll

            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                id: bookContent

                x: Theme.pagePadding
                width: bookScroll.availableWidth - 2 * Theme.pagePadding
                spacing: Theme.pagePadding

                Item { Layout.preferredHeight: 0 }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.pagePadding

                    ColumnLayout {
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: page.coverWidth
                        spacing: Theme.gutter

                        Rectangle {
                            objectName: "bookCover"

                            Layout.preferredWidth: page.coverWidth
                            Layout.preferredHeight: cover.status === Image.Ready && cover.implicitWidth > 0
                                ? Math.round(page.coverWidth * cover.implicitHeight / cover.implicitWidth) + 2
                                : Math.round(page.coverWidth * 1.5)
                            color: coverDrop.containsDrag ? Theme.hover : Theme.fill
                            border.width: 1
                            border.color: coverDrop.containsDrag ? Theme.accent : Theme.divider

                            DropArea {
                                id: coverDrop

                                anchors.fill: parent
                                enabled: page.library.writable
                                onEntered: function(drag) {
                                    drag.accepted = drag.hasUrls
                                }
                                onDropped: function(drop) {
                                    if (drop.hasUrls)
                                        page.setCover(drop.urls[0])
                                }
                            }

                            Image {
                                id: cover
                                objectName: "bookCoverImage"

                                anchors.fill: parent
                                anchors.margins: 1
                                visible: status === Image.Ready
                                source: page.book.cover || ""
                                sourceSize.width: Math.round(page.coverWidth * 2 * Screen.devicePixelRatio)
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                            }

                            Label {
                                anchors.centerIn: parent
                                width: parent.width - 2 * Theme.gutter
                                visible: cover.status !== Image.Ready
                                text: page.library.writable ? qsTr("No cover\nDrop an image here") : qsTr("No cover")
                                color: Theme.textFaint
                                font.pointSize: Theme.fontPointSizeCaption
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.Wrap
                            }
                        }

                        FlatButton {
                            id: coverButton
                            objectName: "coverButton"

                            visible: page.library.writable
                            variant: "outline"
                            text: qsTr("Cover")
                            icon.source: "assets/icons/image.svg"
                            onClicked: coverMenu.popup(coverButton, 0, coverButton.height)
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: Math.round(6 * Theme.spacingScale)

                        Label {
                            objectName: "bookTitle"

                            Layout.fillWidth: true
                            text: page.book.title || ""
                            color: Theme.textBright
                            font.pointSize: Theme.fontPointSizeTitle
                            font.weight: Font.Bold
                            wrapMode: Text.Wrap
                        }

                        Label {
                            objectName: "bookAuthors"

                            Layout.fillWidth: true
                            text: (page.book.authors || []).join(" & ")
                            wrapMode: Text.Wrap
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: page.book.seriesText || ""
                            color: Theme.textMuted
                            wrapMode: Text.Wrap
                        }

                        Label {
                            objectName: "bookRating"

                            visible: text.length > 0
                            text: page.book.rating || ""
                            color: Theme.textMuted
                        }

                        Label {
                            objectName: "factsLine"

                            Layout.fillWidth: true
                            Layout.topMargin: Theme.gutter
                            visible: !page.detailsVisible && page.facts.length > 0
                            text: page.facts.join("  ·  ")
                            color: Theme.textMuted
                            wrapMode: Text.Wrap
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.gutter

                    HeadingLabel {
                        text: qsTr("Description")
                    }

                    Label {
                        objectName: "bookDescription"

                        Layout.fillWidth: true
                        text: page.book.description || qsTr("No description.")
                        textFormat: page.book.description ? Text.RichText : Text.PlainText
                        color: page.book.description ? Theme.text : Theme.textMuted
                        linkColor: Theme.accent
                        wrapMode: Text.Wrap
                        onLinkActivated: function(link) { Qt.openUrlExternally(link) }

                        HoverHandler {
                            cursorShape: parent.hoveredLink ? Qt.PointingHandCursor : Qt.ArrowCursor
                        }
                    }
                }

                Item { Layout.preferredHeight: Theme.pagePadding }
            }
        }

        Divider {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            visible: details.visible
        }

        BookDetailsPane {
            id: details
            objectName: "detailsPane"

            Layout.preferredWidth: Theme.paneWidth
            Layout.fillHeight: true
            visible: page.detailsVisible
            library: page.library
            book: page.book
            onFailed: function(message) { page.failed(message) }
        }
    }

    ActionMenu {
        id: coverMenu
        objectName: "coverMenu"

        actions: page.coverActions
        onChosen: function(key) { page.runCoverAction(key) }
    }

    FileDialog {
        id: coverDialog

        title: qsTr("Choose a cover")
        nameFilters: [qsTr("Images (*.jpg *.jpeg *.png *.webp *.gif *.bmp)")]
        onAccepted: page.setCover(selectedFile)
    }
}
