import QtQuick
import QtQuick.Controls.Basic

import Cousoir

// A popup in Cousoir's frame: the canvas within a thin border, rounded as
// the windows are, over the window dimmed. Modal and taking the keys unless
// it says otherwise.
Popup {
    modal: true
    focus: true
    padding: Theme.panePadding

    Overlay.modal: Rectangle {
        color: Qt.rgba(0, 0, 0, 0.35)
    }

    background: Rectangle {
        radius: Theme.radius
        color: Theme.canvas
        border.width: 1
        border.color: Theme.controlBorder
    }
}
