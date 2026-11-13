import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Controls.impl
import QtQuick.Layouts

import Cousoir

// The window's toolbar: the way back, where the books shown are, the search
// and the virtual library; the open book's buttons, adding books, reloading
// and the settings.
Rectangle {
    id: toolbar
    objectName: "toolbar"

    required property var window
    required property var session
    required property var bookPage
    required property var booksPage
    required property var facetPage
    required property var layoutSettings
    required property var sidebar
    required property var addBooksDialog
    // The search box, which the window's keys reach.
    readonly property alias searchField: searchField

    // The book's text buttons, as wide as their words in the
    // language shown: icons alone when the toolbar has no room
    // for them beside the breadcrumb and the other buttons.
    readonly property real bookButtonsWidth: (toolbar.window.library.canRevert ? [qsTr("Revert changes")] : [])
        .concat([qsTr("Read"), qsTr("Open in reader")])
        .reduce((sum, text) => sum + toolbarMetrics.advanceWidth(text) + 2 * Theme.controlInset
                + Theme.iconSize + 2 * Theme.gutter, 0)
    readonly property bool compact: width < bookButtonsWidth + 6 * Theme.rowHeight + 28 * Theme.charWidth

    FontMetrics {
        id: toolbarMetrics
        font: toolbar.window.font
    }

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
            enabled: toolbar.session.bookOpen || toolbar.session.filterOpen
            text: qsTr("Back")
            tip: qsTr("Back (Esc)")
            icon.source: "assets/icons/arrow-left.svg"
            onClicked: toolbar.window.goBack()
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

                visible: toolbar.session.bookOpen || toolbar.session.filterOpen
                text: toolbar.sidebar.current.title
                color: rootHover.hovered ? Theme.text : Theme.textMuted
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Back to %1").arg(toolbar.sidebar.current.title)

                HoverHandler {
                    id: rootHover
                    cursorShape: Qt.PointingHandCursor
                }

                TapHandler {
                    onTapped: {
                        if (toolbar.session.bookOpen)
                            toolbar.window.closeBook()
                        if (toolbar.session.filterOpen)
                            toolbar.window.closeFilter()
                    }
                }
            }

            Label {
                visible: toolbar.session.bookOpen || toolbar.session.filterOpen
                text: " / "
                color: Theme.textFaint
            }

            Label {
                objectName: "filterCrumb"

                visible: toolbar.session.bookOpen && toolbar.session.filterOpen
                text: toolbar.window.filter.name || ""
                color: filterHover.hovered ? Theme.text : Theme.textMuted
                elide: Text.ElideRight
                Layout.maximumWidth: Math.round(24 * Theme.charWidth)
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Back to %1").arg(toolbar.window.filter.name || "")

                HoverHandler {
                    id: filterHover
                    cursorShape: Qt.PointingHandCursor
                }

                TapHandler {
                    onTapped: toolbar.window.closeBook()
                }
            }

            Label {
                visible: toolbar.session.bookOpen && toolbar.session.filterOpen
                text: " / "
                color: Theme.textFaint
            }

            Label {
                objectName: "pageTitle"

                Layout.fillWidth: true
                text: toolbar.session.bookOpen ? (toolbar.window.book.title || "")
                    : toolbar.session.filterOpen ? (toolbar.window.filter.name || "")
                    : toolbar.sidebar.current.title
                font.weight: Font.Bold
                elide: Text.ElideRight
            }
        }

        // Every change saves at once; this undoes them all until the
        // book closes, after asking.
        FlatButton {
            objectName: "revertChangesButton"

            visible: toolbar.session.bookOpen && toolbar.window.library.canRevert && !toolbar.bookPage.confirmingRevert
            variant: "outline"
            iconOnly: toolbar.compact
            icon.source: toolbar.compact ? "assets/icons/refresh.svg" : ""
            text: qsTr("Revert changes")
            tip: qsTr("Undo every change since you opened the book")
            // Out of any field first, so Enter and Esc answer.
            onClicked: {
                toolbar.bookPage.finishEditing()
                toolbar.bookPage.forceActiveFocus()
                toolbar.bookPage.confirmingRevert = true
            }
        }

        Label {
            objectName: "revertQuestion"

            // Whole when there is room: a layout rounds a fractional
            // width down, which would elide the last letter.
            Layout.fillWidth: true
            Layout.preferredWidth: Math.ceil(implicitWidth)
            Layout.maximumWidth: Math.ceil(implicitWidth)
            visible: toolbar.session.bookOpen && toolbar.bookPage.confirmingRevert
            text: qsTr("Undo every change since you opened it?")
            elide: Text.ElideRight
        }

        FlatButton {
            objectName: "revertKeepButton"

            visible: toolbar.session.bookOpen && toolbar.bookPage.confirmingRevert
            variant: "outline"
            text: qsTr("Keep")
            onClicked: toolbar.bookPage.confirmingRevert = false
        }

        FlatButton {
            objectName: "confirmRevertButton"

            visible: toolbar.session.bookOpen && toolbar.bookPage.confirmingRevert
            variant: "danger"
            text: qsTr("Revert")
            onClicked: toolbar.bookPage.revertChanges()
        }

        FlatButton {
            objectName: "downloadMetadataButton"

            visible: toolbar.session.bookOpen && !toolbar.bookPage.confirmingRevert && toolbar.window.library.writable
            iconOnly: true
            text: qsTr("Download metadata and covers")
            icon.source: "assets/icons/globe.svg"
            onClicked: toolbar.window.downloadMetadata()
        }

        FlatButton {
            objectName: "trashBookButton"

            visible: toolbar.session.bookOpen && !toolbar.bookPage.confirmingRevert && toolbar.window.library.writable
            iconOnly: true
            text: qsTr("Move to Trash")
            icon.source: "assets/icons/trash.svg"
            onClicked: toolbar.window.trashBook(toolbar.window.book.id)
        }

        FlatButton {
            objectName: "readButton"

            visible: toolbar.session.bookOpen && !toolbar.bookPage.confirmingRevert && toolbar.window.library.readerAvailable
            // Whatever Ctrl+Shift+O reads: Kindle books and
            // those Calibre makes readable too.
            enabled: toolbar.window.book.id > 0 && toolbar.window.library.readableFormat(toolbar.window.book.id) !== ""
            variant: "outline"
            iconOnly: toolbar.compact
            text: qsTr("Read")
            tip: qsTr("Read in Cousoir (Ctrl+Shift+O)")
            icon.source: "assets/icons/book.svg"
            onClicked: toolbar.window.readBook()
        }

        FlatButton {
            objectName: "openInReaderButton"

            visible: toolbar.session.bookOpen && !toolbar.bookPage.confirmingRevert
            enabled: (toolbar.window.book.formats || []).some(format => !format.missing)
            variant: "outline"
            iconOnly: toolbar.compact
            text: qsTr("Open in reader")
            tip: qsTr("Open in reader (Ctrl+O)")
            icon.source: "assets/icons/external.svg"
            onClicked: toolbar.window.openInReader()
        }

        // Where you stopped and your highlights, put on the Kobo
        // when asked and only then.
        FlatButton {
            objectName: "syncReadingButton"

            visible: toolbar.window.deviceShown && toolbar.window.devices.current.kind === "kobo"
            enabled: !toolbar.window.devices.busy && !toolbar.window.devices.syncing && toolbar.window.devices.writable
            variant: "outline"
            text: toolbar.window.devices.syncing ? qsTr("Syncing…") : qsTr("Sync reading")
            tip: toolbar.window.devices.writable
                 ? qsTr("Put where you stopped in your books, and your highlights, on %1").arg(toolbar.window.devices.current.name || "")
                 : toolbar.window.devices.readOnlyReason
            icon.source: "assets/icons/refresh.svg"
            onClicked: toolbar.window.syncReading(toolbar.window.devices.currentRoot)
        }

        // While a Kobo is mounted, Calibre's way of letting it
        // go: unmounted and powered off. A Kindle needs none.
        FlatButton {
            objectName: "ejectButton"

            visible: toolbar.window.deviceShown && toolbar.window.devices.current.ejectable === true
            enabled: !toolbar.window.devices.busy && !toolbar.window.devices.syncing
            variant: "outline"
            text: qsTr("Eject")
            tip: qsTr("Eject %1 so it can be unplugged").arg(toolbar.window.devices.current.name || "")
            icon.source: "assets/icons/eject.svg"
            onClicked: toolbar.window.ejectDevice(toolbar.window.devices.currentRoot)
        }

        FlatButton {
            objectName: "addBooksButton"

            visible: !toolbar.session.bookOpen && !toolbar.window.deviceShown
            enabled: toolbar.window.library.writable && !toolbar.window.library.importing
            iconOnly: true
            text: qsTr("Add books")
            tip: qsTr("Add books (Ctrl+N)")
            icon.source: "assets/icons/plus.svg"
            onClicked: toolbar.addBooksDialog.open()
        }

        FlatButton {
            objectName: "viewToggle"

            visible: toolbar.window.booksShown
            iconOnly: true
            text: toolbar.layoutSettings.gridMode ? qsTr("Show as list") : qsTr("Show covers")
            tip: qsTr("%1 (Ctrl+G)").arg(text)
            icon.source: toolbar.layoutSettings.gridMode ? "assets/icons/list.svg" : "assets/icons/grid.svg"
            onClicked: toolbar.window.toggleGrid()
        }

        // Calibre's virtual libraries: one narrows every section,
        // as in Calibre.
        ChoiceBox {
            objectName: "virtualLibraryBox"

            readonly property var choices: [{ value: "", label: qsTr("All books") }].concat(
                toolbar.window.library.virtualLibraries.map(name => ({ value: name, label: name })))

            visible: toolbar.window.library.virtualLibraries.length > 0 && !toolbar.session.bookOpen
                && !toolbar.window.deviceShown && !toolbar.window.checkShown && !toolbar.window.trashShown && !toolbar.window.highlightsShown
                && !toolbar.window.textShown && !toolbar.window.readingShown
            // The longest name, its arrow and paddings.
            Layout.preferredWidth: Math.round(Math.min(24, choices.reduce(
                (longest, choice) => Math.max(longest, choice.label.length), 0)) * Theme.charWidth)
                + leftPadding + rightPadding
            Accessible.name: qsTr("Virtual library")
            model: choices
            currentIndex: Math.max(0, choices.findIndex(choice => choice.value === toolbar.window.library.virtualLibrary))
            onActivated: function(index) {
                toolbar.window.library.virtualLibrary = choices[index].value
            }
        }

        SearchField {
            id: searchField
            objectName: "searchField"

            function submit() {
                if (toolbar.window.facetShown)
                    toolbar.facetPage.activate(0)
                else
                    toolbar.booksPage.activate(0)
            }

            visible: !toolbar.session.bookOpen && !toolbar.window.deviceShown && !toolbar.window.checkShown && !toolbar.window.highlightsShown
                && !toolbar.window.textShown && !toolbar.window.readingShown
            Layout.fillWidth: true
            Layout.preferredWidth: Math.round(280 * Theme.spacingScale)
            Layout.maximumWidth: Math.round(280 * Theme.spacingScale)
            Layout.minimumWidth: Math.round(120 * Theme.spacingScale)
            placeholderText: !toolbar.window.facetShown ? qsTr("Search, as in Calibre")
                : ({ authors: qsTr("Search authors"), series: qsTr("Search series"), tags: qsTr("Search tags"),
                     languages: qsTr("Search languages"), publishers: qsTr("Search publishers") })[toolbar.window.section]
                  || qsTr("Search")
            text: toolbar.window.library.searchText

            onTextEdited: toolbar.window.library.searchText = text
            Keys.onDownPressed: toolbar.window.focusCurrentList()
            Keys.onReturnPressed: submit()
            Keys.onEnterPressed: submit()
            Keys.onEscapePressed: {
                // Clear through the model so the text binding survives.
                if (toolbar.window.library.searchText.length > 0)
                    toolbar.window.library.searchText = ""
                else
                    toolbar.window.focusCurrentList()
            }
        }

        FlatButton {
            objectName: "refreshButton"

            visible: !toolbar.session.bookOpen
            iconOnly: true
            text: toolbar.window.deviceShown ? qsTr("Read the device again") : qsTr("Reload library")
            tip: qsTr("%1 (F5)").arg(text)
            icon.source: "assets/icons/refresh.svg"
            onClicked: toolbar.window.reload()
        }

        Divider {
            visible: !toolbar.session.bookOpen
            Layout.preferredWidth: 1
            Layout.preferredHeight: Math.round(Theme.rowHeight * 0.6)
        }

        FlatButton {
            objectName: "settingsButton"

            visible: !toolbar.session.bookOpen
            iconOnly: true
            text: qsTr("Settings")
            icon.source: "assets/icons/settings.svg"
            onClicked: toolbar.window.openSettings("")
        }
    }
}
