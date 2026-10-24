pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

import Cousoir

// The library's highlights and notes as kbextract2 shows a Kobo's: the books
// that have some, a cover or a letter for each, and the book picked as a
// page of its own, by chapter, each highlight a quote and its note a card,
// the whole page selectable. Ctrl+U shows it as the Markdown Copy writes;
// Copy writes it as kbextract2 does, Markdown, Obsidian's or plain text.
// From Calibre's viewer, from Cousoir's reader and from the Kobos; after the
// library's books, a Kobo's the library has not. Too narrow for both, the
// page shows the books, then the book picked.
Rectangle {
    id: page

    required property BookLibrary library
    required property HighlightSync sync
    readonly property HighlightBrowser browser: HighlightBrowser {
        library: page.library
        sync: page.sync
    }
    readonly property HighlightDocument builder: HighlightDocument {
        textDocument: documentView.textDocument
        content: page.browser.document
        markdown: {
            page.browser.document
            return settings.markdownShown ? page.browser.formatted("markdown", page.browser.current) : ""
        }
        font: Qt.font({ family: Theme.documentFont, pointSize: Theme.fontPointSizeBody * 1.15 })
        monoFont: Qt.font({ family: Theme.uiFont, pointSize: Theme.fontPointSizeBody })
        scale: Theme.spacingScale
        textColor: Theme.text
        mutedColor: Theme.mix(Theme.canvas, Theme.text, 0.62)
        faintColor: Theme.mix(Theme.canvas, Theme.text, 0.45)
        accentColor: Theme.accent
    }
    readonly property var formatNames: ({ markdown: qsTr("Markdown"), obsidian: qsTr("Obsidian Markdown"),
                                          plain: qsTr("plain text") })
    // kbextract2's tiles, GNOME's blue, green, orange, red, purple and brown.
    readonly property var tileColors: ["#1c71d8", "#26a269", "#e66100", "#c01c28", "#9141ac", "#986a44"]
    readonly property bool bookShown: browser.current !== ""
    readonly property alias markdownShown: settings.markdownShown
    // Too narrow for the books and the page side by side.
    readonly property bool narrow: width < Math.round(600 * Theme.spacingScale)
    // When narrow, the page rather than the books.
    property bool reading: false
    property bool copied: false

    signal failed(string message)
    signal noticed(string message)

    color: Theme.canvas

    function focusList() {
        page.reading = false
        bookList.forceActiveFocus()
    }

    function focusDocument() {
        if (!page.bookShown)
            return
        page.reading = true
        documentView.forceActiveFocus()
    }

    function show(key) {
        page.browser.current = key
    }

    // The next book's, or the one before's.
    function step(offset) {
        const books = page.browser.books
        if (books.length === 0)
            return
        const index = books.findIndex(book => book.key === page.browser.current)
        const next = index < 0 ? (offset > 0 ? 0 : books.length - 1)
                               : Math.max(0, Math.min(books.length - 1, index + offset))
        page.show(books[next].key)
    }

    // The book shown as kbextract2 writes it; the button says so a moment.
    function copyFormatted(format) {
        const text = page.browser.formatted(format)
        if (!text)
            return
        page.browser.copy(text)
        page.copied = true
        copiedTimer.restart()
    }

    function toggleMarkdown() {
        settings.markdownShown = !settings.markdownShown
        if (page.bookShown && (!page.narrow || page.reading))
            documentView.forceActiveFocus()
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
        property bool markdownShown: false
    }

    Timer {
        id: copiedTimer

        interval: 1500
        onTriggered: page.copied = false
    }

    // Another book: from its top, fading in.
    Connections {
        target: page.browser

        function onCurrentChanged() {
            documentFlick.contentY = 0
            fadeIn.restart()
            if (!page.bookShown)
                page.reading = false
        }
    }

    NumberAnimation {
        id: fadeIn

        target: documentFlick
        property: "opacity"
        from: 0
        to: 1
        duration: 220
        easing.type: Easing.OutCubic
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // The books.
        ColumnLayout {
            Layout.fillHeight: true
            Layout.fillWidth: page.narrow
            Layout.preferredWidth: page.narrow ? -1 : Math.round(Math.max(280 * Theme.spacingScale,
                                                                          Math.min(380 * Theme.spacingScale, page.width * 0.3)))
            visible: !page.narrow || !page.reading
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.toolbarHeight
                Layout.leftMargin: Theme.panePadding
                Layout.rightMargin: Theme.gutter
                spacing: Theme.gutter

                HeadingLabel {
                    text: qsTr("Books")
                }

                Item { Layout.fillWidth: true }

                Label {
                    objectName: "highlightBookCount"

                    visible: page.browser.books.length > 0
                    text: page.browser.books.length
                    color: Theme.textMuted
                    font.pointSize: Theme.fontPointSizeCaption
                }

                FlatButton {
                    id: koboButton
                    objectName: "koboMenuButton"

                    iconOnly: true
                    text: qsTr("Kobo")
                    tip: qsTr("Sync the Kobos' highlights, or import a Kobo's database")
                    icon.source: "assets/icons/more.svg"
                    onClicked: koboMenu.popup(koboButton, 0, koboButton.height)
                }
            }

            Label {
                objectName: "highlightSyncStatus"

                Layout.fillWidth: true
                Layout.leftMargin: Theme.panePadding
                Layout.rightMargin: Theme.panePadding
                Layout.bottomMargin: visible ? Theme.gutter : 0
                visible: text !== ""
                text: page.sync.status
                color: page.sync.pending ? Theme.danger : Theme.textMuted
                font.pointSize: Theme.fontPointSizeCaption
                wrapMode: Text.Wrap
            }

            Label {
                objectName: "highlightError"

                Layout.fillWidth: true
                Layout.leftMargin: Theme.panePadding
                Layout.rightMargin: Theme.panePadding
                visible: page.browser.error !== ""
                text: page.browser.error
                color: Theme.danger
                wrapMode: Text.Wrap
            }

            ListView {
                id: bookList
                objectName: "highlightBooks"

                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                focus: true
                keyNavigationEnabled: false
                model: page.browser.books
                currentIndex: page.browser.books.findIndex(book => book.key === page.browser.current)
                boundsBehavior: Flickable.StopAtBounds
                activeFocusOnTab: true
                Accessible.name: qsTr("Books with highlights")

                // Over the book picked, not under it.
                ScrollBar.vertical: ThinScrollBar {
                    solid: true
                }

                Keys.onUpPressed: page.step(-1)
                Keys.onDownPressed: page.step(1)
                Keys.onReturnPressed: page.focusDocument()
                Keys.onEnterPressed: page.focusDocument()
                Keys.onRightPressed: page.focusDocument()

                // The books only on a Kobo, under its name.
                section.property: "section"
                section.delegate: HeadingLabel {
                    required property string section

                    width: ListView.view.width
                    visible: section !== ""
                    height: visible ? implicitHeight + 2 * Theme.gutter : 0
                    leftPadding: Theme.panePadding
                    rightPadding: Theme.panePadding
                    topPadding: Theme.gutter * 2
                    text: section
                    elide: Text.ElideRight
                }

                delegate: Rectangle {
                    id: row

                    required property var modelData
                    required property int index
                    readonly property bool current: modelData.key === page.browser.current
                    readonly property int tileWidth: Math.round(36 * Theme.spacingScale)

                    objectName: "highlightBook_" + (modelData.bookId || modelData.key)
                    width: ListView.view.width
                    implicitHeight: Math.max(tile.height, texts.implicitHeight) + 2 * Theme.gutter
                    color: current ? (bookList.activeFocus ? Theme.selected : Theme.hover)
                        : hover.hovered ? Theme.hover : "transparent"

                    HoverHandler {
                        id: hover
                    }

                    TapHandler {
                        onTapped: {
                            page.show(row.modelData.key)
                            if (page.narrow)
                                page.focusDocument()
                            else
                                bookList.forceActiveFocus()
                        }
                        onDoubleTapped: page.focusDocument()
                    }

                    RowLayout {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: Theme.panePadding
                        anchors.rightMargin: Theme.panePadding
                        spacing: Math.round(12 * Theme.spacingScale)

                        // The cover, or kbextract2's tile: the title's
                        // letter on a colour of its own.
                        Rectangle {
                            id: tile

                            Layout.preferredWidth: row.tileWidth
                            Layout.preferredHeight: Math.round(row.tileWidth * 4 / 3)
                            Layout.alignment: Qt.AlignTop
                            radius: Math.round(4 * Theme.spacingScale)
                            color: row.modelData.cover ? Theme.fill : page.tileColors[row.modelData.tint]
                            border.width: 1
                            border.color: Qt.rgba(0, 0, 0, 0.08)
                            clip: true
                            Accessible.ignored: true

                            Image {
                                objectName: "highlightCover"

                                anchors.fill: parent
                                visible: row.modelData.cover !== ""
                                source: row.modelData.cover
                                sourceSize.width: row.tileWidth * 2
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                cache: false
                            }

                            Label {
                                objectName: "highlightMonogram"

                                anchors.centerIn: parent
                                visible: row.modelData.cover === ""
                                text: row.modelData.monogram
                                color: "white"
                                font.family: Theme.documentFont
                                font.pointSize: Theme.fontPointSizeTitle
                                font.weight: Font.ExtraBold
                            }
                        }

                        ColumnLayout {
                            id: texts

                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: Math.round(2 * Theme.spacingScale)

                            Label {
                                Layout.fillWidth: true
                                text: row.modelData.title
                                font.weight: Font.DemiBold
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: row.modelData.authors !== ""
                                text: row.modelData.authors
                                color: Theme.textMuted
                                elide: Text.ElideRight
                            }

                            Label {
                                objectName: "highlightCounts"

                                Layout.fillWidth: true
                                text: row.modelData.counts
                                color: Theme.textMuted
                                font.pointSize: Theme.fontPointSizeCaption
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: Theme.panePadding
                visible: page.browser.books.length === 0 && page.browser.error === ""
                spacing: Theme.gutter

                Item { Layout.fillHeight: true }

                Label {
                    objectName: "highlightsEmpty"

                    Layout.fillWidth: true
                    text: qsTr("No highlights yet")
                    font.weight: Font.Bold
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("The highlights made in Cousoir's reader or Calibre's viewer show here, and a Kobo's once it is plugged in.")
                    color: Theme.textMuted
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                }

                FlatButton {
                    objectName: "importKoboButton"

                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: Theme.gutter
                    variant: "outline"
                    enabled: page.library.loaded
                    text: qsTr("Import a Kobo's Database…")
                    onClicked: page.importDatabase()
                }

                Item { Layout.fillHeight: true }
            }
        }

        Divider {
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            visible: !page.narrow
        }

        // The book picked.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !page.narrow || page.reading
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.toolbarHeight
                Layout.leftMargin: Theme.gutter
                Layout.rightMargin: Theme.panePadding
                spacing: Theme.gutter

                FlatButton {
                    objectName: "highlightsBack"

                    visible: page.narrow
                    iconOnly: true
                    text: qsTr("Books")
                    tip: qsTr("Back to the books (Esc)")
                    icon.source: "assets/icons/arrow-left.svg"
                    onClicked: page.focusList()
                }

                FlatButton {
                    objectName: "markdownToggle"

                    visible: page.bookShown
                    iconOnly: true
                    selected: settings.markdownShown
                    text: settings.markdownShown ? qsTr("Show the highlights") : qsTr("Show the Markdown")
                    tip: qsTr("%1 (Ctrl+U)").arg(text)
                    icon.source: "assets/icons/markdown.svg"
                    onClicked: page.toggleMarkdown()
                }

                // The book's title, once the page's own has scrolled away.
                ColumnLayout {
                    objectName: "highlightHeaderTitle"

                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 0
                    opacity: page.bookShown && (settings.markdownShown || documentFlick.titleScrolled) ? 1 : 0

                    Behavior on opacity {
                        NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: page.browser.document.title || ""
                        font.weight: Font.Bold
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: page.browser.document.authors || ""
                        color: Theme.textMuted
                        font.pointSize: Theme.fontPointSizeCaption
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                    }
                }

                FlatButton {
                    objectName: "copyHighlights"

                    enabled: page.bookShown
                    variant: "outline"
                    text: page.copied ? qsTr("Copied") : qsTr("Copy")
                    icon.source: page.copied ? "assets/icons/check.svg" : ""
                    tip: qsTr("Copy the book's highlights as Markdown (Ctrl+Shift+C)")
                    onClicked: page.copyFormatted("markdown")
                }

                FlatButton {
                    id: copyMenuButton
                    objectName: "copyMenuButton"

                    enabled: page.bookShown
                    iconOnly: true
                    icon.source: "assets/icons/chevron-down.svg"
                    text: qsTr("Copy as…")
                    onClicked: copyMenu.popup(copyMenuButton, 0, copyMenuButton.height)
                }
            }

            Divider {
                Layout.fillWidth: true
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                Flickable {
                    id: documentFlick

                    // Whether the page's own title is out of sight.
                    readonly property bool titleScrolled: {
                        documentView.contentHeight
                        const end = documentView.positionToRectangle(page.builder.titleEnd)
                        return contentY > end.y + end.height
                    }

                    anchors.fill: parent
                    visible: page.bookShown
                    clip: true
                    contentWidth: width
                    contentHeight: documentColumn.height
                    boundsBehavior: Flickable.StopAtBounds

                    ScrollBar.vertical: ThinScrollBar { }

                    function scrollBy(delta) {
                        contentY = Math.max(0, Math.min(contentHeight - height, contentY + delta))
                    }

                    Item {
                        id: documentColumn

                        // kbextract2's measure: 70 average characters and the
                        // margins; the Markdown's, 120 "M"s.
                        readonly property real measure: settings.markdownShown
                            ? 120 * monoMetrics.advanceWidth("M") + 64 * Theme.spacingScale
                            : page.builder.measure * documentMetrics.averageCharacterWidth + 130 * Theme.spacingScale

                        x: Math.max(0, Math.round((documentFlick.width - width) / 2))
                        width: Math.min(documentFlick.width, Math.round(measure))
                        height: documentView.implicitHeight

                        FontMetrics {
                            id: documentMetrics

                            font.family: Theme.documentFont
                            font.pointSize: Theme.fontPointSizeBody * 1.15
                        }

                        FontMetrics {
                            id: monoMetrics

                            font.family: Theme.uiFont
                            font.pointSize: Theme.fontPointSizeBody
                        }

                        // kbextract2's quote bars, note cards and rule, under
                        // the text.
                        Repeater {
                            model: settings.markdownShown ? [] : page.builder.blocks

                            delegate: Rectangle {
                                id: decoration

                                required property var modelData
                                readonly property string kind: modelData.kind
                                readonly property rect first: {
                                    documentView.contentHeight
                                    documentView.width
                                    return documentView.positionToRectangle(modelData.start)
                                }
                                readonly property rect last: {
                                    documentView.contentHeight
                                    documentView.width
                                    return documentView.positionToRectangle(modelData.end)
                                }

                                objectName: "highlightDecoration_" + kind
                                x: modelData.left
                                y: kind === "rule" ? last.y + last.height + modelData.outset : first.y - modelData.outset
                                width: kind === "quote" ? Math.max(2, Math.round(3 * Theme.spacingScale))
                                                        : documentColumn.width - modelData.left - modelData.right
                                height: kind === "rule" ? 1 : last.y + last.height - first.y + 2 * modelData.outset
                                radius: kind === "quote" ? width / 2 : kind === "note" ? Math.round(12 * Theme.spacingScale) : 0
                                color: kind === "quote" ? Theme.accent
                                    : Theme.withAlpha(Theme.text, kind === "note" ? 0.045 : 0.12)
                                border.width: kind === "note" ? 1 : 0
                                border.color: Theme.withAlpha(Theme.text, 0.09)
                            }
                        }

                        TextEdit {
                            id: documentView
                            objectName: "highlightDocument"

                            width: parent.width
                            // document.go's margins above and below, and
                            // readerview.go's.
                            topPadding: Math.round((settings.markdownShown ? 28 : 30) * Theme.spacingScale)
                            bottomPadding: Math.round((settings.markdownShown ? 28 : 64) * Theme.spacingScale)
                            readOnly: true
                            selectByMouse: true
                            persistentSelection: true
                            textFormat: TextEdit.RichText
                            wrapMode: TextEdit.Wrap
                            font: settings.markdownShown ? page.builder.monoFont : page.builder.font
                            color: Theme.text
                            selectionColor: Theme.textSelection
                            selectedTextColor: Theme.text
                            activeFocusOnTab: true
                            Accessible.name: qsTr("Highlights of %1").arg(page.browser.document.title || "")

                            Keys.onUpPressed: documentFlick.scrollBy(-Theme.rowHeight)
                            Keys.onDownPressed: documentFlick.scrollBy(Theme.rowHeight)
                            Keys.onPressed: function(event) {
                                if (event.key === Qt.Key_PageUp) {
                                    documentFlick.scrollBy(-documentFlick.height * 0.9)
                                    event.accepted = true
                                } else if (event.key === Qt.Key_PageDown || event.key === Qt.Key_Space) {
                                    documentFlick.scrollBy(documentFlick.height * 0.9)
                                    event.accepted = true
                                } else if (event.key === Qt.Key_Home && !(event.modifiers & Qt.ShiftModifier)) {
                                    documentFlick.contentY = 0
                                    event.accepted = true
                                } else if (event.key === Qt.Key_End && !(event.modifiers & Qt.ShiftModifier)) {
                                    documentFlick.scrollBy(documentFlick.contentHeight)
                                    event.accepted = true
                                } else if (event.key === Qt.Key_Escape || event.key === Qt.Key_Left
                                           || (event.key === Qt.Key_Back)) {
                                    page.focusList()
                                    event.accepted = true
                                }
                            }
                        }
                    }
                }

                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 2 * Theme.panePadding, Math.round(420 * Theme.spacingScale))
                    visible: !page.bookShown && page.browser.books.length > 0
                    spacing: Theme.gutter

                    Label {
                        objectName: "noBookSelected"

                        Layout.fillWidth: true
                        text: qsTr("No book selected")
                        font.weight: Font.Bold
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Select a book to view its highlights and notes.")
                        color: Theme.textMuted
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }

    ActionMenu {
        id: copyMenu
        objectName: "copyMenu"

        actions: [
            { key: "markdown", title: qsTr("Copy as Markdown") },
            { key: "obsidian", title: qsTr("Copy as Obsidian Markdown") },
            { key: "plain", title: qsTr("Copy as Plain Text") }
        ]
        onChosen: function(key) { page.copyFormatted(key) }
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
}
