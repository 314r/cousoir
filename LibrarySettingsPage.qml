pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

import Reliure

// Which library Reliure opens, whether it can be written now, Calibre's
// language for it, the backups, and the Dropbox conflict copies found beside
// its database.
Item {
    id: libraryPage

    required property BookLibrary library
    required property ConcurrencyGuard guard
    // The folder picked here; empty for the library Calibre opens.
    required property string customPath
    // The language set here; empty for Calibre's own.
    required property string calibreLanguage

    signal libraryPathSelected(string path)
    signal calibreLanguageSelected(string language)

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
                    ? qsTr("Nothing else is using the library: books can be changed.")
                    : libraryPage.guard.readOnlyReason
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Reliure changes the library only while nothing else can be writing to it: Calibre closed on this computer, Dropbox done syncing, no change from another computer in the last two minutes, and no Reliure editing on another computer. While it saves, Calibre cannot start.")
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }

            Label {
                objectName: "backupLabel"

                Layout.fillWidth: true
                text: qsTr("Before its first change in a session, Reliure copies metadata.db to %1. It keeps the last 30 copies.")
                    .arg(libraryPage.library.backupFolder)
                color: Theme.textMuted
                wrapMode: Text.WrapAnywhere
            }

            FlatButton {
                objectName: "showBackupsButton"

                variant: "outline"
                text: qsTr("Show backups")
                icon.source: "assets/icons/folder.svg"
                onClicked: libraryPage.library.openBackupFolder()
            }
        }

        ColumnLayout {
            objectName: "calibreSection"

            Layout.fillWidth: true
            spacing: Theme.gutter

            HeadingLabel {
                text: qsTr("Calibre's language")
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gutter

                FormField {
                    id: languageField
                    objectName: "calibreLanguageField"

                    Layout.preferredWidth: Math.round(16 * Theme.charWidth)
                    text: libraryPage.calibreLanguage
                    placeholderText: qsTr("Calibre's")
                    onEditingFinished: {
                        if (text.trim() !== libraryPage.calibreLanguage)
                            libraryPage.calibreLanguageSelected(text.trim())
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("In effect: %1").arg(libraryPage.library.interfaceLanguage)
                    color: Theme.textMuted
                    elide: Text.ElideRight
                }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Calibre sorts titles, names a book without author and spells names of other scripts in its interface language, “fr” or “en” for instance. Reliure reads it from Calibre on this computer; set it here if you use Calibre in another language on the computers sharing this library.")
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
