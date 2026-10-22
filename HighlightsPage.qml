pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

import Cousoir

// Every highlight and bookmark the library keeps, from Calibre's viewer and
// from Cousoir's reader, as Calibre's "Browse annotations" shows them: by
// book, each book's in their order; a search of Calibre's full text index
// finds them, the words found in bold. Enter opens the book in the reader
// there; the export is Calibre's, as text or Markdown.
Rectangle {
    id: page

    required property BookLibrary library
    readonly property HighlightBrowser browser: HighlightBrowser {
        library: page.library
    }
    readonly property var highlightColors: ({ yellow: "#ffeb6b", green: "#c0ed72", blue: "#add8ff",
                                              red: "#ffb0ca", purple: "#d9b2ff" })

    signal readRequested(int bookId, int spineIndex, string cfi)
    signal bookRequested(int bookId)
    signal failed(string message)
    signal noticed(string message)

    color: Theme.canvas

    function focusList() {
        highlightList.forceActiveFocus()
    }

    function focusSearch() {
        searchField.forceActiveFocus(Qt.ShortcutFocusReason)
        searchField.selectAll()
    }

    function open(row) {
        if (row && row.kind === "annotation")
            page.readRequested(row.bookId, row.spineIndex, row.cfi)
        else if (row)
            page.bookRequested(row.bookId)
    }

    function copyExport(markdown) {
        page.browser.copy(page.browser.exportText(markdown))
        page.noticed(qsTr("Highlights copied"))
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.pagePadding
            Layout.rightMargin: Theme.pagePadding
            Layout.topMargin: Theme.gutter
            Layout.bottomMargin: Theme.gutter
            spacing: Theme.gutter

            SearchField {
                id: searchField
                objectName: "highlightSearch"

                Layout.fillWidth: true
                Layout.maximumWidth: Math.round(360 * Theme.spacingScale)
                placeholderText: qsTr("Search highlights, as in Calibre")
                text: page.browser.query
                onTextEdited: page.browser.query = text
                Keys.onDownPressed: page.focusList()
                Keys.onEscapePressed: {
                    if (text.length > 0)
                        page.browser.query = ""
                    else
                        page.focusList()
                }
            }

            ChoiceBox {
                id: typeBox
                objectName: "highlightType"

                readonly property var choices: [
                    { value: "", label: qsTr("Highlights and bookmarks") },
                    { value: "highlight", label: qsTr("Highlights") },
                    { value: "bookmark", label: qsTr("Bookmarks") }
                ]
                Layout.preferredWidth: Math.round(26 * Theme.charWidth) + leftPadding + rightPadding
                Accessible.name: qsTr("Kind")
                model: choices
                currentIndex: Math.max(0, choices.findIndex(choice => choice.value === page.browser.type))
                onActivated: function(index) { page.browser.type = typeBox.choices[index].value }
            }

            CheckBox {
                objectName: "highlightStemming"

                text: qsTr("Word stems")
                checked: page.browser.stemming
                onToggled: page.browser.stemming = checked
                ToolTip.visible: hovered
                ToolTip.text: qsTr("“read” also finds “reading”, in Calibre's interface language")
            }

            Item { Layout.fillWidth: true }

            FlatButton {
                objectName: "copyHighlights"

                enabled: page.browser.count > 0
                text: qsTr("Copy")
                tip: qsTr("Copy the highlights shown as Markdown")
                onClicked: page.copyExport(true)
            }

            FlatButton {
                objectName: "saveHighlights"

                enabled: page.browser.count > 0
                variant: "outline"
                text: qsTr("Export…")
                tip: qsTr("Save the highlights shown as Calibre exports them")
                onClicked: exportDialog.open()
            }
        }

        Label {
            objectName: "highlightError"

            Layout.fillWidth: true
            Layout.leftMargin: Theme.pagePadding
            Layout.rightMargin: Theme.pagePadding
            visible: page.browser.error !== ""
            text: page.browser.error
            color: Theme.danger
            wrapMode: Text.Wrap
        }

        Divider {
            Layout.fillWidth: true
        }

        ListView {
            id: highlightList
            objectName: "highlightList"

            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            focus: true
            keyNavigationEnabled: true
            model: page.browser.rows
            boundsBehavior: Flickable.StopAtBounds
            Accessible.name: qsTr("Highlights")

            ScrollBar.vertical: ThinScrollBar { }

            onCountChanged: {
                if (count > 0 && (currentIndex < 0 || currentIndex >= count))
                    currentIndex = Math.min(Math.max(currentIndex, 0), count - 1)
            }

            Keys.onReturnPressed: page.open(page.browser.rows[currentIndex])
            Keys.onEnterPressed: page.open(page.browser.rows[currentIndex])

            delegate: Rectangle {
                id: row

                required property var modelData
                required property int index
                readonly property bool isBook: modelData.kind === "book"

                objectName: isBook ? "highlightBook_" + modelData.bookId : "highlight_" + modelData.index
                width: ListView.view.width
                implicitHeight: content.implicitHeight + 2 * Theme.gutter
                color: ListView.isCurrentItem && highlightList.activeFocus ? Theme.selected
                    : hover.hovered ? Theme.hover : "transparent"

                HoverHandler {
                    id: hover
                }

                TapHandler {
                    onTapped: highlightList.currentIndex = row.index
                    onDoubleTapped: page.open(row.modelData)
                }

                RowLayout {
                    id: content

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.pagePadding
                    anchors.rightMargin: Theme.pagePadding
                    spacing: Theme.gutter

                    // The highlight's colour, as the reader shows it.
                    Rectangle {
                        Layout.preferredWidth: Math.round(Theme.gutter / 2)
                        Layout.fillHeight: true
                        visible: !row.isBook
                        color: row.modelData.type === "bookmark" ? Theme.divider
                            : (page.highlightColors[row.modelData.color] || page.highlightColors.yellow)
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            Layout.fillWidth: true
                            visible: row.isBook
                            text: row.isBook ? row.modelData.title : ""
                            font.weight: Font.Bold
                            elide: Text.ElideRight
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: row.isBook
                            text: row.isBook ? row.modelData.authors : ""
                            color: Theme.textMuted
                            elide: Text.ElideRight
                        }

                        Label {
                            objectName: "highlightText"

                            Layout.fillWidth: true
                            visible: !row.isBook
                            text: row.isBook ? "" : row.modelData.html
                            textFormat: Text.StyledText
                            wrapMode: Text.Wrap
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: !row.isBook && row.modelData.notes !== ""
                            text: row.isBook ? "" : row.modelData.notes
                            color: Theme.textMuted
                            font.italic: true
                            wrapMode: Text.Wrap
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: !row.isBook
                            text: row.isBook ? ""
                                : [row.modelData.type === "bookmark" ? qsTr("Bookmark") : "",
                                   row.modelData.chapter, row.modelData.date].filter(part => part).join(" · ")
                            color: Theme.textFaint
                            font.pointSize: Theme.fontPointSizeCaption
                            elide: Text.ElideRight
                        }
                    }

                    Label {
                        visible: row.isBook
                        text: row.isBook ? String(row.modelData.count) : ""
                        color: Theme.textMuted
                    }

                    FlatButton {
                        objectName: "readHighlight_" + row.index

                        visible: !row.isBook && row.modelData.type === "highlight" && page.library.readerAvailable
                            && row.modelData.spineIndex >= 0
                        text: qsTr("Read")
                        tip: qsTr("Open the book in the reader here")
                        onClicked: page.open(row.modelData)
                    }
                }
            }
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * Theme.panePadding, Math.round(420 * Theme.spacingScale))
        visible: page.browser.count === 0 && page.browser.error === ""
        spacing: Theme.gutter

        Label {
            objectName: "highlightsEmpty"

            Layout.fillWidth: true
            text: page.browser.query ? qsTr("No highlight matches “%1”").arg(page.browser.query) : qsTr("No highlights yet")
            font.weight: Font.Bold
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }

        Label {
            Layout.fillWidth: true
            visible: !page.browser.query
            text: qsTr("The highlights and bookmarks made in Cousoir's reader or Calibre's viewer show here.")
            color: Theme.textMuted
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
    }

    FileDialog {
        id: exportDialog

        title: qsTr("Export the highlights")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "md"
        nameFilters: [qsTr("Markdown (*.md)"), qsTr("Plain text (*.txt)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: {
            const markdown = !selectedFile.toString().toLowerCase().endsWith(".txt")
            const error = page.browser.save(page.browser.exportText(markdown), selectedFile)
            if (error)
                page.failed(error)
            else
                page.noticed(qsTr("Highlights exported"))
        }
    }
}
