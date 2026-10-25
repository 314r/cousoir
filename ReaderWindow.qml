pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtWebEngine

import Cousoir

// A book read in Cousoir: its pages a screen at a time, one or two side by
// side, or as a scroll, the lines no longer than read well, in the type and
// colours the reader picks; its table of contents beside them, where reading
// stopped kept as it goes.
ThemedWindow {
    id: readerWindow
    objectName: "readerWindow"

    required property BookLibrary library
    property alias reader: bookReader
    property alias settings: readerSettings
    // The spine item shown, and what to do once it is loaded: "start",
    // "end", "cfi:<CFI>", "fragment:<id>" or "fraction:<how far, 0 to 1>".
    property int spineIndex: -1
    property string pending: "start"
    property string position: ""
    property double fileFraction: 0
    // The page shown in the spine item, and how many it has; in a scroll,
    // windows.
    property int page: 0
    property int pages: 1
    // How the page shown is laid out: "1" or "2" columns a screen, or
    // "scroll".
    property string shownLayout: ""
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
    readonly property double bookFraction: bookReader.bookFraction(spineIndex, fileFraction)

    readonly property bool fullScreen: visibility === Window.FullScreen
    // In full screen the toolbar shows over the page with the mouse at the
    // top, or after a click in the page's middle, until a page is turned.
    property bool chromeAsked: false
    readonly property bool chromeShown: !fullScreen || chromeAsked || topEdge.containsMouse || barHover.hovered
                                        || tocVisible || settingsPopup.opened

    // The page's colours: Cousoir's theme's, or the reader's pick.
    readonly property var pageThemes: ({
        light: { background: "#ffffff", color: "#1d1d1f", link: "#2f6db5" },
        sepia: { background: "#f5ecd9", color: "#5b4636", link: "#8a5a2b" },
        dark: { background: "#1e1e1e", color: "#d2d2d2", link: "#7fb0e0" },
        black: { background: "#000000", color: "#b8b8b8", link: "#6d9fd0" }
    })
    readonly property var pageColors: pageThemes[readerSettings.pageTheme]
        || { background: Theme.canvas.toString(), color: Theme.text.toString(), link: Theme.accent.toString() }
    readonly property color pageBackground: pageColors.background
    readonly property color pageText: pageColors.color
    readonly property color pageFaint: Theme.mix(pageBackground, pageText, 0.55)
    readonly property bool darkPage: pageBackground.hslLightness < 0.5
    // Calibre's builtin_colors_light and builtin_colors_dark.
    readonly property var highlightColors: darkPage
        ? { yellow: "#906e00", green: "#306f50", blue: "#265589", red: "#a23e5a", purple: "#505088" }
        : { yellow: "#ffeb6b", green: "#c0ed72", blue: "#add8ff", red: "#ffb0ca", purple: "#d9b2ff" }

    // The page laid out: lines of at most the measure picked, in characters
    // of the size picked (half the 16 pixels of the text each, on average),
    // in two columns side by side when the window holds two of nearly that,
    // the page centred and the margins beside it.
    readonly property int sideMargin: Math.round(Math.max(20, Math.min(48, pageArea.width * 0.035)))
    readonly property real characterWidth: 8 * readerSettings.zoom
    readonly property int measure: ({ narrow: 52, medium: 66, wide: 80 })[readerSettings.measure] || 0
    readonly property real columnWidth: measure > 0 ? measure * characterWidth : Number.POSITIVE_INFINITY
    readonly property int columns: !readerSettings.scroll && readerSettings.spreads
        && pageArea.width >= 2 * (0.85 * (measure || 66) * characterWidth + 2 * sideMargin) ? 2 : 1
    readonly property int viewWidth: Math.floor(Math.min(pageArea.width, columns * (columnWidth + 2 * sideMargin)))

    // How fast the reader reads, in bytes of the book's files a second;
    // until measured, about 250 words a minute.
    readonly property double readingRate: readerSettings.readingRate > 0 ? readerSettings.readingRate : 30
    // The page last turned to, and when, for the reading rate.
    property var lastTurn: null

    // Families offered beside the book's own, serif and sans serif: those
    // installed that read as text.
    readonly property var fontChoices: {
        const choices = [{ value: "", label: qsTr("The book's") },
                         { value: "serif", label: qsTr("Serif") },
                         { value: "sans-serif", label: qsTr("Sans serif") }]
        const skipped = /Emoji|Awesome|Nerd Font|Symbol|Mono|Code|Icon|Dingbat|Math|Fangsong|Wingding/
        const seen = {}
        for (const family of Qt.fontFamilies()) {
            const name = family.split(",")[0].replace(/ \[.*\]$/, "")
            if (seen[name] || skipped.test(name)
                || (/^Noto /.test(name) && !/^Noto (Sans|Serif)( Display)?$/.test(name)))
                continue
            seen[name] = true
            choices.push({ value: name, label: name })
        }
        if (readerSettings.fontFamily && !seen[readerSettings.fontFamily] && choices.findIndex(choice => choice.value === readerSettings.fontFamily) < 0)
            choices.push({ value: readerSettings.fontFamily, label: readerSettings.fontFamily })
        return choices
    }

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
        lastTurn = null
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

    // What the page's script lays the page out by.
    function pageSettings() {
        const family = readerSettings.fontFamily
        return {
            background: pageColors.background, color: pageColors.color, link: pageColors.link,
            margin: sideMargin / readerSettings.zoom, columns: columns, scroll: readerSettings.scroll,
            fontFamily: family === "" || family === "serif" || family === "sans-serif" ? family : JSON.stringify(family) + ", serif",
            lineHeight: readerSettings.lineHeight, align: readerSettings.align
        }
    }

    // A page loaded: the reader's script, the layout and the highlights,
    // once; then where to go.
    function prepare() {
        view.runJavaScript(bookReader.script)
        view.runJavaScript("window.cousoir.setup(" + JSON.stringify(pageSettings()) + ")")
        highlightsShown = 0
        applyHighlights(bookReader.highlights.filter(h => h.spineIndex === spineIndex))
        go()
        updateAnchors()
        view.opacity = 1
    }

    // A setting changed: the page laid out again, where it was.
    function relayout() {
        if (spineIndex >= 0 && !view.loading)
            view.runJavaScript("window.cousoir.setup(" + JSON.stringify(pageSettings()) + ")",
                               function() { readerWindow.updateAnchors() })
    }

    function go() {
        if (pending.startsWith("cfi:"))
            view.runJavaScript("window.cousoir.goToCfi(" + JSON.stringify(pending.slice(4)) + ")")
        else if (pending.startsWith("fragment:"))
            view.runJavaScript("window.cousoir.goToFragment(" + JSON.stringify(pending.slice(9)) + ")")
        else if (pending.startsWith("fraction:"))
            view.runJavaScript("window.cousoir.goToFraction(" + Number(pending.slice(9)) + ")")
        else if (pending === "end")
            view.runJavaScript("window.cousoir.goToEnd()")
        else
            view.runJavaScript("window.cousoir.goToStart()")
        hasSelection = false
        chosenHighlight = ""
        view.forceActiveFocus()
    }

    function next() {
        view.runJavaScript("window.cousoir.next()")
        view.forceActiveFocus()
    }

    function previous() {
        view.runJavaScript("window.cousoir.previous()")
        view.forceActiveFocus()
    }

    // Somewhere in the book, by how far into it, from 0 to 1.
    function goToBookFraction(fraction) {
        const place = bookReader.locate(fraction)
        if (place.spineIndex >= 0)
            load(place.spineIndex, "fraction:" + place.fileFraction)
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

    function save() {
        if (position)
            bookReader.savePosition(spineIndex, position, fileFraction)
    }

    function setZoom(value) {
        readerSettings.zoom = Math.round(Math.max(0.5, Math.min(3, value)) * 10) / 10
    }

    function toggleFullScreen() {
        if (fullScreen)
            showNormal()
        else
            showFullScreen()
        chromeAsked = false
    }

    function chapterTitle() {
        return currentEntry >= 0 ? bookReader.toc[currentEntry].title : ""
    }

    // A page turned forward after a while: how fast the reader reads, the
    // last pages counting most.
    function noteTurn(index, newPage, count) {
        const now = Date.now()
        const last = lastTurn
        lastTurn = { spineIndex: index, page: newPage, pages: count, time: now }
        if (!last || !(index === last.spineIndex && newPage === last.page + 1
                       || index === last.spineIndex + 1 && newPage === 0 && last.page === last.pages - 1))
            return
        const seconds = (now - last.time) / 1000
        const size = bookReader.spine[last.spineIndex] ? bookReader.spine[last.spineIndex].size : 0
        if (seconds < 3 || seconds > 300 || size <= 0)
            return
        const rate = Math.max(5, Math.min(500, size / Math.max(1, last.pages) / seconds))
        readerSettings.readingRate = readerSettings.readingRate > 0 ? 0.85 * readerSettings.readingRate + 0.15 * rate : rate
    }

    // "4 min", "1 h 20 min".
    function duration(seconds) {
        const minutes = Math.max(1, Math.round(seconds / 60))
        if (minutes < 60)
            return qsTr("%1 min").arg(minutes)
        return minutes % 60 ? qsTr("%1 h %2 min").arg(Math.floor(minutes / 60)).arg(minutes % 60)
                            : qsTr("%1 h").arg(minutes / 60)
    }

    function chapterTimeLeft() {
        const item = bookReader.spine[spineIndex]
        return item ? duration(item.size * Math.max(0, 1 - fileFraction) / readingRate) : ""
    }

    function bookTimeLeft() {
        return duration(bookReader.totalSize * Math.max(0, 1 - bookFraction) / readingRate)
    }

    visible: false
    width: 900
    height: 1000
    minimumWidth: 480
    minimumHeight: 400
    title: bookReader.title || qsTr("Reader")
    color: pageBackground

    onClosing: {
        save()
        bookReader.close()
        spineIndex = -1
        position = ""
        if (fullScreen)
            showNormal()
    }

    BookReader {
        id: bookReader
        library: readerWindow.library
    }

    // The reader's settings, for every book.
    Settings {
        id: readerSettings
        category: "Reader"
        property real zoom: 1
        // Empty for the book's own, "serif", "sans-serif" or a family.
        property string fontFamily: ""
        // 0 for the book's own.
        property real lineHeight: 0
        // "narrow", "medium", "wide" or "full".
        property string measure: "medium"
        // Empty for the book's own, "start" or "justify".
        property string align: ""
        // "app" for Cousoir's theme, "light", "sepia", "dark" or "black".
        property string pageTheme: "app"
        // Two pages side by side when the window holds them.
        property bool spreads: true
        property bool scroll: false
        property double readingRate: 0
    }

    // The page laid out again when a setting or the room changes.
    readonly property string layoutKey: JSON.stringify(pageSettings())
    onLayoutKeyChanged: Qt.callLater(relayout)

    // Saved as reading goes, a little after each turn.
    Timer {
        id: saveTimer
        interval: 1500
        onTriggered: readerWindow.save()
    }

    Shortcut {
        sequences: ["Esc", StandardKey.Close]
        onActivated: readerWindow.fullScreen ? readerWindow.toggleFullScreen() : readerWindow.close()
    }

    Shortcut {
        sequence: "F11"
        onActivated: readerWindow.toggleFullScreen()
    }

    Item {
        anchors.fill: parent

        Rectangle {
            id: bar

            z: 2
            anchors.left: parent.left
            anchors.right: parent.right
            y: readerWindow.chromeShown ? 0 : -height
            height: Theme.toolbarHeight
            color: Theme.canvas

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
                    selected: readerWindow.tocVisible
                    onClicked: readerWindow.tocVisible = !readerWindow.tocVisible
                }

                Label {
                    Layout.fillWidth: true
                    text: bookReader.title
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

                FlatButton {
                    id: settingsButton
                    objectName: "readerSettingsButton"

                    iconOnly: true
                    text: qsTr("Text and page")
                    icon.source: "assets/icons/type.svg"
                    selected: settingsPopup.opened
                    onClicked: settingsPopup.opened ? settingsPopup.close() : settingsPopup.open()
                }

                FlatButton {
                    objectName: "readerFullScreenButton"

                    iconOnly: true
                    text: readerWindow.fullScreen ? qsTr("Leave full screen (F11)") : qsTr("Full screen (F11)")
                    icon.source: "assets/icons/fullscreen.svg"
                    selected: readerWindow.fullScreen
                    onClicked: readerWindow.toggleFullScreen()
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

        // In full screen, the top edge brings the toolbar.
        MouseArea {
            id: topEdge

            z: 3
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 8
            enabled: readerWindow.fullScreen
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
        }

        RowLayout {
            anchors.fill: parent
            anchors.topMargin: readerWindow.fullScreen ? 0 : bar.height
            spacing: 0

            Rectangle {
                // Its own width: a layout would otherwise take its lists' fill.
                Layout.preferredWidth: Math.round(30 * Theme.charWidth)
                Layout.maximumWidth: Layout.preferredWidth
                Layout.fillWidth: false
                Layout.fillHeight: true
                visible: readerWindow.tocVisible
                color: Theme.canvas

                ColumnLayout {
                    anchors.fill: parent
                    anchors.topMargin: readerWindow.fullScreen ? bar.height : 0
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

                                // A Kobo's, made on it.
                                Label {
                                    Layout.fillWidth: true
                                    visible: mark.modelData.sourceName !== ""
                                    text: mark.modelData.sourceName
                                    color: Theme.textFaint
                                    font.pointSize: Theme.fontPointSizeCaption
                                    elide: Text.ElideRight
                                }
                            }
                            background: Rectangle {
                                color: mark.hovered ? Theme.hover : "transparent"
                            }
                        }

                        ScrollBar.vertical: ThinScrollBar {}
                    }

                    ListView {
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
            }

            Divider {
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                visible: readerWindow.tocVisible
            }

            Rectangle {
                id: pageArea
                objectName: "readerPage"

                Layout.fillWidth: true
                Layout.fillHeight: true
                color: readerWindow.pageBackground

                // Beside the page, as on it: a click on either side turns
                // the page, the wheel too.
                MouseArea {
                    property double wheelLock: 0

                    anchors.fill: parent
                    onClicked: function(mouse) {
                        if (mouse.x < pageArea.width / 2)
                            readerWindow.previous()
                        else
                            readerWindow.next()
                    }
                    onWheel: function(wheel) {
                        const now = Date.now()
                        if (now < wheelLock || wheel.angleDelta.y === 0)
                            return
                        wheelLock = now + 250
                        if (wheel.angleDelta.y < 0)
                            readerWindow.next()
                        else
                            readerWindow.previous()
                    }
                }

                WebEngineView {
                    id: view
                    objectName: "readerView"

                    x: Math.round((pageArea.width - width) / 2)
                    width: readerWindow.viewWidth
                    anchors.top: parent.top
                    anchors.bottom: footer.top
                    backgroundColor: readerWindow.pageBackground
                    zoomFactor: readerSettings.zoom
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
                            if (event.page !== readerWindow.page || readerWindow.lastTurn === null
                                || readerWindow.lastTurn.spineIndex !== readerWindow.spineIndex) {
                                readerWindow.noteTurn(readerWindow.spineIndex, event.page, event.pages)
                                readerWindow.chromeAsked = false
                            }
                            readerWindow.page = event.page
                            readerWindow.pages = event.pages
                            readerWindow.shownLayout = event.layout
                            saveTimer.restart()
                        } else if (event.event === "next") {
                            readerWindow.load(readerWindow.spineIndex + 1, "start")
                        } else if (event.event === "previous") {
                            readerWindow.load(readerWindow.spineIndex - 1, "end")
                        } else if (event.event === "layout") {
                            readerWindow.updateAnchors()
                        } else if (event.event === "menu") {
                            if (readerWindow.fullScreen)
                                readerWindow.chromeAsked = !readerWindow.chromeAsked
                        } else if (event.event === "selection") {
                            readerWindow.hasSelection = !event.empty
                            if (!event.empty)
                                readerWindow.chosenHighlight = ""
                        } else if (event.event === "highlight") {
                            readerWindow.chosenHighlight = event.uuid
                        } else if (event.event === "key") {
                            if (event.key === "Escape" && readerWindow.chosenHighlight)
                                readerWindow.chosenHighlight = ""
                            else if (event.key === "Escape" && readerWindow.fullScreen)
                                readerWindow.toggleFullScreen()
                            else if (event.key === "Escape")
                                readerWindow.close()
                            else if (event.key === "F11")
                                readerWindow.toggleFullScreen()
                            else if (!event.ctrl && (event.key === "h" || event.key === "H") && readerWindow.hasSelection)
                                readerWindow.highlightSelection()
                            else if (event.key === "Delete" && readerWindow.chosenHighlight)
                                readerWindow.removeChosenHighlight()
                            else if (!event.ctrl && (event.key === "t" || event.key === "T"))
                                readerWindow.tocVisible = !readerWindow.tocVisible
                            else if (event.ctrl && (event.key === "+" || event.key === "="))
                                readerWindow.setZoom(readerSettings.zoom + 0.1)
                            else if (event.ctrl && event.key === "-")
                                readerWindow.setZoom(readerSettings.zoom - 0.1)
                            else if (event.ctrl && event.key === "0")
                                readerWindow.setZoom(1)
                            else if (event.ctrl && (event.key === "w" || event.key === "W"))
                                readerWindow.close()
                        }
                    }
                }

                // The chapter, how far into it and the book, and the time
                // left in each at the reader's pace; the book's progress
                // above them, a click on it going there.
                Item {
                    id: footer

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: Theme.statusHeight + 8

                    Rectangle {
                        id: track

                        x: view.x + readerWindow.sideMargin
                        width: Math.max(0, view.width - 2 * readerWindow.sideMargin)
                        anchors.top: parent.top
                        height: scrubber.containsMouse ? 4 : 2
                        color: Theme.withAlpha(readerWindow.pageText, 0.1)

                        Rectangle {
                            width: parent.width * readerWindow.bookFraction
                            height: parent.height
                            color: readerWindow.pageFaint
                        }
                    }

                    MouseArea {
                        id: scrubber
                        objectName: "readerProgressBar"

                        x: track.x
                        width: track.width
                        height: 12
                        anchors.verticalCenter: track.verticalCenter
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: function(mouse) { readerWindow.goToBookFraction(mouse.x / width) }

                        ToolTip.visible: containsMouse
                        ToolTip.delay: 300
                        ToolTip.text: qsTr("Go to %1 %").arg(Math.round(100 * Math.max(0, Math.min(1, mouseX / width))))
                    }

                    RowLayout {
                        x: track.x
                        width: track.width
                        anchors.bottom: parent.bottom
                        height: Theme.statusHeight
                        spacing: Theme.gutter * 2

                        Label {
                            Layout.fillWidth: true
                            text: readerWindow.chapterTitle()
                            color: readerWindow.pageFaint
                            font.pointSize: Theme.fontPointSizeCaption
                            elide: Text.ElideRight
                        }

                        Label {
                            objectName: "readerChapterProgress"

                            visible: readerWindow.spineIndex >= 0
                            text: qsTr("%1 / %2  ·  %3 left in chapter").arg(readerWindow.page + 1).arg(readerWindow.pages)
                                                                          .arg(readerWindow.chapterTimeLeft())
                            color: readerWindow.pageFaint
                            font.pointSize: Theme.fontPointSizeCaption
                        }

                        Label {
                            objectName: "readerProgress"

                            text: qsTr("%1 %  ·  %2 left").arg(Math.round(readerWindow.bookFraction * 100))
                                                          .arg(readerWindow.bookTimeLeft())
                            color: readerWindow.pageFaint
                            font.pointSize: Theme.fontPointSizeCaption
                        }
                    }
                }
            }
        }
    }

    // A row of choices, the one picked marked.
    component Choices: Flow {
        id: choices

        // { value, label }
        property var options: []
        property var value
        signal picked(var value)

        Layout.fillWidth: true
        spacing: Math.round(6 * Theme.spacingScale)

        Repeater {
            model: choices.options

            delegate: FlatButton {
                required property var modelData

                objectName: choices.objectName + "_" + modelData.value
                variant: "outline"
                selected: choices.value === modelData.value
                text: modelData.label
                onClicked: choices.picked(modelData.value)
            }
        }
    }

    // The text and the page, as the reader likes them.
    Popup {
        id: settingsPopup
        objectName: "readerSettings"

        x: readerWindow.width - width - Theme.gutter
        y: bar.y + bar.height
        width: Math.min(readerWindow.width - 2 * Theme.gutter, Math.round(44 * Theme.charWidth))
        padding: Theme.panePadding
        onClosed: view.forceActiveFocus()

        background: Rectangle {
            color: Theme.canvas
            border.width: 1
            border.color: Theme.controlBorder
        }

        contentItem: ColumnLayout {
            spacing: Theme.gutter

            HeadingLabel {
                text: qsTr("Text size")
            }

            Row {
                spacing: Math.round(6 * Theme.spacingScale)

                FlatButton {
                    variant: "outline"
                    text: qsTr("A−")
                    tip: qsTr("Smaller text (Ctrl+Minus)")
                    enabled: readerSettings.zoom > 0.5
                    onClicked: readerWindow.setZoom(readerSettings.zoom - 0.1)
                }

                FlatButton {
                    objectName: "readerZoomReset"

                    variant: "outline"
                    text: qsTr("%1 %").arg(Math.round(readerSettings.zoom * 100))
                    tip: qsTr("The book's size (Ctrl+0)")
                    onClicked: readerWindow.setZoom(1)
                }

                FlatButton {
                    variant: "outline"
                    text: qsTr("A+")
                    tip: qsTr("Larger text (Ctrl+Plus)")
                    enabled: readerSettings.zoom < 3
                    onClicked: readerWindow.setZoom(readerSettings.zoom + 0.1)
                }
            }

            HeadingLabel {
                Layout.topMargin: Theme.gutter
                text: qsTr("Font")
            }

            ChoiceBox {
                objectName: "readerFont"

                Layout.fillWidth: true
                model: readerWindow.fontChoices
                currentIndex: Math.max(0, readerWindow.fontChoices.findIndex(choice => choice.value === readerSettings.fontFamily))
                onActivated: function(index) { readerSettings.fontFamily = readerWindow.fontChoices[index].value }
            }

            HeadingLabel {
                Layout.topMargin: Theme.gutter
                text: qsTr("Line spacing")
            }

            Choices {
                objectName: "readerLineHeight"
                options: [{ value: 0, label: qsTr("The book's") }, { value: 1.2, label: "1.2" }, { value: 1.4, label: "1.4" },
                          { value: 1.6, label: "1.6" }, { value: 1.8, label: "1.8" }]
                value: readerSettings.lineHeight
                onPicked: function(value) { readerSettings.lineHeight = value }
            }

            HeadingLabel {
                Layout.topMargin: Theme.gutter
                text: qsTr("Line length")
            }

            Choices {
                objectName: "readerMeasure"
                options: [{ value: "narrow", label: qsTr("Short") }, { value: "medium", label: qsTr("Medium") },
                          { value: "wide", label: qsTr("Long") }, { value: "full", label: qsTr("The window's") }]
                value: readerSettings.measure
                onPicked: function(value) { readerSettings.measure = value }
            }

            HeadingLabel {
                Layout.topMargin: Theme.gutter
                text: qsTr("Alignment")
            }

            Choices {
                objectName: "readerAlign"
                options: [{ value: "", label: qsTr("The book's") }, { value: "start", label: qsTr("Ragged") },
                          { value: "justify", label: qsTr("Justified") }]
                value: readerSettings.align
                onPicked: function(value) { readerSettings.align = value }
            }

            HeadingLabel {
                Layout.topMargin: Theme.gutter
                text: qsTr("Page")
            }

            Flow {
                Layout.fillWidth: true
                spacing: Math.round(6 * Theme.spacingScale)

                Repeater {
                    model: [{ value: "app", label: qsTr("Cousoir's") }, { value: "light", label: qsTr("Light") },
                            { value: "sepia", label: qsTr("Sepia") }, { value: "dark", label: qsTr("Dark") },
                            { value: "black", label: qsTr("Black") }]

                    delegate: Button {
                        id: swatch

                        required property var modelData
                        readonly property var colors: readerWindow.pageThemes[modelData.value]
                            || { background: Theme.canvas.toString(), color: Theme.text.toString() }

                        objectName: "readerTheme_" + modelData.value
                        implicitWidth: Math.max(Theme.rowHeight * 2, swatchLabel.implicitWidth + 2 * Theme.controlInset)
                        implicitHeight: Theme.rowHeight * 1.4
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: modelData.label
                        onClicked: readerSettings.pageTheme = modelData.value

                        contentItem: Label {
                            id: swatchLabel

                            text: swatch.modelData.label
                            color: swatch.colors.color
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            font.weight: readerSettings.pageTheme === swatch.modelData.value ? Font.Bold : Font.Normal
                        }

                        background: Rectangle {
                            color: swatch.colors.background
                            border.width: readerSettings.pageTheme === swatch.modelData.value || swatch.visualFocus ? 2 : 1
                            border.color: readerSettings.pageTheme === swatch.modelData.value || swatch.visualFocus
                                ? Theme.accent : Theme.controlBorder
                        }
                    }
                }
            }

            Choices {
                objectName: "readerLayout"
                options: [{ value: "single", label: qsTr("One page") }, { value: "spreads", label: qsTr("Two pages") },
                          { value: "scroll", label: qsTr("Scroll") }]
                value: readerSettings.scroll ? "scroll" : readerSettings.spreads ? "spreads" : "single"
                onPicked: function(value) {
                    readerSettings.scroll = value === "scroll"
                    if (value !== "scroll")
                        readerSettings.spreads = value === "spreads"
                }
            }

            Label {
                Layout.fillWidth: true
                visible: readerSettings.spreads && !readerSettings.scroll
                text: qsTr("Two pages side by side when the window is wide enough.")
                color: Theme.textMuted
                font.pointSize: Theme.fontPointSizeCaption
                wrapMode: Text.WordWrap
            }
        }
    }
}
