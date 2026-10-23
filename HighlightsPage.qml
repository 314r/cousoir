pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

import Cousoir

// Every highlight and bookmark the library keeps, from Calibre's viewer,
// from Cousoir's reader and from the Kobos, as Calibre's "Browse
// annotations" shows them: by book, each book's in their order, under their
// chapters; a search of Calibre's full text index finds them, the words
// found in bold. After them, what a Kobo has that the library cannot hold.
// Enter opens the book in the reader there. Copy writes a book's, or all
// shown, as kbextract2 does, Markdown, Obsidian's or plain text; Ctrl+U
// shows the Markdown; the export is Calibre's.
Rectangle {
    id: page

    required property BookLibrary library
    required property HighlightSync sync
    readonly property HighlightBrowser browser: HighlightBrowser {
        library: page.library
        sync: page.sync
    }
    readonly property var highlightColors: ({ yellow: "#ffeb6b", green: "#c0ed72", blue: "#add8ff",
                                              red: "#ffb0ca", purple: "#d9b2ff" })
    readonly property var formatNames: ({ markdown: qsTr("Markdown"), obsidian: qsTr("Obsidian Markdown"),
                                          plain: qsTr("plain text") })
    // The book of the row picked: its highlights are what Copy and the
    // Markdown view take; all shown when none is.
    readonly property string currentKey: {
        const row = browser.rows[highlightList.currentIndex]
        return row && row.key ? row.key : ""
    }
    readonly property alias markdownShown: settings.markdownShown

    signal readRequested(int bookId, int spineIndex, string cfi)
    signal bookRequested(int bookId)
    signal failed(string message)
    signal noticed(string message)

    color: Theme.canvas

    function focusList() {
        if (settings.markdownShown)
            markdownView.forceActiveFocus()
        else
            highlightList.forceActiveFocus()
    }

    function focusSearch() {
        searchField.forceActiveFocus(Qt.ShortcutFocusReason)
        searchField.selectAll()
    }

    function open(row) {
        if (row && row.kind === "annotation" && row.placed)
            page.readRequested(row.bookId, row.spineIndex, row.cfi)
        else if (row && row.bookId > 0)
            page.bookRequested(row.bookId)
    }

    function copyExport(markdown) {
        page.browser.copy(page.browser.exportText(markdown))
        page.noticed(qsTr("Highlights copied"))
    }

    // The highlights of the current book, or all shown, as kbextract2
    // writes them; the format kept for the next Copy.
    function copyFormatted(format, all) {
        const text = page.browser.formatted(format, all ? "" : page.currentKey)
        if (!text)
            return
        settings.copyFormat = format
        page.browser.copy(text)
        page.noticed(all ? qsTr("Highlights copied as %1").arg(page.formatNames[format])
                         : qsTr("The book's highlights copied as %1").arg(page.formatNames[format]))
    }

    function toggleMarkdown() {
        settings.markdownShown = !settings.markdownShown
        page.focusList()
    }

    function importDatabase() {
        importDialog.open()
    }

    function syncNow() {
        page.sync.syncNow()
        page.noticed(qsTr("Syncing the Kobos' highlights"))
    }

    // Asks before taking a Kobo's highlights out of the library.
    function askRemoval(device) {
        removeDialog.device = device
        removeDialog.title = qsTr("Take %1's highlights out of the library?").arg(page.sync.nameOf(device))
        removeDialog.message = qsTr("They stay on the Kobo. Cousoir no longer brings them in, until Sync highlights.")
        removeDialog.open()
    }

    Settings {
        id: settings

        category: "Highlights"
        property string copyFormat: "markdown"
        property bool markdownShown: false
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
                Layout.maximumWidth: Math.round(320 * Theme.spacingScale)
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

            ChoiceBox {
                id: sourceBox
                objectName: "highlightSource"

                readonly property var choices: page.browser.sources.map(source => ({ value: source.key, label: source.name }))
                Layout.preferredWidth: Math.round(24 * Theme.charWidth) + leftPadding + rightPadding
                visible: choices.length > 2
                Accessible.name: qsTr("Whose")
                model: choices
                currentIndex: Math.max(0, choices.findIndex(choice => choice.value === page.browser.source))
                onActivated: function(index) { page.browser.source = sourceBox.choices[index].value }
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
                tip: page.currentKey ? qsTr("Copy the book's highlights as %1 (Ctrl+Shift+C)").arg(page.formatNames[settings.copyFormat])
                                     : qsTr("Copy the highlights shown as %1").arg(page.formatNames[settings.copyFormat])
                onClicked: page.copyFormatted(settings.copyFormat, !page.currentKey)
            }

            FlatButton {
                id: copyMenuButton
                objectName: "copyMenuButton"

                enabled: page.browser.count > 0
                iconOnly: true
                icon.source: "assets/icons/chevron-down.svg"
                text: qsTr("Copy as…")
                onClicked: copyMenu.popup(copyMenuButton, 0, copyMenuButton.height)
            }

            FlatButton {
                objectName: "saveHighlights"

                enabled: page.browser.count > 0
                variant: "outline"
                text: qsTr("Export…")
                tip: qsTr("Save the highlights shown as Calibre exports them")
                onClicked: exportDialog.open()
            }

            FlatButton {
                id: koboButton
                objectName: "koboMenuButton"

                variant: "outline"
                text: qsTr("Kobo")
                tip: qsTr("Sync the Kobos' highlights, or import a Kobo's database")
                onClicked: koboMenu.popup(koboButton, 0, koboButton.height)
            }
        }

        Label {
            objectName: "highlightSyncStatus"

            Layout.fillWidth: true
            Layout.leftMargin: Theme.pagePadding
            Layout.rightMargin: Theme.pagePadding
            Layout.bottomMargin: visible ? Theme.gutter : 0
            visible: text !== ""
            text: page.sync.status
            color: page.sync.pending ? Theme.danger : Theme.textMuted
            elide: Text.ElideRight
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

        // The Markdown Copy writes, as kbextract2 shows it.
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: settings.markdownShown
            ScrollBar.vertical: ThinScrollBar { }

            TextArea {
                id: markdownView
                objectName: "highlightMarkdown"

                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                leftPadding: Theme.pagePadding
                rightPadding: Theme.pagePadding
                topPadding: Theme.gutter
                text: settings.markdownShown ? page.browser.formatted("markdown", page.currentKey) : ""
                color: Theme.text
                selectionColor: Theme.textSelection
                font.family: Theme.uiFont
                font.pointSize: Theme.fontPointSizeBody
                background: null
                Keys.onEscapePressed: page.toggleMarkdown()
            }
        }

        ListView {
            id: highlightList
            objectName: "highlightList"

            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !settings.markdownShown
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
                readonly property bool isHeading: modelData.kind === "heading"
                readonly property bool isAnnotation: modelData.kind === "annotation"

                objectName: isBook ? "highlightBook_" + (modelData.bookId || modelData.key)
                    : isHeading ? "highlightHeading_" + index : "highlight_" + modelData.index
                width: ListView.view.width
                implicitHeight: content.implicitHeight + 2 * Theme.gutter
                color: ListView.isCurrentItem && highlightList.activeFocus ? Theme.selected
                    : hover.hovered && !isHeading ? Theme.hover : "transparent"

                HoverHandler {
                    id: hover
                }

                TapHandler {
                    onTapped: highlightList.currentIndex = row.index
                    onDoubleTapped: page.open(row.modelData)
                }

                ColumnLayout {
                    id: content

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.pagePadding
                    anchors.rightMargin: Theme.pagePadding
                    spacing: Theme.gutter

                    // What a Kobo has that the library cannot hold.
                    Label {
                        Layout.fillWidth: true
                        Layout.topMargin: Theme.gutter
                        visible: row.isHeading
                        text: row.isHeading ? row.modelData.title : ""
                        color: Theme.textMuted
                        font.weight: Font.Bold
                        elide: Text.ElideRight
                    }

                    // The chapter, before its first highlight.
                    Label {
                        objectName: "highlightChapter"

                        Layout.fillWidth: true
                        Layout.topMargin: Math.round(Theme.gutter / 2)
                        visible: row.isAnnotation && row.modelData.newChapter !== ""
                        text: row.isAnnotation ? row.modelData.newChapter : ""
                        font.weight: Font.Bold
                        elide: Text.ElideRight
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: !row.isHeading
                        spacing: Theme.gutter

                        // The highlight's colour, as the reader shows it.
                        Rectangle {
                            Layout.preferredWidth: Math.round(Theme.gutter / 2)
                            Layout.fillHeight: true
                            visible: row.isAnnotation
                            color: row.modelData.type === "bookmark" ? Theme.divider
                                : (page.highlightColors[row.modelData.color] || page.highlightColors.yellow)
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Math.round(Theme.gutter / 2)

                            Label {
                                Layout.fillWidth: true
                                visible: row.isBook
                                text: row.isBook ? row.modelData.title : ""
                                font.weight: Font.Bold
                                elide: Text.ElideRight
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: row.isBook && row.modelData.authors !== ""
                                text: row.isBook ? row.modelData.authors : ""
                                color: Theme.textMuted
                                elide: Text.ElideRight
                            }

                            Label {
                                objectName: "highlightText"

                                Layout.fillWidth: true
                                visible: row.isAnnotation && row.modelData.html !== ""
                                text: row.isAnnotation ? row.modelData.html : ""
                                textFormat: Text.StyledText
                                wrapMode: Text.Wrap
                            }

                            // The note, a card of its own.
                            Rectangle {
                                objectName: "highlightNote"

                                Layout.fillWidth: true
                                implicitHeight: noteText.implicitHeight + 2 * Theme.gutter
                                visible: row.isAnnotation && row.modelData.notes !== ""
                                color: Theme.fill
                                border.width: 1
                                border.color: Theme.divider

                                Label {
                                    id: noteText

                                    anchors.fill: parent
                                    anchors.margins: Theme.gutter
                                    text: row.isAnnotation ? row.modelData.notes : ""
                                    wrapMode: Text.Wrap
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: row.isAnnotation
                                text: row.isAnnotation
                                    ? [row.modelData.type === "bookmark" ? qsTr("Bookmark") : "",
                                       row.modelData.source !== "local:viewer" ? row.modelData.sourceName : "",
                                       row.modelData.date, row.modelData.reason].filter(part => part).join(" · ")
                                    : ""
                                color: row.isAnnotation && !row.modelData.placed && row.modelData.reason ? Theme.danger
                                                                                                        : Theme.textFaint
                                font.pointSize: Theme.fontPointSizeCaption
                                elide: Text.ElideRight
                            }
                        }

                        Label {
                            visible: row.isBook
                            text: row.isBook ? row.modelData.counts : ""
                            color: Theme.textMuted
                        }

                        FlatButton {
                            objectName: "readHighlight_" + row.index

                            visible: row.isAnnotation && row.modelData.placed && row.modelData.type === "highlight"
                                && page.library.readerAvailable && row.modelData.spineIndex >= 0
                            text: qsTr("Read")
                            tip: qsTr("Open the book in the reader here")
                            onClicked: page.open(row.modelData)
                        }
                    }
                }
            }
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * Theme.panePadding, Math.round(420 * Theme.spacingScale))
        visible: page.browser.count === 0 && page.browser.error === "" && !settings.markdownShown
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
            text: qsTr("The highlights and bookmarks made in Cousoir's reader or Calibre's viewer show here, and a Kobo's once it is plugged in.")
            color: Theme.textMuted
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
    }

    ActionMenu {
        id: copyMenu
        objectName: "copyMenu"

        actions: [
            { key: "markdown", title: qsTr("The book as Markdown") },
            { key: "obsidian", title: qsTr("The book as Obsidian Markdown") },
            { key: "plain", title: qsTr("The book as plain text") },
            { key: "all:markdown", title: qsTr("All shown as Markdown") },
            { key: "all:obsidian", title: qsTr("All shown as Obsidian Markdown") },
            { key: "all:plain", title: qsTr("All shown as plain text") }
        ]
        onChosen: function(key) {
            const all = key.startsWith("all:") || !page.currentKey
            page.copyFormatted(key.replace("all:", ""), all)
        }
    }

    ActionMenu {
        id: koboMenu
        objectName: "koboMenu"

        actions: [
            { key: "sync", title: qsTr("Sync highlights") },
            { key: "import", title: qsTr("Import a Kobo's database…") },
            { key: "markdown", title: settings.markdownShown ? qsTr("Show the highlights (Ctrl+U)") : qsTr("Show as Markdown (Ctrl+U)") }
        ].concat(page.sync.sources.filter(source => !source.off).map(source => ({
            key: "remove:" + source.device, title: qsTr("Take %1's highlights out…").arg(source.name) })))
        onChosen: function(key) {
            if (key === "sync")
                page.syncNow()
            else if (key === "import")
                page.importDatabase()
            else if (key === "markdown")
                page.toggleMarkdown()
            else if (key.startsWith("remove:"))
                page.askRemoval(key.slice(7))
        }
    }

    ConfirmDialog {
        id: removeDialog
        objectName: "removeKoboDialog"

        property string device: ""

        acceptText: qsTr("Take Out")
        acceptVariant: "danger"
        onAccepted: {
            const error = page.sync.removeFromLibrary(removeDialog.device)
            if (error)
                page.failed(error)
        }
    }

    FileDialog {
        id: importDialog

        title: qsTr("Import a Kobo's highlights")
        nameFilters: [qsTr("Kobo database (KoboReader.sqlite)"), qsTr("SQLite databases (*.sqlite *.db)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.HomeLocation)
        onAccepted: {
            const error = page.sync.importDatabase(selectedFile)
            if (error)
                page.failed(error)
            else
                page.noticed(qsTr("Importing the Kobo's highlights"))
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
