import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Controls.impl
import QtQuick.Layouts

import Cousoir

// The window's status line: how many books or items, the hints of the keys
// that work now, what keeps the library from being written, the jobs
// running with their stop buttons, the messages and the details toggle.
Rectangle {
    id: statusBar
    objectName: "statusBar"

    required property var window
    required property var session
    // The hints' font, which the window measures them by.
    readonly property alias hintMetrics: hintMetrics

    // An icon button of the status bar.
    component StatusButton: FlatButton {
        implicitWidth: implicitHeight
        implicitHeight: Theme.statusHeight - 6
        iconOnly: true
        icon.width: Theme.smallIconSize
        icon.height: Theme.smallIconSize
        icon.color: Theme.textMuted
    }

    readonly property real countWidth: Math.max(statusCount.implicitWidth, Theme.statusHeight)
    readonly property real stateWidth: Math.min((writeState.visible ? writeState.implicitWidth + Theme.gutter : 0)
        + (stopConversions.visible ? stopConversions.width + Theme.gutter : 0)
        + (stopSending.visible ? stopSending.width + Theme.gutter : 0)
        + (stopAdding.visible ? stopAdding.width + Theme.gutter : 0)
        + (messagesButton.visible ? messagesButton.width + Theme.gutter : 0)
        + (detailsToggle.visible ? detailsToggle.width : 0), width * 0.45)
    readonly property real sideWidth: Math.max(countWidth, stateWidth)
    // Equal sides keep the hints centered while 40 characters of them
    // still fit; a long write state then takes room from the count's side.
    readonly property bool balanced: 2 * sideWidth + 40 * Theme.captionCharWidth <= width

    implicitHeight: Theme.statusHeight
    color: Theme.surface

    Divider {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.gutter
        anchors.rightMargin: Math.round(Theme.gutter / 2)
        spacing: Theme.gutter

        Label {
            id: statusCount
            objectName: "bookCount"

            Layout.preferredWidth: statusBar.balanced ? statusBar.sideWidth : statusBar.countWidth
            text: statusBar.window.statusText()
            color: Theme.textMuted
            font.pointSize: Theme.fontPointSizeCaption
            elide: Text.ElideRight
        }

        Label {
            id: keyHintsLabel
            objectName: "keyHints"

            Layout.fillWidth: true
            text: statusBar.window.notice || statusBar.window.hintText(width)

            FontMetrics {
                id: hintMetrics
                font: keyHintsLabel.font
            }
            color: statusBar.window.notice && statusBar.window.noticeIsError ? Theme.danger : statusBar.window.notice ? Theme.text : Theme.textMuted
            font.pointSize: Theme.fontPointSizeCaption
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight

            // A message cut short is read whole among the messages.
            HoverHandler {
                id: hintsHover
            }
            TapHandler {
                enabled: statusBar.window.notice !== ""
                onTapped: statusBar.window.showMessages()
            }
            ToolTip.visible: statusBar.window.notice !== "" && hintsHover.hovered && truncated
            ToolTip.delay: 400
            ToolTip.text: statusBar.window.notice
        }

        RowLayout {
            Layout.preferredWidth: statusBar.balanced ? statusBar.sideWidth : statusBar.stateWidth
            Layout.fillHeight: true
            spacing: Theme.gutter

            Item { Layout.fillWidth: true }

            // Why the library cannot be written now; a click shows the
            // library settings, where conflict copies can be ignored.
            FlatButton {
                id: writeState
                objectName: "writeState"

                Layout.maximumWidth: statusBar.width * 0.45 - (detailsToggle.visible ? detailsToggle.width : 0)
                visible: statusBar.window.library.loaded && !statusBar.window.guard.writable
                implicitHeight: Theme.statusHeight - 6
                horizontalPadding: Math.round(6 * Theme.spacingScale)
                text: statusBar.window.guard.readOnlyReason
                tip: statusBar.window.guard.readOnlyReason
                font.pointSize: Theme.fontPointSizeCaption
                icon.source: "assets/icons/lock.svg"
                icon.width: Theme.smallIconSize
                icon.height: Theme.smallIconSize
                icon.color: Theme.textMuted
                palette.buttonText: Theme.textMuted
                onClicked: statusBar.window.openSettings("library")

                contentItem: RowLayout {
                    spacing: Math.round(6 * Theme.spacingScale)

                    IconImage {
                        Layout.preferredWidth: writeState.icon.width
                        Layout.preferredHeight: writeState.icon.height
                        sourceSize: Qt.size(writeState.icon.width, writeState.icon.height)
                        source: writeState.icon.source
                        color: writeState.icon.color
                    }

                    Label {
                        Layout.fillWidth: true
                        text: writeState.text
                        color: Theme.textMuted
                        font: writeState.font
                        elide: Text.ElideRight
                    }
                }
            }

            StatusButton {
                id: stopConversions
                objectName: "stopConversions"

                visible: statusBar.window.conversions.busy
                text: qsTr("Stop converting")
                tip: qsTr("Stop the %n conversions", "", statusBar.window.conversions.jobs.length)
                icon.source: "assets/icons/close.svg"
                onClicked: statusBar.window.conversions.cancel(0)
            }

            StatusButton {
                id: stopAdding
                objectName: "stopAdding"

                visible: statusBar.window.library.importing
                text: qsTr("Stop adding")
                tip: qsTr("Stop adding books")
                icon.source: "assets/icons/close.svg"
                onClicked: statusBar.window.library.cancelImport()
            }

            StatusButton {
                id: messagesButton
                objectName: "messagesButton"

                visible: statusBar.window.messages.length > 0
                text: qsTr("Messages")
                tip: statusBar.window.unreadErrors > 0 ? qsTr("Messages: %n errors not seen", "", statusBar.window.unreadErrors)
                                             : qsTr("Messages")
                icon.source: "assets/icons/info.svg"
                icon.color: statusBar.window.unreadErrors > 0 ? Theme.danger : Theme.textMuted
                onClicked: statusBar.window.showMessages()
            }

            StatusButton {
                id: stopSending
                objectName: "stopSending"

                visible: statusBar.window.devices.busy
                text: qsTr("Stop sending")
                tip: qsTr("Stop the %n sendings", "", statusBar.window.devices.jobs.length)
                icon.source: "assets/icons/close.svg"
                onClicked: statusBar.window.devices.cancel(0)
            }

            StatusButton {
                id: detailsToggle
                objectName: "detailsToggle"

                visible: statusBar.session.bookOpen || statusBar.window.editingMany
                selected: statusBar.window.detailsVisible
                text: statusBar.window.detailsVisible ? qsTr("Hide details") : qsTr("Show details")
                tip: qsTr("Details (Ctrl+I)")
                icon.source: "assets/icons/panel-right.svg"
                icon.color: selected ? Theme.text : Theme.textMuted
                onClicked: statusBar.window.toggleDetails()
            }
        }
    }
}
