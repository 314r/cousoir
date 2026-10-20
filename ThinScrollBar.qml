import QtQuick
import QtQuick.Controls.Basic

import Cousoir

ScrollBar {
    id: scrollBar

    policy: ScrollBar.AsNeeded
    padding: 2

    contentItem: Rectangle {
        implicitWidth: 6
        implicitHeight: 6
        color: Theme.withAlpha(Theme.text, scrollBar.pressed ? 0.5 : scrollBar.hovered ? 0.4 : 0.25)
        opacity: scrollBar.size < 1.0 ? 1 : 0
    }

    background: Item { }
}
