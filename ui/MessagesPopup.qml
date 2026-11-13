import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// The session's messages, newest first, errors in the danger colour, to read
// one the status bar cut, or to copy: selected with the mouse or the keys,
// or all with Copy.
ThemedPopup {
    id: popup

    // [{ text, error, time }], oldest first.
    property var messages: []

    signal cleared()

    function escaped(text) {
        return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }

    readonly property string plainText: messages.slice().reverse()
        .map(message => Qt.formatTime(message.time, Qt.locale().timeFormat(Locale.ShortFormat)) + "  " + message.text)
        .join("\n")

    objectName: "messagesPopup"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - 2 * Theme.pagePadding : 0, Math.round(80 * Theme.charWidth))
    height: Math.min(parent ? parent.height - 2 * Theme.pagePadding : 0, Math.round(24 * Theme.rowHeight))
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    onOpened: log.forceActiveFocus()

    contentItem: ColumnLayout {
        spacing: Theme.gutter

        HeadingLabel {
            Layout.fillWidth: true
            text: qsTr("Messages")
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            TextArea {
                id: log
                objectName: "messagesLog"

                readOnly: true
                selectByMouse: true
                selectByKeyboard: true
                wrapMode: Text.Wrap
                textFormat: TextEdit.RichText
                padding: 0
                color: Theme.text
                selectionColor: Theme.textSelection
                selectedTextColor: Theme.textBright
                background: null
                Accessible.name: qsTr("Messages")
                text: popup.messages.length === 0
                    ? "<span style='color:" + Theme.textMuted + "'>" + popup.escaped(qsTr("No messages yet.")) + "</span>"
                    : popup.messages.slice().reverse().map(message =>
                        "<p style='margin:0 0 6px 0'><span style='color:" + Theme.textMuted + "'>"
                        + Qt.formatTime(message.time, Qt.locale().timeFormat(Locale.ShortFormat)) + "</span>&nbsp;&nbsp;"
                        + "<span style='color:" + (message.error ? Theme.danger : Theme.text) + "'>"
                        + popup.escaped(message.text) + "</span></p>").join("")
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: Theme.gutter

            FlatButton {
                objectName: "messagesClear"

                variant: "outline"
                text: qsTr("Clear")
                enabled: popup.messages.length > 0
                onClicked: popup.cleared()
            }

            FlatButton {
                objectName: "messagesCopy"

                variant: "outline"
                text: qsTr("Copy")
                enabled: popup.messages.length > 0
                onClicked: {
                    // Through the text area, which holds the clipboard as
                    // any field does.
                    copier.text = popup.plainText
                    copier.selectAll()
                    copier.copy()
                }
            }

            FlatButton {
                variant: "primary"
                text: qsTr("Close")
                onClicked: popup.close()
            }
        }

        TextEdit {
            id: copier

            visible: false
        }
    }
}
