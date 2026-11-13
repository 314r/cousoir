pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// A picture of the book shown whole, zoomed and moved.
Popup {
    id: imageViewer
    objectName: "readerImageViewer"

    required property var view

    property url source
    property real zoom: 1

    function zoomBy(factor) {
        zoom = Math.max(1, Math.min(8, zoom * factor))
        if (zoom === 1) {
            picture.x = 0
            picture.y = 0
        }
    }

    parent: Overlay.overlay
    x: 0
    y: 0
    width: parent.width
    height: parent.height
    modal: true
    focus: true
    padding: 0
    onOpened: {
        zoom = 1
        picture.x = 0
        picture.y = 0
    }
    onClosed: imageViewer.view.forceActiveFocus()

    background: Rectangle {
        color: Qt.rgba(0, 0, 0, 0.85)
    }

    contentItem: Item {
        clip: true
        focus: true
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal)
                imageViewer.zoomBy(1.25)
            else if (event.key === Qt.Key_Minus)
                imageViewer.zoomBy(0.8)
            else if (event.key === Qt.Key_0)
                imageViewer.zoomBy(0)
            else
                return
            event.accepted = true
        }

        Image {
            id: picture
            objectName: "readerPicture"

            width: parent.width
            height: parent.height
            source: imageViewer.source
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            smooth: true
            mipmap: true
            scale: imageViewer.zoom

            DragHandler {
                enabled: imageViewer.zoom > 1
            }
        }

        WheelHandler {
            onWheel: function(event) { imageViewer.zoomBy(event.angleDelta.y > 0 ? 1.15 : 1 / 1.15) }
        }

        TapHandler {
            onTapped: imageViewer.close()
        }

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Theme.gutter
            text: picture.status === Image.Error ? qsTr("This picture cannot be shown.")
                : qsTr("The wheel zooms, dragging moves, Esc closes")
            color: "#d0d0d0"
            font.pointSize: Theme.fontPointSizeCaption
        }
    }
}
