import QtQuick
import QtQuick.Controls.Basic

import Cousoir

ScrollBar {
    id: scrollBar

    // Over rows that light up, a handle that covers them: on the canvas it
    // looks as the see-through one does.
    property bool solid: false
    readonly property real strength: pressed ? 0.5 : hovered ? 0.4 : 0.25

    policy: ScrollBar.AsNeeded
    padding: 2

    contentItem: Rectangle {
        implicitWidth: 6
        implicitHeight: 6
        color: scrollBar.solid ? Theme.mix(Theme.canvas, Theme.text, scrollBar.strength)
                               : Theme.withAlpha(Theme.text, scrollBar.strength)
        opacity: scrollBar.size < 1.0 ? 1 : 0
    }

    background: Item { }
}
