import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Controls.impl
import QtQuick.Layouts

import Reliure

// A navigation row: transparent until hovered, tinted and bold when current.
// Text too long for it, such as a reader's name, is cut short, the whole of
// it in a tooltip.
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

    ToolTip.visible: hovered && label.truncated
    ToolTip.delay: 600
    ToolTip.text: text

    contentItem: RowLayout {
        spacing: control.spacing

        IconImage {
            Layout.preferredWidth: control.icon.width
            Layout.preferredHeight: control.icon.height
            sourceSize: Qt.size(control.icon.width, control.icon.height)
            source: control.icon.source
            color: control.icon.color
        }

        Label {
            id: label
            objectName: "sidebarItemLabel"

            Layout.fillWidth: true
            text: control.text
            font: control.font
            color: control.highlighted ? control.palette.highlightedText : control.palette.text
            elide: Text.ElideRight
        }
    }

    background: Rectangle {
        color: control.down ? Theme.pressed
            : control.highlighted ? Theme.selected
            : control.hovered ? Theme.hover : "transparent"
        border.width: control.visualFocus ? 1 : 0
        border.color: Theme.accent
    }
}
