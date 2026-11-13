import QtQuick

import Cousoir

// The window's keys: each a Shortcut of the window, working wherever the
// focus is in it unless a field takes the key.
Item {
    id: shortcuts

    required property var window
    required property var session
    required property var bookPage
    required property var booksPage
    required property var commandPalette
    required property var highlightsPage
    required property var shortcutsOverlay
    required property var addBooksDialog

    visible: false

    Shortcut {
        sequences: [StandardKey.ZoomIn, "Ctrl+="]
        onActivated: shortcuts.window.adjustTextScalePercent(10)
    }

    Shortcut {
        sequences: [StandardKey.ZoomOut]
        onActivated: shortcuts.window.adjustTextScalePercent(-10)
    }

    Shortcut {
        sequence: "Ctrl+0"
        onActivated: shortcuts.window.setTextScalePercent(100)
    }

    Shortcut {
        sequences: ["Esc", "Alt+Left", StandardKey.Back]
        enabled: shortcuts.session.bookOpen || shortcuts.session.filterOpen || (shortcuts.window.booksShown && shortcuts.booksPage.selectedCount > 1)
        // The revert question keeps the changes instead; several books
        // selected go back to the current one first.
        onActivated: {
            if (shortcuts.bookPage.confirmingRevert)
                shortcuts.bookPage.confirmingRevert = false
            else if (shortcuts.window.booksShown && shortcuts.booksPage.selectedCount > 1)
                shortcuts.booksPage.collapseSelection()
            else
                shortcuts.window.goBack()
        }
    }

    Shortcut {
        sequences: ["Return", "Enter"]
        enabled: shortcuts.session.bookOpen && shortcuts.bookPage.confirmingRevert
        onActivated: shortcuts.bookPage.revertChanges()
    }

    Shortcut {
        sequences: [StandardKey.Paste]
        enabled: shortcuts.session.bookOpen && shortcuts.window.library.writable && !shortcuts.bookPage.editing
        onActivated: shortcuts.bookPage.pasteCover()
    }

    Shortcut {
        sequences: [StandardKey.Find]
        onActivated: shortcuts.window.focusSearch()
    }

    Shortcut {
        sequence: "Ctrl+K"
        enabled: shortcuts.window.library.loaded
        onActivated: {
            shortcuts.commandPalette.commands = shortcuts.window.paletteCommands()
            shortcuts.commandPalette.show()
        }
    }

    Shortcut {
        sequences: [StandardKey.Refresh, "Ctrl+R"]
        onActivated: shortcuts.window.reload()
    }

    Shortcut {
        sequence: "Ctrl+I"
        enabled: shortcuts.session.bookOpen || shortcuts.window.editingMany
        onActivated: shortcuts.window.toggleDetails()
    }

    Shortcut {
        sequence: "Ctrl+O"
        enabled: shortcuts.session.bookOpen || shortcuts.window.booksShown
        onActivated: shortcuts.window.openInReader()
    }

    Shortcut {
        sequence: "Ctrl+Shift+O"
        enabled: (shortcuts.session.bookOpen || shortcuts.window.booksShown) && shortcuts.window.library.readerAvailable
        onActivated: shortcuts.window.readBook()
    }

    Shortcut {
        sequence: "Ctrl+G"
        enabled: shortcuts.window.booksShown
        onActivated: shortcuts.window.toggleGrid()
    }

    // The highlights as kbextract2 copies them, and their Markdown shown.
    Shortcut {
        sequence: "Ctrl+Shift+C"
        enabled: shortcuts.window.highlightsShown && shortcuts.highlightsPage.bookShown
        onActivated: shortcuts.highlightsPage.copyFormatted("markdown")
    }

    Shortcut {
        sequence: "Ctrl+U"
        enabled: shortcuts.window.highlightsShown
        onActivated: shortcuts.highlightsPage.toggleMarkdown()
    }

    Shortcut {
        sequences: [StandardKey.New]
        enabled: shortcuts.window.library.writable && !shortcuts.window.library.importing
        onActivated: shortcuts.addBooksDialog.open()
    }

    Shortcut {
        sequence: "Ctrl+Shift+K"
        enabled: (shortcuts.session.bookOpen || shortcuts.window.booksShown) && shortcuts.window.library.writable
        onActivated: shortcuts.window.convertBooks(shortcuts.window.targetIds(), "KEPUB")
    }

    Shortcut {
        sequence: "Ctrl+Shift+A"
        enabled: (shortcuts.session.bookOpen || shortcuts.window.booksShown) && shortcuts.window.library.writable
        onActivated: shortcuts.window.convertBooks(shortcuts.window.targetIds(), "AZW3")
    }

    Shortcut {
        sequence: "Ctrl+D"
        enabled: (shortcuts.session.bookOpen || shortcuts.window.booksShown) && shortcuts.window.devices.devices.length > 0
            && shortcuts.window.devices.writable
        onActivated: shortcuts.window.sendBooks(shortcuts.window.targetIds(), "")
    }

    Shortcut {
        sequences: [StandardKey.Delete]
        enabled: shortcuts.window.booksShown && shortcuts.window.library.writable
        onActivated: shortcuts.window.trashBooks(shortcuts.window.targetIds())
    }

    // A field being edited undoes its own typing; otherwise the last change
    // to several books, or every change to the open book, asked first.
    Shortcut {
        sequences: [StandardKey.Undo]
        enabled: (shortcuts.window.editingMany && shortcuts.window.library.bulkUndoCount > 0)
            || (shortcuts.session.bookOpen && shortcuts.window.library.canRevert && !shortcuts.bookPage.editing)
        onActivated: {
            if (shortcuts.window.editingMany) {
                shortcuts.window.library.undoBulkEdit()
            } else {
                shortcuts.bookPage.forceActiveFocus()
                shortcuts.bookPage.confirmingRevert = true
            }
        }
    }

    Shortcut {
        sequence: "?"
        onActivated: shortcuts.shortcutsOverlay.toggle()
    }

    Shortcut {
        sequences: ["Q", StandardKey.Quit]
        onActivated: shortcuts.window.quit()
    }

    // F6 and Shift+F6: from the sidebar to the list, to the details, and
    // round.
    Shortcut {
        sequence: "F6"
        onActivated: shortcuts.window.cycleFocus(1)
    }

    Shortcut {
        sequence: "Shift+F6"
        onActivated: shortcuts.window.cycleFocus(-1)
    }
}
