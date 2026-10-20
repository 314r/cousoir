import QtQuick
import QtQuick.Controls.Basic

import Cousoir

// Omarchy buttons: plain is transparent until hovered; outline, primary and
// danger express emphasis through edge and text, never a solid fill.
Button {
    id: control

    property bool iconOnly: false
    property string variant: "plain"
    property bool selected: false
    property string tip: iconOnly ? text : ""
    readonly property color ink: variant === "primary" ? Theme.accent
        : variant === "danger" ? Theme.danger : Theme.text
    readonly property color edge: variant === "outline" ? Theme.controlBorder
        : variant === "primary" ? Theme.accent
        : variant === "danger" ? Theme.danger : "transparent"

    display: iconOnly ? AbstractButton.IconOnly : AbstractButton.TextBesideIcon
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    spacing: Theme.gutter
    horizontalPadding: iconOnly ? 0 : Math.round(10 * Theme.spacingScale)
    verticalPadding: 0
    implicitWidth: iconOnly
        ? Theme.rowHeight
        : Math.max(Theme.rowHeight, Math.ceil(implicitContentWidth) + leftPadding + rightPadding)
    implicitHeight: Theme.rowHeight
    opacity: enabled ? 1 : 0.45
    font.weight: selected ? Font.Bold : Font.Normal
    icon.width: Theme.iconSize
    icon.height: Theme.iconSize
    icon.color: ink
    icon.cache: true
    palette.buttonText: ink
    Accessible.name: text

    ToolTip.visible: tip.length > 0 && hovered
    ToolTip.delay: 600
    ToolTip.text: tip

    background: Rectangle {
        color: control.down ? Theme.pressed
            : control.selected ? Theme.selected
            : control.hovered && control.enabled ? Theme.hover
            : control.variant === "outline" ? Theme.fill : "transparent"
        border.width: 1
        border.color: control.visualFocus ? Theme.accent : control.edge
    }
}
