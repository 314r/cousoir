import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Controls.impl
import QtQuick.Layouts
import QtCore

import Reliure

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
    readonly property bool facetShown: section !== "books" && !session.filterOpen && !session.bookOpen
    readonly property bool booksShown: !facetShown && !session.bookOpen
    // An error to show in place of the key hints for a while.
    property string notice: ""

    visible: true
    width: 1240
    height: 820
    minimumWidth: 720
    minimumHeight: 480
    title: session.bookOpen && book.title ? qsTr("%1 — Reliure").arg(book.title)
        : session.filterOpen ? qsTr("%1 — Reliure").arg(filter.name)
        : section !== "books" ? qsTr("%1 — Reliure").arg(sidebar.current.title)
        : qsTr("Reliure")
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
        session.closeBook()
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
        return facetShown ? 1 : 0
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

    function showNotice(message) {
        notice = message
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
        if (session.bookOpen)
            return qsTr("Esc back · Ctrl+O open in reader · Ctrl+I details")
        if (facetShown)
            return library.facetRows.length > 0 ? qsTr("↑↓ move · Enter show books · Ctrl+F search")
                                                : qsTr("Ctrl+F search · F5 reload")
        if (library.rows.length === 0)
            return session.filterOpen ? qsTr("Esc back · F5 reload") : qsTr("Ctrl+F search · F5 reload")
        const back = session.filterOpen ? qsTr("Esc back · ") : ""
        return back + qsTr("↑↓ move · Enter open · Ctrl+O read · Ctrl+G %1")
            .arg(layoutSettings.gridMode ? qsTr("list") : qsTr("covers"))
    }

    AppSession {
        id: session
        adaptiveTypography: true
        libraryPath: librarySettings.path
    }
    readonly property BookLibrary library: session.library
    readonly property ConcurrencyGuard guard: session.guard

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

        function onSortOptionChanged() {
            layoutSettings.sortOption = window.library.sortOption
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

        onThemeModeSelected: function(mode) {
            window.setThemeMode(mode)
        }
        onTextScalePercentSelected: function(percent) {
            window.setTextScalePercent(percent)
        }
        onLibraryPathSelected: function(path) {
            librarySettings.path = path
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
        onActivated: window.goBack()
    }

    Shortcut {
        sequences: [StandardKey.Find]
        onActivated: window.focusSearch()
    }

    Shortcut {
        sequences: [StandardKey.Refresh, "Ctrl+R"]
        onActivated: session.reload()
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
        library.section = section
    }

    onWideChanged: {
        if (wide)
            detailsForced = false
    }

    onClosing: settingsWindow.close()

    footer: Rectangle {
        id: statusBar
        objectName: "statusBar"

        readonly property real countWidth: Math.max(statusCount.implicitWidth, Theme.statusHeight)
        readonly property real stateWidth: Math.min((writeState.visible ? writeState.implicitWidth + Theme.gutter : 0)
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
                color: window.notice ? Theme.danger : Theme.textMuted
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

                    FlatButton {
                        objectName: "openInReaderButton"

                        visible: session.bookOpen
                        enabled: (window.book.formats || []).some(format => !format.missing)
                        variant: "outline"
                        text: qsTr("Open in reader")
                        tip: qsTr("Open in reader (Ctrl+O)")
                        icon.source: "assets/icons/external.svg"
                        onClicked: window.openInReader()
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

                        visible: !session.bookOpen
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
                        text: qsTr("Reload library")
                        tip: qsTr("Reload library (F5)")
                        icon.source: "assets/icons/refresh.svg"
                        onClicked: session.reload()
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

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: window.pageIndex()

                BooksPage {
                    id: booksPage
                    objectName: "booksPage"
                    library: window.library
                    gridMode: layoutSettings.gridMode
                    onBookActivated: function(bookId) {
                        window.openBook(bookId)
                    }
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
                    book: window.book
                    detailsVisible: window.detailsVisible
                    onFailed: function(message) { window.showNotice(message) }
                }
            }
        }
    }
}
