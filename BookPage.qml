pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Window

import Reliure

// A book: its cover, title, authors and description, beside the details
// pane with the rest of its metadata and its files.
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

    signal failed(string message)

    color: Theme.canvas

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

                    Rectangle {
                        objectName: "bookCover"

                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: page.coverWidth
                        Layout.preferredHeight: cover.status === Image.Ready && cover.implicitWidth > 0
                            ? Math.round(page.coverWidth * cover.implicitHeight / cover.implicitWidth) + 2
                            : Math.round(page.coverWidth * 1.5)
                        color: Theme.fill
                        border.width: 1
                        border.color: Theme.divider

                        Image {
                            id: cover

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
                            visible: cover.status !== Image.Ready
                            text: qsTr("No cover")
                            color: Theme.textFaint
                            font.pointSize: Theme.fontPointSizeCaption
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
}
