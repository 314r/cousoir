pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Pdf

import Cousoir

// A book's PDF read in Cousoir, a page after the other, the page kept as
// reading goes.
ThemedWindow {
    id: pdfWindow
    objectName: "pdfReaderWindow"

    required property BookLibrary library
    property alias reader: bookReader
    readonly property int page: view.currentPage
    readonly property int pages: document.pageCount

    signal failed(string message)

    function openBook(bookId) {
        const error = bookReader.openPdf(bookId)
        if (error) {
            pdfWindow.failed(error)
            return false
        }
        document.source = bookReader.fileUrl
        show()
        raise()
        requestActivate()
        startPage.restart()
        return true
    }

    visible: false
    width: 900
    height: 1000
    minimumWidth: 480
    minimumHeight: 400
    title: bookReader.title || document.title || qsTr("Reader")

    onClosing: {
        bookReader.savePage(view.currentPage, document.pageCount)
        bookReader.close()
    }

    BookReader {
        id: bookReader
        library: pdfWindow.library
    }

    PdfDocument {
        id: document

        onStatusChanged: {
            if (document.status === PdfDocument.Error)
                pdfWindow.failed(qsTr("This PDF cannot be read."))
        }
    }

    // The page read last, once the document is laid out.
    Timer {
        id: startPage
        interval: 50
        onTriggered: {
            if (document.status === PdfDocument.Error)
                return
            if (document.status !== PdfDocument.Ready) {
                restart()
                return
            }
            view.scaleToWidth(view.width, view.height)
            view.goToPage(bookReader.startPosition.page || 0)
        }
    }

    Timer {
        id: saveTimer
        interval: 1500
        onTriggered: bookReader.savePage(view.currentPage, document.pageCount)
    }

    Shortcut {
        sequences: ["Esc", StandardKey.Close]
        onActivated: pdfWindow.close()
    }

    Shortcut {
        sequences: ["Right", "PgDown", "Space"]
        onActivated: view.goToPage(Math.min(view.currentPage + 1, document.pageCount - 1))
    }

    Shortcut {
        sequences: ["Left", "PgUp", "Shift+Space"]
        onActivated: view.goToPage(Math.max(view.currentPage - 1, 0))
    }

    Shortcut {
        sequences: [StandardKey.ZoomIn, "Ctrl+="]
        onActivated: view.renderScale *= 1.15
    }

    Shortcut {
        sequences: [StandardKey.ZoomOut]
        onActivated: view.renderScale /= 1.15
    }

    header: Rectangle {
        implicitHeight: Theme.toolbarHeight
        color: Theme.canvas

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.gutter
            anchors.rightMargin: Theme.gutter
            spacing: Theme.gutter

            Label {
                Layout.fillWidth: true
                text: bookReader.title || document.title || ""
                elide: Text.ElideRight
                color: Theme.textMuted
            }

            Label {
                objectName: "pdfPage"

                text: qsTr("Page %1 of %2").arg(view.currentPage + 1).arg(document.pageCount)
                color: Theme.textMuted
            }

            FlatButton {
                text: qsTr("Width")
                tip: qsTr("Fit the page's width")
                onClicked: view.scaleToWidth(view.width, view.height)
            }

            FlatButton {
                text: qsTr("Page")
                tip: qsTr("Fit the whole page")
                onClicked: view.scaleToPage(view.width, view.height)
            }

            FlatButton {
                iconOnly: true
                text: qsTr("Close the book (Esc)")
                icon.source: "assets/icons/close.svg"
                onClicked: pdfWindow.close()
            }
        }

        Divider {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
        }
    }

    PdfMultiPageView {
        id: view
        objectName: "pdfView"

        anchors.fill: parent
        document: document
        onCurrentPageChanged: saveTimer.restart()
    }
}
