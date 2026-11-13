pragma ComponentBehavior: Bound

import QtCore
import QtQml.Models
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Pdf

import Cousoir

// A book's PDF read in Cousoir, a page after the other, the page kept as
// reading goes: its outline and a search of its text beside the pages, a
// page to go to, full screen, and the pages in the theme's colours. The
// zoom stays as it was left, fitting the width, the page, or as set.
ThemedWindow {
    id: pdfWindow
    objectName: "pdfReaderWindow"

    required property BookLibrary library
    readonly property int pages: document.pageCount
    readonly property bool fullScreen: visibility === Window.FullScreen
    // Why the PDF could not be shown, in the window.
    property string error: ""
    property int opens: 0
    property int bookId: 0
    // A file of no library read, its path.
    property string loosePath: ""
    // The theme's colours need shaders, which Qt Quick's software
    // renderer does not run.
    readonly property bool canTheme: contentItem.GraphicsInfo.api !== GraphicsInfo.Software
    readonly property bool themed: pdfSettings.themedPages && canTheme

    signal failed(string message)

    // Each book's zoom, by library and book, the last ones kept.
    function viewKey() {
        return pdfWindow.loosePath !== "" ? "file:" + pdfWindow.loosePath
                                          : pdfWindow.library.libraryPath + "#" + pdfWindow.bookId
    }

    function bookViews() {
        try {
            return JSON.parse(pdfSettings.bookViews) || {}
        } catch (e) {
            return {}
        }
    }

    function keepBookView() {
        const views = bookViews()
        delete views[viewKey()]
        views[viewKey()] = { fit: pdfSettings.fit, scale: view.renderScale }
        const keys = Object.keys(views)
        for (let i = 0; i < keys.length - 200; ++i)
            delete views[keys[i]]
        pdfSettings.bookViews = JSON.stringify(views)
    }

    // A PDF of no library, read as it is: its zoom kept by its path, no page.
    function openFile(path) {
        pdfWindow.error = ""
        bookReader.close()
        pdfWindow.bookId = -1
        pdfWindow.loosePath = path
        pdfWindow.opens += 1
        document.source = "file://" + path.split("/").map(encodeURIComponent).join("/") + "#" + pdfWindow.opens
        searchField.text = ""
        show()
        raise()
        requestActivate()
        startPage.restart()
        return true
    }

    function openBook(bookId) {
        pdfWindow.error = ""
        pdfWindow.loosePath = ""
        const error = bookReader.openPdf(bookId)
        if (error) {
            pdfWindow.failed(error)
            return false
        }
        pdfWindow.bookId = bookId
        // Read again, the same file too: it may have changed. A fragment
        // the file's path leaves aside makes the address another.
        pdfWindow.opens += 1
        document.source = bookReader.fileUrl + "#" + pdfWindow.opens
        searchField.text = ""
        show()
        raise()
        requestActivate()
        startPage.restart()
        return true
    }

    // The zoom kept: fitting the width or the page, or as set.
    function fit(mode) {
        pdfSettings.fit = mode
        if (mode === "width")
            view.scaleToWidth(pageArea.width, pageArea.height)
        else if (mode === "page")
            view.scaleToPage(pageArea.width, pageArea.height)
    }

    function zoom(factor) {
        pdfSettings.fit = "custom"
        view.renderScale = Math.max(0.1, Math.min(10, view.renderScale * factor))
    }

    function toggleFullScreen() {
        if (fullScreen)
            showNormal()
        else
            showFullScreen()
    }

    function showSidebar(tab) {
        pdfSettings.sidebarShown = true
        pdfSettings.sidebarTab = tab
        if (tab === "search") {
            searchField.forceActiveFocus()
            searchField.selectAll()
        } else {
            outline.forceActiveFocus()
        }
    }

    function toggleOutline() {
        if (pdfSettings.sidebarShown && pdfSettings.sidebarTab === "contents") {
            pdfSettings.sidebarShown = false
            view.forceActiveFocus()
        } else {
            showSidebar("contents")
        }
    }

    // The outline's entry at `row`, gone to at its place, the zoom kept.
    function openEntry(row) {
        const index = outline.index(row, 0)
        if (!index.valid)
            return
        const page = outlineModel.data(index, outlineModel.pageRole)
        if (page === undefined || page < 0)
            return
        view.goToLocation(page, outlineModel.data(index, outlineModel.locationRole), 0)
    }

    // The first place found from the page read on, gone to.
    function showFirstFound() {
        const found = view.searchModel
        if (found.count === 0)
            return
        let target = 0
        for (let i = 0; i < found.count; ++i) {
            // PdfSearchModel's Page role.
            if (found.data(found.index(i, 0), 256) >= view.currentPage) {
                target = i
                break
            }
        }
        if (found.currentResult === target) {
            const link = found.currentResultLink
            view.goToLocation(link.page, link.location, 0)
        } else {
            found.currentResult = target
        }
    }

    function pressEscape() {
        if (shortcutsOverlay.opened)
            shortcutsOverlay.close()
        else if (goToBox.opened)
            goToBox.close()
        else if (searchField.activeFocus || outline.activeFocus || results.activeFocus)
            view.forceActiveFocus()
        else if (fullScreen)
            showNormal()
        else
            close()
    }

    // A window of its own, not one over the library's: the library's
    // shortcuts stay its own.
    transientParent: null
    visible: false
    // As the reader was left.
    width: pdfSettings.windowWidth
    height: pdfSettings.windowHeight
    minimumWidth: 480
    minimumHeight: 400
    title: bookReader.title || document.title || qsTr("Reader")

    onClosing: {
        if (!fullScreen) {
            pdfSettings.windowWidth = width
            pdfSettings.windowHeight = height
        }
        pdfSettings.renderScale = view.renderScale
        if (document.status === PdfDocument.Ready) {
            if (pdfWindow.loosePath === "")
                bookReader.savePage(view.currentPage, document.pageCount)
            pdfWindow.keepBookView()
        }
        bookReader.close()
        if (fullScreen)
            showNormal()
    }

    Settings {
        id: pdfSettings
        category: "PdfReader"
        property int windowWidth: 900
        property int windowHeight: 1000
        property real renderScale: 1
        // "width", "page" or "custom".
        property string fit: "width"
        property bool sidebarShown: false
        // "contents" or "search".
        property string sidebarTab: "contents"
        property int sidebarWidth: 280
        property bool themedPages: false
        // Each book's zoom, as JSON: { "library#id": { fit, scale } }.
        property string bookViews: "{}"
    }

    BookReader {
        id: bookReader
        library: pdfWindow.library
    }

    PdfDocument {
        id: document

        onStatusChanged: {
            if (document.status === PdfDocument.Error) {
                pdfWindow.error = qsTr("This PDF cannot be read: %1").arg(document.error || qsTr("unknown error"))
                pdfWindow.failed(pdfWindow.error)
            }
        }
    }

    // The page read last, once the document is laid out, at the zoom kept.
    Timer {
        id: startPage
        interval: 50
        onTriggered: {
            if (document.status === PdfDocument.Error)
                return
            if (document.status !== PdfDocument.Ready) {
                restart()
                return
            }
            // The book's own zoom, else the last one used.
            const kept = pdfWindow.bookViews()[pdfWindow.viewKey()]
            if (kept) {
                pdfSettings.fit = kept.fit
                pdfSettings.renderScale = kept.scale
            }
            if (pdfSettings.fit === "custom")
                view.renderScale = pdfSettings.renderScale
            else
                pdfWindow.fit(pdfSettings.fit)
            view.goToPage(pdfWindow.loosePath === "" ? bookReader.startPosition.page || 0 : 0)
            outline.expandRecursively()
            view.forceActiveFocus()
        }
    }

    // A fit follows the window's size.
    Timer {
        id: refit
        interval: 150
        onTriggered: {
            if (document.status === PdfDocument.Ready && pdfSettings.fit !== "custom")
                pdfWindow.fit(pdfSettings.fit)
        }
    }

    Timer {
        id: saveTimer
        interval: 1500
        onTriggered: {
            if (pdfWindow.loosePath === "")
                bookReader.savePage(view.currentPage, document.pageCount)
        }
    }

    Shortcut {
        sequences: ["Esc"]
        onActivated: pdfWindow.pressEscape()
    }

    Shortcut {
        sequences: [StandardKey.Close]
        onActivated: pdfWindow.close()
    }

    Shortcut {
        sequence: "?"
        enabled: !searchField.activeFocus && !goToBox.opened
        onActivated: shortcutsOverlay.toggle()
    }

    ShortcutsOverlay {
        id: shortcutsOverlay

        sections: [{ title: qsTr("Reading"), rows: [
            { keys: "→ Space, Page Down", action: qsTr("Next page") },
            { keys: "← Shift+Space, Page Up", action: qsTr("Previous page") },
            { keys: "Home, End", action: qsTr("Go to the first or the last page") },
            { keys: "Alt+←, Alt+→", action: qsTr("Go back where you were before a link, and forward again") },
            { keys: "T", action: qsTr("Show or hide the outline") },
            { keys: "Ctrl+F", action: qsTr("Search the book") },
            { keys: "F3, Shift+F3", action: qsTr("Go to the next or the previous place found") },
            { keys: "Ctrl+G", action: qsTr("Go to a page") },
            { keys: "Ctrl++, Ctrl+-", action: qsTr("Zoom in or out") },
            { keys: "Ctrl+0", action: qsTr("Fit the page's width") },
            { keys: "I", action: qsTr("Pages in the theme's colours, or as printed") },
            { keys: "F11", action: qsTr("Full screen") },
            { keys: "Esc", action: qsTr("Back to the pages, leave full screen, then close the book") },
            { keys: "Ctrl+W", action: qsTr("Close the book") },
            { keys: "?", action: qsTr("Show or hide these shortcuts") }
        ] }]
    }

    // The keys that turn pages, not while a field has them.
    Shortcut {
        sequences: ["Right", "PgDown", "Space"]
        enabled: !searchField.activeFocus && !goToBox.opened && !outline.activeFocus
        onActivated: view.goToPage(Math.min(view.currentPage + 1, document.pageCount - 1))
    }

    Shortcut {
        sequences: ["Left", "PgUp", "Shift+Space"]
        enabled: !searchField.activeFocus && !goToBox.opened && !outline.activeFocus
        onActivated: view.goToPage(Math.max(view.currentPage - 1, 0))
    }

    Shortcut {
        sequences: ["Home"]
        enabled: !searchField.activeFocus && !goToBox.opened
        onActivated: view.goToPage(0)
    }

    Shortcut {
        sequences: ["End"]
        enabled: !searchField.activeFocus && !goToBox.opened
        onActivated: view.goToPage(document.pageCount - 1)
    }

    Shortcut {
        sequences: ["Alt+Left"]
        enabled: view.backEnabled
        onActivated: view.back()
    }

    Shortcut {
        sequences: ["Alt+Right"]
        enabled: view.forwardEnabled
        onActivated: view.forward()
    }

    Shortcut {
        sequences: ["T"]
        enabled: !searchField.activeFocus && !goToBox.opened
        onActivated: pdfWindow.toggleOutline()
    }

    Shortcut {
        sequences: [StandardKey.Find]
        onActivated: pdfWindow.showSidebar("search")
    }

    Shortcut {
        sequences: ["F3"]
        enabled: view.searchModel.count > 0
        onActivated: view.searchForward()
    }

    Shortcut {
        sequences: ["Shift+F3"]
        enabled: view.searchModel.count > 0
        onActivated: view.searchBack()
    }

    Shortcut {
        sequences: ["Ctrl+G"]
        enabled: document.pageCount > 0
        onActivated: goToBox.ask()
    }

    Shortcut {
        sequences: [StandardKey.ZoomIn, "Ctrl+="]
        onActivated: pdfWindow.zoom(1.15)
    }

    Shortcut {
        sequences: [StandardKey.ZoomOut]
        onActivated: pdfWindow.zoom(1 / 1.15)
    }

    Shortcut {
        sequences: ["Ctrl+0"]
        onActivated: pdfWindow.fit("width")
    }

    Shortcut {
        sequences: ["I"]
        enabled: pdfWindow.canTheme && !searchField.activeFocus && !goToBox.opened
        onActivated: pdfSettings.themedPages = !pdfSettings.themedPages
    }

    Shortcut {
        sequences: ["F11"]
        onActivated: pdfWindow.toggleFullScreen()
    }

    header: Rectangle {
        implicitHeight: pdfWindow.fullScreen ? 0 : Theme.toolbarHeight
        visible: !pdfWindow.fullScreen
        color: Theme.surface

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.gutter
            anchors.rightMargin: Theme.gutter
            spacing: Theme.gutter

            FlatButton {
                objectName: "pdfOutlineButton"

                iconOnly: true
                selected: pdfSettings.sidebarShown
                text: qsTr("Outline and search (T)")
                icon.source: "assets/icons/list.svg"
                onClicked: {
                    if (pdfSettings.sidebarShown) {
                        pdfSettings.sidebarShown = false
                        view.forceActiveFocus()
                    } else {
                        pdfWindow.showSidebar(pdfSettings.sidebarTab)
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                text: bookReader.title || document.title || ""
                elide: Text.ElideRight
                color: Theme.textMuted
            }

            FlatButton {
                objectName: "pdfPage"

                text: qsTr("Page %1 of %2").arg(view.currentPage + 1).arg(document.pageCount)
                tip: qsTr("Go to a page (Ctrl+G)")
                enabled: document.pageCount > 0
                onClicked: goToBox.ask()
            }

            FlatButton {
                objectName: "pdfFitWidth"

                text: qsTr("Width")
                selected: pdfSettings.fit === "width"
                tip: qsTr("Fit the page's width (Ctrl+0)")
                onClicked: pdfWindow.fit("width")
            }

            FlatButton {
                objectName: "pdfFitPage"

                text: qsTr("Page")
                selected: pdfSettings.fit === "page"
                tip: qsTr("Fit the whole page")
                onClicked: pdfWindow.fit("page")
            }

            FlatButton {
                objectName: "pdfThemedPages"

                visible: pdfWindow.canTheme
                iconOnly: true
                selected: pdfSettings.themedPages
                text: qsTr("Pages in the theme's colours (I)")
                icon.source: "assets/icons/theme-dark.svg"
                onClicked: pdfSettings.themedPages = !pdfSettings.themedPages
            }

            FlatButton {
                iconOnly: true
                text: qsTr("Full screen (F11)")
                icon.source: "assets/icons/fullscreen.svg"
                onClicked: pdfWindow.toggleFullScreen()
            }

            FlatButton {
                iconOnly: true
                text: qsTr("Close the book (Esc)")
                icon.source: "assets/icons/close.svg"
                onClicked: pdfWindow.close()
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

        // The outline, or the places found.
        Rectangle {
            id: sidebar
            objectName: "pdfSidebar"

            Layout.preferredWidth: pdfSettings.sidebarWidth
            Layout.fillHeight: true
            visible: pdfSettings.sidebarShown && !pdfWindow.fullScreen
            color: Theme.surface

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.gutter
                spacing: Theme.gutter

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.gutter

                    FlatButton {
                        objectName: "pdfContentsTab"

                        text: qsTr("Outline")
                        selected: pdfSettings.sidebarTab === "contents"
                        onClicked: pdfWindow.showSidebar("contents")
                    }

                    FlatButton {
                        objectName: "pdfSearchTab"

                        text: qsTr("Search")
                        selected: pdfSettings.sidebarTab === "search"
                        onClicked: pdfWindow.showSidebar("search")
                    }

                    Item { Layout.fillWidth: true }
                }

                TreeView {
                    id: outline
                    objectName: "pdfOutline"

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: pdfSettings.sidebarTab === "contents"
                    clip: true
                    model: PdfBookmarkModel {
                        id: outlineModel

                        readonly property int pageRole: 258
                        readonly property int locationRole: 259

                        document: document
                    }
                    selectionModel: ItemSelectionModel {}
                    keyNavigationEnabled: true
                    boundsBehavior: Flickable.StopAtBounds
                    Accessible.role: Accessible.Tree
                    Accessible.name: qsTr("Outline")
                    ScrollBar.vertical: ThinScrollBar {}
                    Keys.onReturnPressed: pdfWindow.openEntry(outline.currentRow)
                    Keys.onEnterPressed: pdfWindow.openEntry(outline.currentRow)

                    delegate: Rectangle {
                        id: entry

                        required property TreeView treeView
                        required property bool expanded
                        required property bool hasChildren
                        required property int depth
                        required property int row
                        required property bool current
                        required property string title
                        required property int page

                        objectName: "pdfOutlineEntry" + row
                        implicitWidth: outline.width
                        implicitHeight: Theme.rowHeight
                        color: entry.current && outline.activeFocus ? Theme.selected
                            : entryHover.hovered ? Theme.hover : "transparent"
                        radius: Theme.smallRadius
                        Accessible.role: Accessible.TreeItem
                        Accessible.name: title

                        HoverHandler {
                            id: entryHover
                        }

                        TapHandler {
                            onTapped: {
                                outline.selectionModel.setCurrentIndex(outline.index(entry.row, 0),
                                                                       ItemSelectionModel.NoUpdate)
                                pdfWindow.openEntry(entry.row)
                            }
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.gutter + entry.depth * Theme.gutter * 2
                            anchors.rightMargin: Theme.gutter
                            spacing: Theme.gutter

                            Label {
                                Layout.fillWidth: true
                                text: entry.title
                                elide: Text.ElideRight
                                font.weight: entry.depth === 0 ? Font.DemiBold : Font.Normal
                            }

                            Label {
                                text: entry.page + 1
                                color: Theme.textFaint
                                font.pointSize: Theme.fontPointSizeCaption
                            }
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: pdfSettings.sidebarTab === "contents" && outline.rows === 0
                    text: qsTr("This PDF has no outline.")
                    color: Theme.textMuted
                    wrapMode: Text.Wrap
                }

                SearchField {
                    id: searchField
                    objectName: "pdfSearchField"

                    Layout.fillWidth: true
                    visible: pdfSettings.sidebarTab === "search"
                    placeholderText: qsTr("Search the book")
                    Accessible.name: qsTr("Search the book")
                    onTextChanged: searchDelay.restart()
                    Keys.onReturnPressed: view.searchForward()
                    Keys.onEnterPressed: view.searchForward()
                    Keys.onDownPressed: results.forceActiveFocus()

                    Timer {
                        id: searchDelay
                        interval: 250
                        onTriggered: view.searchString = searchField.text
                    }
                }

                Label {
                    objectName: "pdfSearchStatus"

                    Layout.fillWidth: true
                    visible: pdfSettings.sidebarTab === "search" && view.searchString !== ""
                    text: view.searchModel.count === 0 ? qsTr("Nothing found.")
                        : qsTr("%1 of %n places", "", view.searchModel.count).arg(view.searchModel.currentResult + 1)
                    color: Theme.textMuted
                    font.pointSize: Theme.fontPointSizeCaption
                }

                ListView {
                    id: results
                    objectName: "pdfSearchResults"

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: pdfSettings.sidebarTab === "search"
                    clip: true
                    model: view.searchModel
                    keyNavigationEnabled: true
                    boundsBehavior: Flickable.StopAtBounds
                    highlightMoveDuration: 0
                    Accessible.role: Accessible.List
                    Accessible.name: qsTr("Places found")
                    ScrollBar.vertical: ThinScrollBar {}
                    onCurrentIndexChanged: {
                        if (currentIndex >= 0 && currentIndex !== view.searchModel.currentResult)
                            view.searchModel.currentResult = currentIndex
                    }
                    Keys.onUpPressed: function(event) {
                        if (currentIndex <= 0)
                            searchField.forceActiveFocus()
                        else
                            decrementCurrentIndex()
                    }

                    delegate: Rectangle {
                        id: found

                        required property int index
                        required property int page
                        required property string contextBefore
                        required property string contextAfter

                        objectName: "pdfSearchResult" + index
                        width: results.width
                        implicitHeight: foundText.implicitHeight + Theme.gutter
                        radius: Theme.smallRadius
                        color: ListView.isCurrentItem ? Theme.selected : foundHover.hovered ? Theme.hover : "transparent"
                        Accessible.role: Accessible.ListItem
                        Accessible.name: qsTr("Page %1").arg(found.page + 1) + ", " + found.contextBefore + " "
                            + view.searchString + " " + found.contextAfter

                        HoverHandler {
                            id: foundHover
                        }

                        TapHandler {
                            onTapped: view.searchModel.currentResult = found.index
                        }

                        Label {
                            id: foundText

                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.margins: Theme.gutter / 2
                            textFormat: Text.StyledText
                            wrapMode: Text.Wrap
                            maximumLineCount: 3
                            elide: Text.ElideRight
                            font.pointSize: Theme.fontPointSizeCaption
                            function escaped(text) {
                                // Qt marks a line's end with ⏎.
                                return text.replace(/[\s\u23ce]+/g, " ").replace(/&/g, "&amp;").replace(/</g, "&lt;")
                                    .replace(/>/g, "&gt;")
                            }
                            text: "<font color=\"" + Theme.textFaint + "\">" + (found.page + 1) + "</font>  "
                                + escaped(found.contextBefore) + " <b>" + escaped(view.searchString) + "</b> "
                                + escaped(found.contextAfter)
                        }
                    }
                }
            }

            // Drag to make it wider or narrower.
            Rectangle {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 1
                color: Theme.divider

                MouseArea {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: Theme.gutter
                    cursorShape: Qt.SplitHCursor
                    preventStealing: true
                    property real startX: 0
                    property int startWidth: 0
                    onPressed: function(mouse) {
                        startX = mapToItem(null, mouse.x, 0).x
                        startWidth = pdfSettings.sidebarWidth
                    }
                    onPositionChanged: function(mouse) {
                        const delta = mapToItem(null, mouse.x, 0).x - startX
                        pdfSettings.sidebarWidth = Math.max(180, Math.min(pdfWindow.width / 2, startWidth + delta))
                    }
                }
            }
        }

        // The pages, on paper white that the theme's colours make the
        // theme's background.
        Rectangle {
            id: pageArea

            Layout.fillWidth: true
            Layout.fillHeight: true
            color: pdfWindow.themed ? "white" : Theme.canvas
            onWidthChanged: refit.restart()
            onHeightChanged: refit.restart()
            layer.enabled: pdfWindow.themed
            layer.effect: ShaderEffect {
                property color paper: Theme.canvas
                property color ink: Theme.text
                fragmentShader: "shaders/themed_pages.frag.qsb"
            }

            PdfMultiPageView {
                id: view
                objectName: "pdfView"

                anchors.fill: parent
                document: document
                visible: pdfWindow.error === ""
                onCurrentPageChanged: saveTimer.restart()
            }

            Connections {
                target: view.searchModel

                function onCountChanged() {
                    pdfWindow.showFirstFound()
                }

                // The list follows the place shown, set rather than bound:
                // the list sets the place too.
                function onCurrentResultChanged() {
                    if (results.currentIndex !== view.searchModel.currentResult)
                        results.currentIndex = view.searchModel.currentResult
                }
            }

            Label {
                objectName: "pdfError"

                anchors.centerIn: parent
                width: Math.min(parent.width - 2 * Theme.pagePadding, Math.round(480 * Theme.spacingScale))
                visible: pdfWindow.error !== ""
                text: pdfWindow.error
                color: Theme.danger
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }
        }
    }

    // Ctrl+G: a page by its number.
    Popup {
        id: goToBox
        objectName: "pdfGoTo"

        function ask() {
            goToField.text = String(view.currentPage + 1)
            open()
        }

        function go() {
            const page = parseInt(goToField.text.trim(), 10)
            if (isNaN(page))
                return
            close()
            view.goToPage(Math.max(1, Math.min(document.pageCount, page)) - 1)
        }

        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round(parent.height * 0.2)
        width: Math.round(32 * Theme.charWidth)
        modal: true
        focus: true
        padding: Theme.panePadding
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onOpened: {
            goToField.forceActiveFocus(Qt.PopupFocusReason)
            goToField.selectAll()
        }
        onClosed: view.forceActiveFocus()

        Overlay.modal: Rectangle {
            color: Qt.rgba(0, 0, 0, 0.35)
        }

        background: Rectangle {
            radius: Theme.radius
            color: Theme.canvas
            border.width: 1
            border.color: Theme.controlBorder
        }

        contentItem: ColumnLayout {
            spacing: Theme.gutter

            Label {
                Layout.fillWidth: true
                text: qsTr("Go to page, of %1").arg(document.pageCount)
                color: Theme.textMuted
            }

            FormField {
                id: goToField
                objectName: "pdfGoToField"

                Layout.fillWidth: true
                inputMethodHints: Qt.ImhDigitsOnly
                Accessible.name: qsTr("Go to page")
                Keys.onReturnPressed: goToBox.go()
                Keys.onEnterPressed: goToBox.go()
            }
        }
    }
}
