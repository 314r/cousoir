import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Reliure

// A question in the middle of the window, answered by its two buttons, Enter
// or Esc. `accepted` follows the action's button; the other one only closes.
Popup {
    id: dialog

    property string title
    property string message
    property string acceptText: qsTr("OK")
    property string rejectText: qsTr("Cancel")
    // "danger" for an action that removes something.
    property string acceptVariant: "primary"

    signal accepted()

    function ask() {
        open()
        acceptButton.forceActiveFocus(Qt.PopupFocusReason)
    }

    function accept() {
        close()
        dialog.accepted()
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - 2 * Theme.pagePadding : 0, Math.round(440 * Theme.spacingScale))
    modal: true
    focus: true
    padding: Theme.panePadding
    closePolicy: Popup.CloseOnEscape

    Overlay.modal: Rectangle {
        color: Qt.rgba(0, 0, 0, 0.35)
    }

    background: Rectangle {
        color: Theme.canvas
        border.width: 1
        border.color: Theme.controlBorder
    }

    contentItem: ColumnLayout {
        spacing: Theme.gutter

        Keys.onReturnPressed: dialog.accept()
        Keys.onEnterPressed: dialog.accept()

        Label {
            objectName: "confirmTitle"

            Layout.fillWidth: true
            text: dialog.title
            font.weight: Font.Bold
            wrapMode: Text.Wrap
        }

        Label {
            objectName: "confirmMessage"

            Layout.fillWidth: true
            visible: text.length > 0
            text: dialog.message
            color: Theme.textMuted
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            Layout.topMargin: Theme.gutter
            spacing: Theme.gutter

            FlatButton {
                objectName: "confirmReject"

                variant: "outline"
                text: dialog.rejectText
                onClicked: dialog.close()
            }

            FlatButton {
                id: acceptButton
                objectName: "confirmAccept"

                variant: dialog.acceptVariant
                text: dialog.acceptText
                onClicked: dialog.accept()
            }
        }
    }
}
