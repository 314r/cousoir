import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Controls.impl
import QtQuick.Dialogs
import QtQuick.Layouts
import QtCore

import Cousoir

ApplicationWindow {
    id: window

    // The details pane hides on narrow windows unless the toggle forces it.
    readonly property bool wide: width >= 960
    property bool detailsForced: false
    readonly property bool detailsVisible: layoutSettings.showDetails && (wide || detailsForced)
    readonly property var book: library.currentBook
    readonly property var filter: library.filter
    // The section picked in the sidebar; a browse section's item and a book
    // open over it.
    property string section: "books"
    readonly property bool deviceSection: section.startsWith("device:")
    readonly property bool facetShown: section !== "books" && section !== "trash" && !deviceSection
        && !session.filterOpen && !session.bookOpen
    readonly property bool trashShown: section === "trash" && !session.bookOpen
    readonly property bool deviceShown: deviceSection && !session.bookOpen
    readonly property bool booksShown: !facetShown && !trashShown && !deviceShown && !session.bookOpen
    // A message to show in place of the key hints for a while: an error,
    // unless noticeIsError says otherwise.
    property string notice: ""
    property bool noticeIsError: true
    // Books being converted to be sent: { bookId, root }.
    property var pendingSends: []

    visible: true
    width: 1240
    height: 820
    minimumWidth: 720
    minimumHeight: 480
    title: session.bookOpen && book.title ? qsTr("%1 — Cousoir").arg(book.title)
        : session.filterOpen ? qsTr("%1 — Cousoir").arg(filter.name)
        : section !== "books" ? qsTr("%1 — Cousoir").arg(sidebar.current.title)
        : qsTr("Cousoir")
    color: Theme.canvas
    font.family: Theme.uiFont
    font.pointSize: Theme.fontPointSizeBody

    palette.window: Theme.canvas
    palette.windowText: Theme.text
    palette.base: Theme.canvas
    palette.text: Theme.text
    palette.button: Theme.canvas
    palette.buttonText: Theme.text
    palette.highlight: Theme.textSelection
    palette.highlightedText: Theme.textBright
    palette.placeholderText: Theme.textFaint
    palette.toolTipBase: Theme.canvas
    palette.toolTipText: Theme.text
    palette.dark: Theme.controlBorder
    palette.mid: Theme.divider
    palette.link: Theme.accent

    function applyOmarchyTheme() {
        Theme.omarchyDark = omarchyTheme.dark
        Theme.omarchyPalette = omarchyTheme.palette
        Theme.omarchyAvailable = omarchyTheme.available
    }

    function availableThemeModes() {
        const modes = ["system", "light", "dark"]
        if (omarchyTheme.available)
            modes.push("omarchy")
        return modes
    }

    function normalizedThemeMode(candidate) {
        return availableThemeModes().indexOf(candidate) >= 0 ? candidate : "system"
    }

    function setThemeMode(candidate) {
        const normalizedMode = normalizedThemeMode(candidate)
        Theme.mode = normalizedMode
        if (appearanceSettings.colorMode !== normalizedMode)
            appearanceSettings.colorMode = normalizedMode
    }

    function normalizedTextScalePercent(candidate) {
        const rounded = Math.round(Number(candidate) / 10) * 10
        return Math.max(80, Math.min(200, isFinite(rounded) ? rounded : 100))
    }

    function setTextScalePercent(candidate) {
        const normalized = normalizedTextScalePercent(candidate)
        Theme.textScalePercent = normalized
        if (appearanceSettings.textScalePercent !== normalized) {
            appearanceSettings.textScalePercent = normalized
            appearanceSettings.setValue("textScalePercent", normalized)
            appearanceSettings.sync()
        }
    }

    function adjustTextScalePercent(delta) {
        setTextScalePercent(Theme.effectiveTextScalePercent + delta)
    }

    function openSettings(key) {
        if (key)
            settingsWindow.showSection(key)
        settingsWindow.show()
        settingsWindow.raise()
        settingsWindow.requestActivate()
    }

    function openBook(bookId) {
        session.openBook(bookId)
        bookPage.forceActiveFocus()
    }

    function closeBook() {
        // A field being edited saves first.
        bookPage.finishEditing()
        bookPage.confirmingRevert = false
        session.closeBook()
        if (deviceShown)
            devicePage.focusList()
        else
            booksPage.focusList()
    }

    // Back from a browse section's books to its items.
    function closeFilter() {
        const id = filter.id
        library.clearFilter()
        facetPage.focusList(id)
    }

    // One level up: from a book to its list, from an item's books to the
    // section's items.
    function goBack() {
        if (session.bookOpen)
            closeBook()
        else if (session.filterOpen)
            closeFilter()
    }

    function focusSearch() {
        if (session.bookOpen)
            session.closeBook()
        searchField.forceActiveFocus(Qt.ShortcutFocusReason)
        searchField.selectAll()
    }

    function focusCurrentList() {
        if (facetShown)
            facetPage.focusList(0)
        else if (trashShown)
            trashPage.focusList()
        else if (deviceShown)
            devicePage.focusList()
        else
            booksPage.focusList()
    }

    function moveCurrent(delta) {
        if (facetShown)
            facetPage.moveCurrent(delta)
        else
            booksPage.moveCurrent(delta)
    }

    function pageIndex() {
        if (session.bookOpen)
            return 2
        if (trashShown)
            return 3
        if (deviceShown)
            return 4
        return facetShown ? 1 : 0
    }

    // The open book, or the current row's, converted to `format`; asks first
    // when the book has a file of that format.
    function convertBook(bookId, format) {
        const row = bookId === book.id ? book : library.rows.find(row => row.id === bookId)
        if (!row || !library.writable)
            return
        // The open book lists its files; a row joins their formats.
        const formats = typeof row.formats === "string" ? row.formats.split(", ")
                                                        : (row.formats || []).map(file => file.format)
        if (formats.includes(format)) {
            convertDialog.bookId = bookId
            convertDialog.format = format
            convertDialog.title = qsTr("Convert “%1” to %2 again?").arg(row.title).arg(format)
            convertDialog.message = qsTr("Its %1 file is replaced by the new one.").arg(format)
            convertDialog.ask()
            return
        }
        startConversion(bookId, format)
    }

    // An older notice gives way to the conversion's progress.
    function startConversion(bookId, format) {
        const error = conversions.convert(bookId, format)
        if (error)
            showNotice(error)
        else
            notice = ""
    }

    function currentBookId() {
        return session.bookOpen ? book.id : booksShown ? booksPage.currentBookId() : 0
    }

    // Sends the book to the reader on `root`, the first one connected by
    // default. A book without a format the reader reads is converted first,
    // after asking, as Calibre offers to.
    function sendBook(bookId, root) {
        const target = root || (devices.devices.length > 0 ? devices.devices[0].root : "")
        const device = devices.devices.find(device => device.root === target)
        if (!bookId || !device)
            return
        const format = devices.convertFormat(target)
        if (devices.sendFormat(bookId, target) === "" && format && library.writable && conversions.available
            && conversions.formatsFor(bookId).length > 0) {
            const row = bookId === book.id ? book : library.rows.find(row => row.id === bookId)
            convertAndSendDialog.bookId = bookId
            convertAndSendDialog.root = target
            convertAndSendDialog.format = format
            convertAndSendDialog.title = qsTr("Convert “%1” to %2 for %3?").arg(row ? row.title : "").arg(format)
                .arg(device.name)
            convertAndSendDialog.message = qsTr("%1 reads none of its formats. Calibre converts it first, then it is sent.")
                .arg(device.name)
            convertAndSendDialog.ask()
            return
        }
        const error = devices.send([bookId], target)
        if (error)
            showNotice(error)
        else
            notice = ""
    }

    // Sends the books converted for a reader once their files are in the
    // library; drops those whose conversion stopped or failed.
    function sendConverted() {
        if (pendingSends.length === 0 || library.saving || library.loading)
            return
        const waiting = []
        for (const pending of pendingSends) {
            if (devices.sendFormat(pending.bookId, pending.root) !== "")
                devices.send([pending.bookId], pending.root)
            else if (conversions.jobs.some(job => job.bookId === pending.bookId))
                waiting.push(pending)
        }
        pendingSends = waiting
    }

    // Asks before removing a book from the Kobo the device page shows.
    function removeFromDevice(lpath, title) {
        removeFromDeviceDialog.lpath = lpath
        removeFromDeviceDialog.title = qsTr("Remove “%1” from %2?").arg(title).arg(devices.current.name)
        removeFromDeviceDialog.ask()
    }

    function ejectDevice() {
        const error = devices.eject(devices.currentRoot)
        if (error)
            showNotice(error)
    }

    // Reads the library again, or the Kobo the device page shows.
    function reload() {
        if (deviceShown)
            devices.refresh()
        else
            session.reload()
    }

    function addBooks(urls) {
        const error = library.addBooks(urls)
        if (error)
            showNotice(error)
    }

    // Asks before moving a book to the trash.
    function trashBook(bookId) {
        const row = bookId === book.id ? book : library.rows.find(row => row.id === bookId)
        if (!row || !library.writable)
            return
        trashDialog.bookId = bookId
        trashDialog.title = qsTr("Move “%1” to the trash?").arg(row.title)
        trashDialog.ask()
    }

    // Opens the open book, or the current row's, in the desktop's reader.
    function openInReader() {
        const bookId = session.bookOpen ? book.id : booksShown ? booksPage.currentBookId() : 0
        if (!bookId)
            return
        const error = library.openFormat(bookId, "")
        if (error)
            showNotice(error)
    }

    function toggleGrid() {
        layoutSettings.gridMode = !layoutSettings.gridMode
    }

    function showNotice(message, isError = true) {
        notice = message
        noticeIsError = isError
        noticeTimer.restart()
    }

    function toggleDetails() {
        if (detailsVisible) {
            layoutSettings.showDetails = false
            detailsForced = false
        } else {
            layoutSettings.showDetails = true
            detailsForced = !wide
        }
    }

    function plural(count, one, many) {
        return (count === 1 ? one : many).arg(count)
    }

    // "%1 author" and "%1 authors" for the browse sections.
    function facetNouns(key) {
        return ({
            "authors": [qsTr("%1 author"), qsTr("%1 authors"), qsTr("%1 of %2 authors")],
            "series": [qsTr("%1 series"), qsTr("%1 series"), qsTr("%1 of %2 series")],
            "tags": [qsTr("%1 tag"), qsTr("%1 tags"), qsTr("%1 of %2 tags")],
            "languages": [qsTr("%1 language"), qsTr("%1 languages"), qsTr("%1 of %2 languages")],
            "publishers": [qsTr("%1 publisher"), qsTr("%1 publishers"), qsTr("%1 of %2 publishers")]
        })[key] || []
    }

    function statusText() {
        if (!library.loaded)
            return ""
        if (session.bookOpen)
            return (book.formats || []).map(format => format.format).join(" · ")
        if (section === "trash")
            return plural(library.trashEntries.length, qsTr("%1 item"), qsTr("%1 items"))
        if (deviceSection)
            return plural(devices.rows.length, qsTr("%1 book"), qsTr("%1 books")) + " · "
                + qsTr("%1 free").arg(devices.current.free || "")
        // From the section itself rather than facetShown, which may not have
        // followed a section change yet.
        const nouns = facetNouns(section)
        if (nouns.length > 0 && !session.filterOpen) {
            const shown = library.facetRows.length
            const total = library.facetCount
            return shown !== total ? nouns[2].arg(shown).arg(total) : plural(total, nouns[0], nouns[1])
        }
        const shown = library.rows.length
        const total = library.scopeCount
        if (shown !== total)
            return qsTr("%1 of %2 books").arg(shown).arg(total)
        return plural(total, qsTr("%1 book"), qsTr("%1 books"))
    }

    function hintText() {
        if (library.importing)
            return qsTr("Reading %1 of %2 files…").arg(library.importDone).arg(library.importTotal)
        if (conversions.busy) {
            const job = conversions.jobs[0]
            const waiting = conversions.jobs.length - 1
            return (job.running ? qsTr("Converting “%1” to %2 · %3%").arg(job.title).arg(job.format)
                                      .arg(Math.round(job.progress * 100))
                                : qsTr("Converting “%1” to %2 after the changes…").arg(job.title).arg(job.format))
                + (waiting > 0 ? " " + plural(waiting, qsTr("(%1 more waiting)"), qsTr("(%1 more waiting)")) : "")
        }
        if (devices.busy) {
            const job = devices.jobs[0]
            const what = job.titles.length === 1 ? qsTr("“%1”").arg(job.titles[0])
                                                 : qsTr("%1 books").arg(job.titles.length)
            const waiting = devices.jobs.length - 1
            return (job.running ? qsTr("Sending %1 to %2 · %3%").arg(what).arg(job.name).arg(Math.round(job.progress * 100))
                                : qsTr("Sending %1 to %2 after the changes…").arg(what).arg(job.name))
                + (waiting > 0 ? " " + plural(waiting, qsTr("(%1 more waiting)"), qsTr("(%1 more waiting)")) : "")
        }
        if (library.saving)
            return qsTr("Saving…")
        if (session.bookOpen && bookPage.confirmingRevert)
            return qsTr("Enter revert · Esc keep")
        if (session.bookOpen && bookPage.editing)
            return qsTr("Enter save · Esc revert · Tab next field")
        if (session.bookOpen && library.writable && devices.devices.length > 0)
            return qsTr("Esc back · Ctrl+O open in reader · Ctrl+D send · Ctrl+I details")
        if (session.bookOpen && library.writable)
            return qsTr("Esc back · Ctrl+O open in reader · Ctrl+V paste cover · Ctrl+I details")
        if (session.bookOpen)
            return qsTr("Esc back · Ctrl+O open in reader · Ctrl+I details")
        if (trashShown)
            return library.trashEntries.length > 0 ? qsTr("↑↓ move · Enter restore") : qsTr("F5 reload")
        if (deviceShown)
            return devices.rows.length === 0 ? qsTr("F5 read again")
                : devices.writable ? qsTr("↑↓ move · Enter show in library · Del remove from %1").arg(devices.current.name)
                : qsTr("↑↓ move · Enter show in library")
        if (facetShown)
            return library.facetRows.length > 0 ? qsTr("↑↓ move · Enter show books · Ctrl+F search")
                                                : qsTr("Ctrl+F search · F5 reload")
        if (library.rows.length === 0)
            return session.filterOpen ? qsTr("Esc back · F5 reload") : qsTr("Ctrl+F search · F5 reload")
        const back = session.filterOpen ? qsTr("Esc back · ") : ""
        const trash = library.writable ? qsTr(" · Del trash") : ""
        return back + qsTr("↑↓ move · Enter open · Ctrl+O read · Ctrl+G %1").arg(layoutSettings.gridMode ? qsTr("list")
                                                                                                   : qsTr("covers"))
            + trash
    }

    AppSession {
        id: session
        adaptiveTypography: true
        libraryPath: librarySettings.path
        calibreLanguage: librarySettings.calibreLanguage
    }
    readonly property BookLibrary library: session.library
    readonly property ConcurrencyGuard guard: session.guard
    readonly property ConversionService conversions: session.conversions
    readonly property DeviceLibrary devices: session.devices

    Timer {
        id: noticeTimer
        interval: 8000
        onTriggered: window.notice = ""
    }

    Connections {
        target: window.library

        function onLoadFailed(message) {
            window.showNotice(message)
        }

        function onSaveFailed(message) {
            window.showNotice(message)
        }

        function onRecovered(message) {
            window.showNotice(qsTr("Cousoir %1.").arg(message))
        }

        function onBooksAdded(bookIds) {
            window.showNotice(plural(bookIds.length, qsTr("Added %1 book."), qsTr("Added %1 books.")), false)
            if (window.booksShown)
                booksPage.select(bookIds[0])
        }

        function onDuplicatesFound(titles, total) {
            const one = titles.length === 1
            const shown = titles.slice(0, 4).map(title => "“" + title + "”").join(", ")
            duplicatesDialog.title = one ? qsTr("“%1” is already in the library").arg(titles[0])
                                         : qsTr("%1 of these books are already in the library").arg(titles.length)
            duplicatesDialog.message = (one ? "" : titles.length > 4 ? qsTr("%1 and %2 more.").arg(shown).arg(titles.length - 4)
                                                                     : shown + ".")
                + (total > titles.length ? (one ? "" : " ") + plural(total - titles.length,
                    qsTr("The other book is added either way."), qsTr("The %1 other books are added either way.")) : "")
            duplicatesDialog.acceptText = one ? qsTr("Add it anyway") : qsTr("Add them anyway")
            duplicatesDialog.rejectText = one ? qsTr("Skip it") : qsTr("Skip them")
            duplicatesDialog.ask()
        }

        function onImportWarning(message) {
            window.showNotice(message)
        }

        function onSortOptionChanged() {
            layoutSettings.sortOption = window.library.sortOption
        }

        function onSavingChanged() {
            window.sendConverted()
        }

        function onLibraryChanged() {
            window.sendConverted()
        }
    }

    Connections {
        target: window.conversions

        function onConverted(bookId, title, format) {
            window.showNotice(qsTr("“%1” converted to %2.").arg(title).arg(format), false)
        }

        function onFailed(message) {
            window.showNotice(message)
        }

        function onJobsChanged() {
            window.sendConverted()
        }
    }

    Connections {
        target: window.devices

        function onSent(titles, name) {
            window.showNotice(titles.length === 1 ? qsTr("“%1” sent to %2.").arg(titles[0]).arg(name)
                                                  : qsTr("%1 books sent to %2.").arg(titles.length).arg(name), false)
        }

        function onEjected(name) {
            window.showNotice(qsTr("%1 can be unplugged.").arg(name), false)
        }

        function onFailed(message) {
            window.showNotice(message)
        }

        // A reader unplugged takes its page with it.
        function onDevicesChanged() {
            if (window.deviceSection && !window.devices.devices.some(device => "device:" + device.root === window.section))
                window.section = "books"
        }
    }

    OmarchyTheme {
        id: omarchyTheme
        active: Theme.mode === "omarchy" && available
        onPaletteChanged: window.applyOmarchyTheme()
        onAvailableChanged: {
            window.applyOmarchyTheme()
            if (!available && Theme.mode === "omarchy")
                window.setThemeMode("system")
        }
    }

    Settings {
        id: appearanceSettings
        category: "Appearance"
        property string colorMode: "system"
        property int textScalePercent: 100
    }

    Settings {
        id: layoutSettings
        category: "Layout"
        property bool showDetails: true
        property bool gridMode: false
        property string sortOption: "title"
    }

    Settings {
        id: librarySettings
        category: "Library"
        // Empty for the library Calibre opens.
        property string path: ""
        // Empty for Calibre's own interface language.
        property string calibreLanguage: ""
    }

    SettingsWindow {
        id: settingsWindow

        transientParent: window
        currentThemeMode: Theme.mode
        availableThemeModes: window.availableThemeModes()
        currentTextScalePercent: Theme.effectiveTextScalePercent
        library: window.library
        guard: window.guard
        customLibraryPath: librarySettings.path
        calibreLanguage: librarySettings.calibreLanguage

        onThemeModeSelected: function(mode) {
            window.setThemeMode(mode)
        }
        onTextScalePercentSelected: function(percent) {
            window.setTextScalePercent(percent)
        }
        onLibraryPathSelected: function(path) {
            librarySettings.path = path
        }
        onCalibreLanguageSelected: function(language) {
            librarySettings.calibreLanguage = language
        }
    }

    Shortcut {
        sequences: [StandardKey.ZoomIn, "Ctrl+="]
        onActivated: window.adjustTextScalePercent(10)
    }

    Shortcut {
        sequences: [StandardKey.ZoomOut]
        onActivated: window.adjustTextScalePercent(-10)
    }

    Shortcut {
        sequence: "Ctrl+0"
        onActivated: window.setTextScalePercent(100)
    }

    Shortcut {
        sequences: ["Esc", "Alt+Left", StandardKey.Back]
        enabled: session.bookOpen || session.filterOpen
        // The revert question keeps the changes instead.
        onActivated: {
            if (bookPage.confirmingRevert)
                bookPage.confirmingRevert = false
            else
                window.goBack()
        }
    }

    Shortcut {
        sequences: ["Return", "Enter"]
        enabled: session.bookOpen && bookPage.confirmingRevert
        onActivated: bookPage.revertChanges()
    }

    Shortcut {
        sequences: [StandardKey.Paste]
        enabled: session.bookOpen && library.writable && !bookPage.editing
        onActivated: bookPage.pasteCover()
    }

    Shortcut {
        sequences: [StandardKey.Find]
        onActivated: window.focusSearch()
    }

    Shortcut {
        sequences: [StandardKey.Refresh, "Ctrl+R"]
        onActivated: window.reload()
    }

    Shortcut {
        sequence: "Ctrl+I"
        enabled: session.bookOpen
        onActivated: window.toggleDetails()
    }

    Shortcut {
        sequence: "Ctrl+O"
        enabled: session.bookOpen || window.booksShown
        onActivated: window.openInReader()
    }

    Shortcut {
        sequence: "Ctrl+G"
        enabled: window.booksShown
        onActivated: window.toggleGrid()
    }

    Shortcut {
        sequences: [StandardKey.New]
        enabled: library.writable && !library.importing
        onActivated: addBooksDialog.open()
    }

    Shortcut {
        sequence: "Ctrl+Shift+K"
        enabled: (session.bookOpen || window.booksShown) && library.writable && conversions.available
        onActivated: window.convertBook(window.currentBookId(), "KEPUB")
    }

    Shortcut {
        sequence: "Ctrl+Shift+A"
        enabled: (session.bookOpen || window.booksShown) && library.writable && conversions.available
        onActivated: window.convertBook(window.currentBookId(), "AZW3")
    }

    Shortcut {
        sequence: "Ctrl+D"
        enabled: (session.bookOpen || window.booksShown) && devices.devices.length > 0 && devices.available
            && devices.writable
        onActivated: window.sendBook(window.currentBookId(), "")
    }

    Shortcut {
        sequences: [StandardKey.Delete]
        enabled: window.booksShown && library.writable
        onActivated: window.trashBook(booksPage.currentBookId())
    }

    FileDialog {
        id: addBooksDialog

        title: qsTr("Add books")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("Books (*.epub *.kepub *.azw3 *.azw *.mobi *.prc *.pdf *.fb2 *.cbz *.cbr *.djvu *.docx *.rtf *.txt)"),
                      qsTr("All files (*)")]
        onAccepted: window.addBooks(selectedFiles)
    }

    ConfirmDialog {
        id: trashDialog
        objectName: "trashDialog"

        property int bookId: 0

        message: qsTr("Its files go to the library's trash, where Cousoir and Calibre can restore it.")
        acceptText: qsTr("Move to Trash")
        acceptVariant: "danger"
        onAccepted: {
            const error = window.library.moveToTrash([bookId])
            if (error)
                window.showNotice(error)
            else if (session.bookOpen && window.book.id === bookId)
                window.closeBook()
        }
    }

    ConfirmDialog {
        id: convertDialog
        objectName: "convertDialog"

        property int bookId: 0
        property string format

        acceptText: qsTr("Convert")
        onAccepted: window.startConversion(bookId, format)
    }

    ConfirmDialog {
        id: convertAndSendDialog
        objectName: "convertAndSendDialog"

        property int bookId: 0
        property string root
        property string format

        acceptText: qsTr("Convert and send")
        onAccepted: {
            window.pendingSends = window.pendingSends.concat([{ bookId: bookId, root: root }])
            window.startConversion(bookId, format)
        }
    }

    ConfirmDialog {
        id: removeFromDeviceDialog
        objectName: "removeFromDeviceDialog"

        property string lpath

        message: qsTr("Its file is deleted from the device. The library keeps the book.")
        acceptText: qsTr("Remove")
        acceptVariant: "danger"
        onAccepted: {
            const error = window.devices.remove([lpath])
            if (error)
                window.showNotice(error)
        }
    }

    ConfirmDialog {
        id: duplicatesDialog
        objectName: "duplicatesDialog"

        onAccepted: window.library.addDuplicates(true)
        // Closed otherwise, the duplicates are left out; after Add, there is
        // nothing left to skip.
        onClosed: Qt.callLater(() => window.library.addDuplicates(false))
    }

    Component.onCompleted: {
        applyOmarchyTheme()
        setThemeMode(appearanceSettings.colorMode)
        setTextScalePercent(appearanceSettings.textScalePercent)
        library.sortOption = layoutSettings.sortOption
        booksPage.focusList()
    }

    onSectionChanged: {
        notice = ""
        session.closeBook()
        // From the section itself: deviceSection may not have followed yet.
        if (section.startsWith("device:"))
            devices.currentRoot = section.slice(7)
        else
            library.section = section
    }

    onWideChanged: {
        if (wide)
            detailsForced = false
    }

    onClosing: {
        // A field being edited saves before the window goes.
        bookPage.finishEditing()
        settingsWindow.close()
    }

    footer: Rectangle {
        id: statusBar
        objectName: "statusBar"

        readonly property real countWidth: Math.max(statusCount.implicitWidth, Theme.statusHeight)
        readonly property real stateWidth: Math.min((writeState.visible ? writeState.implicitWidth + Theme.gutter : 0)
            + (stopConversions.visible ? stopConversions.width + Theme.gutter : 0)
            + (stopSending.visible ? stopSending.width + Theme.gutter : 0)
            + (detailsToggle.visible ? detailsToggle.width : 0), width * 0.45)
        readonly property real sideWidth: Math.max(countWidth, stateWidth)
        // Equal sides keep the hints centered while 40 characters of them
        // still fit; a long write state then takes room from the count's side.
        readonly property bool balanced: 2 * sideWidth + 40 * Theme.captionCharWidth <= width

        implicitHeight: Theme.statusHeight
        color: Theme.canvas

        Divider {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.gutter
            anchors.rightMargin: Math.round(Theme.gutter / 2)
            spacing: Theme.gutter

            Label {
                id: statusCount
                objectName: "bookCount"

                Layout.preferredWidth: statusBar.balanced ? statusBar.sideWidth : statusBar.countWidth
                text: window.statusText()
                color: Theme.textMuted
                font.pointSize: Theme.fontPointSizeCaption
                elide: Text.ElideRight
            }

            Label {
                objectName: "keyHints"

                Layout.fillWidth: true
                text: window.notice || window.hintText()
                color: window.notice && window.noticeIsError ? Theme.danger : window.notice ? Theme.text : Theme.textMuted
                font.pointSize: Theme.fontPointSizeCaption
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }

            RowLayout {
                Layout.preferredWidth: statusBar.balanced ? statusBar.sideWidth : statusBar.stateWidth
                Layout.fillHeight: true
                spacing: Theme.gutter

                Item { Layout.fillWidth: true }

                // Why the library cannot be written now; a click shows the
                // library settings, where conflict copies can be ignored.
                FlatButton {
                    id: writeState
                    objectName: "writeState"

                    Layout.maximumWidth: statusBar.width * 0.45 - (detailsToggle.visible ? detailsToggle.width : 0)
                    visible: window.library.loaded && !window.guard.writable
                    implicitHeight: Theme.statusHeight - 6
                    horizontalPadding: Math.round(6 * Theme.spacingScale)
                    text: window.guard.readOnlyReason
                    tip: window.guard.readOnlyReason
                    font.pointSize: Theme.fontPointSizeCaption
                    icon.source: "assets/icons/lock.svg"
                    icon.width: Math.round(Theme.iconSize * 0.85)
                    icon.height: Math.round(Theme.iconSize * 0.85)
                    icon.color: Theme.textMuted
                    palette.buttonText: Theme.textMuted
                    onClicked: window.openSettings("library")

                    contentItem: RowLayout {
                        spacing: Math.round(6 * Theme.spacingScale)

                        IconImage {
                            Layout.preferredWidth: writeState.icon.width
                            Layout.preferredHeight: writeState.icon.height
                            sourceSize: Qt.size(writeState.icon.width, writeState.icon.height)
                            source: writeState.icon.source
                            color: writeState.icon.color
                        }

                        Label {
                            Layout.fillWidth: true
                            text: writeState.text
                            color: Theme.textMuted
                            font: writeState.font
                            elide: Text.ElideRight
                        }
                    }
                }

                FlatButton {
                    id: stopConversions
                    objectName: "stopConversions"

                    visible: window.conversions.busy
                    implicitWidth: implicitHeight
                    implicitHeight: Theme.statusHeight - 6
                    iconOnly: true
                    text: qsTr("Stop converting")
                    tip: window.conversions.jobs.length === 1 ? qsTr("Stop the conversion")
                        : qsTr("Stop the %1 conversions").arg(window.conversions.jobs.length)
                    icon.source: "assets/icons/close.svg"
                    icon.width: Math.round(Theme.iconSize * 0.85)
                    icon.height: Math.round(Theme.iconSize * 0.85)
                    icon.color: Theme.textMuted
                    onClicked: window.conversions.cancel(0)
                }

                FlatButton {
                    id: stopSending
                    objectName: "stopSending"

                    visible: window.devices.busy
                    implicitWidth: implicitHeight
                    implicitHeight: Theme.statusHeight - 6
                    iconOnly: true
                    text: qsTr("Stop sending")
                    tip: window.devices.jobs.length === 1 ? qsTr("Stop sending")
                        : qsTr("Stop the %1 sendings").arg(window.devices.jobs.length)
                    icon.source: "assets/icons/close.svg"
                    icon.width: Math.round(Theme.iconSize * 0.85)
                    icon.height: Math.round(Theme.iconSize * 0.85)
                    icon.color: Theme.textMuted
                    onClicked: window.devices.cancel(0)
                }

                FlatButton {
                    id: detailsToggle
                    objectName: "detailsToggle"

                    visible: session.bookOpen
                    implicitWidth: implicitHeight
                    implicitHeight: Theme.statusHeight - 6
                    iconOnly: true
                    selected: window.detailsVisible
                    text: window.detailsVisible ? qsTr("Hide details") : qsTr("Show details")
                    tip: qsTr("Details (Ctrl+I)")
                    icon.source: "assets/icons/panel-right.svg"
                    icon.width: Math.round(Theme.iconSize * 0.85)
                    icon.height: Math.round(Theme.iconSize * 0.85)
                    icon.color: selected ? Theme.text : Theme.textMuted
                    onClicked: window.toggleDetails()
                }
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Sidebar {
            id: sidebar
            objectName: "sidebar"

            Layout.fillHeight: true
            Layout.preferredWidth: Math.min(implicitWidth, Math.round(window.width * 0.3))
            visible: !session.bookOpen
            currentKey: window.section
            counts: window.library.loaded ? window.library.counts : ({})
            devices: window.devices.devices
            onActivated: function(key) {
                window.section = key
            }
        }

        Divider {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            visible: sidebar.visible
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                objectName: "toolbar"

                Layout.fillWidth: true
                implicitHeight: Theme.toolbarHeight
                color: Theme.canvas

                Divider {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.gutter
                    anchors.rightMargin: Theme.gutter
                    spacing: Theme.gutter

                    FlatButton {
                        id: backButton
                        objectName: "backButton"

                        iconOnly: true
                        enabled: session.bookOpen || session.filterOpen
                        text: qsTr("Back")
                        tip: qsTr("Back (Esc)")
                        icon.source: "assets/icons/arrow-left.svg"
                        onClicked: window.goBack()
                    }

                    // The section, then the item or book open in it; each
                    // crumb but the last goes back to its level.
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: Math.min(implicitWidth, Math.round(10 * Theme.charWidth))
                        Layout.leftMargin: Math.round(Theme.gutter / 2)
                        spacing: 0

                        Label {
                            objectName: "rootCrumb"

                            visible: session.bookOpen || session.filterOpen
                            text: sidebar.current.title
                            color: rootHover.hovered ? Theme.text : Theme.textMuted
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Back to %1").arg(sidebar.current.title)

                            HoverHandler {
                                id: rootHover
                                cursorShape: Qt.PointingHandCursor
                            }

                            TapHandler {
                                onTapped: {
                                    if (session.bookOpen)
                                        window.closeBook()
                                    if (session.filterOpen)
                                        window.closeFilter()
                                }
                            }
                        }

                        Label {
                            visible: session.bookOpen || session.filterOpen
                            text: " / "
                            color: Theme.textFaint
                        }

                        Label {
                            objectName: "filterCrumb"

                            visible: session.bookOpen && session.filterOpen
                            text: window.filter.name || ""
                            color: filterHover.hovered ? Theme.text : Theme.textMuted
                            elide: Text.ElideRight
                            Layout.maximumWidth: Math.round(24 * Theme.charWidth)
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Back to %1").arg(window.filter.name || "")

                            HoverHandler {
                                id: filterHover
                                cursorShape: Qt.PointingHandCursor
                            }

                            TapHandler {
                                onTapped: window.closeBook()
                            }
                        }

                        Label {
                            visible: session.bookOpen && session.filterOpen
                            text: " / "
                            color: Theme.textFaint
                        }

                        Label {
                            objectName: "pageTitle"

                            Layout.fillWidth: true
                            text: session.bookOpen ? (window.book.title || "")
                                : session.filterOpen ? (window.filter.name || "")
                                : sidebar.current.title
                            font.weight: Font.Bold
                            elide: Text.ElideRight
                        }
                    }

                    // Every change saves at once; this undoes them all until the
                    // book closes, after asking.
                    FlatButton {
                        objectName: "revertChangesButton"

                        visible: session.bookOpen && window.library.canRevert && !bookPage.confirmingRevert
                        variant: "outline"
                        text: qsTr("Revert changes")
                        tip: qsTr("Undo every change since you opened the book")
                        // Out of any field first, so Enter and Esc answer.
                        onClicked: {
                            bookPage.finishEditing()
                            bookPage.forceActiveFocus()
                            bookPage.confirmingRevert = true
                        }
                    }

                    Label {
                        objectName: "revertQuestion"

                        // Whole when there is room: a layout rounds a fractional
                        // width down, which would elide the last letter.
                        Layout.fillWidth: true
                        Layout.preferredWidth: Math.ceil(implicitWidth)
                        Layout.maximumWidth: Math.ceil(implicitWidth)
                        visible: session.bookOpen && bookPage.confirmingRevert
                        text: qsTr("Undo every change since you opened it?")
                        elide: Text.ElideRight
                    }

                    FlatButton {
                        objectName: "revertKeepButton"

                        visible: session.bookOpen && bookPage.confirmingRevert
                        variant: "outline"
                        text: qsTr("Keep")
                        onClicked: bookPage.confirmingRevert = false
                    }

                    FlatButton {
                        objectName: "confirmRevertButton"

                        visible: session.bookOpen && bookPage.confirmingRevert
                        variant: "danger"
                        text: qsTr("Revert")
                        onClicked: bookPage.revertChanges()
                    }

                    FlatButton {
                        objectName: "trashBookButton"

                        visible: session.bookOpen && !bookPage.confirmingRevert && window.library.writable
                        iconOnly: true
                        text: qsTr("Move to Trash")
                        icon.source: "assets/icons/trash.svg"
                        onClicked: window.trashBook(window.book.id)
                    }

                    FlatButton {
                        objectName: "openInReaderButton"

                        visible: session.bookOpen && !bookPage.confirmingRevert
                        enabled: (window.book.formats || []).some(format => !format.missing)
                        variant: "outline"
                        text: qsTr("Open in reader")
                        tip: qsTr("Open in reader (Ctrl+O)")
                        icon.source: "assets/icons/external.svg"
                        onClicked: window.openInReader()
                    }

                    // While a Kobo is mounted, Calibre's way of letting it
                    // go: unmounted and powered off. A Kindle needs none.
                    FlatButton {
                        objectName: "ejectButton"

                        visible: window.deviceShown && window.devices.current.ejectable === true
                        enabled: !window.devices.busy
                        variant: "outline"
                        text: qsTr("Eject")
                        tip: qsTr("Eject %1 so it can be unplugged").arg(window.devices.current.name || "")
                        icon.source: "assets/icons/eject.svg"
                        onClicked: window.ejectDevice()
                    }

                    FlatButton {
                        objectName: "addBooksButton"

                        visible: !session.bookOpen && !window.deviceShown
                        enabled: window.library.writable && !window.library.importing
                        iconOnly: true
                        text: qsTr("Add books")
                        tip: qsTr("Add books (Ctrl+N)")
                        icon.source: "assets/icons/plus.svg"
                        onClicked: addBooksDialog.open()
                    }

                    FlatButton {
                        objectName: "viewToggle"

                        visible: window.booksShown
                        iconOnly: true
                        text: layoutSettings.gridMode ? qsTr("Show as list") : qsTr("Show covers")
                        tip: qsTr("%1 (Ctrl+G)").arg(text)
                        icon.source: layoutSettings.gridMode ? "assets/icons/list.svg" : "assets/icons/grid.svg"
                        onClicked: window.toggleGrid()
                    }

                    SearchField {
                        id: searchField
                        objectName: "searchField"

                        function submit() {
                            if (window.facetShown)
                                facetPage.activate(0)
                            else
                                booksPage.activate(0)
                        }

                        visible: !session.bookOpen && !window.deviceShown
                        Layout.fillWidth: true
                        Layout.preferredWidth: Math.round(280 * Theme.spacingScale)
                        Layout.maximumWidth: Math.round(280 * Theme.spacingScale)
                        Layout.minimumWidth: Math.round(120 * Theme.spacingScale)
                        placeholderText: window.facetShown ? qsTr("Search %1").arg(sidebar.current.title.toLowerCase())
                            : qsTr("Search title, author, series or tag")
                        text: window.library.searchText

                        onTextEdited: window.library.searchText = text
                        Keys.onDownPressed: window.focusCurrentList()
                        Keys.onReturnPressed: submit()
                        Keys.onEnterPressed: submit()
                        Keys.onEscapePressed: {
                            // Clear through the model so the text binding survives.
                            if (window.library.searchText.length > 0)
                                window.library.searchText = ""
                            else
                                window.focusCurrentList()
                        }
                    }

                    FlatButton {
                        objectName: "refreshButton"

                        visible: !session.bookOpen
                        iconOnly: true
                        text: window.deviceShown ? qsTr("Read the device again") : qsTr("Reload library")
                        tip: qsTr("%1 (F5)").arg(text)
                        icon.source: "assets/icons/refresh.svg"
                        onClicked: window.reload()
                    }

                    Divider {
                        visible: !session.bookOpen
                        Layout.preferredWidth: 1
                        Layout.preferredHeight: Math.round(Theme.rowHeight * 0.6)
                    }

                    FlatButton {
                        id: settingsButton
                        objectName: "settingsButton"

                        visible: !session.bookOpen
                        iconOnly: true
                        text: qsTr("Settings")
                        icon.source: "assets/icons/settings.svg"
                        onClicked: window.openSettings("")
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                StackLayout {
                    id: pages

                    anchors.fill: parent
                    currentIndex: window.pageIndex()

                    BooksPage {
                        id: booksPage
                        objectName: "booksPage"
                        library: window.library
                        conversions: window.conversions
                        devices: window.devices
                        gridMode: layoutSettings.gridMode
                        onBookActivated: function(bookId) {
                            window.openBook(bookId)
                        }
                        onTrashRequested: function(bookId) { window.trashBook(bookId) }
                        onConvertRequested: function(bookId, format) { window.convertBook(bookId, format) }
                        onSendRequested: function(bookId, root) { window.sendBook(bookId, root) }
                        onReloadRequested: session.reload()
                        onSettingsRequested: window.openSettings("library")
                        onFailed: function(message) { window.showNotice(message) }
                    }

                    FacetPage {
                        id: facetPage
                        objectName: "facetPage"
                        library: window.library
                        onItemActivated: function(id) {
                            window.library.openFacetItem(id)
                            booksPage.focusList()
                        }
                    }

                    BookPage {
                        id: bookPage
                        objectName: "bookPage"
                        library: window.library
                        conversions: window.conversions
                        devices: window.devices
                        book: window.book
                        detailsVisible: window.detailsVisible
                        onFailed: function(message) { window.showNotice(message) }
                        onConvertRequested: function(format) { window.convertBook(window.book.id, format) }
                        onSendRequested: function(root) { window.sendBook(window.book.id, root) }
                    }

                    TrashPage {
                        id: trashPage
                        objectName: "trashPage"
                        library: window.library
                        onFailed: function(message) { window.showNotice(message) }
                    }

                    DevicePage {
                        id: devicePage
                        objectName: "devicePage"
                        devices: window.devices
                        onBookRequested: function(bookId) { window.openBook(bookId) }
                        onRemoveRequested: function(lpath, title) { window.removeFromDevice(lpath, title) }
                    }
                }

                // Files dropped on the lists are added as books; the book
                // page takes them itself.
                DropArea {
                    id: bookDrop
                    objectName: "bookDrop"

                    anchors.fill: parent
                    enabled: !session.bookOpen && !window.deviceShown && window.library.writable
                    onEntered: function(drag) {
                        drag.accepted = drag.hasUrls
                    }
                    onDropped: function(drop) {
                        if (drop.hasUrls)
                            window.addBooks(drop.urls)
                    }

                    Rectangle {
                        anchors.fill: parent
                        visible: bookDrop.containsDrag
                        color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.08)
                        border.width: 2
                        border.color: Theme.accent

                        Label {
                            anchors.centerIn: parent
                            text: qsTr("Drop to add to the library")
                            color: Theme.accent
                            font.weight: Font.Bold
                        }
                    }
                }
            }
        }
    }
}
