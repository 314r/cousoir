import QtQuick
import QtQuick.Controls.Basic

import Reliure

// A navigation row: transparent until hovered, tinted and bold when current.
ItemDelegate {
    id: control

    implicitHeight: Theme.rowHeight
    spacing: Theme.gutter
    horizontalPadding: Theme.gutter
    verticalPadding: 0
    font.weight: highlighted ? Font.Bold : Font.Normal
    icon.width: Theme.iconSize
    icon.height: Theme.iconSize
    icon.color: highlighted ? Theme.textBright : Theme.text
    focusPolicy: Qt.TabFocus
    hoverEnabled: true
    Accessible.role: Accessible.PageTab
    Accessible.selected: highlighted

    background: Rectangle {
        color: control.down ? Theme.pressed
            : control.highlighted ? Theme.selected
            : control.hovered ? Theme.hover : "transparent"
        border.width: control.visualFocus ? 1 : 0
        border.color: Theme.accent
    }
}
