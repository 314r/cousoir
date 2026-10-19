import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Reliure

// A muted label beside its value, for detail panes.
RowLayout {
    id: metaRow

    property string label
    property string value
    property real labelChars: 11
    // A page the value opens in the browser when clicked.
    property string link

    Layout.fillWidth: true
    spacing: Theme.rowPadding

    Label {
        Layout.preferredWidth: Math.round(metaRow.labelChars * Theme.charWidth)
        Layout.alignment: Qt.AlignTop
        text: metaRow.label
        color: Theme.textMuted
        elide: Text.ElideRight
    }

    Label {
        Layout.fillWidth: true
        text: metaRow.value
        color: metaRow.link ? Theme.accent : Theme.text
        font.underline: metaRow.link.length > 0 && linkHover.hovered
        wrapMode: Text.Wrap

        HoverHandler {
            id: linkHover
            enabled: metaRow.link.length > 0
            cursorShape: Qt.PointingHandCursor
        }

        TapHandler {
            enabled: metaRow.link.length > 0
            onTapped: Qt.openUrlExternally(metaRow.link)
        }
    }
}
