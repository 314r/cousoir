import QtQuick
import Cousoir

// One session per window: owns the library, the guard that says whether it
// may be written, the conversions, the devices and the metadata downloads,
// and pushes the system appearance into the Theme singleton.
QtObject {
    id: session

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
    readonly property ConversionService conversions: ConversionService {
        library: session.library
    }
    readonly property DeviceLibrary devices: DeviceLibrary {
        library: session.library
    }
    readonly property MetadataDownloader downloads: MetadataDownloader {
        library: session.library
    }
    // The Kobos' highlights, put into the library as each Kobo's own.
    readonly property HighlightSync highlightSync: HighlightSync {
        library: session.library
        devices: session.devices
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
        Theme.systemPalette = appearance.palette
        Theme.systemMonoFont = appearance.fixedFont.family
        Theme.systemFontPointSize = appearance.uiFontPointSize
    }

    readonly property SystemAppearance appearance: SystemAppearance {
        onPaletteChanged: session.applySystemAppearance()
        onFontsChanged: session.applySystemAppearance()
    }

    Component.onCompleted: applySystemAppearance()
}
