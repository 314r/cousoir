pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// Ctrl+G: somewhere by how far into the book, from 0 to 100 %.
ThemedPopup {
    id: goToBox
    objectName: "readerGoTo"

    required property var readerWindow
    required property var view

    function ask() {
        goToField.text = String(Math.round(goToBox.readerWindow.bookFraction * 100))
        open()
    }

    function go() {
        const percent = Number(goToField.text.replace(",", ".").replace("%", "").trim())
        if (isNaN(percent))
            return
        close()
        goToBox.readerWindow.goToBookFraction(Math.max(0, Math.min(100, percent)) / 100)
    }

    parent: Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: Math.round(parent.height * 0.2)
    width: Math.round(32 * Theme.charWidth)
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    // Once open: a field takes the focus only then.
    onOpened: {
        goToField.forceActiveFocus(Qt.PopupFocusReason)
        goToField.selectAll()
    }
    onClosed: goToBox.view.forceActiveFocus()

    contentItem: ColumnLayout {
        spacing: Theme.gutter

        Label {
            Layout.fillWidth: true
            text: qsTr("Go to, in % of the book")
            color: Theme.textMuted
        }

        FormField {
            id: goToField
            objectName: "readerGoToField"

            Layout.fillWidth: true
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            Accessible.name: qsTr("Go to, in % of the book")
            Keys.onReturnPressed: goToBox.go()
            Keys.onEnterPressed: goToBox.go()
        }
    }
}
