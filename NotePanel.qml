pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// The note Calibre 7 keeps on the author, series, tag or publisher whose
// books show, beside them: read, folded when long, and written as text.
FocusScope {
    id: panel

    required property BookLibrary library
    readonly property var filter: library.filter
    readonly property bool hasNote: (filter.note || "") !== ""
    // What Calibre's editor made that the text would lose is left to it.
    readonly property bool keptForCalibre: filter.noteHasImages === true || filter.noteLosesFormatting === true
    readonly property bool editable: library.writable && filter.notes === true && !keptForCalibre
    property bool editing: false
    // The text being saved, put back in the editor should the save fail.
    property string draft: ""
    property bool saving: false
    property bool unfolded: false
    readonly property real foldedHeight: 8 * Theme.rowHeight

    signal failed(string message)

    function edit() {
        noteArea.text = filter.noteText || ""
        editing = true
        noteArea.forceActiveFocus()
    }

    function save() {
        const text = noteArea.text.trim()
        if (text === (filter.noteText || "").trim()) {
            editing = false
            return
        }
        const error = library.setFilterNote(text)
        if (error) {
            failed(error)
            return
        }
        draft = text
        saving = true
        editing = false
    }

    // The note's images no wider than the pane.
    function fitted(html, width) {
        return html.replace(/<img([^>]*) width="(\d+)" height="(\d+)"/g, function(tag, rest, w, h) {
            if (Number(w) <= width)
                return tag
            return '<img' + rest + ' width="' + Math.floor(width) + '" height="' + Math.round(Number(h) * width / Number(w)) + '"'
        })
    }

    // There is a note to read, or one to write.
    readonly property bool wanted: filter.notes === true && (hasNote || editable)

    // Another item: what was typed for this one goes.
    readonly property string item: (filter.kind || "") + "/" + (filter.id === undefined ? "" : filter.id)
    onItemChanged: {
        editing = false
        unfolded = false
        saving = false
    }

    Connections {
        target: panel.library

        function onSaved() {
            panel.saving = false
        }

        function onSaveFailed() {
            if (!panel.saving)
                return
            panel.saving = false
            noteArea.text = panel.draft
            panel.editing = true
        }
    }

    ScrollView {
        id: noteScroll

        anchors.fill: parent
        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            id: column

            x: Theme.panePadding
            width: noteScroll.availableWidth - 2 * Theme.panePadding
            spacing: Theme.gutter

            Item { Layout.preferredHeight: Theme.pagePadding - Theme.rowPadding }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gutter

                HeadingLabel {
                    Layout.fillWidth: true
                    text: qsTr("Note")
                }

                FlatButton {
                    objectName: "editNoteButton"

                    visible: panel.editable && !panel.editing
                    text: panel.hasNote ? qsTr("Edit") : qsTr("Add a note")
                    tip: qsTr("A note on %1, kept as Calibre keeps notes").arg(panel.filter.name || "")
                    onClicked: panel.edit()
                }
            }

            Label {
                id: noteLabel
                objectName: "noteText"

                Layout.fillWidth: true
                Layout.maximumHeight: panel.unfolded ? Number.POSITIVE_INFINITY : panel.foldedHeight
                visible: panel.hasNote && !panel.editing
                clip: true
                text: panel.fitted(panel.filter.note || "", column.width)
                textFormat: Text.RichText
                wrapMode: Text.Wrap
                onLinkActivated: function(link) { Qt.openUrlExternally(link) }
            }

            FlatButton {
                visible: panel.hasNote && !panel.editing && (panel.unfolded || noteMeasure.contentHeight > panel.foldedHeight)
                text: panel.unfolded ? qsTr("Less") : qsTr("More")
                onClicked: panel.unfolded = !panel.unfolded

                // The note's whole height, to know whether it is folded.
                Text {
                    id: noteMeasure
                    visible: false
                    width: column.width
                    text: noteLabel.text
                    textFormat: Text.RichText
                    wrapMode: Text.Wrap
                    font: noteLabel.font
                }
            }

            Label {
                Layout.fillWidth: true
                visible: panel.hasNote && panel.keptForCalibre && panel.library.writable
                text: panel.filter.noteHasImages === true ? qsTr("This note has images: change it in Calibre.")
                                                          : qsTr("This note has formatting Cousoir cannot write: change it in Calibre.")
                color: Theme.textMuted
                font.pointSize: Theme.fontPointSizeCaption
                wrapMode: Text.Wrap
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: panel.editing
                spacing: Theme.gutter

                TextArea {
                    id: noteArea
                    objectName: "noteEditor"

                    Layout.fillWidth: true
                    Layout.minimumHeight: 5 * Theme.rowHeight
                    wrapMode: TextArea.Wrap
                    textFormat: TextEdit.PlainText
                    color: Theme.text
                    selectionColor: Theme.textSelection
                    selectedTextColor: Theme.textBright
                    placeholderText: qsTr("No note.")
                    placeholderTextColor: Theme.textFaint
                    padding: Theme.controlInset
                    Accessible.name: qsTr("Note")

                    background: Rectangle {
                        color: Theme.fill
                        border.width: 1
                        border.color: noteArea.activeFocus ? Theme.accent : Theme.controlBorder
                    }

                    Keys.onShortcutOverride: function(event) {
                        event.accepted = event.key === Qt.Key_Escape
                    }
                    Keys.onEscapePressed: panel.editing = false
                    Keys.onPressed: function(event) {
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && (event.modifiers & Qt.ControlModifier)) {
                            panel.save()
                            event.accepted = true
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("**bold**   *italics*   [link](https://…)   - list   # heading")
                    color: Theme.textMuted
                    font.pointSize: Theme.fontPointSizeCaption
                    wrapMode: Text.Wrap
                }

                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    spacing: Theme.gutter

                    FlatButton {
                        text: qsTr("Cancel")
                        onClicked: panel.editing = false
                    }

                    FlatButton {
                        objectName: "saveNoteButton"

                        variant: "outline"
                        text: qsTr("Save")
                        tip: qsTr("Save the note (Ctrl+Enter)")
                        onClicked: panel.save()
                    }
                }
            }
        }
    }
}
