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
    // The sidebar's list: "toc", "highlights" or "search".
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
    // The highlight clicked, to colour, note or remove.
    property string chosenHighlight: ""
    // Where the selection and the highlight clicked are on the page, in the
    // page's area: { x, y, width, height }; null when not on the page shown.
    property var selectionRect: null
    property var highlightRect: null
    // The highlight whose note is being written.
    property string noteFor: ""
    // The titles of the bookmarks on the page shown.
    property var pageBookmarks: []
    // The book read aloud; null when Cousoir cannot.
    readonly property var speech: bookReader.speech
    readonly property bool speaking: speech !== null && speech.state !== "stopped"
    // Reading aloud on from where a link or an entry of the contents goes.
    property bool speakAfterJump: false
    // A field has the keys: a search, a bookmark's title, a note.
    readonly property bool editingText: activeFocusItem !== null && activeFocusItem.cursorPosition !== undefined
    // The spine item and page the bookmarks of the page were found for.
    property string bookmarksFor: ""
    // The search's place shown, in bookReader.searchResults; -1 for none.
    property int searchIndex: -1
    // Where reading was before a link, a search's place or an entry of the
    // contents took it elsewhere, for Back; and where Back left, for
    // Forward: { spineIndex, cfi }.
    property var backStack: []
    property var forwardStack: []
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
    onHighlightColorsChanged: {
        for (const highlight of bookReader.highlights.filter(h => h.spineIndex === spineIndex))
            restyleHighlight(highlight)
    }
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
        // Nothing of the book shown before kept for this one.
        spineIndex = -1
        position = ""
        fileFraction = 0
        page = 0
        pageBookmarks = []
        bookmarksFor = ""
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
        backStack = []
        forwardStack = []
        searchIndex = -1
        load(start.spineIndex || 0, start.cfi ? "cfi:" + start.cfi : "start")
        return true
    }

    // Somewhere else in the book, where reading was kept for Back.
    function jump(index, action) {
        if (speaking) {
            speech.stop()
            speakAfterJump = true
        }
        if (position) {
            backStack = backStack.concat([{ spineIndex: spineIndex, cfi: position }]).slice(-50)
            forwardStack = []
        }
        load(index, action)
    }

    function goBack() {
        if (!backStack.length)
            return
        const to = backStack[backStack.length - 1]
        backStack = backStack.slice(0, -1)
        if (position)
            forwardStack = forwardStack.concat([{ spineIndex: spineIndex, cfi: position }])
        load(to.spineIndex, "cfi:" + to.cfi)
    }

    function goForward() {
        if (!forwardStack.length)
            return
        const to = forwardStack[forwardStack.length - 1]
        forwardStack = forwardStack.slice(0, -1)
        if (position)
            backStack = backStack.concat([{ spineIndex: spineIndex, cfi: position }])
        load(to.spineIndex, "cfi:" + to.cfi)
    }

    // The search's place `index` shown and marked.
    function showResult(index) {
        const results = bookReader.searchResults
        if (!results.length)
            return
        // Back returns where reading was, not to each place seen.
        const first = searchIndex < 0
        searchIndex = (index + results.length) % results.length
        const result = results[searchIndex]
        const action = "cfi:epubcfi(/" + 2 * (result.spineIndex + 1) + result.start + ")"
        if (first)
            jump(result.spineIndex, action)
        else
            load(result.spineIndex, action)
    }

    // Searches the book, or with the same words goes on to the next place.
    function find(query) {
        if (query === bookReader.searchQuery && bookReader.searchResults.length) {
            showResult(searchIndex + 1)
            return
        }
        searchIndex = -1
        bookReader.search(query)
    }

    function openSearch() {
        tocVisible = true
        sidebar = "search"
        searchField.forceActiveFocus()
        searchField.selectAll()
    }

    // The search's places on the page marked, the one shown more.
    function markSearch() {
        if (spineIndex < 0 || view.loading)
            return
        const marks = []
        let current = -1
        const results = bookReader.searchResults
        for (let i = 0; i < results.length; ++i) {
            if (results[i].spineIndex !== spineIndex)
                continue
            if (i === searchIndex)
                current = marks.length
            marks.push({ start: results[i].start, end: results[i].end })
        }
        view.runJavaScript("window.cousoir.markSearch(" + JSON.stringify(marks) + "," + current + ")")
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
        // Until the new page tells where it is: a place of the old one would
        // be read as one of this one.
        position = ""
        page = 0
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
        selectionRect = null
        highlightRect = null
        markSearch()
        markSpoken()
        if (!searchField.activeFocus)
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
            jump(place.spineIndex, "fraction:" + place.fileFraction)
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
        const marks = list.map(h => ({ uuid: h.uuid, start: h.start, end: h.end, note: !!h.notes,
                                       color: highlightColors[h.which] || highlightColors.yellow }))
        if (marks.length)
            view.runJavaScript("window.cousoir.applyHighlights(" + JSON.stringify(marks) + ")",
                               function(count) { readerWindow.highlightsShown += count || 0 })
    }

    // A rectangle of the page's script, in the page area.
    function pageRect(rect) {
        const zoom = readerSettings.zoom
        return rect ? { x: view.x + rect.x * zoom, y: view.y + rect.y * zoom, width: rect.width * zoom,
                        height: rect.height * zoom } : null
    }

    function highlightOf(uuid) {
        return uuid ? bookReader.highlights.find(h => h.uuid === uuid) || null : null
    }

    // The selection made a highlight in Calibre's colour `which`, the last
    // one picked by default, kept where Calibre's viewer keeps its own.
    function highlightSelection(which) {
        makeHighlight(which, null)
    }

    // As highlightSelection(); `done` then has its id.
    function makeHighlight(which, done) {
        const colour = which || readerSettings.highlightColor
        readerSettings.highlightColor = colour
        view.runJavaScript("JSON.stringify(window.cousoir.selection())", function(result) {
            const selected = result ? JSON.parse(result) : null
            if (!selected)
                return
            const uuid = bookReader.addHighlight(spineIndex, selected.start, selected.end, selected.text, colour)
            if (!uuid)
                return
            applyHighlights([{ uuid: uuid, start: selected.start, end: selected.end, which: colour }])
            view.runJavaScript("window.cousoir.clearSelection()")
            hasSelection = false
            selectionRect = null
            if (done)
                done(uuid)
        })
    }

    // A highlight shown again as it now is: its colour, and its note.
    function restyle(uuid) {
        const highlight = highlightOf(uuid)
        if (highlight)
            restyleHighlight(highlight)
    }

    function restyleHighlight(highlight) {
        view.runJavaScript("window.cousoir.restyleHighlight(" + JSON.stringify(highlight.uuid) + ","
                           + JSON.stringify(highlightColors[highlight.which] || highlightColors.yellow) + ","
                           + !!highlight.notes + ")")
    }

    function recolor(uuid, which) {
        readerSettings.highlightColor = which
        bookReader.setHighlightStyle(uuid, which)
        restyle(uuid)
    }

    function editNote(uuid) {
        noteFor = uuid
        noteEditor.open()
    }

    function saveNote(text) {
        bookReader.setHighlightNotes(noteFor, text)
        restyle(noteFor)
    }

    // A note on the selection: highlighted first, then its note written.
    function noteSelection() {
        makeHighlight("", function(uuid) { readerWindow.editNote(uuid) })
    }

    function searchSelection() {
        view.runJavaScript("window.getSelection().toString()", function(text) {
            const query = (text || "").replace(/\s+/g, " ").trim()
            if (!query)
                return
            readerWindow.openSearch()
            searchField.text = query
            readerWindow.find(query)
        })
    }

    function removeChosenHighlight() {
        if (!chosenHighlight)
            return
        bookReader.removeHighlight(chosenHighlight)
        view.runJavaScript("window.cousoir.removeHighlight(" + JSON.stringify(chosenHighlight) + ")")
        chosenHighlight = ""
        highlightRect = null
        highlightsShown = Math.max(0, highlightsShown - 1)
    }

    // Reading aloud from the page shown, or no more.
    function toggleSpeech() {
        if (!speech || spineIndex < 0)
            return
        if (speaking)
            speech.stop()
        else
            speech.start(spineIndex, position.replace(/^epubcfi\(\/\d+/, "").replace(/\)$/, ""))
    }

    // The sentence read marked on its page, turned to; another file's
    // loaded.
    function showSpoken() {
        const sentence = speech ? speech.sentence : null
        if (sentence && sentence.start !== undefined && sentence.spineIndex !== spineIndex) {
            load(sentence.spineIndex, "cfi:epubcfi(/" + 2 * (sentence.spineIndex + 1) + sentence.start + ")")
            return
        }
        markSpoken()
    }

    function markSpoken() {
        if (spineIndex < 0 || view.loading)
            return
        const sentence = speech ? speech.sentence : null
        const place = sentence && sentence.spineIndex === spineIndex ? { start: sentence.start, end: sentence.end } : null
        view.runJavaScript("window.cousoir.speak(" + JSON.stringify(place) + ")")
    }

    // Which bookmarks the page shown has.
    function updatePageBookmarks() {
        bookmarksFor = spineIndex + ":" + page
        const here = bookReader.bookmarks.filter(bookmark => bookmark.spineIndex === spineIndex)
        if (!here.length || view.loading || !position) {
            pageBookmarks = []
            return
        }
        const shown = page
        const cfis = here.map(bookmark => bookmark.cfi.replace(/^epubcfi\(\/\d+/, "").replace(/\)$/, ""))
        view.runJavaScript("window.cousoir.pagesOfCfis(" + JSON.stringify(cfis) + ")", function(pages) {
            readerWindow.pageBookmarks = here.filter((bookmark, i) => pages && pages[i] === shown)
                                             .map(bookmark => bookmark.title)
        })
    }

    // A bookmark at the page, named after its chapter; or the page's
    // bookmarks removed.
    function toggleBookmark() {
        if (pageBookmarks.length) {
            for (const title of pageBookmarks)
                bookReader.removeBookmark(title)
            return
        }
        if (!position)
            return
        const base = chapterTitle() || bookReader.title || qsTr("Bookmark")
        const titles = bookReader.bookmarks.map(bookmark => bookmark.title)
        let title = base
        for (let n = 2; titles.includes(title); ++n)
            title = base + " (" + n + ")"
        bookReader.addBookmark(title, position)
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

    // A window of its own, not one over the library's: the library's
    // shortcuts, Ctrl+F among them, stay its own.
    transientParent: null
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
        // The colour of the last highlight made or coloured.
        property string highlightColor: "yellow"
        // Reading aloud: the engine and the voice, empty for the system's,
        // and the speed, from -1 to 1.
        property string speechEngine: ""
        property string speechVoice: ""
        property real speechRate: 0
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

    // The window's keys, the page's focus or not; letters, which would type
    // in a field, are read from the page's key events (see
    // onJavaScriptConsoleMessage). Each key once: a key in two sequences
    // would be ambiguous, and do nothing.
    Shortcut {
        sequences: ["Esc"]
        // A field's own Esc first.
        enabled: !readerWindow.editingText
        onActivated: {
            if (readerWindow.chosenHighlight) {
                readerWindow.chosenHighlight = ""
                readerWindow.highlightRect = null
            } else if (readerWindow.fullScreen) {
                readerWindow.toggleFullScreen()
            } else {
                readerWindow.close()
            }
        }
    }

    Shortcut {
        sequences: [StandardKey.Close]
        onActivated: readerWindow.close()
    }

    Shortcut {
        sequences: ["Ctrl++", "Ctrl+="]
        onActivated: readerWindow.setZoom(readerSettings.zoom + 0.1)
    }

    Shortcut {
        sequence: "Ctrl+-"
        onActivated: readerWindow.setZoom(readerSettings.zoom - 0.1)
    }

    Shortcut {
        sequence: "Ctrl+0"
        onActivated: readerWindow.setZoom(1)
    }

    Shortcut {
        sequence: "F11"
        onActivated: readerWindow.toggleFullScreen()
    }

    Shortcut {
        sequences: [StandardKey.Find]
        onActivated: readerWindow.openSearch()
    }

    Shortcut {
        sequences: [StandardKey.FindNext]
        enabled: bookReader.searchResults.length > 0
        onActivated: readerWindow.showResult(readerWindow.searchIndex + 1)
    }

    Shortcut {
        sequences: [StandardKey.FindPrevious]
        enabled: bookReader.searchResults.length > 0
        onActivated: readerWindow.showResult(readerWindow.searchIndex - 1)
    }

    Shortcut {
        sequence: "Ctrl+D"
        onActivated: readerWindow.toggleBookmark()
    }

    Shortcut {
        sequences: [StandardKey.Back]
        onActivated: readerWindow.goBack()
    }

    Shortcut {
        sequences: [StandardKey.Forward]
        onActivated: readerWindow.goForward()
    }

    Connections {
        target: bookReader

        function onBookmarksChanged() {
            readerWindow.updatePageBookmarks()
        }
    }

    Connections {
        target: readerWindow.speech
        ignoreUnknownSignals: true

        function onSentenceChanged() {
            readerWindow.showSpoken()
        }

        function onWordChanged() {
            if (readerWindow.speech.word && readerWindow.speech.sentence.spineIndex === readerWindow.spineIndex
                && !view.loading)
                view.runJavaScript("window.cousoir.follow(" + JSON.stringify(readerWindow.speech.word) + ")")
        }
    }

    // The voice, its speed and the engine, kept for every book.
    Binding {
        target: readerWindow.speech
        property: "engine"
        value: readerSettings.speechEngine
        when: readerWindow.speech !== null
    }

    Binding {
        target: readerWindow.speech
        property: "voice"
        value: readerSettings.speechVoice
        when: readerWindow.speech !== null
    }

    Binding {
        target: readerWindow.speech
        property: "rate"
        value: readerSettings.speechRate
        when: readerWindow.speech !== null
    }

    // A new search's places: the first from the page read on shown.
    Connections {
        target: bookReader

        function onSearchChanged() {
            if (bookReader.searching || !bookReader.searchResults.length) {
                readerWindow.markSearch()
                return
            }
            const results = bookReader.searchResults
            const here = []
            for (let i = 0; i < results.length; ++i) {
                if (results[i].spineIndex === readerWindow.spineIndex)
                    here.push(i)
            }
            const later = results.findIndex(result => result.spineIndex > readerWindow.spineIndex)
            const fallback = later >= 0 ? later : 0
            if (!here.length || view.loading) {
                readerWindow.showResult(here.length ? here[0] : fallback)
                return
            }
            const query = bookReader.searchQuery
            view.runJavaScript("window.cousoir.pagesOfCfis(" + JSON.stringify(here.map(i => results[i].start)) + ")",
                               function(pages) {
                if (bookReader.searchQuery !== query)
                    return
                const at = (pages || []).findIndex(page => page >= readerWindow.page)
                readerWindow.showResult(at >= 0 ? here[at] : fallback)
            })
        }
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

                FlatButton {
                    objectName: "readerBackButton"

                    visible: readerWindow.backStack.length > 0
                    variant: "outline"
                    text: qsTr("Back")
                    tip: qsTr("Back where you were (Alt+Left)")
                    icon.source: "assets/icons/arrow-left.svg"
                    onClicked: readerWindow.goBack()
                }

                Label {
                    Layout.fillWidth: true
                    text: bookReader.title
                    elide: Text.ElideRight
                    color: Theme.textMuted
                }

                FlatButton {
                    objectName: "readerSpeechButton"

                    visible: readerWindow.speech !== null && readerWindow.speech.engines.length > 0
                    enabled: readerWindow.spineIndex >= 0
                    iconOnly: true
                    selected: readerWindow.speaking
                    text: readerWindow.speaking ? qsTr("Stop reading aloud (R)") : qsTr("Read aloud from here (R)")
                    icon.source: "assets/icons/speaker.svg"
                    onClicked: readerWindow.toggleSpeech()
                }

                FlatButton {
                    objectName: "readerBookmarkButton"

                    iconOnly: true
                    enabled: readerWindow.spineIndex >= 0
                    text: readerWindow.pageBookmarks.length ? qsTr("Remove the page's bookmark (Ctrl+D)")
                                                            : qsTr("Bookmark the page (Ctrl+D)")
                    icon.source: readerWindow.pageBookmarks.length ? "assets/icons/bookmark-filled.svg"
                                                                   : "assets/icons/bookmark.svg"
                    onClicked: readerWindow.toggleBookmark()
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
                            objectName: "readerContentsTab"

                            variant: "outline"
                            selected: readerWindow.sidebar === "toc"
                            text: qsTr("Contents")
                            onClicked: readerWindow.sidebar = "toc"
                        }

                        FlatButton {
                            objectName: "readerHighlightsTab"

                            variant: "outline"
                            selected: readerWindow.sidebar === "highlights"
                            text: bookReader.highlights.length
                            tip: qsTr("Highlights and notes")
                            icon.source: "assets/icons/highlighter.svg"
                            onClicked: readerWindow.sidebar = "highlights"
                        }

                        FlatButton {
                            objectName: "readerBookmarksTab"

                            variant: "outline"
                            selected: readerWindow.sidebar === "bookmarks"
                            text: bookReader.bookmarks.length
                            tip: qsTr("Bookmarks")
                            icon.source: "assets/icons/bookmark.svg"
                            onClicked: readerWindow.sidebar = "bookmarks"
                        }

                        FlatButton {
                            objectName: "readerSearchTab"

                            iconOnly: true
                            variant: "outline"
                            selected: readerWindow.sidebar === "search"
                            text: qsTr("Search the book (Ctrl+F)")
                            icon.source: "assets/icons/search.svg"
                            onClicked: readerWindow.openSearch()
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.leftMargin: Theme.gutter
                        Layout.rightMargin: Theme.gutter
                        visible: readerWindow.sidebar === "search"
                        spacing: Theme.gutter

                        SearchField {
                            id: searchField
                            objectName: "readerSearchField"

                            Layout.fillWidth: true
                            placeholderText: qsTr("Search the book")
                            onAccepted: readerWindow.find(text)
                            Keys.onEscapePressed: function(event) {
                                if (text) {
                                    text = ""
                                    bookReader.search("")
                                } else {
                                    view.forceActiveFocus()
                                }
                                event.accepted = true
                            }
                        }

                        Label {
                            objectName: "readerSearchStatus"

                            Layout.fillWidth: true
                            visible: text !== ""
                            text: bookReader.searching ? qsTr("Searching…")
                                : !bookReader.searchQuery ? ""
                                : !bookReader.searchResults.length ? qsTr("Nothing found.")
                                : readerWindow.searchIndex >= 0
                                    ? (bookReader.searchMore ? qsTr("%1 of the first %2 places") : qsTr("%1 of %2 places"))
                                      .arg(readerWindow.searchIndex + 1).arg(bookReader.searchResults.length)
                                : (bookReader.searchMore ? qsTr("The first %1 places") : qsTr("%1 places"))
                                      .arg(bookReader.searchResults.length)
                            color: Theme.textMuted
                            font.pointSize: Theme.fontPointSizeCaption
                            elide: Text.ElideRight
                        }

                        ListView {
                            id: searchList
                            objectName: "readerSearchResults"

                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.leftMargin: -Theme.gutter
                            Layout.rightMargin: -Theme.gutter
                            clip: true
                            model: bookReader.searchResults
                            currentIndex: readerWindow.searchIndex
                            highlightFollowsCurrentItem: true
                            highlightMoveDuration: 0

                            delegate: ItemDelegate {
                                id: result

                                required property var modelData
                                required property int index

                                width: ListView.view.width
                                leftPadding: Theme.gutter
                                rightPadding: Theme.gutter
                                highlighted: result.index === readerWindow.searchIndex
                                onClicked: readerWindow.showResult(result.index)

                                function escaped(text) {
                                    return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
                                }

                                contentItem: ColumnLayout {
                                    spacing: 2

                                    HeadingLabel {
                                        Layout.fillWidth: true
                                        Layout.topMargin: result.index > 0 ? Theme.gutter : 0
                                        visible: result.modelData.newChapter
                                        text: result.modelData.chapter || qsTr("Page %1").arg(result.modelData.spineIndex + 1)
                                        elide: Text.ElideRight
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        textFormat: Text.StyledText
                                        text: result.escaped(result.modelData.before) + "<b>" + result.escaped(result.modelData.text)
                                              + "</b>" + result.escaped(result.modelData.after)
                                        wrapMode: Text.Wrap
                                        maximumLineCount: 3
                                        elide: Text.ElideRight
                                        color: result.highlighted ? Theme.textBright : Theme.text
                                    }
                                }
                                background: Rectangle {
                                    color: result.highlighted ? Theme.selected : result.hovered ? Theme.hover : "transparent"
                                }
                            }

                            ScrollBar.vertical: ThinScrollBar {}
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
                            onClicked: readerWindow.jump(mark.modelData.spineIndex,
                                                         "cfi:epubcfi(/" + 2 * (mark.modelData.spineIndex + 1) + mark.modelData.start + ")")

                            contentItem: ColumnLayout {
                                spacing: 2

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.gutter

                                    HeadingLabel {
                                        Layout.fillWidth: true
                                        text: mark.modelData.chapter
                                        elide: Text.ElideRight
                                    }

                                    // Its note written or changed, from here.
                                    FlatButton {
                                        objectName: "readerHighlightNote"

                                        iconOnly: true
                                        implicitWidth: Theme.rowHeight * 0.8
                                        implicitHeight: Theme.rowHeight * 0.8
                                        opacity: mark.hovered || hovered ? 1 : 0
                                        text: mark.modelData.notes ? qsTr("Change the note") : qsTr("Write a note")
                                        icon.source: "assets/icons/note.svg"
                                        onClicked: readerWindow.editNote(mark.modelData.uuid)
                                    }
                                }

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
                        id: bookmarkList
                        objectName: "readerBookmarks"

                        // The bookmark being renamed.
                        property string renaming: ""

                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: readerWindow.sidebar === "bookmarks"
                        clip: true
                        model: bookReader.bookmarks

                        delegate: ItemDelegate {
                            id: bookmark

                            required property var modelData

                            width: ListView.view.width
                            leftPadding: Theme.gutter
                            rightPadding: Theme.gutter
                            onClicked: readerWindow.jump(bookmark.modelData.spineIndex, "cfi:" + bookmark.modelData.cfi)
                            onDoubleClicked: bookmarkList.renaming = bookmark.modelData.title

                            contentItem: RowLayout {
                                spacing: Theme.gutter

                                Label {
                                    Layout.fillWidth: true
                                    visible: bookmarkList.renaming !== bookmark.modelData.title
                                    text: bookmark.modelData.title
                                    elide: Text.ElideRight
                                    font.weight: readerWindow.pageBookmarks.includes(bookmark.modelData.title) ? Font.Bold : Font.Normal
                                }

                                TextField {
                                    objectName: "readerBookmarkTitle"

                                    Layout.fillWidth: true
                                    visible: bookmarkList.renaming === bookmark.modelData.title
                                    text: bookmark.modelData.title
                                    onVisibleChanged: {
                                        if (visible) {
                                            forceActiveFocus()
                                            selectAll()
                                        }
                                    }
                                    onAccepted: {
                                        bookReader.renameBookmark(bookmark.modelData.title, text)
                                        bookmarkList.renaming = ""
                                    }
                                    Keys.onEscapePressed: function(event) {
                                        bookmarkList.renaming = ""
                                        event.accepted = true
                                    }
                                }

                                FlatButton {
                                    iconOnly: true
                                    implicitWidth: Theme.rowHeight * 0.8
                                    implicitHeight: Theme.rowHeight * 0.8
                                    opacity: bookmark.hovered || hovered ? 1 : 0
                                    text: qsTr("Rename")
                                    icon.source: "assets/icons/note.svg"
                                    onClicked: bookmarkList.renaming = bookmark.modelData.title
                                }

                                FlatButton {
                                    objectName: "readerRemoveBookmark"

                                    iconOnly: true
                                    implicitWidth: Theme.rowHeight * 0.8
                                    implicitHeight: Theme.rowHeight * 0.8
                                    opacity: bookmark.hovered || hovered ? 1 : 0
                                    text: qsTr("Remove")
                                    icon.source: "assets/icons/trash.svg"
                                    onClicked: bookReader.removeBookmark(bookmark.modelData.title)
                                }
                            }
                            background: Rectangle {
                                color: bookmark.hovered ? Theme.hover : "transparent"
                            }
                        }

                        Label {
                            anchors.fill: parent
                            anchors.margins: Theme.gutter
                            visible: !bookReader.bookmarks.length
                            text: qsTr("No bookmarks. Ctrl+D, or the bookmark above, bookmarks the page.")
                            color: Theme.textMuted
                            wrapMode: Text.WordWrap
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
                            onClicked: readerWindow.jump(entry.modelData.spineIndex,
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
                            // Once the navigation refused is over: loading
                            // another page within it fails a check of
                            // Chromium's.
                            Qt.callLater(readerWindow.jump, index,
                                         hash >= 0 ? "fragment:" + decodeURIComponent(url.slice(hash + 1)) : "start")
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
                            if (readerWindow.speakAfterJump) {
                                readerWindow.speakAfterJump = false
                                readerWindow.speech.start(readerWindow.spineIndex, event.cfi)
                            }
                            const turned = event.page !== readerWindow.page
                            readerWindow.page = event.page
                            readerWindow.pages = event.pages
                            readerWindow.shownLayout = event.layout
                            // The bar over a selection or a highlight stays
                            // with the page it was on; in a scroll, with
                            // what was in sight.
                            if (turned || event.layout === "scroll") {
                                readerWindow.selectionRect = null
                                readerWindow.highlightRect = null
                            }
                            if (readerWindow.bookmarksFor !== readerWindow.spineIndex + ":" + event.page)
                                readerWindow.updatePageBookmarks()
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
                            readerWindow.selectionRect = readerWindow.pageRect(event.rect)
                            if (!event.empty) {
                                readerWindow.chosenHighlight = ""
                                readerWindow.highlightRect = null
                            }
                        } else if (event.event === "highlight") {
                            readerWindow.chosenHighlight = event.uuid
                            readerWindow.highlightRect = readerWindow.pageRect(event.rect)
                        } else if (event.event === "key") {
                            // The keys the window's shortcuts have not
                            // taken: those that would type in a field.
                            if (event.ctrl || event.alt)
                                return
                            if ((event.key === "h" || event.key === "H") && readerWindow.hasSelection)
                                readerWindow.highlightSelection()
                            else if ((event.key === "n" || event.key === "N") && readerWindow.chosenHighlight)
                                readerWindow.editNote(readerWindow.chosenHighlight)
                            else if ((event.key === "n" || event.key === "N") && readerWindow.hasSelection)
                                readerWindow.noteSelection()
                            else if (event.key === "r" || event.key === "R")
                                readerWindow.toggleSpeech()
                            else if (event.key === "Delete" && readerWindow.chosenHighlight)
                                readerWindow.removeChosenHighlight()
                            else if (event.key === "t" || event.key === "T")
                                readerWindow.tocVisible = !readerWindow.tocVisible
                        }
                    }
                }

                // Over the selection, or the highlight clicked: Calibre's five
                // colours, a note, copy, and a search of the book for the
                // selection or the highlight removed.
                Rectangle {
                    id: annotationBar
                    objectName: "readerAnnotationBar"

                    readonly property var chosen: readerWindow.highlightOf(readerWindow.chosenHighlight)
                    readonly property var target: chosen ? readerWindow.highlightRect
                        : readerWindow.hasSelection ? readerWindow.selectionRect : null

                    z: 3
                    visible: target !== null && !noteEditor.opened
                    width: barRow.implicitWidth + Theme.gutter
                    height: barRow.implicitHeight + Theme.gutter
                    x: target ? Math.round(Math.max(Theme.gutter, Math.min(pageArea.width - width - Theme.gutter,
                                                                        target.x + (target.width - width) / 2))) : 0
                    // Above, else below.
                    y: !target ? 0 : target.y - height - Theme.gutter >= 0 ? Math.round(target.y - height - Theme.gutter)
                                                                         : Math.round(target.y + target.height + Theme.gutter)
                    color: Theme.canvas
                    border.width: 1
                    border.color: Theme.controlBorder

                    RowLayout {
                        id: barRow

                        anchors.centerIn: parent
                        spacing: Math.round(4 * Theme.spacingScale)

                        Repeater {
                            model: ["yellow", "green", "blue", "red", "purple"]

                            delegate: Button {
                                id: swatch

                                required property string modelData
                                readonly property bool current: annotationBar.chosen ? annotationBar.chosen.which === modelData
                                                                                     : readerSettings.highlightColor === modelData

                                objectName: "readerColor_" + modelData
                                implicitWidth: Theme.rowHeight
                                implicitHeight: Theme.rowHeight
                                hoverEnabled: true
                                focusPolicy: Qt.NoFocus
                                Accessible.name: modelData
                                ToolTip.visible: hovered
                                ToolTip.delay: 600
                                ToolTip.text: annotationBar.chosen ? qsTr("Colour it so") : qsTr("Highlight (H for the colour marked)")
                                onClicked: {
                                    if (annotationBar.chosen)
                                        readerWindow.recolor(annotationBar.chosen.uuid, modelData)
                                    else
                                        readerWindow.highlightSelection(modelData)
                                }

                                contentItem: Item {}
                                background: Item {
                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: parent.width * (swatch.hovered ? 0.72 : 0.62)
                                        height: width
                                        radius: width / 2
                                        color: readerWindow.highlightColors[swatch.modelData]
                                        border.width: swatch.current ? 2 : 1
                                        border.color: swatch.current ? Theme.text : Theme.controlBorder
                                    }
                                }
                            }
                        }

                        Divider {
                            Layout.preferredWidth: 1
                            Layout.preferredHeight: Theme.rowHeight * 0.7
                        }

                        FlatButton {
                            objectName: "readerNoteButton"

                            iconOnly: true
                            focusPolicy: Qt.NoFocus
                            text: annotationBar.chosen && annotationBar.chosen.notes ? qsTr("Change the note (N)") : qsTr("Note (N)")
                            icon.source: "assets/icons/note.svg"
                            onClicked: {
                                if (annotationBar.chosen)
                                    readerWindow.editNote(annotationBar.chosen.uuid)
                                else
                                    readerWindow.noteSelection()
                            }
                        }

                        FlatButton {
                            objectName: "readerCopyButton"

                            iconOnly: true
                            focusPolicy: Qt.NoFocus
                            text: qsTr("Copy")
                            icon.source: "assets/icons/copy.svg"
                            onClicked: {
                                if (annotationBar.chosen)
                                    bookReader.copyText(annotationBar.chosen.text)
                                else
                                    view.triggerWebAction(WebEngineView.Copy)
                            }
                        }

                        FlatButton {
                            objectName: "readerSearchSelection"

                            visible: !annotationBar.chosen
                            iconOnly: true
                            focusPolicy: Qt.NoFocus
                            text: qsTr("Search the book for it")
                            icon.source: "assets/icons/search.svg"
                            onClicked: readerWindow.searchSelection()
                        }

                        FlatButton {
                            objectName: "readerRemoveHighlightButton"

                            visible: annotationBar.chosen !== null
                            iconOnly: true
                            focusPolicy: Qt.NoFocus
                            text: qsTr("Remove the highlight (Del)")
                            icon.source: "assets/icons/trash.svg"
                            onClicked: readerWindow.removeChosenHighlight()
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
                    height: playerRow.visible ? Theme.rowHeight + 10 : Theme.statusHeight + 8

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
                        height: footer.height - track.height
                        spacing: Theme.gutter * 2

                        Label {
                            Layout.fillWidth: true
                            text: readerWindow.chapterTitle()
                            color: readerWindow.pageFaint
                            font.pointSize: Theme.fontPointSizeCaption
                            elide: Text.ElideRight
                        }

                        // While reading aloud: the sentence before or after,
                        // pause, stop, slower or faster.
                        RowLayout {
                            id: playerRow
                            objectName: "readerPlayer"

                            visible: readerWindow.speaking || (readerWindow.speech !== null && readerWindow.speech.error !== "")
                            spacing: Math.round(4 * Theme.spacingScale)

                            Label {
                                visible: readerWindow.speech !== null && readerWindow.speech.error !== "" && !readerWindow.speaking
                                Layout.maximumWidth: Math.round(40 * Theme.charWidth)
                                text: readerWindow.speech ? qsTr("The voice cannot speak: %1").arg(readerWindow.speech.error) : ""
                                color: Theme.danger
                                elide: Text.ElideRight
                            }

                            FlatButton {
                                visible: readerWindow.speaking
                                iconOnly: true
                                focusPolicy: Qt.NoFocus
                                // In the page's colours, as the footer.
                                icon.color: readerWindow.pageText
                                palette.buttonText: readerWindow.pageText
                                text: qsTr("The sentence before")
                                icon.source: "assets/icons/skip-back.svg"
                                onClicked: readerWindow.speech.skip(-1)
                            }

                            FlatButton {
                                objectName: "readerPauseButton"

                                visible: readerWindow.speaking
                                iconOnly: true
                                focusPolicy: Qt.NoFocus
                                // In the page's colours, as the footer.
                                icon.color: readerWindow.pageText
                                palette.buttonText: readerWindow.pageText
                                text: readerWindow.speaking && readerWindow.speech.state === "paused" ? qsTr("Go on") : qsTr("Pause")
                                icon.source: readerWindow.speaking && readerWindow.speech.state === "paused"
                                    ? "assets/icons/play.svg" : "assets/icons/pause.svg"
                                onClicked: readerWindow.speech.state === "paused" ? readerWindow.speech.resume()
                                                                                  : readerWindow.speech.pause()
                            }

                            FlatButton {
                                visible: readerWindow.speaking
                                iconOnly: true
                                focusPolicy: Qt.NoFocus
                                // In the page's colours, as the footer.
                                icon.color: readerWindow.pageText
                                palette.buttonText: readerWindow.pageText
                                text: qsTr("The sentence after")
                                icon.source: "assets/icons/skip-forward.svg"
                                onClicked: readerWindow.speech.skip(1)
                            }

                            FlatButton {
                                iconOnly: true
                                focusPolicy: Qt.NoFocus
                                // In the page's colours, as the footer.
                                icon.color: readerWindow.pageText
                                palette.buttonText: readerWindow.pageText
                                text: qsTr("Stop (R)")
                                icon.source: "assets/icons/stop.svg"
                                onClicked: readerWindow.speech.stop()
                            }

                            Divider {
                                visible: readerWindow.speaking
                                color: Theme.withAlpha(readerWindow.pageText, 0.15)
                                Layout.preferredWidth: 1
                                Layout.preferredHeight: Theme.rowHeight * 0.7
                            }

                            FlatButton {
                                visible: readerWindow.speaking
                                focusPolicy: Qt.NoFocus
                                // In the page's colours, as the footer.
                                icon.color: readerWindow.pageText
                                palette.buttonText: readerWindow.pageText
                                text: qsTr("Slower")
                                enabled: readerSettings.speechRate > -1
                                onClicked: readerSettings.speechRate = Math.max(-1, Math.round(readerSettings.speechRate * 10 - 1) / 10)
                            }

                            Label {
                                visible: readerWindow.speaking
                                text: readerSettings.speechRate === 0 ? qsTr("Normal")
                                    : (readerSettings.speechRate > 0 ? "+" : "−") + Math.abs(Math.round(readerSettings.speechRate * 10))
                                color: readerWindow.pageFaint
                            }

                            FlatButton {
                                visible: readerWindow.speaking
                                focusPolicy: Qt.NoFocus
                                // In the page's colours, as the footer.
                                icon.color: readerWindow.pageText
                                palette.buttonText: readerWindow.pageText
                                text: qsTr("Faster")
                                enabled: readerSettings.speechRate < 1
                                onClicked: readerSettings.speechRate = Math.min(1, Math.round(readerSettings.speechRate * 10 + 1) / 10)
                            }
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
        onOpened: {
            if (readerWindow.speech)
                readerWindow.speech.loadVoices()
        }
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

            HeadingLabel {
                Layout.topMargin: Theme.gutter
                visible: readerWindow.speech !== null
                text: qsTr("Reading aloud")
            }

            ChoiceBox {
                objectName: "readerSpeechEngine"

                readonly property var choices: [{ value: "", label: qsTr("The system's voice") }].concat(
                    readerWindow.speech ? readerWindow.speech.engines.map(engine => ({ value: engine, label: engine })) : [])

                Layout.fillWidth: true
                visible: readerWindow.speech !== null && readerWindow.speech.engines.length > 1
                model: choices
                currentIndex: Math.max(0, choices.findIndex(choice => choice.value === readerSettings.speechEngine))
                onActivated: function(index) {
                    readerSettings.speechVoice = ""
                    readerSettings.speechEngine = choices[index].value
                    readerWindow.speech.loadVoices()
                }
            }

            ChoiceBox {
                objectName: "readerSpeechVoice"

                readonly property var choices: [{ value: "", label: qsTr("The engine's voice") }].concat(
                    readerWindow.speech ? readerWindow.speech.voices.map(voice => ({ value: voice.name, label: voice.label })) : [])

                Layout.fillWidth: true
                visible: readerWindow.speech !== null
                model: choices
                currentIndex: Math.max(0, choices.findIndex(choice => choice.value === readerSettings.speechVoice))
                onActivated: function(index) { readerSettings.speechVoice = choices[index].value }
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

    // A highlight's note, written or changed.
    Popup {
        id: noteEditor
        objectName: "readerNoteEditor"

        readonly property var highlight: readerWindow.highlightOf(readerWindow.noteFor)

        function save() {
            readerWindow.saveNote(noteText.text)
            close()
        }

        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 3)
        width: Math.min(readerWindow.width - 2 * Theme.gutter, Math.round(56 * Theme.charWidth))
        modal: true
        focus: true
        // A click beside it would lose the note: Esc or Cancel leave it.
        closePolicy: Popup.CloseOnEscape
        padding: Theme.panePadding
        onOpened: {
            noteText.text = highlight ? highlight.notes : ""
            noteText.forceActiveFocus()
            noteText.cursorPosition = noteText.length
        }
        onClosed: view.forceActiveFocus()

        background: Rectangle {
            color: Theme.canvas
            border.width: 1
            border.color: Theme.controlBorder
        }

        contentItem: ColumnLayout {
            spacing: Theme.gutter

            HeadingLabel {
                text: qsTr("Note")
            }

            Label {
                Layout.fillWidth: true
                text: noteEditor.highlight ? noteEditor.highlight.text : ""
                wrapMode: Text.Wrap
                maximumLineCount: 4
                elide: Text.ElideRight
                color: Theme.textMuted
                font.italic: true
                leftPadding: Theme.gutter
                background: Rectangle {
                    width: 3
                    height: parent.height
                    color: noteEditor.highlight ? readerWindow.highlightColors[noteEditor.highlight.which]
                                                  || readerWindow.highlightColors.yellow : "transparent"
                }
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 8 * Theme.rowHeight

                TextArea {
                    id: noteText
                    objectName: "readerNoteText"

                    wrapMode: TextEdit.Wrap
                    color: Theme.text
                    placeholderText: qsTr("Your note")
                    placeholderTextColor: Theme.textFaint
                    selectionColor: Theme.textSelection
                    selectedTextColor: Theme.textBright
                    Keys.onPressed: function(event) {
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                            && (event.modifiers & Qt.ControlModifier)) {
                            noteEditor.save()
                            event.accepted = true
                        }
                    }

                    background: Rectangle {
                        color: Theme.fill
                        border.width: 1
                        border.color: noteText.activeFocus ? Theme.accent : Theme.controlBorder
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gutter

                Item {
                    Layout.fillWidth: true
                }

                FlatButton {
                    variant: "outline"
                    text: qsTr("Cancel")
                    onClicked: noteEditor.close()
                }

                FlatButton {
                    objectName: "readerSaveNote"

                    variant: "primary"
                    text: qsTr("Save")
                    tip: qsTr("Ctrl+Enter")
                    onClicked: noteEditor.save()
                }
            }
        }
    }
}
