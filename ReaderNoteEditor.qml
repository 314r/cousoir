pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// A note written on a highlight, or changed.
Popup {
    id: noteEditor
    objectName: "readerNoteEditor"

    required property var readerWindow
    required property var view

    readonly property var highlight: noteEditor.readerWindow.highlightOf(noteEditor.readerWindow.noteFor)

    function save() {
        noteEditor.readerWindow.saveNote(noteText.text)
        close()
    }

    parent: Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 3)
    width: Math.min(noteEditor.readerWindow.width - 2 * Theme.gutter, Math.round(56 * Theme.charWidth))
    modal: true
    focus: true
    // A click beside it would lose the note: Esc or Cancel leave it.
    closePolicy: Popup.CloseOnEscape
    padding: Theme.panePadding
    onOpened: {
        noteText.text = highlight ? highlight.notes : ""
        noteText.forceActiveFocus()
        noteText.cursorPosition = noteText.length
    }
    onClosed: noteEditor.view.forceActiveFocus()

    background: Rectangle {
        radius: Theme.radius
        color: Theme.canvas
        border.width: 1
        border.color: Theme.controlBorder
    }

    contentItem: ColumnLayout {
        spacing: Theme.gutter

        HeadingLabel {
            text: qsTr("Note")
        }

        Label {
            Layout.fillWidth: true
            text: noteEditor.highlight ? noteEditor.highlight.text : ""
            wrapMode: Text.Wrap
            maximumLineCount: 4
            elide: Text.ElideRight
            color: Theme.textMuted
            font.italic: true
            leftPadding: Theme.gutter
            background: Rectangle {
                width: 3
                height: parent.height
                color: noteEditor.highlight ? noteEditor.readerWindow.highlightColors[noteEditor.highlight.which]
                                              || noteEditor.readerWindow.highlightColors.yellow : "transparent"
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 8 * Theme.rowHeight

            TextArea {
                id: noteText
                objectName: "readerNoteText"

                wrapMode: TextEdit.Wrap
                color: Theme.text
                placeholderText: qsTr("Your note")
                placeholderTextColor: Theme.textFaint
                selectionColor: Theme.textSelection
                selectedTextColor: Theme.textBright
                Keys.onPressed: function(event) {
                    if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                        && (event.modifiers & Qt.ControlModifier)) {
                        noteEditor.save()
                        event.accepted = true
                    }
                }

                background: Rectangle {
                    radius: Theme.radius
                    color: Theme.fill
                    border.width: 1
                    border.color: noteText.activeFocus ? Theme.accent : Theme.controlBorder
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gutter

            Item {
                Layout.fillWidth: true
            }

            FlatButton {
                variant: "outline"
                text: qsTr("Cancel")
                onClicked: noteEditor.close()
            }

            FlatButton {
                objectName: "readerSaveNote"

                variant: "primary"
                text: qsTr("Save")
                tip: qsTr("Ctrl+Enter")
                onClicked: noteEditor.save()
            }
        }
    }
}
