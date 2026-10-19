import QtQuick
import Reliure

// One session per window: owns the library and the guard that says whether
// it may be written, and pushes the system appearance into the Theme
// singleton.
QtObject {
    id: session

    property bool adaptiveTypography: false
    // The library picked in the settings; empty for the one Calibre opens.
    property string libraryPath: ""
    // Calibre's interface language set in the settings; empty for Calibre's.
    property string calibreLanguage: ""
    readonly property BookLibrary library: BookLibrary {
        libraryPath: session.libraryPath
        calibreLanguage: session.calibreLanguage
        writable: session.guard.writable
        onSaved: {
            session.guard.noteWrite()
            session.guard.refresh()
        }
    }
    readonly property ConcurrencyGuard guard: ConcurrencyGuard {
        libraryPath: session.library.libraryPath
        libraryReadable: session.library.loaded
        schemaKnown: session.library.schemaKnown
        libraryUuid: session.library.libraryUuid
        ownChangeKey: session.library.publishedKey
    }
    readonly property bool bookOpen: library.currentBookId > 0
    readonly property bool filterOpen: library.filter.kind !== undefined

    function openBook(bookId) {
        library.currentBookId = bookId
    }

    function closeBook() {
        library.currentBookId = 0
    }

    function reload() {
        library.reload()
        guard.refresh()
    }

    function applySystemAppearance() {
        Theme.systemDark = appearance.dark
        Theme.systemPalette = appearance.palette
        Theme.systemMonoFont = appearance.fixedFont.family
        Theme.systemFontPointSize = appearance.uiFontPointSize
        Theme.adaptiveTypography = adaptiveTypography
    }

    readonly property SystemAppearance appearance: SystemAppearance {
        onPaletteChanged: session.applySystemAppearance()
        onFontsChanged: session.applySystemAppearance()
    }

    Component.onCompleted: applySystemAppearance()

    onAdaptiveTypographyChanged: Theme.adaptiveTypography = adaptiveTypography
}
