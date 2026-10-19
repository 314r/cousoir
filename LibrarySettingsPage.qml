pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

import Reliure

// Which library Reliure opens, whether it could be written now, and the
// Dropbox conflict copies found beside its database.
Item {
    id: libraryPage

    required property BookLibrary library
    required property ConcurrencyGuard guard
    // The folder picked here; empty for the library Calibre opens.
    required property string customPath

    signal libraryPathSelected(string path)

    implicitHeight: contentLayout.implicitHeight + 2 * Theme.panePadding

    function localPath(url) {
        return decodeURIComponent(url.toString().replace(/^file:\/\//, ""))
    }

    ColumnLayout {
        id: contentLayout
        objectName: "libraryForm"

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.panePadding
        spacing: Theme.pagePadding

        ColumnLayout {
            objectName: "librarySection"

            Layout.fillWidth: true
            spacing: Theme.gutter

            HeadingLabel {
                text: qsTr("Library folder")
            }

            Label {
                objectName: "libraryPathLabel"

                Layout.fillWidth: true
                text: libraryPage.library.libraryPath || qsTr("None")
                font.weight: Font.Bold
                wrapMode: Text.WrapAnywhere
            }

            Flow {
                Layout.fillWidth: true
                spacing: Math.round(6 * Theme.spacingScale)

                FlatButton {
                    objectName: "chooseFolderButton"

                    variant: "outline"
                    text: qsTr("Choose folder…")
                    icon.source: "assets/icons/folder.svg"
                    onClicked: folderDialog.open()
                }

                FlatButton {
                    objectName: "calibreLibraryButton"

                    variant: "outline"
                    enabled: libraryPage.customPath.length > 0
                    text: qsTr("Use Calibre's library")
                    onClicked: libraryPage.libraryPathSelected("")
                }
            }

            Label {
                Layout.fillWidth: true
                text: libraryPage.customPath.length > 0
                    ? qsTr("Reliure opens this folder instead of the library Calibre opens on this computer.")
                    : qsTr("The library Calibre opens on this computer.")
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }
        }

        ColumnLayout {
            objectName: "writingSection"

            Layout.fillWidth: true
            spacing: Theme.gutter

            HeadingLabel {
                text: qsTr("Writing")
            }

            Label {
                objectName: "writeStateLabel"

                Layout.fillWidth: true
                text: libraryPage.guard.writable
                    ? qsTr("Nothing else is using the library right now.")
                    : libraryPage.guard.readOnlyReason
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("This version of Reliure never changes the library: it reads a copy of metadata.db. Changing books comes next, and only while nothing else can be writing: Calibre closed on this computer, Dropbox done syncing, and no change from another computer in the last two minutes.")
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }
        }

        ColumnLayout {
            objectName: "conflictsSection"

            Layout.fillWidth: true
            visible: libraryPage.guard.conflicts.length > 0
            spacing: Theme.gutter

            HeadingLabel {
                text: qsTr("Dropbox conflict copies")
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Dropbox keeps both versions when two computers change a file at once. While a conflict copy of the database is new, the library stays read-only: check in Calibre that no change is missing, then ignore the copy.")
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }

            Repeater {
                model: libraryPage.guard.conflicts

                delegate: RowLayout {
                    id: conflictRow

                    required property var modelData
                    required property int index

                    Layout.fillWidth: true
                    spacing: Theme.gutter

                    Label {
                        Layout.fillWidth: true
                        text: conflictRow.modelData.file
                        elide: Text.ElideMiddle
                    }

                    Label {
                        text: Qt.formatDate(conflictRow.modelData.modified, "yyyy-MM-dd")
                        color: Theme.textMuted
                        font.pointSize: Theme.fontPointSizeCaption
                    }

                    FlatButton {
                        objectName: "ignoreConflict" + conflictRow.index

                        visible: conflictRow.modelData.database && !conflictRow.modelData.acknowledged
                        variant: "outline"
                        text: qsTr("Ignore")
                        onClicked: libraryPage.guard.acknowledgeConflict(conflictRow.modelData.file)
                    }

                    Label {
                        visible: conflictRow.modelData.acknowledged
                        text: qsTr("Ignored")
                        color: Theme.textFaint
                        font.pointSize: Theme.fontPointSizeCaption
                    }
                }
            }
        }
    }

    FolderDialog {
        id: folderDialog

        title: qsTr("Choose a Calibre library")
        currentFolder: libraryPage.library.libraryPath ? "file://" + libraryPage.library.libraryPath : ""
        onAccepted: libraryPage.libraryPathSelected(libraryPage.localPath(selectedFolder))
    }
}
