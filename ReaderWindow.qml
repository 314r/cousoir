pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtWebEngine

import Cousoir

// A book read in Cousoir: its pages a screen at a time, its table of
// contents beside them, where reading stopped kept as it goes.
ThemedWindow {
    id: readerWindow
    objectName: "readerWindow"

    required property BookLibrary library
    property alias reader: bookReader
    // The spine item shown, and what to do once it is loaded: "start",
    // "end", "cfi:<CFI>" or "fragment:<id>".
    property int spineIndex: -1
    property string pending: "start"
    property string position: ""
    property double fileFraction: 0
    property int page: 0
    property int pages: 1
    property real zoom: 1
    property bool tocVisible: false
    // The sidebar's list: "toc" or "highlights".
    property string sidebar: "toc"
    property bool hasSelection: false
    // The highlights shown on the page.
    property int highlightsShown: 0
    // The page each table of contents anchor of the spine item shown is on.
    property var anchorPages: ({})
    // The table of contents entry being read.
    readonly property int currentEntry: {
        let found = -1
        const toc = bookReader.toc
        for (let i = 0; i < toc.length; ++i) {
            const entry = toc[i]
            if (entry.spineIndex < 0 || entry.spineIndex > spineIndex)
                continue
            if (entry.spineIndex === spineIndex && entry.fragment
                && !(anchorPages[entry.fragment] >= 0 && anchorPages[entry.fragment] <= page))
                continue
            found = i
        }
        return found
    }
    // The highlight clicked, to remove.
    property string chosenHighlight: ""
    readonly property bool darkPage: Theme.canvas.hslLightness < 0.5
    // Calibre's builtin_colors_light and builtin_colors_dark.
    readonly property var highlightColors: darkPage
        ? { yellow: "#906e00", green: "#306f50", blue: "#265589", red: "#a23e5a", purple: "#505088" }
        : { yellow: "#ffeb6b", green: "#c0ed72", blue: "#add8ff", red: "#ffb0ca", purple: "#d9b2ff" }
    readonly property double bookFraction: bookReader.bookFraction(spineIndex, fileFraction)

    signal failed(string message)

    // Reads the book from where it was left.
    function openBook(bookId) {
        const error = bookReader.open(bookId)
        if (error) {
            readerWindow.failed(error)
            return false
        }
        const start = bookReader.startPosition
        show()
        raise()
        requestActivate()
        // Another book's page may still be there: this one's loaded anew.
        spineIndex = -1
        load(start.spineIndex || 0, start.cfi ? "cfi:" + start.cfi : "start")
        return true
    }

    function load(index, action) {
        if (index < 0 || index >= bookReader.spine.length)
            return
        pending = action
        // Within the page shown: only where to go.
        if (index === spineIndex && !view.loading) {
            go()
            return
        }
        spineIndex = index
        view.opacity = 0
        const url = bookReader.spine[index].url
        if (view.url.toString() === url.toString())
            view.reload()
        else
            view.url = url
    }

    // A page loaded: the reader's script, the theme's colours and the
    // highlights, once; then where to go.
    function prepare() {
        const colors = { background: Theme.canvas.toString(), color: Theme.text.toString(), link: Theme.accent.toString() }
        view.runJavaScript(bookReader.script)
        view.runJavaScript("window.cousoir.setup(" + JSON.stringify(colors) + ")")
        highlightsShown = 0
        applyHighlights(bookReader.highlights.filter(h => h.spineIndex === spineIndex))
        go()
        updateAnchors()
        view.opacity = 1
    }

    function go() {
        if (pending.startsWith("cfi:"))
            view.runJavaScript("window.cousoir.goToCfi(" + JSON.stringify(pending.slice(4)) + ")")
        else if (pending.startsWith("fragment:"))
            view.runJavaScript("window.cousoir.goToFragment(" + JSON.stringify(pending.slice(9)) + ")")
        else if (pending === "end")
            view.runJavaScript("window.cousoir.goToEnd()")
        else
            view.runJavaScript("window.cousoir.goToStart()")
        hasSelection = false
        chosenHighlight = ""
        view.forceActiveFocus()
    }

    // Where the table of contents' anchors of the spine item are, for the
    // chapter being read.
    function updateAnchors() {
        const anchors = bookReader.toc.filter(entry => entry.spineIndex === spineIndex && entry.fragment)
                                      .map(entry => entry.fragment)
        anchorPages = ({})
        if (anchors.length)
            view.runJavaScript("window.cousoir.pagesOf(" + JSON.stringify(anchors) + ")",
                               function(pages) { readerWindow.anchorPages = pages || ({}) })
    }

    function applyHighlights(list) {
        const marks = list.map(h => ({ uuid: h.uuid, start: h.start, end: h.end,
                                       color: highlightColors[h.which] || highlightColors.yellow }))
        if (marks.length)
            view.runJavaScript("window.cousoir.applyHighlights(" + JSON.stringify(marks) + ")",
                               function(count) { readerWindow.highlightsShown += count || 0 })
    }

    // The selection made a highlight, kept where Calibre's viewer keeps its
    // own.
    function highlightSelection() {
        view.runJavaScript("JSON.stringify(window.cousoir.selection())", function(result) {
            const selected = result ? JSON.parse(result) : null
            if (!selected)
                return
            const uuid = bookReader.addHighlight(spineIndex, selected.start, selected.end, selected.text, "yellow")
            if (!uuid)
                return
            applyHighlights([{ uuid: uuid, start: selected.start, end: selected.end, which: "yellow" }])
            view.runJavaScript("window.cousoir.clearSelection()")
            hasSelection = false
        })
    }

    function removeChosenHighlight() {
        if (!chosenHighlight)
            return
        bookReader.removeHighlight(chosenHighlight)
        view.runJavaScript("window.cousoir.removeHighlight(" + JSON.stringify(chosenHighlight) + ")")
        chosenHighlight = ""
        highlightsShown = Math.max(0, highlightsShown - 1)
    }

    function nextPage() {
        view.runJavaScript("window.cousoir.next()")
    }

    function previousPage() {
        view.runJavaScript("window.cousoir.previous()")
    }

    function save() {
        if (position)
            bookReader.savePosition(spineIndex, position, fileFraction)
    }

    function setZoom(value) {
        zoom = Math.max(0.5, Math.min(3, value))
    }

    function chapterTitle() {
        return currentEntry >= 0 ? bookReader.toc[currentEntry].title : ""
    }

    visible: false
    width: 900
    height: 1000
    minimumWidth: 480
    minimumHeight: 400
    title: bookReader.title || qsTr("Reader")

    onClosing: {
        save()
        bookReader.close()
        spineIndex = -1
        position = ""
    }

    BookReader {
        id: bookReader
        library: readerWindow.library
    }

    // Saved as reading goes, a little after each turn.
    Timer {
        id: saveTimer
        interval: 1500
        onTriggered: readerWindow.save()
    }

    Shortcut {
        sequences: ["Esc", StandardKey.Close]
        onActivated: readerWindow.close()
    }

    header: Rectangle {
        implicitHeight: Theme.toolbarHeight
        color: Theme.canvas

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
                selected: readerWindow.tocVisible
                onClicked: readerWindow.tocVisible = !readerWindow.tocVisible
            }

            Label {
                Layout.fillWidth: true
                text: [bookReader.title, readerWindow.chapterTitle()].filter(part => part).join("  ·  ")
                elide: Text.ElideRight
                color: Theme.textMuted
            }

            FlatButton {
                objectName: "readerHighlightButton"

                visible: readerWindow.hasSelection && readerWindow.spineIndex >= 0
                variant: "outline"
                text: qsTr("Highlight")
                tip: qsTr("Highlight the selection (H)")
                onClicked: readerWindow.highlightSelection()
            }

            FlatButton {
                objectName: "readerRemoveHighlightButton"

                visible: readerWindow.chosenHighlight !== ""
                variant: "outline"
                text: qsTr("Remove highlight")
                icon.source: "assets/icons/trash.svg"
                onClicked: readerWindow.removeChosenHighlight()
            }

            Label {
                objectName: "readerProgress"

                text: qsTr("%1 %").arg(Math.round(readerWindow.bookFraction * 100))
                color: Theme.textMuted
            }

            FlatButton {
                text: qsTr("A−")
                tip: qsTr("Smaller text (Ctrl+Minus)")
                onClicked: readerWindow.setZoom(readerWindow.zoom - 0.1)
            }

            FlatButton {
                text: qsTr("A+")
                tip: qsTr("Larger text (Ctrl+Plus)")
                onClicked: readerWindow.setZoom(readerWindow.zoom + 0.1)
            }

            FlatButton {
                iconOnly: true
                text: qsTr("Close the book (Esc)")
                icon.source: "assets/icons/close.svg"
                onClicked: readerWindow.close()
            }
        }

        Divider {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        ColumnLayout {
            // Its own width: a layout would otherwise take its lists' fill.
            Layout.preferredWidth: Math.round(30 * Theme.charWidth)
            Layout.maximumWidth: Layout.preferredWidth
            Layout.fillWidth: false
            Layout.fillHeight: true
            visible: readerWindow.tocVisible
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: Theme.gutter
                spacing: Theme.gutter

                FlatButton {
                    variant: "outline"
                    selected: readerWindow.sidebar === "toc"
                    text: qsTr("Contents")
                    onClicked: readerWindow.sidebar = "toc"
                }

                FlatButton {
                    objectName: "readerHighlightsTab"

                    variant: "outline"
                    selected: readerWindow.sidebar === "highlights"
                    text: qsTr("Highlights (%1)").arg(bookReader.highlights.length)
                    onClicked: readerWindow.sidebar = "highlights"
                }
            }

            ListView {
                objectName: "readerHighlights"

                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: readerWindow.sidebar === "highlights"
                clip: true
                model: bookReader.highlights
                spacing: Theme.gutter

                delegate: ItemDelegate {
                    id: mark

                    required property var modelData

                    width: ListView.view.width
                    leftPadding: Theme.gutter
                    rightPadding: Theme.gutter
                    onClicked: readerWindow.load(mark.modelData.spineIndex,
                                                 "cfi:epubcfi(/" + 2 * (mark.modelData.spineIndex + 1) + mark.modelData.start + ")")

                    contentItem: ColumnLayout {
                        spacing: 2

                        Label {
                            Layout.fillWidth: true
                            text: mark.modelData.text
                            wrapMode: Text.Wrap
                            maximumLineCount: 4
                            elide: Text.ElideRight
                            leftPadding: Theme.gutter
                            background: Rectangle {
                                width: 3
                                height: parent.height
                                color: readerWindow.highlightColors[mark.modelData.which] || readerWindow.highlightColors.yellow
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: mark.modelData.notes !== ""
                            text: mark.modelData.notes
                            wrapMode: Text.Wrap
                            color: Theme.textMuted
                            font.italic: true
                        }
                    }
                    background: Rectangle {
                        color: mark.hovered ? Theme.hover : "transparent"
                    }
                }

                ScrollBar.vertical: ThinScrollBar {}
            }

            ListView {
                id: tocList
                objectName: "readerToc"

                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: readerWindow.sidebar === "toc"
                clip: true
                model: bookReader.toc

                delegate: ItemDelegate {
                    id: entry

                    required property var modelData
                    required property int index

                    width: ListView.view.width
                    leftPadding: Theme.gutter + entry.modelData.depth * Theme.gutter * 2
                    text: entry.modelData.title
                    highlighted: entry.index === readerWindow.currentEntry
                    enabled: entry.modelData.spineIndex >= 0
                    onClicked: readerWindow.load(entry.modelData.spineIndex,
                                                 entry.modelData.fragment ? "fragment:" + entry.modelData.fragment : "start")

                    contentItem: Label {
                        text: entry.text
                        elide: Text.ElideRight
                        color: entry.highlighted ? Theme.textBright : Theme.text
                        font.weight: entry.highlighted ? Font.Bold : Font.Normal
                    }
                    background: Rectangle {
                        color: entry.hovered ? Theme.hover : "transparent"
                    }
                }

                ScrollBar.vertical: ThinScrollBar {}
            }
        }

        Divider {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            visible: readerWindow.tocVisible
        }

        WebEngineView {
            id: view
            objectName: "readerView"

            Layout.fillWidth: true
            Layout.fillHeight: true
            backgroundColor: Theme.canvas
            zoomFactor: readerWindow.zoom
            settings.javascriptCanOpenWindows: false
            settings.localContentCanAccessRemoteUrls: false

            onLoadingChanged: function(request) {
                if (request.status === WebEngineView.LoadSucceededStatus)
                    readerWindow.prepare()
                else if (request.status === WebEngineView.LoadFailedStatus)
                    readerWindow.failed(qsTr("This page of the book cannot be shown: %1").arg(request.errorString))
            }

            // Links within the book go to their page; others to the desktop's
            // browser.
            onNavigationRequested: function(request) {
                if (request.navigationType !== WebEngineNavigationRequest.LinkClickedNavigation)
                    return
                request.reject()
                const index = bookReader.spineIndexOf(request.url)
                if (index >= 0) {
                    const url = request.url.toString()
                    const hash = url.indexOf("#")
                    readerWindow.load(index, hash >= 0 ? "fragment:" + decodeURIComponent(url.slice(hash + 1)) : "start")
                } else if (/^(https?|mailto):/.test(request.url.toString())) {
                    Qt.openUrlExternally(request.url)
                }
            }

            onJavaScriptConsoleMessage: function(level, message) {
                if (!message.startsWith("cousoir:") || view.loading)
                    return
                let event
                try {
                    event = JSON.parse(message.slice(8))
                } catch (e) {
                    return
                }
                // From the page shown, not one being left.
                if (bookReader.spineIndexOf(event.page_url) !== readerWindow.spineIndex)
                    return
                if (event.event === "position") {
                    readerWindow.position = "epubcfi(/" + 2 * (readerWindow.spineIndex + 1) + event.cfi + ")"
                    readerWindow.fileFraction = event.fraction
                    readerWindow.page = event.page
                    readerWindow.pages = event.pages
                    saveTimer.restart()
                } else if (event.event === "next") {
                    readerWindow.load(readerWindow.spineIndex + 1, "start")
                } else if (event.event === "previous") {
                    readerWindow.load(readerWindow.spineIndex - 1, "end")
                } else if (event.event === "layout") {
                    readerWindow.updateAnchors()
                } else if (event.event === "selection") {
                    readerWindow.hasSelection = !event.empty
                    if (!event.empty)
                        readerWindow.chosenHighlight = ""
                } else if (event.event === "highlight") {
                    readerWindow.chosenHighlight = event.uuid
                } else if (event.event === "key") {
                    if (event.key === "Escape" && readerWindow.chosenHighlight)
                        readerWindow.chosenHighlight = ""
                    else if (event.key === "Escape")
                        readerWindow.close()
                    else if (!event.ctrl && (event.key === "h" || event.key === "H") && readerWindow.hasSelection)
                        readerWindow.highlightSelection()
                    else if (event.key === "Delete" && readerWindow.chosenHighlight)
                        readerWindow.removeChosenHighlight()
                    else if (!event.ctrl && (event.key === "t" || event.key === "T"))
                        readerWindow.tocVisible = !readerWindow.tocVisible
                    else if (event.ctrl && (event.key === "+" || event.key === "="))
                        readerWindow.setZoom(readerWindow.zoom + 0.1)
                    else if (event.ctrl && event.key === "-")
                        readerWindow.setZoom(readerWindow.zoom - 0.1)
                    else if (event.ctrl && event.key === "0")
                        readerWindow.setZoom(1)
                    else if (event.ctrl && (event.key === "w" || event.key === "W"))
                        readerWindow.close()
                }
            }
        }
    }
}
