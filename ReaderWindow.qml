pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtWebEngine

import Cousoir

// A book read in Cousoir: its pages a screen at a time, its table of
// contents beside them, where reading stopped kept as it goes.
ThemedWindow {
    id: readerWindow
    objectName: "readerWindow"

    required property BookLibrary library
    property alias reader: bookReader
    // The spine item shown, and what to do once it is loaded: "start",
    // "end", "cfi:<CFI>" or "fragment:<id>".
    property int spineIndex: -1
    property string pending: "start"
    property string position: ""
    property double fileFraction: 0
    property int page: 0
    property int pages: 1
    property real zoom: 1
    property bool tocVisible: false
    readonly property double bookFraction: bookReader.bookFraction(spineIndex, fileFraction)

    signal failed(string message)

    // Reads the book from where it was left.
    function openBook(bookId) {
        const error = bookReader.open(bookId)
        if (error) {
            readerWindow.failed(error)
            return false
        }
        const start = bookReader.startPosition
        show()
        raise()
        requestActivate()
        load(start.spineIndex || 0, start.cfi ? "cfi:" + start.cfi : "start")
        return true
    }

    function load(index, action) {
        if (index < 0 || index >= bookReader.spine.length)
            return
        pending = action
        if (index === spineIndex && !view.loading) {
            act()
            return
        }
        spineIndex = index
        view.opacity = 0
        view.url = bookReader.spine[index].url
    }

    function act() {
        const colors = { background: Theme.canvas.toString(), color: Theme.text.toString(), link: Theme.accent.toString() }
        view.runJavaScript(bookReader.script)
        view.runJavaScript("window.cousoir.setup(" + JSON.stringify(colors) + ")")
        if (pending.startsWith("cfi:"))
            view.runJavaScript("window.cousoir.goToCfi(" + JSON.stringify(pending.slice(4)) + ")")
        else if (pending.startsWith("fragment:"))
            view.runJavaScript("window.cousoir.goToFragment(" + JSON.stringify(pending.slice(9)) + ")")
        else if (pending === "end")
            view.runJavaScript("window.cousoir.goToEnd()")
        else
            view.runJavaScript("window.cousoir.goToStart()")
        view.opacity = 1
        view.forceActiveFocus()
    }

    function nextPage() {
        view.runJavaScript("window.cousoir.next()")
    }

    function previousPage() {
        view.runJavaScript("window.cousoir.previous()")
    }

    function save() {
        if (position)
            bookReader.savePosition(spineIndex, position, fileFraction)
    }

    function setZoom(value) {
        zoom = Math.max(0.5, Math.min(3, value))
    }

    // The title of the chapter shown: the last table of contents entry at or
    // before the spine item.
    function chapterTitle() {
        let title = ""
        for (const entry of bookReader.toc) {
            if (entry.spineIndex >= 0 && entry.spineIndex <= spineIndex)
                title = entry.title
        }
        return title
    }

    visible: false
    width: 900
    height: 1000
    minimumWidth: 480
    minimumHeight: 400
    title: bookReader.title || qsTr("Reader")

    onClosing: {
        save()
        bookReader.close()
        spineIndex = -1
        position = ""
    }

    BookReader {
        id: bookReader
        library: readerWindow.library
    }

    // Saved as reading goes, a little after each turn.
    Timer {
        id: saveTimer
        interval: 1500
        onTriggered: readerWindow.save()
    }

    Shortcut {
        sequences: ["Esc", StandardKey.Close]
        onActivated: readerWindow.close()
    }

    header: Rectangle {
        implicitHeight: Theme.toolbarHeight
        color: Theme.canvas

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.gutter
            anchors.rightMargin: Theme.gutter
            spacing: Theme.gutter

            FlatButton {
                objectName: "readerTocButton"

                iconOnly: true
                text: qsTr("Table of contents (T)")
                icon.source: "assets/icons/list.svg"
                selected: readerWindow.tocVisible
                onClicked: readerWindow.tocVisible = !readerWindow.tocVisible
            }

            Label {
                Layout.fillWidth: true
                text: [bookReader.title, readerWindow.chapterTitle()].filter(part => part).join("  ·  ")
                elide: Text.ElideRight
                color: Theme.textMuted
            }

            Label {
                objectName: "readerProgress"

                text: qsTr("%1 %").arg(Math.round(readerWindow.bookFraction * 100))
                color: Theme.textMuted
            }

            FlatButton {
                text: qsTr("A−")
                tip: qsTr("Smaller text (Ctrl+Minus)")
                onClicked: readerWindow.setZoom(readerWindow.zoom - 0.1)
            }

            FlatButton {
                text: qsTr("A+")
                tip: qsTr("Larger text (Ctrl+Plus)")
                onClicked: readerWindow.setZoom(readerWindow.zoom + 0.1)
            }

            FlatButton {
                iconOnly: true
                text: qsTr("Close the book (Esc)")
                icon.source: "assets/icons/close.svg"
                onClicked: readerWindow.close()
            }
        }

        Divider {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        ListView {
            id: tocList
            objectName: "readerToc"

            Layout.preferredWidth: Math.round(30 * Theme.charWidth)
            Layout.fillHeight: true
            visible: readerWindow.tocVisible
            clip: true
            model: bookReader.toc

            delegate: ItemDelegate {
                id: entry

                required property var modelData

                width: ListView.view.width
                leftPadding: Theme.gutter + entry.modelData.depth * Theme.gutter * 2
                text: entry.modelData.title
                highlighted: entry.modelData.spineIndex === readerWindow.spineIndex
                enabled: entry.modelData.spineIndex >= 0
                onClicked: readerWindow.load(entry.modelData.spineIndex,
                                             entry.modelData.fragment ? "fragment:" + entry.modelData.fragment : "start")

                contentItem: Label {
                    text: entry.text
                    elide: Text.ElideRight
                    color: entry.highlighted ? Theme.textBright : Theme.text
                    font.weight: entry.highlighted ? Font.Bold : Font.Normal
                }
                background: Rectangle {
                    color: entry.hovered ? Theme.hover : "transparent"
                }
            }

            ScrollBar.vertical: ThinScrollBar {}
        }

        Divider {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            visible: tocList.visible
        }

        WebEngineView {
            id: view
            objectName: "readerView"

            Layout.fillWidth: true
            Layout.fillHeight: true
            backgroundColor: Theme.canvas
            zoomFactor: readerWindow.zoom
            settings.javascriptCanOpenWindows: false
            settings.localContentCanAccessRemoteUrls: false

            onLoadingChanged: function(request) {
                if (request.status === WebEngineView.LoadSucceededStatus)
                    readerWindow.act()
                else if (request.status === WebEngineView.LoadFailedStatus)
                    readerWindow.failed(qsTr("This page of the book cannot be shown: %1").arg(request.errorString))
            }

            // Links within the book go to their page; others to the desktop's
            // browser.
            onNavigationRequested: function(request) {
                if (request.navigationType !== WebEngineNavigationRequest.LinkClickedNavigation)
                    return
                request.reject()
                const index = bookReader.spineIndexOf(request.url)
                if (index >= 0) {
                    const url = request.url.toString()
                    const hash = url.indexOf("#")
                    readerWindow.load(index, hash >= 0 ? "fragment:" + decodeURIComponent(url.slice(hash + 1)) : "start")
                } else if (/^(https?|mailto):/.test(request.url.toString())) {
                    Qt.openUrlExternally(request.url)
                }
            }

            onJavaScriptConsoleMessage: function(level, message) {
                if (!message.startsWith("cousoir:"))
                    return
                let event
                try {
                    event = JSON.parse(message.slice(8))
                } catch (e) {
                    return
                }
                if (event.event === "position") {
                    readerWindow.position = "epubcfi(/" + 2 * (readerWindow.spineIndex + 1) + event.cfi + ")"
                    readerWindow.fileFraction = event.fraction
                    readerWindow.page = event.page
                    readerWindow.pages = event.pages
                    saveTimer.restart()
                } else if (event.event === "next") {
                    readerWindow.load(readerWindow.spineIndex + 1, "start")
                } else if (event.event === "previous") {
                    readerWindow.load(readerWindow.spineIndex - 1, "end")
                } else if (event.event === "key") {
                    if (event.key === "Escape")
                        readerWindow.close()
                    else if (!event.ctrl && (event.key === "t" || event.key === "T"))
                        readerWindow.tocVisible = !readerWindow.tocVisible
                    else if (event.ctrl && (event.key === "+" || event.key === "="))
                        readerWindow.setZoom(readerWindow.zoom + 0.1)
                    else if (event.ctrl && event.key === "-")
                        readerWindow.setZoom(readerWindow.zoom - 0.1)
                    else if (event.ctrl && event.key === "0")
                        readerWindow.setZoom(1)
                    else if (event.ctrl && (event.key === "w" || event.key === "W"))
                        readerWindow.close()
                }
            }
        }
    }
}
