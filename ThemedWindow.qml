import QtQuick
import QtQuick.Controls.Basic

import Cousoir

// A window in the theme's colours and font, which the controls in it take
// through its palette.
ApplicationWindow {
    color: Theme.canvas
    font.family: Theme.uiFont
    font.pointSize: Theme.fontPointSizeBody

    palette.window: Theme.canvas
    palette.windowText: Theme.text
    palette.base: Theme.canvas
    palette.text: Theme.text
    palette.button: Theme.canvas
    palette.buttonText: Theme.text
    palette.highlight: Theme.textSelection
    palette.highlightedText: Theme.textBright
    palette.placeholderText: Theme.textFaint
    palette.toolTipBase: Theme.canvas
    palette.toolTipText: Theme.text
    palette.dark: Theme.controlBorder
    palette.mid: Theme.divider
    palette.link: Theme.accent
}
