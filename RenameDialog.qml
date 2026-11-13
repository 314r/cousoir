import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// A name asked for, in the middle of the window: Enter gives it, Esc
// leaves it as it was. `accepted` comes with the name typed.
Popup {
    id: dialog

    property string title
    property string message
    property string name
    property string acceptText: qsTr("Rename")
    property Item focusBefore: null

    signal accepted(string name)

    function ask(current) {
        name = current
        focusBefore = parent && parent.Window.window ? parent.Window.window.activeFocusItem : null
        open()
    }

    function accept() {
        const typed = field.text
        close()
        dialog.accepted(typed)
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - 2 * Theme.pagePadding : 0, Math.round(440 * Theme.spacingScale))
    modal: true
    focus: true
    padding: Theme.panePadding
    closePolicy: Popup.CloseOnEscape

    onOpened: {
        field.text = dialog.name
        field.forceActiveFocus(Qt.PopupFocusReason)
        field.selectAll()
    }
    onClosed: {
        if (focusBefore && focusBefore.visible)
            focusBefore.forceActiveFocus(Qt.PopupFocusReason)
        focusBefore = null
    }

    Overlay.modal: Rectangle {
        color: Qt.rgba(0, 0, 0, 0.35)
    }

    background: Rectangle {
        radius: Theme.radius
        color: Theme.canvas
        border.width: 1
        border.color: Theme.controlBorder
    }

    contentItem: ColumnLayout {
        spacing: Theme.gutter

        Label {
            Layout.fillWidth: true
            text: dialog.title
            font.weight: Font.Bold
            wrapMode: Text.Wrap
        }

        Label {
            Layout.fillWidth: true
            visible: text.length > 0
            text: dialog.message
            color: Theme.textMuted
            wrapMode: Text.Wrap
        }

        FormField {
            id: field
            objectName: "renameField"

            Layout.fillWidth: true
            Accessible.name: dialog.title
            Keys.onReturnPressed: dialog.accept()
            Keys.onEnterPressed: dialog.accept()
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            Layout.topMargin: Theme.gutter
            spacing: Theme.gutter

            FlatButton {
                variant: "outline"
                text: qsTr("Cancel")
                onClicked: dialog.close()
            }

            FlatButton {
                objectName: "renameAccept"

                variant: "primary"
                text: dialog.acceptText
                onClicked: dialog.accept()
            }
        }
    }
}
