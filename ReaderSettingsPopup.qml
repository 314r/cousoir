pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// The reader's settings: the type, its size and spacing, the line length,
// the pages one or two at a time or as a scroll, their colours.
Popup {
    id: settingsPopup
    objectName: "readerSettings"

    required property var readerWindow
    required property var readerSettings
    required property var view
    required property var bar

    // A row of choices, the one picked marked.
    component Choices: Flow {
        id: choices

        // { value, label }
        property var options: []
        property var value
        signal picked(var value)

        Layout.fillWidth: true
        spacing: Math.round(6 * Theme.spacingScale)

        Repeater {
            model: choices.options

            delegate: FlatButton {
                required property var modelData

                objectName: choices.objectName + "_" + modelData.value
                variant: "outline"
                selected: choices.value === modelData.value
                text: modelData.label
                onClicked: choices.picked(modelData.value)
            }
        }
    }

    x: settingsPopup.readerWindow.width - width - Theme.gutter
    y: settingsPopup.bar.y + settingsPopup.bar.height
    width: Math.min(settingsPopup.readerWindow.width - 2 * Theme.gutter, Math.round(44 * Theme.charWidth))
    padding: Theme.panePadding
    onOpened: {
        if (settingsPopup.readerWindow.speech)
            settingsPopup.readerWindow.speech.loadVoices()
    }
    onClosed: settingsPopup.view.forceActiveFocus()

    background: Rectangle {
        radius: Theme.radius
        color: Theme.canvas
        border.width: 1
        border.color: Theme.controlBorder
    }

    contentItem: ColumnLayout {
        spacing: Theme.gutter

        HeadingLabel {
            text: qsTr("Text size")
        }

        Row {
            spacing: Math.round(6 * Theme.spacingScale)

            FlatButton {
                variant: "outline"
                text: qsTr("A−")
                tip: qsTr("Smaller text (Ctrl+Minus)")
                enabled: settingsPopup.readerSettings.zoom > 0.5
                onClicked: settingsPopup.readerWindow.setZoom(settingsPopup.readerSettings.zoom - 0.1)
            }

            FlatButton {
                objectName: "readerZoomReset"

                variant: "outline"
                text: qsTr("%1 %").arg(Math.round(settingsPopup.readerSettings.zoom * 100))
                tip: qsTr("The book's size (Ctrl+0)")
                onClicked: settingsPopup.readerWindow.setZoom(1)
            }

            FlatButton {
                variant: "outline"
                text: qsTr("A+")
                tip: qsTr("Larger text (Ctrl+Plus)")
                enabled: settingsPopup.readerSettings.zoom < 3
                onClicked: settingsPopup.readerWindow.setZoom(settingsPopup.readerSettings.zoom + 0.1)
            }
        }

        HeadingLabel {
            Layout.topMargin: Theme.gutter
            text: qsTr("Font")
        }

        ChoiceBox {
            objectName: "readerFont"

            Layout.fillWidth: true
            model: settingsPopup.readerWindow.fontChoices
            currentIndex: Math.max(0, settingsPopup.readerWindow.fontChoices.findIndex(choice => choice.value === settingsPopup.readerSettings.fontFamily))
            onActivated: function(index) { settingsPopup.readerSettings.fontFamily = settingsPopup.readerWindow.fontChoices[index].value }
        }

        HeadingLabel {
            Layout.topMargin: Theme.gutter
            text: qsTr("Line spacing")
        }

        Choices {
            objectName: "readerLineHeight"
            options: [{ value: 0, label: qsTr("The book's") }, { value: 1.2, label: "1.2" }, { value: 1.4, label: "1.4" },
                      { value: 1.6, label: "1.6" }, { value: 1.8, label: "1.8" }]
            value: settingsPopup.readerSettings.lineHeight
            onPicked: function(value) { settingsPopup.readerSettings.lineHeight = value }
        }

        HeadingLabel {
            Layout.topMargin: Theme.gutter
            text: qsTr("Line length")
        }

        Choices {
            objectName: "readerMeasure"
            options: [{ value: "narrow", label: qsTr("Short") }, { value: "medium", label: qsTr("Medium") },
                      { value: "wide", label: qsTr("Long") }, { value: "full", label: qsTr("The window's") }]
            value: settingsPopup.readerSettings.measure
            onPicked: function(value) { settingsPopup.readerSettings.measure = value }
        }

        HeadingLabel {
            Layout.topMargin: Theme.gutter
            text: qsTr("Alignment")
        }

        Choices {
            objectName: "readerAlign"
            options: [{ value: "", label: qsTr("The book's") }, { value: "start", label: qsTr("Ragged") },
                      { value: "justify", label: qsTr("Justified") }]
            value: settingsPopup.readerSettings.align
            onPicked: function(value) { settingsPopup.readerSettings.align = value }
        }

        HeadingLabel {
            Layout.topMargin: Theme.gutter
            text: qsTr("Page")
        }

        Flow {
            Layout.fillWidth: true
            spacing: Math.round(6 * Theme.spacingScale)

            Repeater {
                model: [{ value: "app", label: qsTr("Cousoir's") }, { value: "light", label: qsTr("Light") },
                        { value: "sepia", label: qsTr("Sepia") }, { value: "dark", label: qsTr("Dark") },
                        { value: "black", label: qsTr("Black") }]

                delegate: Button {
                    id: swatch

                    required property var modelData
                    readonly property var colors: settingsPopup.readerWindow.pageThemes[modelData.value]
                        || { background: Theme.canvas.toString(), color: Theme.text.toString() }

                    objectName: "readerTheme_" + modelData.value
                    implicitWidth: Math.max(Theme.rowHeight * 2, swatchLabel.implicitWidth + 2 * Theme.controlInset)
                    implicitHeight: Theme.rowHeight * 1.4
                    hoverEnabled: true
                    focusPolicy: Qt.TabFocus
                    Accessible.name: modelData.label
                    onClicked: settingsPopup.readerSettings.pageTheme = modelData.value

                    contentItem: Label {
                        id: swatchLabel

                        text: swatch.modelData.label
                        color: swatch.colors.color
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.weight: settingsPopup.readerSettings.pageTheme === swatch.modelData.value ? Font.Bold : Font.Normal
                    }

                    background: Rectangle {
                        radius: Theme.radius
                        color: swatch.colors.background
                        border.width: settingsPopup.readerSettings.pageTheme === swatch.modelData.value || swatch.visualFocus ? 2 : 1
                        border.color: settingsPopup.readerSettings.pageTheme === swatch.modelData.value || swatch.visualFocus
                            ? Theme.accent : Theme.controlBorder
                    }
                }
            }
        }

        Choices {
            objectName: "readerLayout"
            options: [{ value: "single", label: qsTr("One page") }, { value: "spreads", label: qsTr("Two pages") },
                      { value: "scroll", label: qsTr("Scroll") }]
            value: settingsPopup.readerSettings.scroll ? "scroll" : settingsPopup.readerSettings.spreads ? "spreads" : "single"
            onPicked: function(value) {
                settingsPopup.readerSettings.scroll = value === "scroll"
                if (value !== "scroll")
                    settingsPopup.readerSettings.spreads = value === "spreads"
            }
        }

        HeadingLabel {
            Layout.topMargin: Theme.gutter
            visible: settingsPopup.readerWindow.speech !== null
            text: qsTr("Reading aloud")
        }

        ChoiceBox {
            objectName: "readerSpeechEngine"

            readonly property var choices: [{ value: "", label: qsTr("The system's voice") }].concat(
                settingsPopup.readerWindow.speech ? settingsPopup.readerWindow.speech.engines.map(engine => ({ value: engine, label: engine })) : [])

            Layout.fillWidth: true
            visible: settingsPopup.readerWindow.speech !== null && settingsPopup.readerWindow.speech.engines.length > 1
            model: choices
            currentIndex: Math.max(0, choices.findIndex(choice => choice.value === settingsPopup.readerSettings.speechEngine))
            onActivated: function(index) {
                settingsPopup.readerSettings.speechVoice = ""
                settingsPopup.readerSettings.speechEngine = choices[index].value
                settingsPopup.readerWindow.speech.loadVoices()
            }
        }

        ChoiceBox {
            objectName: "readerSpeechVoice"

            readonly property var choices: [{ value: "", label: qsTr("The engine's voice") }].concat(
                settingsPopup.readerWindow.speech ? settingsPopup.readerWindow.speech.voices.map(voice => ({ value: voice.name, label: voice.label })) : [])

            Layout.fillWidth: true
            visible: settingsPopup.readerWindow.speech !== null
            model: choices
            currentIndex: Math.max(0, choices.findIndex(choice => choice.value === settingsPopup.readerSettings.speechVoice))
            onActivated: function(index) { settingsPopup.readerSettings.speechVoice = choices[index].value }
        }

        Label {
            Layout.fillWidth: true
            visible: settingsPopup.readerSettings.spreads && !settingsPopup.readerSettings.scroll
            text: qsTr("Two pages side by side when the window is wide enough.")
            color: Theme.textMuted
            font.pointSize: Theme.fontPointSizeCaption
            wrapMode: Text.WordWrap
        }
    }
}
