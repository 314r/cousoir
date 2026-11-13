pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

Item {
    id: appearancePage

    required property string currentThemeMode
    required property var availableThemeModes
    required property int currentTextScalePercent

    signal themeModeSelected(string mode)
    signal textScalePercentSelected(int percent)

    implicitHeight: contentLayout.implicitHeight + 2 * Theme.panePadding

    function themeModeLabel(mode) {
        return mode === "system" ? qsTr("System")
            : mode === "light" ? qsTr("Light")
            : mode === "dark" ? qsTr("Dark") : qsTr("Omarchy")
    }

    ColumnLayout {
        id: contentLayout
        objectName: "appearanceForm"

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.panePadding
        spacing: Theme.pagePadding

        ColumnLayout {
            objectName: "themeSection"

            Layout.fillWidth: true
            spacing: Theme.gutter

            HeadingLabel {
                objectName: "themeFieldLabel"
                text: qsTr("Theme")
            }

            Flow {
                objectName: "themeSelector"

                Layout.fillWidth: true
                spacing: Math.round(6 * Theme.spacingScale)
                Accessible.role: Accessible.PageTabList
                Accessible.name: qsTr("Theme")

                Repeater {
                    model: appearancePage.availableThemeModes

                    delegate: FlatButton {
                        required property string modelData

                        objectName: "themeOption_" + modelData
                        variant: "outline"
                        selected: appearancePage.currentThemeMode === modelData
                        text: appearancePage.themeModeLabel(modelData)
                        icon.source: "assets/icons/theme-" + modelData + ".svg"
                        Accessible.role: Accessible.RadioButton
                        Accessible.checked: selected
                        onClicked: appearancePage.themeModeSelected(modelData)
                    }
                }
            }

            Label {
                objectName: "themeDescription"

                Layout.fillWidth: true
                text: qsTr("Omarchy follows the current desktop theme and its changes.")
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }
        }

        ColumnLayout {
            objectName: "textSizeSection"

            Layout.fillWidth: true
            spacing: Theme.gutter

            HeadingLabel {
                objectName: "textSizeFieldLabel"
                text: qsTr("Text size")
            }

            Row {
                spacing: Math.round(6 * Theme.spacingScale)

                FlatButton {
                    objectName: "textSizeDecreaseButton"

                    variant: "outline"
                    text: qsTr("−")
                    enabled: appearancePage.currentTextScalePercent > 80
                    Accessible.name: qsTr("Decrease text size")
                    onClicked: appearancePage.textScalePercentSelected(appearancePage.currentTextScalePercent - 10)
                }

                FlatButton {
                    objectName: "textSizeResetButton"

                    variant: "outline"
                    text: qsTr("%1 %").arg(appearancePage.currentTextScalePercent)
                    tip: qsTr("Reset to the system size (Ctrl+0)")
                    Accessible.name: qsTr("Reset text size to system default")
                    Accessible.description: qsTr("Current text size is %1 percent").arg(appearancePage.currentTextScalePercent)
                    onClicked: appearancePage.textScalePercentSelected(100)
                }

                FlatButton {
                    objectName: "textSizeIncreaseButton"

                    variant: "outline"
                    text: qsTr("+")
                    enabled: appearancePage.currentTextScalePercent < 200
                    Accessible.name: qsTr("Increase text size")
                    onClicked: appearancePage.textScalePercentSelected(appearancePage.currentTextScalePercent + 10)
                }
            }

            Label {
                objectName: "textSizeDescription"

                Layout.fillWidth: true
                text: qsTr("Scales the system text size throughout Cousoir. In the main window, Ctrl+Plus, Ctrl+Minus and Ctrl+0 do the same.")
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }
        }
    }
}
