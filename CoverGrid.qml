pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Window

import Cousoir

// Books as covers, with ListTable's rows, current index, selection and
// signals so the book list can switch between the two. A cell is a cover
// with a hairline edge and the title under it; the current and selected
// ones are tinted.
Item {
    id: grid

    property var rows: []
    // Prefix of the objectNames: <name>Grid, <name>Cell<i>.
    property string name: "cover"
    property alias currentIndex: view.currentIndex
    property int selectionCount: 0
    readonly property int coverWidth: Math.round(120 * Theme.spacingScale)
    readonly property int coverHeight: Math.round(coverWidth * 1.5)

    signal activated(int index)
    // A right click on a cell, or Menu or Shift+F10 on the current one.
    signal menuRequested(int index, Item cell, real x, real y)
    signal selectRequested(int index, string mode)
    signal selectAllRequested()

    function focusList() {
        if (view.currentIndex < 0 && view.count > 0)
            view.currentIndex = 0
        view.forceActiveFocus()
    }

    function activate(index) {
        if (index >= 0 && index < view.count)
            grid.activated(index)
    }

    function moveTo(index, mode) {
        if (index < 0 || index >= view.count)
            return
        view.currentIndex = index
        view.positionViewAtIndex(index, GridView.Contain)
        grid.selectRequested(index, mode)
    }

    function requestMenu() {
        if (view.currentIndex < 0)
            return
        view.positionViewAtIndex(view.currentIndex, GridView.Contain)
        const cell = view.itemAtIndex(view.currentIndex)
        if (cell)
            grid.menuRequested(view.currentIndex, cell, Theme.gutter, cell.height)
    }

    GridView {
        id: view

        objectName: grid.name + "Grid"
        anchors.fill: parent
        anchors.leftMargin: Theme.pagePadding - Theme.gutter
        topMargin: Theme.pagePadding - Theme.gutter
        bottomMargin: Theme.pagePadding
        clip: true
        model: grid.rows
        currentIndex: -1
        keyNavigationEnabled: true
        boundsBehavior: Flickable.StopAtBounds
        activeFocusOnTab: true
        // As many columns as fit, sharing out the room left over.
        readonly property int columns: Math.max(1, Math.floor((width - Theme.pagePadding + Theme.gutter)
            / (grid.coverWidth + 2 * Theme.gutter)))
        cellWidth: Math.floor((width - Theme.pagePadding + Theme.gutter) / columns)
        cellHeight: grid.coverHeight + 2 * Theme.gutter + 2 * Math.ceil(Theme.captionMetrics.height) + Theme.gutter
        Accessible.role: Accessible.List

        Keys.onReturnPressed: grid.activate(currentIndex)
        Keys.onEnterPressed: grid.activate(currentIndex)
        Keys.onSpacePressed: grid.activate(currentIndex)
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Menu
                || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                grid.requestMenu()
                event.accepted = true
                return
            }
            if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) {
                grid.selectAllRequested()
                event.accepted = true
                return
            }
            const page = columns * Math.max(1, Math.floor(height / cellHeight))
            const moves = {}
            moves[Qt.Key_Left] = currentIndex - 1
            moves[Qt.Key_Right] = currentIndex + 1
            moves[Qt.Key_Up] = currentIndex - columns
            moves[Qt.Key_Down] = currentIndex + columns
            moves[Qt.Key_PageUp] = currentIndex - page
            moves[Qt.Key_PageDown] = currentIndex + page
            moves[Qt.Key_Home] = 0
            moves[Qt.Key_End] = count - 1
            const target = moves[event.key]
            // Up and Down stay in the grid: from the first or last row they
            // do not move.
            if (target !== undefined && count > 0 && (target >= 0 && target < count
                    || (event.key !== Qt.Key_Up && event.key !== Qt.Key_Down))) {
                grid.moveTo(Math.max(0, Math.min(count - 1, target)),
                            (event.modifiers & Qt.ShiftModifier) ? "range" : "only")
                event.accepted = true
            } else if (target !== undefined) {
                event.accepted = true
            }
        }

        delegate: Item {
            id: cell

            required property int index
            required property var modelData
            readonly property bool current: GridView.isCurrentItem
            readonly property bool selected: current || modelData.selected === true

            objectName: grid.name + "Cell" + index
            width: view.cellWidth
            height: view.cellHeight
            Accessible.role: Accessible.ListItem
            Accessible.name: modelData.title
            Accessible.selected: selected

            Rectangle {
                anchors.fill: parent
                anchors.margins: Math.round(Theme.gutter / 2)
                color: cell.selected ? Theme.selected : cellMouse.containsMouse ? Theme.hover : "transparent"
                border.width: cell.current && view.activeFocus ? 1 : 0
                border.color: Theme.accent
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.gutter
                spacing: Math.round(Theme.gutter / 2)

                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: grid.coverWidth
                    Layout.preferredHeight: grid.coverHeight
                    color: Theme.fill
                    border.width: 1
                    border.color: Theme.divider

                    Image {
                        id: cover

                        anchors.fill: parent
                        anchors.margins: 1
                        visible: status === Image.Ready
                        source: cell.modelData.cover
                        sourceSize: Qt.size(Math.round(grid.coverWidth * Screen.devicePixelRatio),
                                            Math.round(grid.coverHeight * Screen.devicePixelRatio))
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        smooth: true
                    }

                    // Without a cover, its title on a plain card.
                    Label {
                        anchors.fill: parent
                        anchors.margins: Theme.gutter
                        visible: cover.status !== Image.Ready
                        text: cell.modelData.title
                        color: Theme.textMuted
                        font.pointSize: Theme.fontPointSizeCaption
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        wrapMode: Text.Wrap
                        elide: Text.ElideRight
                    }
                }

                Label {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    text: cell.modelData.title
                    color: cell.selected ? Theme.textBright : Theme.text
                    font.pointSize: Theme.fontPointSizeCaption
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignTop
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                }
            }

            MouseArea {
                id: cellMouse

                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onPressed: function(mouse) {
                    if (mouse.button !== Qt.RightButton)
                        return
                    view.currentIndex = cell.index
                    view.forceActiveFocus(Qt.MouseFocusReason)
                    if (cell.modelData.selected !== true)
                        grid.selectRequested(cell.index, "only")
                    grid.menuRequested(cell.index, cell, mouse.x, mouse.y)
                }
                onClicked: function(mouse) {
                    if (mouse.button !== Qt.LeftButton)
                        return
                    view.currentIndex = cell.index
                    view.forceActiveFocus(Qt.MouseFocusReason)
                    grid.selectRequested(cell.index, (mouse.modifiers & Qt.ControlModifier) ? "toggle"
                        : (mouse.modifiers & Qt.ShiftModifier) ? "range" : "only")
                }
                onDoubleClicked: function(mouse) {
                    if (mouse.button === Qt.LeftButton)
                        grid.activate(cell.index)
                }
            }
        }

        ScrollBar.vertical: ThinScrollBar {
            objectName: grid.name + "ScrollBar"
        }
    }
}
