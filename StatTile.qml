import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Reliure

// A headline number over its uppercase label.
ColumnLayout {
    id: tile

    property string label
    property string value

    spacing: 2

    Label {
        text: tile.value
        color: Theme.textBright
        font.pointSize: Theme.fontPointSizeDisplay
        font.weight: Font.Bold
    }

    HeadingLabel {
        text: tile.label
    }
}
