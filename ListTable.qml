pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// A file-manager list: a header and plain rows sharing column lanes. The
// first column stretches; the others are `chars` caption characters wide,
// which a monospace font makes exact. When the first column would drop below
// `minNameChars`, lanes leave in `drop` order.
//
// A column is { key, title, chars?, right?, drop?, sort?, format? }. Its text
// is format(row), or the row's value under `key`. Columns with a `sort` key
// are clickable and show the direction of `sortOption`.
//
// Rows whose `selected` is true are shown selected besides the current one;
// clicks with Ctrl or Shift, Shift with the moving keys and Ctrl+A ask for a
// selection, which the page makes. The moving keys are the arrows, Page Up,
// Page Down, Home and End.
Item {
    id: table

    property var columns: []
    property var rows: []
    property string sortOption: ""
    property int minNameChars: 24
    // Prefix of the objectNames: <name>List, <name>Row<i>, <name><Key>Column,
    // <name>ScrollBar.
    property string name: "item"
    property alias currentIndex: list.currentIndex
    // How many rows are selected: past one, the current row is outlined.
    property int selectionCount: 0
    // Whether the rows say which are selected: then the current row is
    // filled only when it is one of them, else outlined.
    property bool selectable: false
    // The scroll bar's lane, right of the rows while it shows, so it never
    // lies over a row's highlight.
    readonly property real scrollRoom: scrollBar.size < 1.0 ? scrollBar.width : 0
    readonly property var shownColumns: {
        const room = table.width - 2 * Theme.rowPadding - table.minNameChars * Theme.charWidth
        let shown = table.columns.filter(column => column.chars)
        const used = () => shown.reduce((total, column) => total + table.columnWidth(column) + Theme.columnGap, 0)
        const order = shown.filter(column => column.drop).sort((a, b) => a.drop - b.drop)
        for (const column of order) {
            if (used() <= room)
                break
            shown = shown.filter(candidate => candidate !== column)
        }
        return shown.map(column => column.key)
    }

    signal activated(int index)
    signal sortRequested(string sort)
    // A right click on a row, or Menu or Shift+F10 on the current one: show
    // the row's actions at (x, y) in `row`.
    signal menuRequested(int index, Item row, real x, real y)
    // Delete on the current row.
    signal removeRequested(int index)
    // Select the row "only", "toggle" it, or the "range" to it.
    signal selectRequested(int index, string mode)
    signal selectAllRequested()

    function focusList() {
        if (list.currentIndex < 0 && list.count > 0)
            list.currentIndex = 0
        list.forceActiveFocus()
    }

    function columnVisible(column) {
        return !column.chars || table.shownColumns.indexOf(column.key) >= 0
    }

    function columnWidth(column) {
        return column.chars ? Math.ceil(column.chars * Theme.captionCharWidth) + 1 : 1
    }

    function cellText(row, column) {
        const text = column.format ? column.format(row) : row[column.key]
        return text === undefined || text === null || text === "" ? "—" : String(text)
    }

    function activate(index) {
        if (index >= 0 && index < list.count)
            table.activated(index)
    }

    // Makes the row current, shown, and selected by `mode`.
    function moveTo(index, mode) {
        if (index < 0 || index >= list.count)
            return
        list.currentIndex = index
        list.positionViewAtIndex(index, ListView.Contain)
        table.selectRequested(index, mode)
    }

    // Opens the current row's menu under its name.
    function requestMenu() {
        if (list.currentIndex < 0)
            return
        list.positionViewAtIndex(list.currentIndex, ListView.Contain)
        const row = list.itemAtIndex(list.currentIndex)
        if (row)
            table.menuRequested(list.currentIndex, row, Theme.rowPadding, row.height)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            objectName: table.name + "Header"

            Layout.fillWidth: true
            implicitHeight: Theme.rowHeight
            color: Theme.canvas
            visible: list.count > 0

            Divider {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.rowPadding
                anchors.rightMargin: Theme.rowPadding + table.scrollRoom
                spacing: Theme.columnGap

                Repeater {
                    model: table.columns

                    delegate: Item {
                        id: headerCell

                        required property var modelData
                        readonly property bool sortable: modelData.sort !== undefined
                        readonly property bool active: sortable && table.sortOption.startsWith(modelData.sort)
                        readonly property bool descending: table.sortOption.endsWith("Desc")

                        objectName: table.name + modelData.key.charAt(0).toUpperCase() + modelData.key.slice(1) + "Column"
                        visible: table.columnVisible(modelData)
                        Layout.fillWidth: !modelData.chars
                        Layout.preferredWidth: table.columnWidth(modelData)
                        Layout.fillHeight: true
                        Accessible.role: Accessible.ColumnHeader
                        Accessible.name: modelData.title

                        Rectangle {
                            anchors.fill: parent
                            anchors.leftMargin: -Math.round(Theme.columnGap / 2)
                            anchors.rightMargin: -Math.round(Theme.columnGap / 2)
                            color: headerMouse.pressed ? Theme.pressed : Theme.hover
                            visible: headerMouse.containsMouse
                        }

                        Label {
                            anchors.fill: parent
                            text: headerCell.active
                                ? qsTr("%1 %2").arg(headerCell.modelData.title).arg(headerCell.descending ? "↑" : "↓")
                                : headerCell.modelData.title
                            font.pointSize: Theme.fontPointSizeCaption
                            font.weight: Font.Bold
                            horizontalAlignment: headerCell.modelData.right ? Text.AlignRight : Text.AlignLeft
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }

                        MouseArea {
                            id: headerMouse

                            anchors.fill: parent
                            enabled: headerCell.sortable
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: table.sortRequested(headerCell.modelData.sort)
                        }
                    }
                }
            }
        }

        ListView {
            id: list

            objectName: table.name + "List"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: table.rows
            currentIndex: -1
            keyNavigationEnabled: true
            boundsBehavior: Flickable.StopAtBounds
            activeFocusOnTab: true
            Accessible.role: Accessible.List

            Keys.onReturnPressed: table.activate(currentIndex)
            Keys.onEnterPressed: table.activate(currentIndex)
            Keys.onSpacePressed: table.activate(currentIndex)
            Keys.onDeletePressed: {
                if (currentIndex >= 0)
                    table.removeRequested(currentIndex)
            }
            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Menu
                    || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                    table.requestMenu()
                    event.accepted = true
                    return
                }
                if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) {
                    table.selectAllRequested()
                    event.accepted = true
                    return
                }
                const page = Math.max(1, Math.floor(list.height / Theme.rowHeight) - 1)
                const moves = {}
                moves[Qt.Key_Up] = currentIndex - 1
                moves[Qt.Key_Down] = currentIndex + 1
                moves[Qt.Key_PageUp] = currentIndex - page
                moves[Qt.Key_PageDown] = currentIndex + page
                moves[Qt.Key_Home] = 0
                moves[Qt.Key_End] = count - 1
                if (moves[event.key] !== undefined && count > 0) {
                    table.moveTo(Math.max(0, Math.min(count - 1, moves[event.key])),
                                 (event.modifiers & Qt.ShiftModifier) ? "range" : "only")
                    event.accepted = true
                }
            }

            delegate: Rectangle {
                id: row

                required property int index
                required property var modelData
                readonly property bool current: ListView.isCurrentItem
                readonly property bool selected: table.selectable ? modelData.selected === true : current

                objectName: table.name + "Row" + index
                width: list.width - table.scrollRoom
                height: Theme.rowHeight
                color: selected ? Theme.selected
                    : rowMouse.containsMouse ? Theme.hover : "transparent"
                border.width: current && list.activeFocus && (table.selectionCount > 1 || !selected) ? 1 : 0
                border.color: Theme.accent
                Accessible.role: Accessible.ListItem
                Accessible.name: table.columns.length > 0 ? table.cellText(modelData, table.columns[0]) : ""
                Accessible.selected: selected

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.rowPadding
                    anchors.rightMargin: Theme.rowPadding
                    spacing: Theme.columnGap

                    Repeater {
                        model: table.columns

                        delegate: Label {
                            required property var modelData
                            required property int index

                            visible: table.columnVisible(modelData)
                            Layout.fillWidth: index === 0
                            Layout.preferredWidth: table.columnWidth(modelData)
                            text: table.cellText(row.modelData, modelData)
                            color: index > 0 ? Theme.textMuted
                                : row.selected ? Theme.textBright : Theme.text
                            font.pointSize: index === 0 ? Theme.fontPointSizeBody : Theme.fontPointSizeCaption
                            horizontalAlignment: modelData.right ? Text.AlignRight : Text.AlignLeft
                            elide: Text.ElideRight
                        }
                    }
                }

                MouseArea {
                    id: rowMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onPressed: function(mouse) {
                        if (mouse.button !== Qt.RightButton)
                            return
                        list.currentIndex = row.index
                        list.forceActiveFocus(Qt.MouseFocusReason)
                        // A row outside the selection is selected alone.
                        if (row.modelData.selected !== true)
                            table.selectRequested(row.index, "only")
                        table.menuRequested(row.index, row, mouse.x, mouse.y)
                    }
                    onClicked: function(mouse) {
                        if (mouse.button !== Qt.LeftButton)
                            return
                        list.currentIndex = row.index
                        list.forceActiveFocus(Qt.MouseFocusReason)
                        table.selectRequested(row.index, (mouse.modifiers & Qt.ControlModifier) ? "toggle"
                            : (mouse.modifiers & Qt.ShiftModifier) ? "range" : "only")
                    }
                    onDoubleClicked: function(mouse) {
                        if (mouse.button === Qt.LeftButton)
                            table.activate(row.index)
                    }
                }
            }

            ScrollBar.vertical: ThinScrollBar {
                id: scrollBar
                objectName: table.name + "ScrollBar"
            }
        }
    }
}
