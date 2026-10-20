pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Window

import Cousoir

// Books as covers, with ListTable's rows, current index and signals so the
// book list can switch between the two. A cell is a cover with a hairline
// edge and the title under it; the current one is tinted.
Item {
    id: grid

    property var rows: []
    // Prefix of the objectNames: <name>Grid, <name>Cell<i>.
    property string name: "cover"
    property alias currentIndex: view.currentIndex
    readonly property alias count: view.count
    readonly property int coverWidth: Math.round(120 * Theme.spacingScale)
    readonly property int coverHeight: Math.round(coverWidth * 1.5)

    signal activated(int index)
    // A right click on a cell, or Menu or Shift+F10 on the current one.
    signal menuRequested(int index, Item cell, real x, real y)

    function focusList() {
        if (view.currentIndex < 0 && view.count > 0)
            view.currentIndex = 0
        view.forceActiveFocus()
    }

    function moveCurrent(delta) {
        if (view.count === 0)
            return
        view.currentIndex = Math.max(0, Math.min(view.count - 1, view.currentIndex + delta))
        view.positionViewAtIndex(view.currentIndex, GridView.Contain)
    }

    function activate(index) {
        if (index >= 0 && index < view.count)
            grid.activated(index)
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
            }
        }

        delegate: Item {
            id: cell

            required property int index
            required property var modelData
            readonly property bool selected: GridView.isCurrentItem

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
                border.width: cell.selected && view.activeFocus ? 1 : 0
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
                    grid.menuRequested(cell.index, cell, mouse.x, mouse.y)
                }
                onClicked: function(mouse) {
                    if (mouse.button !== Qt.LeftButton)
                        return
                    view.currentIndex = cell.index
                    view.forceActiveFocus(Qt.MouseFocusReason)
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
