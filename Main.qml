import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Controls.impl
import QtQuick.Layouts
import QtCore

import Cousoir

ThemedWindow {
    id: window

    // The details pane hides on narrow windows unless the toggle forces it.
    // At the text size chosen: larger text needs a wider window for both.
    readonly property bool wide: width >= Math.round(960 * Theme.spacingScale)
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
        && section !== "highlights" && section !== "text" && section !== "reading"
        && !deviceSection && !searchSection && !session.filterOpen && !session.bookOpen
    readonly property bool trashShown: section === "trash" && !session.bookOpen
    readonly property bool checkShown: section === "check" && !session.bookOpen
    readonly property bool highlightsShown: section === "highlights" && !session.bookOpen
    readonly property bool textShown: section === "text" && !session.bookOpen
    readonly property bool readingShown: section === "reading" && !session.bookOpen
    readonly property bool deviceShown: deviceSection && !session.bookOpen
    readonly property bool booksShown: !facetShown && !trashShown && !checkShown && !deviceShown && !textShown
        && !readingShown && !session.bookOpen
    // Several books selected in the list, which the pane beside it edits.
    readonly property bool editingMany: booksShown && library.books.selectedCount > 1
    // A message to show in place of the key hints for a while: an error,
    // unless noticeIsError says otherwise.
    property string notice: ""
    // Every message of the session, oldest first: { text, error, time }.
    property var messages: []
    // The errors not seen yet in the messages.
    property int unreadErrors: 0
    property bool noticeIsError: true
    // Books being converted to be sent: { bookId, root }.
    property var pendingSends: []
    // What quitting would cut short, and whether quitting anyway was chosen.
    readonly property bool working: conversions.busy || devices.busy || library.importing
    property bool quitConfirmed: false

    visible: true
    // As the window was left.
    width: windowSettings.width
    height: windowSettings.height
    minimumWidth: 720
    minimumHeight: 480
    title: session.bookOpen && book.title ? qsTr("%1 — Cousoir").arg(book.title)
        : session.filterOpen ? qsTr("%1 — Cousoir").arg(filter.name)
        : section !== "books" ? qsTr("%1 — Cousoir").arg(sidebar.current.title)
        : qsTr("Cousoir")

    function applyOmarchyTheme() {
        Theme.omarchyPalette = omarchyTheme.palette
        Theme.omarchyAvailable = omarchyTheme.available
        Theme.windowRounding = omarchyTheme.rounding
        Theme.windowOpacityMode = omarchyTheme.windowOpacity
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

    // What a launch of Cousoir asked for, this one's or one after, which
    // hands it over: a book to show, files to read or add, another library.
    // Kept until the library is loaded.
    property var pendingInvocation: null

    function handleInvocation(invocation, forwarded) {
        if (forwarded) {
            window.show()
            window.raise()
            window.requestActivate()
            appInstance.focusWindow()
            // Another library, unless a change is being saved to this one.
            if (invocation.library && invocation.library !== library.libraryPath && !library.saving)
                librarySettings.path = invocation.library
        }
        if (!library.loaded || (invocation.library && invocation.library !== library.libraryPath)) {
            pendingInvocation = invocation
            return
        }
        pendingInvocation = null
        if (invocation.show > 0)
            openBook(invocation.show)
        // The library's own files are read, and those of no library the
        // reader opens as they are; the others offered to add.
        const outside = []
        for (const file of invocation.files) {
            const bookId = library.bookOfFile(file)
            if (bookId > 0)
                readBookOf(bookId)
            else if (!readFile(file))
                outside.push(file)
        }
        if (invocation.add.length > 0)
            addBooks(invocation.add)
        else if (invocation.addAsked)
            addBooksDialog.open()
        if (outside.length > 0) {
            addFilesDialog.files = outside
            addFilesDialog.title = outside.length === 1
                ? qsTr("Add “%1” to the library?").arg(outside[0].split("/").pop())
                : qsTr("Add %n books to the library?", "", outside.length)
            addFilesDialog.ask()
        }
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
        else if (highlightsShown)
            highlightsPage.focusList()
        else if (textShown)
            textPage.focusList()
        else if (readingShown)
            readingPage.focusList()
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
        // The highlights and the books' text have their own.
        if (highlightsShown) {
            highlightsPage.focusSearch()
            return
        }
        if (textShown) {
            textPage.focusField()
            return
        }
        searchField.forceActiveFocus(Qt.ShortcutFocusReason)
        searchField.selectAll()
    }

    // The areas F6 goes round, the focus moving to the next one: the
    // sidebar, the list or the book, the details beside a book.
    function cycleFocus(step) {
        const areas = ["sidebar", "main"]
        if (session.bookOpen && detailsVisible)
            areas.push("details")
        const inside = container => {
            for (let item = window.activeFocusItem; item; item = item.parent) {
                if (item === container)
                    return true
            }
            return false
        }
        const at = inside(sidebar) ? 0 : areas.length === 3 && inside(bookPage.detailsPane) ? 2 : 1
        const next = areas[(at + step + areas.length) % areas.length]
        if (next === "sidebar")
            sidebar.focusCurrent()
        else if (next === "details")
            bookPage.focusDetails()
        else if (session.bookOpen)
            bookPage.forceActiveFocus(Qt.TabFocusReason)
        else
            focusCurrentList()
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
        else if (highlightsShown)
            highlightsPage.focusList()
        else if (textShown)
            textPage.focusList()
        else if (readingShown)
            readingPage.focusList()
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
        if (highlightsShown)
            return 6
        if (textShown)
            return 7
        if (readingShown)
            return 8
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
        convertManyDialog.title = qsTr("Convert %n books to %1?", "", bookIds.length).arg(format)
        convertManyDialog.message = having > 0
            ? qsTr("%1 of them have a %2 file already, which the new one replaces.").arg(having).arg(format)
            : conversions.calibre ? qsTr("Calibre converts them one after the other, each with its own settings.")
            : qsTr("Cousoir converts them one after the other.")
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
        if (devices.sendFormat(bookId, target) === "" && format && library.writable
            && conversions.formatsFor(bookId).includes(format)) {
            const row = bookRow(bookId)
            convertAndSendDialog.bookIds = [bookId]
            convertAndSendDialog.root = target
            convertAndSendDialog.format = format
            convertAndSendDialog.title = qsTr("Convert “%1” to %2 for %3?").arg(row ? row.title : "").arg(format)
                .arg(device.name)
            convertAndSendDialog.message = (conversions.calibre ? qsTr("%1 reads none of its formats. Calibre converts it first, then it is sent.")
                                                                : qsTr("%1 reads none of its formats. Cousoir converts it first, then it is sent."))
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
                                               && conversions.formatsFor(id).includes(format))
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
            convertAndSendDialog.title = qsTr("Convert %n books to %1 for %2?", "", convertible.length).arg(format).arg(device.name)
            convertAndSendDialog.message = (conversions.calibre ? qsTr("%1 reads none of their formats. Calibre converts them first, then they are sent.")
                                                                : qsTr("%1 reads none of their formats. Cousoir converts them first, then they are sent."))
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

    // Asks before removing a book from the reader the device page shows.
    function removeFromDevice(lpath, title) {
        removeFromDeviceDialog.lpath = lpath
        removeFromDeviceDialog.title = qsTr("Remove “%1” from %2?").arg(title).arg(devices.current.name)
        removeFromDeviceDialog.ask()
    }

    function ejectDevice(root) {
        const error = devices.eject(root)
        if (error)
            showNotice(error)
    }

    // Puts where you stopped and your highlights on the Kobo.
    function syncReading(root) {
        const error = devices.syncReading(root)
        if (error)
            showNotice(error)
    }

    // Reads the library again, or the reader the device page shows.
    function reload() {
        if (deviceShown)
            devices.refresh()
        else
            session.reload()
    }

    // The interface's language changed: its texts follow by themselves, the
    // dates, sizes and messages the library and the readers made are made
    // again.
    function relocalize() {
        session.reload()
        devices.refresh()
    }

    // Q or Ctrl+Q: asks first while something is still being made.
    function quit() {
        if (working && !quitConfirmed) {
            quitDialog.ask()
            return
        }
        bookPage.finishEditing()
        windowSettings.width = window.width
        windowSettings.height = window.height
        // The readers close first, so that the place and the highlights of
        // the book being read are handed to the library before it goes.
        if (readerLoader.item)
            readerLoader.item.close()
        if (pdfReaderLoader.item)
            pdfReaderLoader.item.close()
        Qt.quit()
    }

    // "?": every shortcut of the window.
    function shortcutSections() {
        const row = (keys, action) => ({ keys: keys, action: action })
        return [
            { title: qsTr("Moving around"), rows: [
                row("↑ ↓ ← →", qsTr("Move in the list or the covers")),
                row("Page Up, Page Down", qsTr("Move by a page")),
                row("Home, End", qsTr("Go to the first or the last")),
                row("Enter, Space", qsTr("Open the book, or the section's item")),
                row("Esc, Alt+←", qsTr("Go back up one level")),
                row("Ctrl+F", qsTr("Search")),
                row("Ctrl+K", qsTr("Find a book, an author, a section or an action")),
                row("Ctrl+G", qsTr("Show the books as covers or as a table")),
                row("Ctrl+I", qsTr("Show or hide the details pane")),
                row("F5, Ctrl+R", qsTr("Reload the library, or read the reader again")),
                row("Ctrl++, Ctrl+-, Ctrl+0", qsTr("Text size")),
                row("F6, Shift+F6", qsTr("Move between the sidebar, the list and the details")),
                row("?", qsTr("Show or hide these shortcuts")),
                row("Q, Ctrl+Q", qsTr("Quit"))
            ] },
            { title: qsTr("Books"), rows: [
                row("Ctrl+N", qsTr("Add books")),
                row("Ctrl+O", qsTr("Open in your ebook reader")),
                row("Ctrl+Shift+O", qsTr("Read in Cousoir")),
                row("Ctrl+D", qsTr("Send to the reader plugged in")),
                row("Ctrl+Shift+K", qsTr("Convert to KEPUB")),
                row("Ctrl+Shift+A", qsTr("Convert to AZW3")),
                row("Del", qsTr("Move to the trash, or remove from the reader")),
                row("Menu, Shift+F10", qsTr("The book's actions")),
                row(qsTr("Ctrl+click, Shift+click"), qsTr("Select several books")),
                row("Shift+↑ ↓", qsTr("Select more books")),
                row("Ctrl+A", qsTr("Select every book listed"))
            ] },
            { title: qsTr("Editing"), rows: [
                row("Enter", qsTr("Save the field")),
                row("Tab", qsTr("Save the field and go to the next")),
                row("Esc", qsTr("Put the field back as it was")),
                row("Ctrl+Enter", qsTr("Save the description")),
                row("Ctrl+V", qsTr("Paste the clipboard's image as the cover")),
                row("Ctrl+Z", qsTr("Undo the last change to several books, or every change to the open book"))
            ] },
            { title: qsTr("Elsewhere"), rows: [
                row("Enter", qsTr("Restore the selected entry, in the trash")),
                row("Enter, Menu", qsTr("A finding's actions, in the check")),
                row("Ctrl+Shift+C", qsTr("Copy the book's highlights as Markdown")),
                row("Ctrl+U", qsTr("Show the highlights as Markdown, or as a page"))
            ] }
        ]
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
                                                 : qsTr("Move %n books to the trash?", "", bookIds.length)
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

    // Reads the open book, or the current row's, in Cousoir's own reader:
    // its EPUB, else its PDF.
    function readBook() {
        readBookOf(currentBookId())
    }

    // The book read, searched for `query`: from the books' text.
    function readAndFind(bookId, query) {
        readBookOf(bookId)
        // Calibre's search syntax left out: the reader looks for the words.
        const words = query.replace(/["*]/g, "").trim()
        const reader = library.readableFormat(bookId) === "PDF" ? null : readerLoader.item
        if (reader && words)
            reader.searchFor(words)
    }

    // A file of no library read as it is: false when the readers do not
    // read its format.
    function readFile(path) {
        if (!library.readerAvailable)
            return false
        const extension = path.split(".").pop().toLowerCase()
        const pdf = extension === "pdf"
        if (!pdf && ["epub", "kepub", "azw3", "mobi", "azw", "fb2", "fbz", "txt"].indexOf(extension) < 0)
            return false
        const loader = pdf ? pdfReaderLoader : readerLoader
        if (!loader.item)
            loader.setSource(pdf ? "PdfReaderWindow.qml" : "ReaderWindow.qml", { library: window.library })
        if (loader.item)
            loader.item.openFile(path)
        else
            showNotice(qsTr("The reader cannot be opened."))
        return true
    }

    function readBookOf(bookId) {
        if (!bookId || !library.readerAvailable)
            return
        const format = library.readableFormat(bookId)
        if (!format) {
            showNotice(qsTr("This book has no EPUB, Kindle book or PDF to read."))
            return
        }
        const loader = format === "PDF" ? pdfReaderLoader : readerLoader
        if (!loader.item)
            loader.setSource(format === "PDF" ? "PdfReaderWindow.qml" : "ReaderWindow.qml", { library: window.library })
        if (loader.item)
            loader.item.openBook(bookId)
        else
            showNotice(qsTr("The reader cannot be opened."))
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
        add("newLibrary", qsTr("New library…"), "", "folder", true)
        add("saveToDisk", books > 1 ? qsTr("Save %n books to disk…", "", books) : qsTr("Save to disk…"), "", "folder",
            books > 0 && library.loaded)
        add("read", qsTr("Read in Cousoir"), "Ctrl+Shift+O", "book", currentBookId() > 0 && library.readerAvailable)
        add("reader", qsTr("Open in reader"), "Ctrl+O", "external", currentBookId() > 0)
        add("download", !session.bookOpen && books > 1 ? qsTr("Download metadata for %n books", "", books)
                                                       : qsTr("Download metadata and covers"), "", "globe",
            (session.bookOpen || books > 0) && library.writable)
        add("kepub", books > 1 ? qsTr("Convert %n books to KEPUB", "", books) : qsTr("Convert to KEPUB"), "Ctrl+Shift+K",
            "refresh", books > 0 && library.writable)
        add("azw3", books > 1 ? qsTr("Convert %n books to AZW3", "", books) : qsTr("Convert to AZW3"), "Ctrl+Shift+A",
            "refresh", books > 0 && library.writable)
        for (const device of devices.devices) {
            add("send:" + device.root, books > 1 ? qsTr("Send %n books to %1", "", books).arg(device.name)
                                                 : qsTr("Send to %1").arg(device.name),
                device === devices.devices[0] ? "Ctrl+D" : "", "ereader",
                books > 0 && devices.writable)
            add("sync:" + device.root, qsTr("Sync reading with %1").arg(device.name), "", "refresh",
                device.kind === "kobo" && devices.writable && !devices.syncing)
            add("eject:" + device.root, qsTr("Eject %1").arg(device.name), "", "eject", device.ejectable === true)
        }
        add("trash", books > 1 ? qsTr("Move %n books to the trash", "", books) : qsTr("Move to the trash"), "Del",
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
        add("shortcuts", qsTr("Keyboard shortcuts"), "?", "info", true)
        // The list's sort and columns, as a header's click and menu make
        // them.
        if (booksShown) {
            for (const column of library.availableColumns) {
                add("sort:" + column.key, qsTr("Sort by “%1”").arg(column.title),
                    library.sortOption === column.key ? qsTr("Z to A next") : "", "list", true)
                if (column.key !== "title")
                    add((column.shown ? "hideColumn:" : "showColumn:") + column.key,
                        column.shown ? qsTr("Hide the “%1” column").arg(column.title)
                                     : qsTr("Show the “%1” column").arg(column.title), "", "list", !layoutSettings.gridMode)
            }
        }
        if (facetShown) {
            add("facetSort:name", qsTr("Sort by name"), "", "list", true)
            add("facetSort:count", qsTr("Sort by number of books"), "", "list", true)
        }
        add("messages", qsTr("Messages"), "", "info", messages.length > 0)
        for (const mode of availableThemeModes()) {
            const titles = { system: qsTr("System theme"), light: qsTr("Light theme"), dark: qsTr("Dark theme"),
                             omarchy: qsTr("Omarchy theme") }
            add("theme:" + mode, titles[mode], "", "theme-" + mode, true)
        }
        add("backups", qsTr("Show the backups of the library"), "", "folder", library.loaded)
        add("syncHighlights", qsTr("Sync the Kobos' highlights"), "", "highlighter", library.loaded)
        add("importHighlights", qsTr("Import a Kobo's highlights…"), "", "highlighter", library.loaded)
        for (const format of ["markdown", "obsidian", "plain"])
            add("copyHighlights:" + format, qsTr("Copy the book's highlights as %1").arg(highlightsPage.formatNames[format]),
                format === "markdown" ? "Ctrl+Shift+C" : "", "highlighter", highlightsShown && highlightsPage.bookShown)
        add("markdownHighlights", highlightsPage.markdownShown ? qsTr("Show the highlights") : qsTr("Show the highlights as Markdown"),
            "Ctrl+U", "highlighter", highlightsShown)
        for (const source of session.highlightSync.sources)
            add("removeHighlights:" + source.device, qsTr("Take %1's highlights out of the library").arg(source.name), "",
                "trash", !source.off && library.writable)
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
        } else if (hit.key === "newLibrary") {
            newLibraryDialog.open()
        } else if (hit.key === "saveToDisk") {
            saveToDisk(targetIds())
        } else if (hit.key === "read") {
            readBook()
        } else if (hit.key === "reader") {
            openInReader()
        } else if (hit.key === "download") {
            downloadMetadata()
        } else if (hit.key === "kepub" || hit.key === "azw3") {
            convertBooks(targetIds(), hit.key.toUpperCase())
        } else if (hit.key.startsWith("send:")) {
            sendBooks(targetIds(), hit.key.slice(5))
        } else if (hit.key.startsWith("sync:")) {
            syncReading(hit.key.slice(5))
        } else if (hit.key.startsWith("eject:")) {
            ejectDevice(hit.key.slice(6))
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
        } else if (hit.key.startsWith("sort:")) {
            library.sortBy(hit.key.slice(5))
        } else if (hit.key.startsWith("showColumn:")) {
            library.columns = library.columns.concat([hit.key.slice(11)])
        } else if (hit.key.startsWith("hideColumn:")) {
            library.columns = library.columns.filter(key => key !== hit.key.slice(11))
        } else if (hit.key.startsWith("facetSort:")) {
            facetPage.sortBy(hit.key.slice(10))
        } else if (hit.key === "messages") {
            showMessages()
        } else if (hit.key === "shortcuts") {
            shortcutsOverlay.open()
        } else if (hit.key.startsWith("theme:")) {
            setThemeMode(hit.key.slice(6))
        } else if (hit.key === "backups") {
            library.openBackupFolder()
        } else if (hit.key === "syncHighlights") {
            highlightsPage.syncNow()
        } else if (hit.key === "importHighlights") {
            section = "highlights"
            highlightsPage.importDatabase()
        } else if (hit.key.startsWith("copyHighlights:")) {
            highlightsPage.copyFormatted(hit.key.slice(15))
        } else if (hit.key === "markdownHighlights") {
            highlightsPage.toggleMarkdown()
        } else if (hit.key.startsWith("removeHighlights:")) {
            section = "highlights"
            highlightsPage.askRemoval(hit.key.slice(17))
        }
    }

    // Calibre's sources, or Open Library, asked about the open book, out of
    // any field first.
    function downloadMetadata() {
        if (!session.bookOpen) {
            // The books selected, together.
            const ids = targetIds()
            if (ids.length > 0)
                bulkDownloadDialog.start(ids)
            return
        }
        bookPage.finishEditing()
        downloadDialog.start(window.book.id, window.book.title || "")
    }

    // A message on the status line, kept with the session's. An error stays
    // until the next message, another section or a click; the rest go after
    // a few seconds. With `desktop`, for work ending while Cousoir is not in
    // front, the desktop shows it too.
    function showNotice(message, isError = true, desktop = false) {
        notice = message
        noticeIsError = isError
        messages = messages.concat([{ text: message, error: isError, time: new Date() }]).slice(-200)
        if (isError) {
            unreadErrors += 1
            noticeTimer.stop()
        } else {
            noticeTimer.restart()
        }
        if (desktop && !window.active && typeof appInstance !== "undefined")
            appInstance.notify(isError ? qsTr("Cousoir: something went wrong") : qsTr("Cousoir"), message)
    }

    function showMessages() {
        unreadErrors = 0
        notice = ""
        messagesPopup.open()
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

    // "3 authors", or "2 of 3 authors" while a search narrows a browse
    // section; nothing for another section.
    function facetCountText(key, shown, total) {
        const narrowed = shown !== total
        switch (key) {
        case "authors":
            return narrowed ? qsTr("%1 of %n authors", "", total).arg(shown) : qsTr("%n authors", "", total)
        case "series":
            return narrowed ? qsTr("%1 of %n series", "", total).arg(shown) : qsTr("%n series", "", total)
        case "tags":
            return narrowed ? qsTr("%1 of %n tags", "", total).arg(shown) : qsTr("%n tags", "", total)
        case "languages":
            return narrowed ? qsTr("%1 of %n languages", "", total).arg(shown) : qsTr("%n languages", "", total)
        case "publishers":
            return narrowed ? qsTr("%1 of %n publishers", "", total).arg(shown) : qsTr("%n publishers", "", total)
        }
        return ""
    }

    function statusText() {
        if (!library.loaded)
            return ""
        if (session.bookOpen)
            return (book.formats || []).map(format => format.format).join(" · ")
        if (section === "trash")
            return qsTr("%n items", "", library.trashEntries.length)
        if (section === "highlights")
            return qsTr("%n highlights", "", highlightsPage.browser.count)
        if (section === "text")
            return textPage.query.trim() ? qsTr("%n books", "", textPage.rows.length) : ""
        if (section === "reading")
            return qsTr("%n books", "", readingPage.rows.length)
        if (section === "check")
            return library.checking ? qsTr("Checking…")
                : qsTr("%n things to check", "", library.check.count || 0)
        if (deviceSection)
            return qsTr("%n books", "", devices.rows.length) + " · "
                + qsTr("%1 free").arg(devices.current.free || "")
        // From the section itself rather than facetShown, which may not have
        // followed a section change yet.
        const facets = session.filterOpen ? "" : facetCountText(section, library.facetRows.length, library.facetCount)
        if (facets)
            return facets
        const shown = library.books.count
        const total = library.scopeCount
        const parts = library.books.selectedCount > 1 ? [qsTr("%n selected", "", library.books.selectedCount)] : []
        parts.push(shown !== total ? qsTr("%1 of %n books", "", total).arg(shown) : qsTr("%n books", "", total))
        return parts.join(" · ")
    }

    // What the status bar says when there is no message: work under way,
    // else the keys of the moment that fit in `available` pixels.
    function hintText(available) {
        if (library.importing)
            return qsTr("Reading %1 of %2 files…").arg(library.importDone).arg(library.importTotal)
        if (conversions.busy) {
            const job = conversions.jobs[0]
            const waiting = conversions.jobs.length - 1
            return (job.running ? qsTr("Converting “%1” to %2 · %3%").arg(job.title).arg(job.format)
                                      .arg(Math.round(job.progress * 100))
                                : qsTr("Converting “%1” to %2 after the changes…").arg(job.title).arg(job.format))
                + (waiting > 0 ? " " + qsTr("(%n more waiting)", "", waiting) : "")
        }
        if (devices.busy) {
            const job = devices.jobs[0]
            const one = job.titles.length === 1
            const percent = Math.round(job.progress * 100)
            const waiting = devices.jobs.length - 1
            return (job.running
                    ? (one ? qsTr("Sending “%1” to %2 · %3%").arg(job.titles[0]).arg(job.name).arg(percent)
                           : qsTr("Sending %n books to %1 · %2%", "", job.titles.length).arg(job.name).arg(percent))
                    : (one ? qsTr("Sending “%1” to %2 after the changes…").arg(job.titles[0]).arg(job.name)
                           : qsTr("Sending %n books to %1 after the changes…", "", job.titles.length).arg(job.name)))
                + (waiting > 0 ? " " + qsTr("(%n more waiting)", "", waiting) : "")
        }
        if (library.saving)
            return qsTr("Saving…")
        return hintLine(available)
    }

    // The keys of the moment and what they do, in the order shown, each with
    // its rank: the line too narrow, the highest rank goes first.
    function hints() {
        const h = (keys, action, rank = 1) => ({ keys: keys, action: action, rank: rank })
        const move = h("↑↓", qsTr("move", "hint"), 3)
        const reload = h("F5", qsTr("reload", "hint"), 3)
        const search = h("Ctrl+F", qsTr("search", "hint"), 2)
        const find = h("Ctrl+K", qsTr("find", "hint"), 2)
        if (session.bookOpen && bookPage.confirmingRevert)
            return [h("Enter", qsTr("revert", "hint")), h("Esc", qsTr("keep", "hint"))]
        if (session.bookOpen && bookPage.editingDescription)
            return [h("Ctrl+Enter", qsTr("save", "hint")), h("Esc", qsTr("cancel", "hint"))]
        if (session.bookOpen && bookPage.editing)
            return [h("Enter", qsTr("save", "hint")), h("Esc", qsTr("revert", "hint")), h("Tab", qsTr("next field", "hint"), 2)]
        if (session.bookOpen) {
            const list = [h("Esc", qsTr("back", "hint")), h("Ctrl+O", qsTr("open in reader", "hint"), 2)]
            if (library.writable && devices.devices.length > 0)
                list.push(h("Ctrl+D", qsTr("send", "hint"), 3))
            else if (library.writable)
                list.push(h("Ctrl+V", qsTr("paste cover", "hint"), 4))
            list.push(h("Ctrl+I", qsTr("details", "hint"), 3))
            return list
        }
        if (trashShown)
            return library.trashEntries.length > 0 ? [move, h("Enter", qsTr("restore", "hint"))] : [reload]
        if (textShown)
            return textPage.rows.length > 0 ? [move, h("Enter", qsTr("read", "hint"))] : []
        if (readingShown)
            return readingPage.rows.length > 0 ? [move, h("Enter", qsTr("read", "hint"))] : []
        if (checkShown)
            return (library.check.count || 0) > 0
                ? [move, h("Enter", qsTr("actions", "hint")), h("F5", qsTr("check again", "hint"), 3)]
                : [h("F5", qsTr("check again", "hint"))]
        if (deviceShown) {
            if (devices.rows.length === 0)
                return [h("F5", qsTr("read again", "hint"))]
            const list = [move, h("Enter", qsTr("show in library", "hint"))]
            if (devices.writable)
                list.push(h("Del", qsTr("remove from %1", "hint").arg(devices.current.name), 2))
            return list
        }
        if (facetShown)
            return library.facetRows.length > 0 ? [move, h("Enter", qsTr("show books", "hint")), search, find]
                                                : [search, find, reload]
        if (library.books.count === 0)
            return session.filterOpen ? [h("Esc", qsTr("back", "hint")), reload] : [search, find, reload]
        if (editingMany) {
            const list = [h("Esc", qsTr("one book", "hint")), h("Shift+↑↓", qsTr("more", "hint"), 2),
                          h("Ctrl+A", qsTr("all", "hint"), 3)]
            if (library.writable)
                list.push(h("Del", qsTr("trash", "hint"), 2))
            if (devices.devices.length > 0)
                list.push(h("Ctrl+D", qsTr("send", "hint"), 3))
            return list
        }
        const list = session.filterOpen ? [h("Esc", qsTr("back", "hint"))] : []
        list.push(move, h("Enter", qsTr("open", "hint")), h("Ctrl+O", qsTr("open in reader", "hint"), 4),
                  h("Ctrl+G", layoutSettings.gridMode ? qsTr("list", "hint") : qsTr("covers", "hint"), 3))
        if (library.writable)
            list.push(h("Del", qsTr("trash", "hint"), 2))
        return list
    }

    // The hints that fit in `available` pixels of the status bar.
    function hintLine(available) {
        let items = hints()
        const line = list => list.map(item => qsTr("%1 %2", "hint").arg(item.keys).arg(item.action)).join(" · ")
        while (items.length > 1 && hintMetrics.advanceWidth(line(items)) > available) {
            const ranks = items.map(item => item.rank)
            const dropped = ranks.lastIndexOf(Math.max(...ranks))
            items = items.slice(0, dropped).concat(items.slice(dropped + 1))
        }
        return line(items)
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
    readonly property MetadataDownloader downloads: session.downloads
    readonly property BulkMetadataDownloader bulkDownloads: session.bulkDownloads
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

        function onRecoveryWaiting() {
            window.showNotice(qsTr("A change that was interrupted will be put right once nothing else uses the library."))
        }

        function onBooksAdded(bookIds) {
            window.showNotice(qsTr("Added %n books.", "", bookIds.length), false)
            if (window.booksShown)
                booksPage.select(bookIds[0])
        }

        function onDuplicatesFound(titles, total) {
            const one = titles.length === 1
            const shown = titles.slice(0, 4).map(title => "“" + title + "”").join(", ")
            duplicatesDialog.title = one ? qsTr("“%1” is already in the library").arg(titles[0])
                                         : qsTr("%n of these books are already in the library", "", titles.length)
            duplicatesDialog.message = (one ? "" : titles.length > 4 ? qsTr("%1 and %2 more.").arg(shown).arg(titles.length - 4)
                                                                     : shown + ".")
                + (total > titles.length ? (one ? "" : " ")
                    + qsTr("The %n other books are added either way.", "", total - titles.length) : "")
            duplicatesDialog.acceptText = one ? qsTr("Add it anyway") : qsTr("Add them anyway")
            duplicatesDialog.rejectText = one ? qsTr("Skip it") : qsTr("Skip them")
            duplicatesDialog.ask()
        }

        function onImportWarning(message) {
            window.showNotice(message)
        }

        function onSavingChanged() {
            window.sendConverted()
        }

        function onLibraryChanged() {
            window.sendConverted()
        }

        // The library goes back to its books when the saved search shown is
        // gone; the sidebar follows it.
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

    Connections {
        target: window.conversions

        function onConverted(bookId, title, format) {
            window.showNotice(qsTr("“%1” converted to %2.").arg(title).arg(format), false, true)
        }

        function onFailed(message) {
            window.showNotice(message, true, true)
        }

        function onJobsChanged() {
            window.sendConverted()
        }
    }

    Connections {
        target: window.devices

        function onSent(titles, name) {
            window.showNotice(titles.length === 1 ? qsTr("“%1” sent to %2.").arg(titles[0]).arg(name)
                                                  : qsTr("%n books sent to %1.", "", titles.length).arg(name), false, true)
        }

        function onEjected(name) {
            window.showNotice(qsTr("%1 can be unplugged.").arg(name), false)
        }

        function onReadingSynced(message, skipped) {
            window.showNotice(skipped.length === 0 ? message
                                                   : skipped.length === 1 ? qsTr("%1 Left out: %2.").arg(message).arg(skipped[0])
                                                   : qsTr("%1 Left out: %2, and %3 more.").arg(message).arg(skipped[0]).arg(skipped.length - 1),
                              false, true)
        }

        function onFailed(message) {
            window.showNotice(message, true, true)
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
        onRoundingChanged: window.applyOmarchyTheme()
        onWindowOpacityChanged: window.applyOmarchyTheme()
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

    // The window's size and the section shown, as they were left.
    Settings {
        id: windowSettings
        category: "Window"
        property int width: 1240
        property int height: 820
        property string section: "books"
    }

    Settings {
        id: layoutSettings
        category: "Layout"
        property bool showDetails: true
        property bool gridMode: false
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

    BulkDownloadDialog {
        id: bulkDownloadDialog

        downloader: window.bulkDownloads
        onFailed: function(message) { window.showNotice(message) }
        onSaved: function(count) {
            window.showNotice(qsTr("%n books updated from the web; Undo puts them back.", "", count), false)
        }
        onClosed: booksPage.focusList()
    }

    MetadataDownloadDialog {
        id: downloadDialog

        downloader: window.downloads
        onFailed: function(message) { window.showNotice(message) }
        onSaved: function(title) {
            window.showNotice(qsTr("“%1” updated from the web; Revert changes undoes it.").arg(title), false)
        }
        onClosed: {
            if (session.bookOpen)
                bookPage.forceActiveFocus()
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
        sequence: "Ctrl+Shift+O"
        enabled: (session.bookOpen || window.booksShown) && library.readerAvailable
        onActivated: window.readBook()
    }

    // Cousoir's reader, made the first time a book is read; built only with
    // Qt WebEngine.
    Loader {
        id: readerLoader
    }

    Loader {
        id: pdfReaderLoader
    }

    Connections {
        target: readerLoader.item
        ignoreUnknownSignals: true

        function onFailed(message) {
            window.showNotice(message)
        }

        function onAdded(title) {
            window.showNotice(qsTr("“%1” is being added to the library.").arg(title), false)
        }
    }

    Connections {
        target: pdfReaderLoader.item
        ignoreUnknownSignals: true

        function onFailed(message) {
            window.showNotice(message)
        }
    }

    Shortcut {
        sequence: "Ctrl+G"
        enabled: window.booksShown
        onActivated: window.toggleGrid()
    }

    // The highlights as kbextract2 copies them, and their Markdown shown.
    Shortcut {
        sequence: "Ctrl+Shift+C"
        enabled: window.highlightsShown && highlightsPage.bookShown
        onActivated: highlightsPage.copyFormatted("markdown")
    }

    Shortcut {
        sequence: "Ctrl+U"
        enabled: window.highlightsShown
        onActivated: highlightsPage.toggleMarkdown()
    }

    Shortcut {
        sequences: [StandardKey.New]
        enabled: window.library.writable && !window.library.importing
        onActivated: addBooksDialog.open()
    }

    Shortcut {
        sequence: "Ctrl+Shift+K"
        enabled: (session.bookOpen || window.booksShown) && window.library.writable
        onActivated: window.convertBooks(window.targetIds(), "KEPUB")
    }

    Shortcut {
        sequence: "Ctrl+Shift+A"
        enabled: (session.bookOpen || window.booksShown) && window.library.writable
        onActivated: window.convertBooks(window.targetIds(), "AZW3")
    }

    Shortcut {
        sequence: "Ctrl+D"
        enabled: (session.bookOpen || window.booksShown) && window.devices.devices.length > 0
            && window.devices.writable
        onActivated: window.sendBooks(window.targetIds(), "")
    }

    Shortcut {
        sequences: [StandardKey.Delete]
        enabled: window.booksShown && window.library.writable
        onActivated: window.trashBooks(window.targetIds())
    }

    // A field being edited undoes its own typing; otherwise the last change
    // to several books, or every change to the open book, asked first.
    Shortcut {
        sequences: [StandardKey.Undo]
        enabled: (window.editingMany && window.library.bulkUndoCount > 0)
            || (session.bookOpen && window.library.canRevert && !bookPage.editing)
        onActivated: {
            if (window.editingMany) {
                window.library.undoBulkEdit()
            } else {
                bookPage.forceActiveFocus()
                bookPage.confirmingRevert = true
            }
        }
    }

    Shortcut {
        sequence: "?"
        onActivated: shortcutsOverlay.toggle()
    }

    Shortcut {
        sequences: ["Q", StandardKey.Quit]
        onActivated: window.quit()
    }

    FilePicker {
        id: addBooksDialog

        title: qsTr("Add books")
        mode: PortalFileChooser.OpenFiles
        nameFilters: [qsTr("Books (*.epub *.kepub *.azw3 *.azw *.mobi *.prc *.pdf *.fb2 *.cbz *.cbr *.djvu *.docx *.rtf *.txt)"),
                      qsTr("All files (*)")]
        onAccepted: function(files) { window.addBooks(files) }
        onFailed: function(message) { window.showNotice(message) }
    }

    // The books to save to disk, while their folder is picked.
    property var savingIds: []

    function saveToDisk(bookIds) {
        if (bookIds.length === 0)
            return
        window.savingIds = bookIds
        saveToDiskDialog.open()
    }

    LibraryFolderDialog {
        id: saveToDiskDialog
        objectName: "saveToDiskDialog"

        title: qsTr("Choose a folder to save the books in")
        libraryPath: ""
        onChosen: function(path) {
            const error = window.library.saveToDisk(window.savingIds, path)
            if (error)
                window.showNotice(error)
        }
        onFailed: function(message) { window.showNotice(message) }
    }

    Connections {
        target: window.library

        function onSavedToDisk(saved, failures, folder) {
            if (failures.length === 0)
                window.showNotice(qsTr("%n books saved to disk in %1.", "", saved).arg(folder), false, true)
            else
                window.showNotice(qsTr("%n books could not be saved to disk; the first: %1", "", failures.length)
                                  .arg(failures[0]))
        }
    }

    // A new library, in an empty folder, opened once made.
    LibraryFolderDialog {
        id: newLibraryDialog
        objectName: "newLibraryDialog"

        title: qsTr("Choose an empty folder for the new library")
        libraryPath: ""
        onChosen: function(path) {
            const error = window.library.createLibrary(path)
            if (error) {
                window.showNotice(error)
                return
            }
            librarySettings.path = path
            window.showNotice(qsTr("A new library was made in %1.").arg(path), false)
        }
        onFailed: function(message) { window.showNotice(message) }
    }

    LibraryFolderDialog {
        id: libraryFolderDialog

        libraryPath: window.library.libraryPath
        onChosen: function(path) { librarySettings.path = path }
        onFailed: function(message) { window.showNotice(message) }
    }

    ShortcutsOverlay {
        id: shortcutsOverlay

        sections: window.shortcutSections()
        onClosed: window.focusCurrentList()
    }

    // F6 and Shift+F6: from the sidebar to the list, to the details, and
    // round.
    Shortcut {
        sequence: "F6"
        onActivated: window.cycleFocus(1)
    }

    Shortcut {
        sequence: "Shift+F6"
        onActivated: window.cycleFocus(-1)
    }

    MessagesPopup {
        id: messagesPopup

        messages: window.messages
        onCleared: window.messages = []
        onClosed: window.focusCurrentList()
    }

    ConfirmDialog {
        id: addFilesDialog
        objectName: "addFilesDialog"

        property var files: []

        message: qsTr("Only the library's books are read in Cousoir.")
        acceptText: qsTr("Add")
        onAccepted: window.addBooks(files)
    }

    Connections {
        // None in the tests, which run the window alone.
        target: typeof appInstance !== "undefined" ? appInstance : null
        ignoreUnknownSignals: true
        function onInvoked(invocation) {
            window.handleInvocation(invocation, true)
        }
    }

    Connections {
        target: window.library
        function onLibraryChanged() {
            if (window.pendingInvocation && window.library.loaded)
                window.handleInvocation(window.pendingInvocation, false)
        }
    }

    ConfirmDialog {
        id: quitDialog
        objectName: "quitDialog"

        title: qsTr("Quit while Cousoir is still working?")
        message: qsTr("The conversions, the books being sent and the files being added stop where they are.")
        acceptText: qsTr("Quit")
        acceptVariant: "danger"
        onAccepted: {
            window.quitConfirmed = true
            window.quit()
        }
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
        // The section left last time, the library's own.
        if (windowSettings.section !== "books" && windowSettings.section !== section)
            section = windowSettings.section
        booksPage.focusList()
        if (typeof startupInvocation !== "undefined")
            handleInvocation(startupInvocation, false)
    }

    onSectionChanged: {
        notice = ""
        // A reader or a saved search may be gone next time.
        if (!section.startsWith("device:") && !section.startsWith("search:"))
            windowSettings.section = section
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

    onClosing: function(close) {
        // Work under way is asked about first.
        if (window.working && !window.quitConfirmed) {
            close.accepted = false
            quitDialog.ask()
            return
        }
        windowSettings.width = window.width
        windowSettings.height = window.height
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
            + (stopAdding.visible ? stopAdding.width + Theme.gutter : 0)
            + (messagesButton.visible ? messagesButton.width + Theme.gutter : 0)
            + (detailsToggle.visible ? detailsToggle.width : 0), width * 0.45)
        readonly property real sideWidth: Math.max(countWidth, stateWidth)
        // Equal sides keep the hints centered while 40 characters of them
        // still fit; a long write state then takes room from the count's side.
        readonly property bool balanced: 2 * sideWidth + 40 * Theme.captionCharWidth <= width

        implicitHeight: Theme.statusHeight
        color: Theme.surface

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
                id: keyHintsLabel
                objectName: "keyHints"

                Layout.fillWidth: true
                text: window.notice || window.hintText(width)

                FontMetrics {
                    id: hintMetrics
                    font: keyHintsLabel.font
                }
                color: window.notice && window.noticeIsError ? Theme.danger : window.notice ? Theme.text : Theme.textMuted
                font.pointSize: Theme.fontPointSizeCaption
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight

                // A message cut short is read whole among the messages.
                HoverHandler {
                    id: hintsHover
                }
                TapHandler {
                    enabled: window.notice !== ""
                    onTapped: window.showMessages()
                }
                ToolTip.visible: window.notice !== "" && hintsHover.hovered && truncated
                ToolTip.delay: 400
                ToolTip.text: window.notice
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
                    tip: qsTr("Stop the %n conversions", "", window.conversions.jobs.length)
                    icon.source: "assets/icons/close.svg"
                    onClicked: window.conversions.cancel(0)
                }

                StatusButton {
                    id: stopAdding
                    objectName: "stopAdding"

                    visible: window.library.importing
                    text: qsTr("Stop adding")
                    tip: qsTr("Stop adding books")
                    icon.source: "assets/icons/close.svg"
                    onClicked: window.library.cancelImport()
                }

                StatusButton {
                    id: messagesButton
                    objectName: "messagesButton"

                    visible: window.messages.length > 0
                    text: qsTr("Messages")
                    tip: window.unreadErrors > 0 ? qsTr("Messages: %n errors not seen", "", window.unreadErrors)
                                                 : qsTr("Messages")
                    icon.source: "assets/icons/info.svg"
                    icon.color: window.unreadErrors > 0 ? Theme.danger : Theme.textMuted
                    onClicked: window.showMessages()
                }

                StatusButton {
                    id: stopSending
                    objectName: "stopSending"

                    visible: window.devices.busy
                    text: qsTr("Stop sending")
                    tip: qsTr("Stop the %n sendings", "", window.devices.jobs.length)
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
                id: toolbar
                objectName: "toolbar"

                // The book's text buttons, as wide as their words in the
                // language shown: icons alone when the toolbar has no room
                // for them beside the breadcrumb and the other buttons.
                readonly property real bookButtonsWidth: (window.library.canRevert ? [qsTr("Revert changes")] : [])
                    .concat([qsTr("Read"), qsTr("Open in reader")])
                    .reduce((sum, text) => sum + toolbarMetrics.advanceWidth(text) + 2 * Theme.controlInset
                            + Theme.iconSize + 2 * Theme.gutter, 0)
                readonly property bool compact: width < bookButtonsWidth + 6 * Theme.rowHeight + 28 * Theme.charWidth

                FontMetrics {
                    id: toolbarMetrics
                    font: window.font
                }

                Layout.fillWidth: true
                implicitHeight: Theme.toolbarHeight
                color: Theme.surface

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
                        iconOnly: toolbar.compact
                        icon.source: toolbar.compact ? "assets/icons/refresh.svg" : ""
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
                        objectName: "downloadMetadataButton"

                        visible: session.bookOpen && !bookPage.confirmingRevert && window.library.writable
                        iconOnly: true
                        text: qsTr("Download metadata and covers")
                        icon.source: "assets/icons/globe.svg"
                        onClicked: window.downloadMetadata()
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
                        objectName: "readButton"

                        visible: session.bookOpen && !bookPage.confirmingRevert && library.readerAvailable
                        // Whatever Ctrl+Shift+O reads: Kindle books and
                        // those Calibre makes readable too.
                        enabled: window.book.id > 0 && window.library.readableFormat(window.book.id) !== ""
                        variant: "outline"
                        iconOnly: toolbar.compact
                        text: qsTr("Read")
                        tip: qsTr("Read in Cousoir (Ctrl+Shift+O)")
                        icon.source: "assets/icons/book.svg"
                        onClicked: window.readBook()
                    }

                    FlatButton {
                        objectName: "openInReaderButton"

                        visible: session.bookOpen && !bookPage.confirmingRevert
                        enabled: (window.book.formats || []).some(format => !format.missing)
                        variant: "outline"
                        iconOnly: toolbar.compact
                        text: qsTr("Open in reader")
                        tip: qsTr("Open in reader (Ctrl+O)")
                        icon.source: "assets/icons/external.svg"
                        onClicked: window.openInReader()
                    }

                    // Where you stopped and your highlights, put on the Kobo
                    // when asked and only then.
                    FlatButton {
                        objectName: "syncReadingButton"

                        visible: window.deviceShown && window.devices.current.kind === "kobo"
                        enabled: !window.devices.busy && !window.devices.syncing && window.devices.writable
                        variant: "outline"
                        text: window.devices.syncing ? qsTr("Syncing…") : qsTr("Sync reading")
                        tip: window.devices.writable
                             ? qsTr("Put where you stopped in your books, and your highlights, on %1").arg(window.devices.current.name || "")
                             : window.devices.readOnlyReason
                        icon.source: "assets/icons/refresh.svg"
                        onClicked: window.syncReading(window.devices.currentRoot)
                    }

                    // While a Kobo is mounted, Calibre's way of letting it
                    // go: unmounted and powered off. A Kindle needs none.
                    FlatButton {
                        objectName: "ejectButton"

                        visible: window.deviceShown && window.devices.current.ejectable === true
                        enabled: !window.devices.busy && !window.devices.syncing
                        variant: "outline"
                        text: qsTr("Eject")
                        tip: qsTr("Eject %1 so it can be unplugged").arg(window.devices.current.name || "")
                        icon.source: "assets/icons/eject.svg"
                        onClicked: window.ejectDevice(window.devices.currentRoot)
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
                            && !window.deviceShown && !window.checkShown && !window.trashShown && !window.highlightsShown
                            && !window.textShown && !window.readingShown
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

                        visible: !session.bookOpen && !window.deviceShown && !window.checkShown && !window.highlightsShown
                            && !window.textShown && !window.readingShown
                        Layout.fillWidth: true
                        Layout.preferredWidth: Math.round(280 * Theme.spacingScale)
                        Layout.maximumWidth: Math.round(280 * Theme.spacingScale)
                        Layout.minimumWidth: Math.round(120 * Theme.spacingScale)
                        placeholderText: !window.facetShown ? qsTr("Search, as in Calibre")
                            : ({ authors: qsTr("Search authors"), series: qsTr("Search series"), tags: qsTr("Search tags"),
                                 languages: qsTr("Search languages"), publishers: qsTr("Search publishers") })[window.section]
                              || qsTr("Search")
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
                        onReadRequested: function(bookId) { window.readBookOf(bookId) }
                        onConvertRequested: function(bookIds, format) { window.convertBooks(bookIds, format) }
                        onSendRequested: function(bookIds, root) { window.sendBooks(bookIds, root) }
                        onDownloadRequested: function(bookIds) { bulkDownloadDialog.start(bookIds) }
                        onReloadRequested: session.reload()
                        onLibraryFolderRequested: libraryFolderDialog.open()
                        onNewLibraryRequested: newLibraryDialog.open()
                        onSaveRequested: function(bookIds) { window.saveToDisk(bookIds) }
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
                        onFailed: function(message) { window.showNotice(message) }
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

                    HighlightsPage {
                        id: highlightsPage
                        objectName: "highlightsPage"
                        library: window.library
                        sync: session.highlightSync
                        onFailed: function(message) { window.showNotice(message) }
                        onNoticed: function(message) { window.showNotice(message, false) }
                    }

                    TextSearchPage {
                        id: textPage
                        objectName: "textPage"
                        library: window.library
                        onReadRequested: function(bookId, query) { window.readAndFind(bookId, query) }
                    }

                    ReadingPage {
                        id: readingPage
                        objectName: "readingPage"
                        library: window.library
                        onReadRequested: function(bookId) { window.readBookOf(bookId) }
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
