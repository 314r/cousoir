pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

import Cousoir

// A context menu with the look of ChoiceBox's list. Its items come from
// `actions`, a list of { key, title }; `chosen` names the one picked. Items
// are named <objectName>_<key>.
Menu {
    id: actionMenu

    property var actions: []

    signal chosen(string key)

    padding: 1

    background: Rectangle {
        implicitWidth: Math.ceil((actionMenu.actions.reduce((longest, action) => Math.max(longest, action.title.length), 0) + 4)
            * Theme.charWidth) + 2 * Theme.controlInset
        color: Theme.canvas
        border.width: 1
        border.color: Theme.controlBorder
    }

    Instantiator {
        model: actionMenu.actions

        delegate: MenuItem {
            id: item

            required property var modelData

            objectName: actionMenu.objectName + "_" + modelData.key
            implicitHeight: Theme.rowHeight
            horizontalPadding: Theme.controlInset - 1
            verticalPadding: 0
            hoverEnabled: true
            text: modelData.title
            onTriggered: actionMenu.chosen(item.modelData.key)

            contentItem: Label {
                text: item.text
                color: item.highlighted ? Theme.textBright : Theme.text
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            background: Rectangle {
                color: item.down ? Theme.pressed
                    : item.highlighted ? Theme.selected : "transparent"
            }
        }

        onObjectAdded: function(index, object) { actionMenu.insertItem(index, object) }
        onObjectRemoved: function(index, object) { actionMenu.removeItem(object) }
    }
}
