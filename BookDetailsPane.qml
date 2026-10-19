pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Reliure

// A book's metadata as Calibre keeps it, and its files, each of which opens
// in the desktop's reader for it.
FocusScope {
    id: pane

    required property BookLibrary library
    property var book: ({})
    readonly property var facts: [
        { key: "title", label: qsTr("Title"), value: book.title || "" },
        // The sorts only when they differ from what they sort.
        { key: "sort", label: qsTr("Title sort"), value: book.sort !== book.title ? book.sort || "" : "" },
        { key: "authors", label: qsTr("Authors"), value: (book.authors || []).join(" & ") },
        { key: "authorSort", label: qsTr("Author sort"),
          value: book.authorSort !== (book.authors || []).join(" & ") ? book.authorSort || "" : "" },
        { key: "series", label: qsTr("Series"), value: book.seriesText || "" },
        { key: "tags", label: qsTr("Tags"), value: (book.tags || []).join(", ") },
        { key: "rating", label: qsTr("Rating"), value: book.rating || "" },
        { key: "publisher", label: qsTr("Publisher"), value: book.publisher || "" },
        { key: "languages", label: qsTr("Languages"), value: (book.languages || []).join(", ") },
        { key: "published", label: qsTr("Published"), value: book.published || "" },
        { key: "added", label: qsTr("Added"), value: book.added || "" },
        { key: "modified", label: qsTr("Modified"), value: book.modified || "" }
    ]

    signal failed(string message)

    function open(format) {
        const error = pane.library.openFormat(pane.book.id, format)
        if (error)
            pane.failed(error)
    }

    ScrollView {
        id: paneScroll

        anchors.fill: parent
        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            x: Theme.panePadding
            width: paneScroll.availableWidth - 2 * Theme.panePadding
            spacing: Theme.rowPadding

            Item { Layout.preferredHeight: Theme.pagePadding - Theme.rowPadding }

            HeadingLabel {
                Layout.fillWidth: true
                text: qsTr("Book")
            }

            ColumnLayout {
                objectName: "bookFacts"

                Layout.fillWidth: true
                spacing: Math.round(6 * Theme.spacingScale)

                Repeater {
                    model: pane.facts

                    delegate: MetaRow {
                        required property var modelData

                        objectName: "fact_" + modelData.key
                        visible: modelData.value.length > 0
                        label: modelData.label
                        value: modelData.value
                    }
                }

                Repeater {
                    model: pane.book.identifiers || []

                    delegate: MetaRow {
                        required property var modelData

                        objectName: "identifier_" + modelData.type
                        label: modelData.label
                        value: modelData.value
                        link: modelData.url
                    }
                }
            }

            HeadingLabel {
                Layout.fillWidth: true
                Layout.topMargin: Theme.gutter
                text: qsTr("Files")
            }

            Label {
                Layout.fillWidth: true
                visible: (pane.book.formats || []).length === 0
                text: qsTr("This book has no file.")
                color: Theme.textMuted
                wrapMode: Text.Wrap
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Repeater {
                    model: pane.book.formats || []

                    delegate: RowLayout {
                        id: formatRow

                        required property var modelData

                        Layout.fillWidth: true
                        spacing: Theme.gutter

                        FlatButton {
                            objectName: "format_" + formatRow.modelData.format

                            enabled: !formatRow.modelData.missing
                            text: formatRow.modelData.format
                            tip: qsTr("Open in reader")
                            icon.source: "assets/icons/external.svg"
                            onClicked: pane.open(formatRow.modelData.format)
                        }

                        Label {
                            Layout.fillWidth: true
                            text: formatRow.modelData.missing ? qsTr("File missing") : formatRow.modelData.size
                            color: formatRow.modelData.missing ? Theme.danger : Theme.textMuted
                            font.pointSize: Theme.fontPointSizeCaption
                            horizontalAlignment: Text.AlignRight
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            FlatButton {
                objectName: "showFolderButton"

                variant: "outline"
                text: qsTr("Show in folder")
                icon.source: "assets/icons/folder.svg"
                onClicked: {
                    const error = pane.library.openFolder(pane.book.id)
                    if (error)
                        pane.failed(error)
                }
            }

            Label {
                objectName: "bookPath"

                Layout.fillWidth: true
                text: pane.book.path || ""
                color: Theme.textFaint
                font.pointSize: Theme.fontPointSizeCaption
                wrapMode: Text.WrapAnywhere
            }

            Item { Layout.preferredHeight: Theme.panePadding }
        }
    }
}
