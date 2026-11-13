import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// A question in the middle of the window, answered by its two buttons, Enter
// or Esc. `accepted` follows the action's button; the other one only closes.
ThemedPopup {
    id: dialog

    property string title
    property string message
    property string acceptText: qsTr("OK")
    property string rejectText: qsTr("Cancel")
    // "danger" for an action that removes something.
    property string acceptVariant: "primary"

    signal accepted()

    // What had the focus before, to give it back once answered.
    property Item focusBefore: null

    function ask() {
        focusBefore = dialog.parent && dialog.parent.Window.window ? dialog.parent.Window.window.activeFocusItem : null
        open()
        acceptButton.forceActiveFocus(Qt.PopupFocusReason)
    }

    onClosed: {
        if (focusBefore && focusBefore.visible)
            focusBefore.forceActiveFocus(Qt.PopupFocusReason)
        focusBefore = null
    }

    function accept() {
        close()
        dialog.accepted()
    }

    // Enter: the button it is on, else the action.
    function pressEnter() {
        const item = dialog.contentItem.Window.activeFocusItem
        if (item && item.checkable !== undefined && item.clicked !== undefined)
            item.clicked()
        else
            dialog.accept()
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - 2 * Theme.pagePadding : 0, Math.round(440 * Theme.spacingScale))
    closePolicy: Popup.CloseOnEscape

    contentItem: ColumnLayout {
        spacing: Theme.gutter

        Keys.onReturnPressed: dialog.pressEnter()
        Keys.onEnterPressed: dialog.pressEnter()

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
