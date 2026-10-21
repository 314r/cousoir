import QtQuick
import QtQuick.Dialogs

// Picks the folder of a Calibre library, the one holding its metadata.db.
FolderDialog {
    // The library open now, where the dialog starts; empty for none.
    property string libraryPath: ""

    // `path` is the local path of the folder picked.
    signal chosen(string path)

    title: qsTr("Choose a Calibre library")
    currentFolder: libraryPath ? "file://" + libraryPath : ""
    onAccepted: chosen(decodeURIComponent(selectedFolder.toString().replace(/^file:\/\//, "")))
}
