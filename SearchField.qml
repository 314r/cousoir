import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Controls.impl

import Cousoir

TextField {
    id: field

    implicitHeight: Theme.rowHeight
    leftPadding: Theme.controlInset + Theme.iconSize + Theme.gutter
    rightPadding: Theme.controlInset
    topPadding: 0
    bottomPadding: 0
    verticalAlignment: TextInput.AlignVCenter
    color: Theme.text
    placeholderTextColor: Theme.textFaint
    selectionColor: Theme.textSelection
    selectedTextColor: Theme.textBright

    background: Rectangle {
        color: Theme.fill
        border.width: 1
        border.color: field.activeFocus ? Theme.accent : Theme.controlBorder

        IconImage {
            x: Theme.controlInset
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.iconSize
            height: Theme.iconSize
            sourceSize: Qt.size(Theme.iconSize, Theme.iconSize)
            source: "assets/icons/search.svg"
            color: Theme.textMuted
        }
    }
}
