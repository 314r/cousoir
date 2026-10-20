pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Controls.impl

import Cousoir

// A drop-down with the form field's look, for a list of { value, label }.
// The list opens below the field; Up and Down change the choice in place.
ComboBox {
    id: control

    readonly property int inset: Math.round(10 * Theme.spacingScale)

    textRole: "label"
    valueRole: "value"
    implicitHeight: Theme.rowHeight
    leftPadding: inset
    rightPadding: inset + indicator.width + Theme.gutter
    topPadding: 0
    bottomPadding: 0
    hoverEnabled: true

    contentItem: Label {
        text: control.displayText
        color: Theme.text
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: IconImage {
        x: control.width - width - control.inset
        y: Math.round((control.height - height) / 2)
        width: Math.round(Theme.iconSize * 0.85)
        height: width
        sourceSize: Qt.size(width, height)
        source: "assets/icons/chevron-down.svg"
        color: Theme.textMuted
    }

    background: Rectangle {
        color: control.down ? Theme.pressed
            : control.hovered ? Theme.hover : Theme.fill
        border.width: 1
        border.color: control.activeFocus ? Theme.accent : Theme.controlBorder
    }

    delegate: ItemDelegate {
        id: option

        required property var modelData
        required property int index

        width: ListView.view ? ListView.view.width : 0
        implicitHeight: Theme.rowHeight
        horizontalPadding: control.inset - 1
        verticalPadding: 0
        highlighted: control.highlightedIndex === index
        hoverEnabled: true
        text: modelData[control.textRole] ?? ""
        font.weight: control.currentIndex === index ? Font.Bold : Font.Normal

        contentItem: Label {
            text: option.text
            font: option.font
            color: option.highlighted ? Theme.textBright : Theme.text
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        background: Rectangle {
            color: option.down ? Theme.pressed
                : option.highlighted ? Theme.selected
                : option.hovered ? Theme.hover : "transparent"
        }
    }

    popup: Popup {
        y: control.height - 1
        width: control.width
        implicitHeight: contentItem.implicitHeight + topPadding + bottomPadding
        padding: 1

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
        }

        background: Rectangle {
            color: Theme.canvas
            border.width: 1
            border.color: Theme.controlBorder
        }
    }
}
