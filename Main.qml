import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Controls.impl
import QtQuick.Dialogs
import QtQuick.Layouts
import QtCore

import Cousoir

ThemedWindow {
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
    // A saved search's books.
    readonly property bool searchSection: section.startsWith("search:")
    readonly property bool facetShown: section !== "books" && section !== "trash" && section !== "check"
        && !deviceSection && !searchSection && !session.filterOpen && !session.bookOpen
    readonly property bool trashShown: section === "trash" && !session.bookOpen
    readonly property bool checkShown: section === "check" && !session.bookOpen
    readonly property bool deviceShown: deviceSection && !session.bookOpen
    readonly property bool booksShown: !facetShown && !trashShown && !checkShown && !deviceShown && !session.bookOpen
    // Several books selected in the list, which the pane beside it edits.
    readonly property bool editingMany: booksShown && library.books.selectedCount > 1
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

    function defaultThemeMode() {
        return omarchyTheme.available ? "omarchy" : "system"
    }

    function normalizedThemeMode(candidate) {
        return availableThemeModes().indexOf(candidate) >= 0 ? candidate : defaultThemeMode()
    }

    // The mode picked in Settings, or the default until one is; a mode that
    // is not available falls back to the default without being saved.
    function applyThemeMode() {
        Theme.mode = normalizedThemeMode(appearanceSettings.colorMode)
    }

    function setThemeMode(candidate) {
        const normalizedMode = normalizedThemeMode(candidate)
        if (appearanceSettings.colorMode !== normalizedMode)
            appearanceSettings.colorMode = normalizedMode
        applyThemeMode()
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
        else if (checkShown)
            checkPage.focusList()
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
            closeBook()
        searchField.forceActiveFocus(Qt.ShortcutFocusReason)
        searchField.selectAll()
    }

    function focusCurrentList() {
        if (facetShown)
            facetPage.focusList(0)
        else if (trashShown)
            trashPage.focusList()
        else if (checkShown)
            checkPage.focusList()
        else if (deviceShown)
            devicePage.focusList()
        else
            booksPage.focusList()
    }

    function pageIndex() {
        if (session.bookOpen)
            return 2
        if (trashShown)
            return 3
        if (deviceShown)
            return 4
        if (checkShown)
            return 5
        return facetShown ? 1 : 0
    }

    // The open book, or the current row's, converted to `format`; asks first
    // when the book has a file of that format.
    function convertBook(bookId, format) {
        const row = bookRow(bookId)
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

    // Several books converted to `format`, after asking; one book as
    // convertBook() converts it.
    function convertBooks(bookIds, format) {
        if (bookIds.length === 1) {
            convertBook(bookIds[0], format)
            return
        }
        if (bookIds.length === 0 || !library.writable)
            return
        const having = bookIds.filter(id => conversions.formatsFor(id).length > 0
                                       && (bookRow(id)?.formats || "").split(", ").includes(format)).length
        convertManyDialog.bookIds = bookIds
        convertManyDialog.format = format
        convertManyDialog.title = qsTr("Convert %1 books to %2?").arg(bookIds.length).arg(format)
        convertManyDialog.message = having > 0
            ? qsTr("%1 of them have a %2 file already, which the new one replaces.").arg(having).arg(format)
            : qsTr("Calibre converts them one after the other, each with its own settings.")
        convertManyDialog.ask()
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

    // The open book, or the row of a book listed, by id.
    function bookRow(bookId) {
        if (bookId === book.id)
            return book
        const index = library.books.indexOf(bookId)
        return index >= 0 ? library.books.get(index) : undefined
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
            const row = bookRow(bookId)
            convertAndSendDialog.bookIds = [bookId]
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

    // Sends several books to the reader on `root`, the first one connected
    // by default: those it reads now, and the others once converted, after
    // asking.
    function sendBooks(bookIds, root) {
        if (bookIds.length === 1) {
            sendBook(bookIds[0], root)
            return
        }
        const target = root || (devices.devices.length > 0 ? devices.devices[0].root : "")
        const device = devices.devices.find(device => device.root === target)
        if (bookIds.length === 0 || !device)
            return
        const format = devices.convertFormat(target)
        const sendable = bookIds.filter(id => devices.sendFormat(id, target) !== "")
        const convertible = bookIds.filter(id => devices.sendFormat(id, target) === "" && format && library.writable
                                               && conversions.available && conversions.formatsFor(id).length > 0)
        if (sendable.length > 0) {
            const error = devices.send(sendable, target)
            if (error)
                showNotice(error)
            else
                notice = ""
        }
        if (convertible.length > 0) {
            convertAndSendDialog.bookIds = convertible
            convertAndSendDialog.root = target
            convertAndSendDialog.format = format
            convertAndSendDialog.title = qsTr("Convert %1 books to %2 for %3?").arg(convertible.length).arg(format)
                .arg(device.name)
            convertAndSendDialog.message = qsTr("%1 reads none of their formats. Calibre converts them first, then they are sent.")
                .arg(device.name)
            convertAndSendDialog.ask()
        }
    }

    // Sends the books converted for a reader once their files are in the
    // library; drops those whose conversion stopped or failed.
    function sendConverted() {
        if (pendingSends.length === 0 || library.saving || library.loading)
            return
        const waiting = []
        for (const pending of pendingSends) {
            if (devices.sendFormat(pending.bookId, pending.root) !== "") {
                const error = devices.send([pending.bookId], pending.root)
                if (error)
                    showNotice(error)
            } else if (conversions.jobs.some(job => job.bookId === pending.bookId)) {
                waiting.push(pending)
            }
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

    // Asks before moving books to the trash.
    function trashBooks(bookIds) {
        if (bookIds.length === 0 || !library.writable)
            return
        const row = bookRow(bookIds[0])
        if (bookIds.length === 1 && !row)
            return
        trashDialog.bookIds = bookIds
        trashDialog.title = bookIds.length === 1 ? qsTr("Move “%1” to the trash?").arg(row.title)
                                                 : qsTr("Move %1 books to the trash?").arg(bookIds.length)
        trashDialog.message = bookIds.length === 1
            ? qsTr("Its files go to the library's trash, where Cousoir and Calibre can restore it.")
            : qsTr("Their files go to the library's trash, where Cousoir and Calibre can restore them.")
        trashDialog.ask()
    }

    function trashBook(bookId) {
        trashBooks(bookId ? [bookId] : [])
    }

    // The books an action of the keyboard acts on: the open book, or those
    // selected in the list, or its current one.
    function targetIds() {
        if (session.bookOpen)
            return book.id ? [book.id] : []
        return booksShown ? booksPage.targetIds() : []
    }

    // Opens the open book, or the current row's, in the desktop's reader.
    function openInReader() {
        const bookId = currentBookId()
        if (!bookId)
            return
        const error = library.openFormat(bookId, "")
        if (error)
            showNotice(error)
    }

    function toggleGrid() {
        layoutSettings.gridMode = !layoutSettings.gridMode
    }

    // What the command palette offers besides the library's books and
    // items: the sidebar's sections, the virtual libraries, and the actions
    // that can be done now, with their shortcut.
    function paletteCommands() {
        const commands = sidebar.items.map(item => ({
            kind: "section", key: item.key, title: item.title, detail: "", icon: item.icon
        }))
        const targets = targetIds()
        const books = targets.length
        const add = (key, title, detail, icon, enabled) => {
            if (enabled)
                commands.push({ kind: "action", key: key, title: title, detail: detail || "", icon: icon || "search" })
        }
        add("add", qsTr("Add books…"), "Ctrl+N", "plus", library.writable && !library.importing)
        add("reader", qsTr("Open in reader"), "Ctrl+O", "external", currentBookId() > 0)
        add("kepub", books > 1 ? qsTr("Convert %1 books to KEPUB").arg(books) : qsTr("Convert to KEPUB"), "Ctrl+Shift+K",
            "refresh", books > 0 && library.writable && conversions.available)
        add("azw3", books > 1 ? qsTr("Convert %1 books to AZW3").arg(books) : qsTr("Convert to AZW3"), "Ctrl+Shift+A",
            "refresh", books > 0 && library.writable && conversions.available)
        for (const device of devices.devices) {
            add("send:" + device.root, books > 1 ? qsTr("Send %1 books to %2").arg(books).arg(device.name)
                                                 : qsTr("Send to %1").arg(device.name),
                device === devices.devices[0] ? "Ctrl+D" : "", "ereader",
                books > 0 && devices.available && devices.writable)
            add("eject:" + device.root, qsTr("Eject %1").arg(device.name), "", "eject", device.ejectable === true)
        }
        add("trash", books > 1 ? qsTr("Move %1 books to the trash").arg(books) : qsTr("Move to the trash"), "Del",
            "trash", books > 0 && library.writable)
        add("grid", layoutSettings.gridMode ? qsTr("Show the list") : qsTr("Show the covers"), "Ctrl+G",
            layoutSettings.gridMode ? "list" : "grid", booksShown)
        add("details", detailsVisible ? qsTr("Hide the details") : qsTr("Show the details"), "Ctrl+I", "panel-right",
            session.bookOpen || editingMany)
        add("search", qsTr("Search the books"), "Ctrl+F", "search", true)
        if (library.virtualLibrary)
            add("vl:", qsTr("Virtual library: none"), "", "layers", true)
        for (const name of library.virtualLibraries)
            add("vl:" + name, qsTr("Virtual library: %1").arg(name), "", "layers", name !== library.virtualLibrary)
        add("reload", qsTr("Reload the library"), "Ctrl+R", "refresh", true)
        add("settings", qsTr("Settings"), "", "settings", true)
        for (const mode of availableThemeModes()) {
            const titles = { system: qsTr("System theme"), light: qsTr("Light theme"), dark: qsTr("Dark theme"),
                             omarchy: qsTr("Omarchy theme") }
            add("theme:" + mode, titles[mode], "", "theme-" + mode, true)
        }
        add("backups", qsTr("Show the backups of the library"), "", "folder", library.loaded)
        return commands
    }

    function runPaletteHit(hit) {
        // A description being edited is saved first; the book open closes
        // for anything but an action on it.
        bookPage.finishEditing()
        if (hit.kind !== "action" && session.bookOpen)
            closeBook()
        if (hit.kind === "book") {
            openBook(hit.id)
        } else if (hit.kind === "section") {
            // The section shown goes back to its own list.
            if (section === hit.key && session.filterOpen)
                library.clearFilter()
            section = hit.key
            focusCurrentList()
        } else if (hit.kind !== "action") {
            // An author, series, tag or publisher: its books.
            section = hit.kind
            library.openFacetItem(hit.id)
            booksPage.focusList()
        } else if (hit.key === "add") {
            addBooksDialog.open()
        } else if (hit.key === "reader") {
            openInReader()
        } else if (hit.key === "kepub" || hit.key === "azw3") {
            convertBooks(targetIds(), hit.key.toUpperCase())
        } else if (hit.key.startsWith("send:")) {
            sendBooks(targetIds(), hit.key.slice(5))
        } else if (hit.key.startsWith("eject:")) {
            const error = devices.eject(hit.key.slice(6))
            if (error)
                showNotice(error)
        } else if (hit.key === "trash") {
            trashBooks(targetIds())
        } else if (hit.key === "grid") {
            toggleGrid()
        } else if (hit.key === "details") {
            toggleDetails()
        } else if (hit.key === "search") {
            focusSearch()
        } else if (hit.key.startsWith("vl:")) {
            library.virtualLibrary = hit.key.slice(3)
        } else if (hit.key === "reload") {
            reload()
        } else if (hit.key === "settings") {
            openSettings("")
        } else if (hit.key.startsWith("theme:")) {
            setThemeMode(hit.key.slice(6))
        } else if (hit.key === "backups") {
            library.openBackupFolder()
        }
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
        if (section === "check")
            return library.checking ? qsTr("Checking…")
                : plural(library.check.count || 0, qsTr("%1 thing to check"), qsTr("%1 things to check"))
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
        const shown = library.books.count
        const total = library.scopeCount
        const selected = library.books.selectedCount > 1 ? qsTr("%1 selected · ").arg(library.books.selectedCount) : ""
        if (shown !== total)
            return selected + qsTr("%1 of %2 books").arg(shown).arg(total)
        return selected + plural(total, qsTr("%1 book"), qsTr("%1 books"))
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
                + (waiting > 0 ? " " + qsTr("(%1 more waiting)").arg(waiting) : "")
        }
        if (devices.busy) {
            const job = devices.jobs[0]
            const what = job.titles.length === 1 ? qsTr("“%1”").arg(job.titles[0])
                                                 : qsTr("%1 books").arg(job.titles.length)
            const waiting = devices.jobs.length - 1
            return (job.running ? qsTr("Sending %1 to %2 · %3%").arg(what).arg(job.name).arg(Math.round(job.progress * 100))
                                : qsTr("Sending %1 to %2 after the changes…").arg(what).arg(job.name))
                + (waiting > 0 ? " " + qsTr("(%1 more waiting)").arg(waiting) : "")
        }
        if (library.saving)
            return qsTr("Saving…")
        if (session.bookOpen && bookPage.confirmingRevert)
            return qsTr("Enter revert · Esc keep")
        if (session.bookOpen && bookPage.editingDescription)
            return qsTr("Ctrl+Enter save · Esc cancel")
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
        if (checkShown)
            return (library.check.count || 0) > 0 ? qsTr("↑↓ move · Enter actions · F5 check again")
                                                  : qsTr("F5 check again")
        if (deviceShown)
            return devices.rows.length === 0 ? qsTr("F5 read again")
                : devices.writable ? qsTr("↑↓ move · Enter show in library · Del remove from %1").arg(devices.current.name)
                : qsTr("↑↓ move · Enter show in library")
        if (facetShown)
            return library.facetRows.length > 0 ? qsTr("↑↓ move · Enter show books · Ctrl+F search · Ctrl+K find")
                                                : qsTr("Ctrl+F search · Ctrl+K find · F5 reload")
        if (library.books.count === 0)
            return session.filterOpen ? qsTr("Esc back · F5 reload") : qsTr("Ctrl+F search · Ctrl+K find · F5 reload")
        if (editingMany)
            return qsTr("Esc one book · Shift+↑↓ more · Ctrl+A all") + (library.writable ? qsTr(" · Del trash") : "")
                + (devices.devices.length > 0 ? qsTr(" · Ctrl+D send") : "")
        const back = session.filterOpen ? qsTr("Esc back · ") : ""
        const trash = library.writable ? qsTr(" · Del trash") : ""
        return back + qsTr("↑↓ move · Enter open · Ctrl+O read · Ctrl+G %1").arg(layoutSettings.gridMode ? qsTr("list")
                                                                                                   : qsTr("covers"))
            + trash
    }

    // An icon button of the status bar.
    component StatusButton: FlatButton {
        implicitWidth: implicitHeight
        implicitHeight: Theme.statusHeight - 6
        iconOnly: true
        icon.width: Theme.smallIconSize
        icon.height: Theme.smallIconSize
        icon.color: Theme.textMuted
    }

    AppSession {
        id: session
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
            window.showNotice(window.plural(bookIds.length, qsTr("Added %1 book."), qsTr("Added %1 books.")), false)
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
                + (total > titles.length ? (one ? "" : " ") + window.plural(total - titles.length,
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
            window.applyThemeMode()
        }
    }

    Settings {
        id: appearanceSettings
        category: "Appearance"
        // Empty until one is picked: Omarchy's colors on Omarchy, the
        // system's elsewhere.
        property string colorMode: ""
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
        enabled: session.bookOpen || session.filterOpen || (window.booksShown && booksPage.selectedCount > 1)
        // The revert question keeps the changes instead; several books
        // selected go back to the current one first.
        onActivated: {
            if (bookPage.confirmingRevert)
                bookPage.confirmingRevert = false
            else if (window.booksShown && booksPage.selectedCount > 1)
                booksPage.collapseSelection()
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
        enabled: session.bookOpen && window.library.writable && !bookPage.editing
        onActivated: bookPage.pasteCover()
    }

    Shortcut {
        sequences: [StandardKey.Find]
        onActivated: window.focusSearch()
    }

    Shortcut {
        sequence: "Ctrl+K"
        enabled: window.library.loaded
        onActivated: {
            commandPalette.commands = window.paletteCommands()
            commandPalette.show()
        }
    }

    CommandPalette {
        id: commandPalette

        library: window.library
        onChosen: function(hit) { window.runPaletteHit(hit) }
        onClosed: {
            if (!session.bookOpen && !searchField.activeFocus)
                window.focusCurrentList()
        }
    }

    Shortcut {
        sequences: [StandardKey.Refresh, "Ctrl+R"]
        onActivated: window.reload()
    }

    Shortcut {
        sequence: "Ctrl+I"
        enabled: session.bookOpen || window.editingMany
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
        enabled: window.library.writable && !window.library.importing
        onActivated: addBooksDialog.open()
    }

    Shortcut {
        sequence: "Ctrl+Shift+K"
        enabled: (session.bookOpen || window.booksShown) && window.library.writable && window.conversions.available
        onActivated: window.convertBooks(window.targetIds(), "KEPUB")
    }

    Shortcut {
        sequence: "Ctrl+Shift+A"
        enabled: (session.bookOpen || window.booksShown) && window.library.writable && window.conversions.available
        onActivated: window.convertBooks(window.targetIds(), "AZW3")
    }

    Shortcut {
        sequence: "Ctrl+D"
        enabled: (session.bookOpen || window.booksShown) && window.devices.devices.length > 0
            && window.devices.available && window.devices.writable
        onActivated: window.sendBooks(window.targetIds(), "")
    }

    Shortcut {
        sequences: [StandardKey.Delete]
        enabled: window.booksShown && window.library.writable
        onActivated: window.trashBooks(window.targetIds())
    }

    FileDialog {
        id: addBooksDialog

        title: qsTr("Add books")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("Books (*.epub *.kepub *.azw3 *.azw *.mobi *.prc *.pdf *.fb2 *.cbz *.cbr *.djvu *.docx *.rtf *.txt)"),
                      qsTr("All files (*)")]
        onAccepted: window.addBooks(selectedFiles)
    }

    LibraryFolderDialog {
        id: libraryFolderDialog

        libraryPath: window.library.libraryPath
        onChosen: function(path) { librarySettings.path = path }
    }

    ConfirmDialog {
        id: trashDialog
        objectName: "trashDialog"

        property var bookIds: []

        acceptText: qsTr("Move to Trash")
        acceptVariant: "danger"
        onAccepted: {
            const error = window.library.moveToTrash(bookIds)
            if (error)
                window.showNotice(error)
            else if (session.bookOpen && bookIds.includes(window.book.id))
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

        property var bookIds: []
        property string root
        property string format

        acceptText: qsTr("Convert and send")
        onAccepted: {
            window.pendingSends = window.pendingSends.concat(bookIds.map(bookId => ({ bookId: bookId, root: root })))
            for (const bookId of bookIds)
                window.startConversion(bookId, format)
        }
    }

    ConfirmDialog {
        id: convertManyDialog
        objectName: "convertManyDialog"

        property var bookIds: []
        property string format

        acceptText: qsTr("Convert")
        onAccepted: {
            for (const bookId of bookIds)
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
        applyThemeMode()
        setTextScalePercent(appearanceSettings.textScalePercent)
        library.sortOption = layoutSettings.sortOption
        booksPage.focusList()
    }

    // The library goes back to its books when the saved search shown is
    // gone; the sidebar follows it.
    Connections {
        target: window.library

        function onSectionChanged() {
            if (!window.deviceSection && window.section !== window.library.section)
                window.section = window.library.section
        }

        function onVirtualLibraryChanged() {
            if (window.library.virtualLibraryError)
                window.showNotice(qsTr("Cousoir cannot narrow the books to “%1”, so all show: %2")
                                  .arg(window.library.virtualLibrary).arg(window.library.virtualLibraryError))
        }
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
                    icon.width: Theme.smallIconSize
                    icon.height: Theme.smallIconSize
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

                StatusButton {
                    id: stopConversions
                    objectName: "stopConversions"

                    visible: window.conversions.busy
                    text: qsTr("Stop converting")
                    tip: window.conversions.jobs.length === 1 ? qsTr("Stop the conversion")
                        : qsTr("Stop the %1 conversions").arg(window.conversions.jobs.length)
                    icon.source: "assets/icons/close.svg"
                    onClicked: window.conversions.cancel(0)
                }

                StatusButton {
                    id: stopSending
                    objectName: "stopSending"

                    visible: window.devices.busy
                    text: qsTr("Stop sending")
                    tip: window.devices.jobs.length === 1 ? qsTr("Stop sending")
                        : qsTr("Stop the %1 sendings").arg(window.devices.jobs.length)
                    icon.source: "assets/icons/close.svg"
                    onClicked: window.devices.cancel(0)
                }

                StatusButton {
                    id: detailsToggle
                    objectName: "detailsToggle"

                    visible: session.bookOpen || window.editingMany
                    selected: window.detailsVisible
                    text: window.detailsVisible ? qsTr("Hide details") : qsTr("Show details")
                    tip: qsTr("Details (Ctrl+I)")
                    icon.source: "assets/icons/panel-right.svg"
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
            // The check's count follows the check, not the library's load.
            counts: window.library.loaded
                ? window.library.savedSearches.reduce((all, search) => {
                    all["search:" + search.name] = search.count
                    return all
                }, Object.assign({}, window.library.counts, { check: window.library.check.count || "" })) : ({})
            devices: window.devices.devices
            savedSearches: window.library.loaded ? window.library.savedSearches : []
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

                    // Calibre's virtual libraries: one narrows every section,
                    // as in Calibre.
                    ChoiceBox {
                        objectName: "virtualLibraryBox"

                        readonly property var choices: [{ value: "", label: qsTr("All books") }].concat(
                            window.library.virtualLibraries.map(name => ({ value: name, label: name })))

                        visible: window.library.virtualLibraries.length > 0 && !session.bookOpen
                            && !window.deviceShown && !window.checkShown && !window.trashShown
                        // The longest name, its arrow and paddings.
                        Layout.preferredWidth: Math.round(Math.min(24, choices.reduce(
                            (longest, choice) => Math.max(longest, choice.label.length), 0)) * Theme.charWidth)
                            + leftPadding + rightPadding
                        Accessible.name: qsTr("Virtual library")
                        model: choices
                        currentIndex: Math.max(0, choices.findIndex(choice => choice.value === window.library.virtualLibrary))
                        onActivated: function(index) {
                            window.library.virtualLibrary = choices[index].value
                        }
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

                        visible: !session.bookOpen && !window.deviceShown && !window.checkShown
                        Layout.fillWidth: true
                        Layout.preferredWidth: Math.round(280 * Theme.spacingScale)
                        Layout.maximumWidth: Math.round(280 * Theme.spacingScale)
                        Layout.minimumWidth: Math.round(120 * Theme.spacingScale)
                        placeholderText: window.facetShown ? qsTr("Search %1").arg(sidebar.current.title.toLowerCase())
                            : qsTr("Search, as in Calibre")
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
                        paneShown: window.detailsVisible
                        onTrashRequested: function(bookIds) { window.trashBooks(bookIds) }
                        onConvertRequested: function(bookIds, format) { window.convertBooks(bookIds, format) }
                        onSendRequested: function(bookIds, root) { window.sendBooks(bookIds, root) }
                        onReloadRequested: session.reload()
                        onLibraryFolderRequested: libraryFolderDialog.open()
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
                        onNoticed: function(message) { window.showNotice(message, false) }
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

                    CheckPage {
                        id: checkPage
                        objectName: "checkPage"
                        library: window.library
                        guard: window.guard
                        onBookRequested: function(bookId) { window.openBook(bookId) }
                        onFailed: function(message) { window.showNotice(message) }
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
                        color: Theme.withAlpha(Theme.accent, 0.08)
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
