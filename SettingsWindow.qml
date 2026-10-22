pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

ThemedWindow {
    id: settingsWindow
    objectName: "settingsWindow"

    property string currentThemeMode: "system"
    property var availableThemeModes: ["system", "light", "dark"]
    property int currentTextScalePercent: 100
    required property BookLibrary library
    required property ConcurrencyGuard guard
    property string customLibraryPath: ""
    property string calibreLanguage: ""
    required property LibraryShare share
    // Where the sharing's settings are kept: enabled, port and password.
    required property QtObject sharingSettings
    // The readers, for the wireless device server's settings.
    property DeviceLibrary devices: null

    property int currentSectionIndex: 0
    readonly property var sections: [
        { key: "appearance", title: qsTr("Appearance"), icon: "assets/icons/settings-appearance.svg" },
        { key: "library", title: qsTr("Library"), icon: "assets/icons/folder.svg" },
        { key: "network", title: qsTr("Network"), icon: "assets/icons/wifi.svg" },
        { key: "about", title: qsTr("About"), icon: "assets/icons/info.svg" }
    ]

    signal themeModeSelected(string mode)
    signal textScalePercentSelected(int percent)
    signal libraryPathSelected(string path)
    signal calibreLanguageSelected(string language)

    function showSection(key) {
        const index = sections.findIndex(section => section.key === key)
        if (index >= 0)
            currentSectionIndex = index
    }

    visible: false
    width: 760
    height: 520
    minimumWidth: 600
    minimumHeight: 400
    modality: Qt.NonModal
    title: qsTr("Settings")
    Shortcut {
        sequences: [StandardKey.Close, "Esc"]
        onActivated: settingsWindow.close()
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            objectName: "settingsSidebar"

            Layout.preferredWidth: Math.round(22 * Theme.charWidth) + 2 * Theme.gutter
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.gutter
                anchors.rightMargin: Theme.gutter
                spacing: 2

                HeadingLabel {
                    objectName: "settingsSidebarTitle"

                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.toolbarHeight
                    leftPadding: Theme.gutter
                    text: qsTr("Settings")
                    verticalAlignment: Text.AlignVCenter
                }

                Repeater {
                    id: navigationItems

                    model: settingsWindow.sections

                    delegate: SidebarItem {
                        required property int index
                        required property var modelData

                        objectName: modelData.key + "Button"
                        Layout.fillWidth: true
                        text: modelData.title
                        icon.source: modelData.icon
                        highlighted: settingsWindow.currentSectionIndex === index
                        focusPolicy: Qt.StrongFocus

                        onClicked: settingsWindow.currentSectionIndex = index
                        Keys.onDownPressed: navigationItems.itemAt((index + 1) % navigationItems.count).forceActiveFocus(Qt.TabFocusReason)
                        Keys.onUpPressed: navigationItems.itemAt((index + navigationItems.count - 1) % navigationItems.count).forceActiveFocus(Qt.TabFocusReason)
                    }
                }

                Item { Layout.fillHeight: true }
            }
        }

        Divider {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                objectName: "settingsHeader"

                Layout.fillWidth: true
                implicitHeight: Theme.toolbarHeight
                color: Theme.canvas

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.panePadding
                    anchors.rightMargin: Theme.gutter
                    spacing: Theme.gutter

                    Label {
                        objectName: "settingsSectionTitle"

                        Layout.fillWidth: true
                        text: settingsWindow.sections[settingsWindow.currentSectionIndex].title
                        font.weight: Font.Bold
                        elide: Text.ElideRight
                    }

                    FlatButton {
                        objectName: "settingsCloseButton"

                        iconOnly: true
                        text: qsTr("Close settings")
                        icon.source: "assets/icons/close.svg"
                        onClicked: settingsWindow.close()
                    }
                }

                Divider {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: settingsWindow.currentSectionIndex

                ScrollView {
                    id: appearanceScroll

                    clip: true
                    contentWidth: availableWidth

                    AppearanceSettingsPage {
                        width: appearanceScroll.availableWidth
                        height: Math.max(implicitHeight, appearanceScroll.availableHeight)
                        currentThemeMode: settingsWindow.currentThemeMode
                        availableThemeModes: settingsWindow.availableThemeModes
                        currentTextScalePercent: settingsWindow.currentTextScalePercent
                        onThemeModeSelected: function(mode) { settingsWindow.themeModeSelected(mode) }
                        onTextScalePercentSelected: function(percent) { settingsWindow.textScalePercentSelected(percent) }
                    }

                    ScrollBar.vertical: ThinScrollBar {
                        parent: appearanceScroll
                        x: appearanceScroll.width - width
                        y: appearanceScroll.topPadding
                        height: appearanceScroll.availableHeight
                    }
                }

                ScrollView {
                    id: libraryScroll

                    clip: true
                    contentWidth: availableWidth

                    LibrarySettingsPage {
                        width: libraryScroll.availableWidth
                        height: Math.max(implicitHeight, libraryScroll.availableHeight)
                        library: settingsWindow.library
                        guard: settingsWindow.guard
                        customPath: settingsWindow.customLibraryPath
                        calibreLanguage: settingsWindow.calibreLanguage
                        onLibraryPathSelected: function(path) { settingsWindow.libraryPathSelected(path) }
                        onCalibreLanguageSelected: function(language) { settingsWindow.calibreLanguageSelected(language) }
                    }

                    ScrollBar.vertical: ThinScrollBar {
                        parent: libraryScroll
                        x: libraryScroll.width - width
                        y: libraryScroll.topPadding
                        height: libraryScroll.availableHeight
                    }
                }

                ScrollView {
                    id: networkScroll

                    clip: true
                    contentWidth: availableWidth

                    NetworkSettingsPage {
                        width: networkScroll.availableWidth
                        height: Math.max(implicitHeight, networkScroll.availableHeight)
                        share: settingsWindow.share
                        settings: settingsWindow.sharingSettings
                        devices: settingsWindow.devices
                    }

                    ScrollBar.vertical: ThinScrollBar {
                        parent: networkScroll
                        x: networkScroll.width - width
                        y: networkScroll.topPadding
                        height: networkScroll.availableHeight
                    }
                }

                ScrollView {
                    id: aboutScroll

                    clip: true
                    contentWidth: availableWidth

                    AboutSettingsPage {
                        width: aboutScroll.availableWidth
                        height: Math.max(implicitHeight, aboutScroll.availableHeight)
                    }

                    ScrollBar.vertical: ThinScrollBar {
                        parent: aboutScroll
                        x: aboutScroll.width - width
                        y: aboutScroll.topPadding
                        height: aboutScroll.availableHeight
                    }
                }
            }
        }
    }
}
