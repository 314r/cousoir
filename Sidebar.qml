pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Reliure

// Home navigation, grouped like the Places sidebar of a file manager.
Item {
    id: sidebar

    property string currentKey: "books"
    property bool focusFollows: false
    // Each item's count, by key, shown at the row's end.
    property var counts: ({})
    readonly property var groups: [
        { title: qsTr("Library"), items: [
            { key: "books", title: qsTr("Books"), icon: "book" },
            { key: "trash", title: qsTr("Trash"), icon: "trash" }
        ] },
        { title: qsTr("Browse"), items: [
            { key: "authors", title: qsTr("Authors"), icon: "user" },
            { key: "series", title: qsTr("Series"), icon: "layers" },
            { key: "tags", title: qsTr("Tags"), icon: "tag" },
            { key: "languages", title: qsTr("Languages"), icon: "globe" },
            { key: "publishers", title: qsTr("Publishers"), icon: "building" }
        ] }
    ]
    readonly property var items: groups.reduce((all, group) => all.concat(group.items), [])
    readonly property var current: section(currentKey)

    signal activated(string key)

    // Wide enough for the longest entry and a count, plus its icon and
    // paddings.
    implicitWidth: Math.ceil((items.reduce((longest, item) => Math.max(longest, item.title.length), 0) + 6)
        * Theme.charWidth) + Theme.iconSize + 5 * Theme.gutter

    function section(key) {
        return items.find(item => item.key === key) || items[0]
    }

    function step(delta) {
        const index = items.findIndex(item => item.key === currentKey)
        focusFollows = true
        sidebar.activated(items[(index + delta + items.length) % items.length].key)
        focusFollows = false
    }

    Flickable {
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: column.implicitHeight + 2 * Theme.gutter
        boundsBehavior: Flickable.StopAtBounds

        ScrollBar.vertical: ThinScrollBar { }

        ColumnLayout {
            id: column

            x: Theme.gutter
            y: Theme.gutter
            width: parent.width - 2 * Theme.gutter
            spacing: 0

            Repeater {
                model: sidebar.groups

                delegate: ColumnLayout {
                    id: group

                    required property var modelData
                    required property int index

                    Layout.fillWidth: true
                    Layout.topMargin: index > 0 ? Theme.gutter : 0
                    spacing: 0

                    HeadingLabel {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Theme.rowHeight
                        leftPadding: Theme.gutter
                        bottomPadding: Math.round(4 * Theme.spacingScale)
                        verticalAlignment: Text.AlignBottom
                        text: group.modelData.title
                    }

                    Repeater {
                        model: group.modelData.items

                        delegate: SidebarItem {
                            id: entry

                            required property var modelData

                            objectName: "sidebar_" + modelData.key
                            Layout.fillWidth: true
                            rightPadding: Theme.gutter + countLabel.implicitWidth + Theme.gutter
                            text: modelData.title
                            icon.source: "assets/icons/" + modelData.icon + ".svg"
                            highlighted: sidebar.currentKey === modelData.key
                            onClicked: sidebar.activated(modelData.key)
                            onHighlightedChanged: {
                                if (highlighted && sidebar.focusFollows)
                                    forceActiveFocus(Qt.TabFocusReason)
                            }
                            Keys.onDownPressed: sidebar.step(1)
                            Keys.onUpPressed: sidebar.step(-1)

                            Label {
                                id: countLabel

                                anchors.right: parent.right
                                anchors.rightMargin: Theme.gutter
                                anchors.verticalCenter: parent.verticalCenter
                                text: sidebar.counts[entry.modelData.key] ?? ""
                                color: Theme.textFaint
                                font.pointSize: Theme.fontPointSizeCaption
                            }
                        }
                    }
                }
            }
        }
    }
}
