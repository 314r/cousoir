import QtQuick

import Cousoir

// Picks the folder of a Calibre library, the one holding its metadata.db.
FilePicker {
    // The library open now, where the chooser starts; empty for none.
    property string libraryPath: ""

    // `path` is the local path of the folder picked.
    signal chosen(string path)

    title: qsTr("Choose a Calibre library")
    mode: PortalFileChooser.OpenFolder
    folder: libraryPath ? "file://" + libraryPath : ""
    onAccepted: function(files) {
        chosen(decodeURIComponent(files[0].toString().replace(/^file:\/\//, "")))
    }
}
