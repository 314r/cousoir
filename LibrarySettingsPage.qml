pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// Which library Cousoir opens, whether it can be written now, whether other
// computers share it, Calibre's language for it, the backups, and the
// conflict copies sync services left beside its database.
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

    // The sync service found, by name; empty for none or a network folder.
    readonly property string serviceName: ({
        "dropbox": "Dropbox",
        "syncthing": "Syncthing",
        "nextcloud": "Nextcloud or ownCloud"
    })[libraryPage.guard.syncService] || ""

    implicitHeight: contentLayout.implicitHeight + 2 * Theme.panePadding

    // When Cousoir may write, for the library as it is shared.
    function writingRules() {
        const saving = qsTr("While it saves, Calibre cannot start.")
        if (!libraryPage.guard.shared) {
            return (libraryPage.guard.syncService === "dropbox"
                ? qsTr("Cousoir changes the library only while Calibre is closed on this computer and Dropbox, when it runs, is done syncing.")
                : qsTr("Cousoir changes the library only while Calibre is closed on this computer.")) + " " + saving
        }
        if (libraryPage.guard.syncService === "dropbox")
            return qsTr("Cousoir changes the library only while nothing else can be writing to it: Calibre closed on this computer, Dropbox done syncing, no change to the library in the last two minutes, and no Cousoir editing on another computer.") + " " + saving
        if (libraryPage.guard.syncRoot)
            return qsTr("Cousoir changes the library only while nothing else can be writing to it: Calibre closed on this computer, no change to the library in the last two minutes, and no Cousoir editing on another computer. %1 does not say when it is syncing; the two minutes leave it time.").arg(libraryPage.serviceName) + " " + saving
        return qsTr("Cousoir changes the library only while Calibre is closed on this computer and the library has not changed in the last two minutes. It cannot see Calibre or Cousoir open on another computer: close them there before changing books here.") + " " + saving
    }

    function sharingInEffect() {
        if (!libraryPage.guard.shared)
            return qsTr("In effect: only this computer")
        if (libraryPage.serviceName)
            return qsTr("In effect: shared through %1").arg(libraryPage.serviceName)
        return libraryPage.guard.syncService === "network" ? qsTr("In effect: shared on a network folder")
                                                           : qsTr("In effect: shared with other computers")
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
                    ? qsTr("Cousoir opens this folder instead of the library Calibre opens on this computer.")
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
                objectName: "writingRulesLabel"

                Layout.fillWidth: true
                text: libraryPage.writingRules()
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }

            Label {
                objectName: "backupLabel"

                Layout.fillWidth: true
                text: qsTr("Before its first change in a session, Cousoir copies metadata.db to %1. It keeps the last 30 copies.")
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
            objectName: "sharingSection"

            Layout.fillWidth: true
            spacing: Theme.gutter

            HeadingLabel {
                text: qsTr("Other computers")
            }

            Flow {
                Layout.fillWidth: true
                spacing: Math.round(6 * Theme.spacingScale)
                Accessible.role: Accessible.PageTabList
                Accessible.name: qsTr("Other computers")

                Repeater {
                    model: [
                        { value: "", key: "auto", label: qsTr("Automatic") },
                        { value: "shared", key: "shared", label: qsTr("Shared") },
                        { value: "local", key: "local", label: qsTr("Only this computer") }
                    ]

                    delegate: FlatButton {
                        required property var modelData

                        objectName: "sharingOption_" + modelData.key
                        variant: "outline"
                        selected: libraryPage.guard.sharing === modelData.value
                        text: modelData.label
                        Accessible.role: Accessible.RadioButton
                        Accessible.checked: selected
                        onClicked: libraryPage.guard.sharing = modelData.value
                    }
                }
            }

            Label {
                objectName: "sharingLabel"

                Layout.fillWidth: true
                text: libraryPage.sharingInEffect()
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Cousoir finds Dropbox, Syncthing, Nextcloud and ownCloud folders and network folders by itself. Choose Shared for a library another computer changes through anything else, OneDrive or Google Drive for instance.")
                color: Theme.textMuted
                wrapMode: Text.WordWrap
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
                text: qsTr("Calibre sorts titles, names a book without author and spells names of other scripts in its interface language, “fr” or “en” for instance. Cousoir reads it from Calibre on this computer; set it here if you use Calibre in another language on the computers sharing this library.")
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
                text: qsTr("Conflict copies")
            }

            Label {
                Layout.fillWidth: true
                text: (libraryPage.serviceName
                       ? qsTr("%1 keeps both versions when two computers change a file at once.").arg(libraryPage.serviceName)
                       : qsTr("Sync services keep both versions when two computers change a file at once."))
                    + " " + qsTr("While a conflict copy of the database is new, the library stays read-only: check in Calibre that no change is missing, then ignore the copy.")
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

    LibraryFolderDialog {
        id: folderDialog

        libraryPath: libraryPage.library.libraryPath
        onChosen: function(path) { libraryPage.libraryPathSelected(path) }
    }
}
