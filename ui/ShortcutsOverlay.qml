pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// "?": every shortcut of the window, by what it acts on. "?" again, Esc or a
// click beside it closes it.
ThemedPopup {
    id: overlay

    // [{ title, rows: [{ keys, action }] }]
    property var sections: []
    // As many columns as fit, each wide enough for its longest line.
    readonly property int columnWidth: Math.round(50 * Theme.charWidth)
    readonly property int columns: Math.max(1, Math.min(sections.length,
        Math.floor((parent ? parent.width - 2 * Theme.pagePadding - 2 * padding + Theme.pagePadding : 0)
                   / (columnWidth + Theme.pagePadding))))

    function toggle() {
        if (opened)
            close()
        else
            open()
    }

    objectName: "shortcutsOverlay"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - 2 * Theme.gutter : 0,
                    columns * columnWidth + (columns - 1) * Theme.pagePadding + 2 * padding)
    height: Math.min(parent ? parent.height - 2 * Theme.pagePadding : 0, implicitHeight)
    focus: true
    padding: Theme.pagePadding
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    contentItem: Flickable {
        id: flick

        implicitHeight: grid.implicitHeight
        contentWidth: width
        contentHeight: grid.implicitHeight
        clip: true
        focus: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        // Open, it keeps the window's shortcuts from firing: "?" closes it
        // from here.
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Question || event.text === "?") {
                overlay.close()
                event.accepted = true
            }
        }
        Keys.onUpPressed: flick.contentY = Math.max(0, flick.contentY - Theme.rowHeight)
        Keys.onDownPressed: flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height,
                                                                  flick.contentY + Theme.rowHeight))

        GridLayout {
            id: grid

            width: flick.width
            columns: overlay.columns
            columnSpacing: Theme.pagePadding
            rowSpacing: Theme.panePadding

            Repeater {
                model: overlay.sections

                delegate: ColumnLayout {
                    id: section

                    required property var modelData

                    Layout.alignment: Qt.AlignTop
                    Layout.fillWidth: true
                    Layout.preferredWidth: overlay.columnWidth
                    spacing: Math.round(Theme.gutter / 2)

                    HeadingLabel {
                        Layout.fillWidth: true
                        Layout.bottomMargin: Math.round(Theme.gutter / 2)
                        text: section.modelData.title
                    }

                    Repeater {
                        model: section.modelData.rows

                        delegate: RowLayout {
                            id: row

                            required property var modelData

                            Layout.fillWidth: true
                            spacing: Theme.gutter

                            Label {
                                Layout.preferredWidth: Math.round(22 * Theme.charWidth)
                                Layout.alignment: Qt.AlignTop
                                text: row.modelData.keys
                                color: Theme.textBright
                                font.weight: Font.Bold
                                wrapMode: Text.Wrap
                            }

                            Label {
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignTop
                                text: row.modelData.action
                                color: Theme.textMuted
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }
        }
    }
}
