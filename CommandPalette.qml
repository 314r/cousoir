pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Controls.impl
import QtQuick.Layouts

import Cousoir

// Ctrl+K: the window's sections and actions, and the library's books,
// authors, series, tags and publishers, found by a few letters. Up and Down
// choose, Enter or a click goes there or does it, Esc closes.
Popup {
    id: palette

    required property BookLibrary library
    // The window's commands: { kind: "section" or "action", key, title,
    // detail, icon }, in the order they show before anything is typed.
    property var commands: []
    property var hits: []
    property int current: 0

    // A hit chosen: { kind, id, key, title, detail }.
    signal chosen(var hit)

    function show() {
        field.text = ""
        refresh()
        open()
        field.forceActiveFocus(Qt.PopupFocusReason)
    }

    function refresh() {
        palette.hits = palette.library.paletteSearch(field.text, palette.commands, 40)
        palette.current = 0
    }

    function move(delta) {
        if (palette.hits.length > 0)
            palette.current = Math.max(0, Math.min(palette.hits.length - 1, palette.current + delta))
    }

    function choose(index) {
        const hit = palette.hits[index]
        if (!hit)
            return
        close()
        palette.chosen(hit)
    }

    function iconOf(hit) {
        if (hit.kind === "book")
            return "book"
        if (hit.kind === "authors")
            return "user"
        if (hit.kind === "series")
            return "layers"
        if (hit.kind === "tags")
            return "tag"
        if (hit.kind === "publishers")
            return "building"
        const command = palette.commands.find(command => command.key === hit.key)
        return command && command.icon ? command.icon : "search"
    }

    function kindText(hit) {
        return { book: qsTr("Book"), authors: qsTr("Author"), series: qsTr("Series"), tags: qsTr("Tag"),
                 publishers: qsTr("Publisher"), section: qsTr("Go to"), action: qsTr("Action") }[hit.kind] || ""
    }

    objectName: "commandPalette"
    parent: Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: Math.round(parent.height * 0.12)
    width: Math.min(parent ? parent.width - 2 * Theme.pagePadding : 0, Math.round(600 * Theme.spacingScale))
    modal: true
    focus: true
    padding: Theme.gutter
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

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

        FormField {
            id: field
            objectName: "paletteField"

            Layout.fillWidth: true
            placeholderText: qsTr("Books, authors, series, tags, sections, actions…")
            Accessible.name: qsTr("Find")
            onTextEdited: palette.refresh()
            Keys.onUpPressed: palette.move(-1)
            Keys.onDownPressed: palette.move(1)
            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_PageUp) {
                    palette.move(-8)
                    event.accepted = true
                } else if (event.key === Qt.Key_PageDown) {
                    palette.move(8)
                    event.accepted = true
                }
            }
            Keys.onReturnPressed: palette.choose(palette.current)
            Keys.onEnterPressed: palette.choose(palette.current)
        }

        ListView {
            id: list
            objectName: "paletteList"

            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 12 * Theme.rowHeight)
            visible: palette.hits.length > 0
            clip: true
            model: palette.hits
            currentIndex: palette.current
            boundsBehavior: Flickable.StopAtBounds
            highlightMoveDuration: 0
            ScrollBar.vertical: ThinScrollBar {}

            delegate: Rectangle {
                id: row

                required property var modelData
                required property int index
                readonly property bool isCurrent: index === palette.current

                objectName: "paletteHit" + index
                width: list.width
                height: Theme.rowHeight
                color: row.isCurrent ? Theme.selected : rowMouse.containsMouse ? Theme.hover : "transparent"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.gutter
                    // Clear of the scroll bar when there is one.
                    anchors.rightMargin: list.contentHeight > list.height ? 2 * Theme.gutter : Theme.gutter
                    spacing: Theme.gutter

                    IconImage {
                        Layout.preferredWidth: Theme.iconSize
                        Layout.preferredHeight: Theme.iconSize
                        sourceSize: Qt.size(Theme.iconSize, Theme.iconSize)
                        source: "assets/icons/" + palette.iconOf(row.modelData) + ".svg"
                        color: row.isCurrent ? Theme.textBright : Theme.text
                    }

                    Label {
                        objectName: "paletteTitle"

                        Layout.fillWidth: true
                        text: row.modelData.title
                        font.weight: row.isCurrent ? Font.Bold : Font.Normal
                        color: row.isCurrent ? Theme.textBright : Theme.text
                        elide: Text.ElideRight
                    }

                    Label {
                        Layout.maximumWidth: Math.round(list.width * 0.4)
                        visible: text.length > 0
                        text: row.modelData.detail
                        color: Theme.textMuted
                        elide: Text.ElideRight
                    }

                    Label {
                        Layout.preferredWidth: Math.round(9 * Theme.captionCharWidth)
                        text: palette.kindText(row.modelData)
                        color: Theme.textFaint
                        font.pointSize: Theme.fontPointSizeCaption
                        horizontalAlignment: Text.AlignRight
                    }
                }

                MouseArea {
                    id: rowMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: palette.choose(row.index)
                }
            }
        }

        Label {
            objectName: "paletteEmpty"

            Layout.fillWidth: true
            Layout.margins: Theme.gutter
            visible: palette.hits.length === 0
            text: qsTr("Nothing found for “%1”").arg(field.text)
            color: Theme.textMuted
            elide: Text.ElideRight
        }
    }
}
