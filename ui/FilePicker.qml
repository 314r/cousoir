import QtQuick
import QtQuick.Dialogs

import Cousoir

// Asks for files or a folder with the desktop's own file chooser, through
// its portal, as Omarchy's apps do; where there is no portal, with Qt's
// dialog.
Item {
    id: picker

    property string title
    // PortalFileChooser.OpenFile, OpenFiles, SaveFile or OpenFolder.
    property int mode: PortalFileChooser.OpenFile
    // As Qt's dialogs take them: "Books (*.epub *.kepub)".
    property var nameFilters: []
    // Where the chooser opens; the desktop's choice when empty.
    property url folder
    // For a file to save: the name proposed, and the suffix a name typed
    // without one takes.
    property string suggestedName
    property string defaultSuffix

    // The files or the folder picked, as URLs.
    signal accepted(var files)
    // The chooser could not show: why.
    signal failed(string message)

    function open() {
        if (portal.available) {
            portal.open(mode, title, nameFilters, folder, suggestedName)
        } else if (mode === PortalFileChooser.OpenFolder) {
            folderDialog.active = true
            folderDialog.item.open()
        } else {
            fileDialog.active = true
            fileDialog.item.open()
        }
    }

    // A name typed without a suffix takes the default one, as Qt's dialog
    // gives it.
    function withSuffix(file) {
        const text = file.toString()
        if (mode !== PortalFileChooser.SaveFile || !defaultSuffix || /\.[^\/.]+$/.test(text))
            return file
        return Qt.url(text + "." + defaultSuffix)
    }

    visible: false

    PortalFileChooser {
        id: portal

        onAccepted: function(files) { picker.accepted(files.map(file => picker.withSuffix(file))) }
        onFailed: function(message) { picker.failed(message) }
    }

    Loader {
        id: fileDialog

        active: false
        sourceComponent: FileDialog {
            title: picker.title
            fileMode: picker.mode === PortalFileChooser.SaveFile ? FileDialog.SaveFile
                : picker.mode === PortalFileChooser.OpenFiles ? FileDialog.OpenFiles : FileDialog.OpenFile
            nameFilters: picker.nameFilters
            defaultSuffix: picker.defaultSuffix
            onAccepted: picker.accepted(selectedFiles)

            Component.onCompleted: {
                if (picker.folder.toString())
                    currentFolder = picker.folder
            }
        }
    }

    Loader {
        id: folderDialog

        active: false
        sourceComponent: FolderDialog {
            title: picker.title
            onAccepted: picker.accepted([selectedFolder])

            Component.onCompleted: {
                if (picker.folder.toString())
                    currentFolder = picker.folder
            }
        }
    }
}
