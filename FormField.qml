import QtQuick
import QtQuick.Controls.Basic

import Cousoir

// A text field with the search field's look, without its icon.
TextField {
    id: field

    property bool invalid: false

    // Setting the text puts the cursor at its end, which scrolls a long text
    // to its end: show the start unless the field is being edited. Switching
    // windows keeps the cursor where it was.
    function showStart() {
        if (!activeFocus)
            cursorPosition = 0
    }

    implicitHeight: Theme.rowHeight
    leftPadding: Math.round(10 * Theme.spacingScale)
    rightPadding: leftPadding
    topPadding: 0
    bottomPadding: 0
    verticalAlignment: TextInput.AlignVCenter
    color: Theme.text
    placeholderTextColor: Theme.textFaint
    selectionColor: Theme.textSelection
    selectedTextColor: Theme.textBright

    Component.onCompleted: showStart()
    onTextChanged: showStart()
    onActiveFocusChanged: {
        if (focusReason !== Qt.ActiveWindowFocusReason)
            showStart()
    }

    background: Rectangle {
        color: Theme.fill
        border.width: 1
        border.color: field.invalid ? Theme.danger
            : field.activeFocus ? Theme.accent : Theme.controlBorder
    }
}
