import QtQuick

import Cousoir

// A row of a list, as a delegate: as wide as the list, shaded under the
// mouse, and marked when it is the one the list's keys act on, while the
// list has them. What it shows is the delegate's.
Rectangle {
    id: listRow

    // The row the keys act on; a list may pick its own instead.
    property bool current: ListView.isCurrentItem
    // Still shaded, lighter, while the keys are elsewhere.
    property bool keepCurrent: false
    readonly property bool hovered: hover.hovered
    readonly property bool listFocused: ListView.view !== null && ListView.view.activeFocus

    width: ListView.view ? ListView.view.width : implicitWidth
    color: current && listFocused ? Theme.selected
        : current && keepCurrent || hover.hovered ? Theme.hover : "transparent"
    Accessible.role: Accessible.ListItem
    Accessible.selected: current

    HoverHandler {
        id: hover
    }
}
