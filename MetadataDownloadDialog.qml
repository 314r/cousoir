pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Controls.impl
import QtQuick.Layouts

import Cousoir

// Download metadata: what Calibre's sources find on the web for the open
// book. The books found on the left; the one picked beside the book's own
// values, each field ticked to take it, the ones that differ ticked first.
// Covers shows the covers found, the book's own first; Save takes the
// fields ticked and the cover picked as one change, which Revert undoes.
// Esc closes and stops the download.
Popup {
    id: dialog

    required property MetadataDownloader downloader
    // The book found picked, its fields and the ones ticked by key.
    property int result: 0
    property var rows: []
    property var ticked: ({})
    // The cover picked by its address, which stays as covers come in and
    // are sorted; the first one, the book's own if it has one, until then.
    property string coverUrl: ""
    readonly property int cover: {
        const covers = downloader.covers
        const index = covers.findIndex(cover => cover.url === coverUrl)
        return index >= 0 ? index : 0
    }
    // The book found whose covers were asked for.
    property int coversResult: -1
    // "fields", then "covers".
    property string page: "fields"
    property string bookTitle: ""
    readonly property bool found: downloader.state === "found"
    readonly property int tickedCount: rows.filter(row => ticked[row.key]).length

    signal failed(string message)
    signal saved(string title)

    function start(bookId, title) {
        const error = dialog.downloader.search(bookId)
        if (error) {
            dialog.failed(error)
            return
        }
        dialog.bookTitle = title
        dialog.page = "fields"
        dialog.rows = []
        dialog.ticked = {}
        dialog.coverUrl = ""
        dialog.coversResult = -1
        open()
    }

    function pick(index) {
        dialog.result = index
        dialog.rows = dialog.downloader.fields(index)
        const ticks = {}
        for (const row of dialog.rows)
            ticks[row.key] = row.chosen
        dialog.ticked = ticks
        resultList.currentIndex = index
    }

    function toggle(key) {
        const ticks = Object.assign({}, dialog.ticked)
        ticks[key] = !ticks[key]
        dialog.ticked = ticks
    }

    function showCovers() {
        dialog.page = "covers"
        if (dialog.coversResult !== dialog.result) {
            dialog.coversResult = dialog.result
            dialog.coverUrl = ""
            dialog.downloader.searchCovers(dialog.result)
        }
        coverGrid.forceActiveFocus()
    }

    function save() {
        if (!dialog.found)
            return
        const fields = dialog.rows.filter(row => dialog.ticked[row.key]).map(row => row.key)
        const error = dialog.downloader.apply(dialog.result, fields, dialog.page === "covers" ? dialog.cover : -1)
        if (error) {
            dialog.failed(error)
            return
        }
        const title = dialog.bookTitle
        close()
        dialog.saved(title)
    }

    function retry() {
        dialog.start(dialog.downloader.bookId || 0, dialog.bookTitle)
    }

    objectName: "metadataDownloadDialog"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - 2 * Theme.pagePadding : 0, Math.round(920 * Theme.spacingScale))
    height: Math.min(parent ? parent.height - 2 * Theme.pagePadding : 0, Math.round(620 * Theme.spacingScale))
    modal: true
    focus: true
    padding: Theme.panePadding
    closePolicy: Popup.CloseOnEscape
    onClosed: dialog.downloader.cancel()

    Connections {
        target: dialog.downloader

        function onStateChanged() {
            if (dialog.downloader.state === "found")
                dialog.pick(0)
        }
    }

    Overlay.modal: Rectangle {
        color: Qt.rgba(0, 0, 0, 0.35)
    }

    background: Rectangle {
        color: Theme.canvas
        border.width: 1
        border.color: Theme.controlBorder
    }

    // A box ticked or not, for a field taken or left.
    component Tick: Rectangle {
        property bool checked: false

        implicitWidth: Math.round(Theme.iconSize * 1.1)
        implicitHeight: implicitWidth
        color: checked ? Theme.accent : "transparent"
        border.width: 1
        border.color: checked ? Theme.accent : Theme.controlBorder

        IconImage {
            anchors.centerIn: parent
            width: Theme.smallIconSize
            height: Theme.smallIconSize
            sourceSize: Qt.size(Theme.smallIconSize, Theme.smallIconSize)
            visible: parent.checked
            source: "assets/icons/check.svg"
            color: Theme.canvas
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.gutter

        Keys.onReturnPressed: dialog.save()
        Keys.onEnterPressed: dialog.save()

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gutter

            IconImage {
                Layout.preferredWidth: Theme.iconSize
                Layout.preferredHeight: Theme.iconSize
                sourceSize: Qt.size(Theme.iconSize, Theme.iconSize)
                source: "assets/icons/globe.svg"
                color: Theme.textBright
            }

            Label {
                objectName: "downloadTitle"

                Layout.fillWidth: true
                text: dialog.page === "covers" ? qsTr("Covers for “%1”").arg(dialog.bookTitle)
                                               : qsTr("Metadata for “%1”").arg(dialog.bookTitle)
                color: Theme.textBright
                font.weight: Font.Bold
                elide: Text.ElideRight
            }

            Label {
                text: qsTr("from Calibre's sources")
                color: Theme.textFaint
                font.pointSize: Theme.fontPointSizeCaption
            }
        }

        Divider {
            Layout.fillWidth: true
        }

        // Searching, or what went wrong.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !dialog.found
            spacing: Theme.gutter

            Item { Layout.fillHeight: true }

            BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 3 * Theme.iconSize
                Layout.preferredHeight: 3 * Theme.iconSize
                visible: dialog.downloader.state === "searching"
                running: visible
            }

            Label {
                objectName: "downloadStatus"

                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: dialog.downloader.state === "failed" ? dialog.downloader.error
                    : qsTr("Asking Calibre's sources about the book…")
                color: dialog.downloader.state === "failed" ? Theme.danger : Theme.text
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                visible: dialog.downloader.state === "searching"
                text: qsTr("Each source has up to 30 seconds.")
                color: Theme.textFaint
                font.pointSize: Theme.fontPointSizeCaption
            }

            Item { Layout.fillHeight: true }
        }

        // The books found and the fields of the one picked.
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: dialog.found && dialog.page === "fields"
            spacing: Theme.columnGap

            ListView {
                id: resultList
                objectName: "downloadResults"

                Layout.preferredWidth: Math.round(dialog.availableWidth * 0.36)
                Layout.fillHeight: true
                clip: true
                model: dialog.found ? dialog.downloader.results : []
                boundsBehavior: Flickable.StopAtBounds
                highlightMoveDuration: 0
                keyNavigationEnabled: true
                focus: dialog.page === "fields"
                ScrollBar.vertical: ThinScrollBar {}
                onCurrentIndexChanged: {
                    if (currentIndex >= 0 && currentIndex !== dialog.result)
                        dialog.pick(currentIndex)
                }

                delegate: Rectangle {
                    id: resultRow

                    required property var modelData
                    required property int index
                    readonly property bool isCurrent: index === dialog.result

                    objectName: "downloadResult" + index
                    width: resultList.width
                    implicitHeight: resultColumn.implicitHeight + 2 * Theme.gutter
                    color: isCurrent ? Theme.selected : resultMouse.containsMouse ? Theme.hover : "transparent"

                    ColumnLayout {
                        id: resultColumn

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: Theme.gutter
                        anchors.rightMargin: 2 * Theme.gutter
                        spacing: 2

                        Label {
                            Layout.fillWidth: true
                            text: resultRow.modelData.title || qsTr("Unknown")
                            color: resultRow.isCurrent ? Theme.textBright : Theme.text
                            font.weight: resultRow.isCurrent ? Font.Bold : Font.Normal
                            elide: Text.ElideRight
                        }

                        Label {
                            Layout.fillWidth: true
                            text: [resultRow.modelData.authors, resultRow.modelData.published,
                                   resultRow.modelData.publisher].filter(part => part).join("  ·  ")
                            color: Theme.textMuted
                            font.pointSize: Theme.fontPointSizeCaption
                            elide: Text.ElideRight
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: [resultRow.modelData.hasCover ? qsTr("cover") : "",
                                   resultRow.modelData.hasSummary ? qsTr("description") : ""].filter(part => part).join(", ")
                            color: Theme.textFaint
                            font.pointSize: Theme.fontPointSizeCaption
                        }
                    }

                    MouseArea {
                        id: resultMouse

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: dialog.pick(resultRow.index)
                    }
                }
            }

            Divider {
                Layout.preferredWidth: 1
                Layout.fillHeight: true
            }

            ListView {
                id: fieldList
                objectName: "downloadFields"

                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: Theme.gutter
                model: dialog.rows
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ThinScrollBar {}

                delegate: Rectangle {
                    id: fieldRow

                    required property var modelData
                    required property int index
                    readonly property bool isTicked: dialog.ticked[modelData.key] === true
                    readonly property bool changes: modelData.current !== modelData.found

                    objectName: "downloadField_" + modelData.key
                    width: fieldList.width - 2 * Theme.gutter
                    implicitHeight: fieldLayout.implicitHeight + Theme.gutter
                    color: fieldMouse.containsMouse ? Theme.hover : "transparent"

                    RowLayout {
                        id: fieldLayout

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.topMargin: Math.round(Theme.gutter / 2)
                        anchors.leftMargin: Theme.gutter
                        spacing: Theme.gutter

                        Tick {
                            Layout.alignment: Qt.AlignTop
                            Layout.topMargin: 2
                            checked: fieldRow.isTicked
                        }

                        Label {
                            Layout.alignment: Qt.AlignTop
                            Layout.preferredWidth: Math.round(12 * Theme.charWidth)
                            text: fieldRow.modelData.label
                            color: Theme.textMuted
                            elide: Text.ElideRight
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Label {
                                objectName: "foundValue"

                                Layout.fillWidth: true
                                text: fieldRow.modelData.found
                                color: fieldRow.isTicked ? Theme.textBright : Theme.text
                                wrapMode: Text.Wrap
                                maximumLineCount: fieldRow.modelData.key === "comments" ? 6 : 3
                                elide: Text.ElideRight
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: fieldRow.changes
                                text: fieldRow.modelData.current ? qsTr("now: %1").arg(fieldRow.modelData.current)
                                                                 : qsTr("now: nothing")
                                color: Theme.textFaint
                                font.pointSize: Theme.fontPointSizeCaption
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: !fieldRow.changes
                                text: qsTr("the same")
                                color: Theme.textFaint
                                font.pointSize: Theme.fontPointSizeCaption
                            }
                        }
                    }

                    MouseArea {
                        id: fieldMouse

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: dialog.toggle(fieldRow.modelData.key)
                    }
                }
            }
        }

        // The covers, the book's own first.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: dialog.found && dialog.page === "covers"
            spacing: Theme.gutter

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gutter
                visible: dialog.downloader.searchingCovers || dialog.downloader.covers.length === 0

                BusyIndicator {
                    Layout.preferredWidth: Theme.rowHeight
                    Layout.preferredHeight: Theme.rowHeight
                    visible: dialog.downloader.searchingCovers
                    running: visible
                }

                Label {
                    Layout.fillWidth: true
                    text: dialog.downloader.searchingCovers ? qsTr("Looking for covers…")
                                                            : qsTr("No cover was found.")
                    color: Theme.textMuted
                }
            }

            GridView {
                id: coverGrid
                objectName: "downloadCovers"

                readonly property int coverWidth: Math.round(130 * Theme.spacingScale)

                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                cellWidth: coverWidth + 2 * Theme.gutter
                cellHeight: Math.round(coverWidth * 1.5) + 3 * Theme.rowHeight
                model: dialog.downloader.covers
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ThinScrollBar {}

                delegate: Item {
                    id: coverCell

                    required property var modelData
                    required property int index
                    readonly property bool isCurrent: index === dialog.cover

                    objectName: "downloadCover" + index
                    width: coverGrid.cellWidth
                    height: coverGrid.cellHeight

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 2
                        color: coverCell.isCurrent ? Theme.selected : coverMouse.containsMouse ? Theme.hover : "transparent"
                        border.width: coverCell.isCurrent ? 2 : 0
                        border.color: Theme.accent
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.gutter
                        spacing: 2

                        Image {
                            Layout.preferredWidth: coverGrid.coverWidth
                            Layout.preferredHeight: Math.round(coverGrid.coverWidth * 1.5)
                            source: coverCell.modelData.url
                            sourceSize.width: coverGrid.coverWidth * 2
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            // A file the download replaces as it goes.
                            cache: false
                        }

                        Label {
                            Layout.fillWidth: true
                            text: coverCell.modelData.source
                            color: coverCell.isCurrent ? Theme.textBright : Theme.text
                            font.pointSize: Theme.fontPointSizeCaption
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                        }

                        Label {
                            Layout.fillWidth: true
                            text: coverCell.modelData.width > 0
                                ? qsTr("%1 × %2").arg(coverCell.modelData.width).arg(coverCell.modelData.height) : ""
                            color: Theme.textFaint
                            font.pointSize: Theme.fontPointSizeLabel
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }

                    MouseArea {
                        id: coverMouse

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: dialog.coverUrl = coverCell.modelData.url
                        onDoubleClicked: {
                            dialog.coverUrl = coverCell.modelData.url
                            dialog.save()
                        }
                    }
                }
            }
        }

        Divider {
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gutter

            Label {
                Layout.fillWidth: true
                visible: dialog.found
                text: dialog.page === "fields"
                    ? qsTr("Fields to take: %1 of %2").arg(dialog.tickedCount).arg(dialog.rows.length)
                    : dialog.downloader.covers[dialog.cover] && dialog.downloader.covers[dialog.cover].own
                        ? qsTr("The cover stays as it is")
                        : qsTr("This cover replaces the book's")
                color: Theme.textMuted
                font.pointSize: Theme.fontPointSizeCaption
                elide: Text.ElideRight
            }

            Item {
                Layout.fillWidth: true
                visible: !dialog.found
            }

            FlatButton {
                objectName: "downloadCancel"

                variant: "outline"
                text: qsTr("Cancel")
                onClicked: dialog.close()
            }

            FlatButton {
                objectName: "downloadRetry"

                visible: dialog.downloader.state === "failed"
                variant: "outline"
                text: qsTr("Try again")
                onClicked: dialog.retry()
            }

            FlatButton {
                objectName: "downloadBack"

                visible: dialog.found && dialog.page === "covers"
                variant: "outline"
                text: qsTr("Back")
                icon.source: "assets/icons/arrow-left.svg"
                onClicked: dialog.page = "fields"
            }

            FlatButton {
                objectName: "downloadCoversButton"

                visible: dialog.found && dialog.page === "fields"
                variant: "outline"
                text: qsTr("Covers")
                icon.source: "assets/icons/image.svg"
                onClicked: dialog.showCovers()
            }

            FlatButton {
                objectName: "downloadSave"

                visible: dialog.found
                enabled: dialog.tickedCount > 0 || dialog.page === "covers"
                variant: "primary"
                text: qsTr("Save")
                onClicked: dialog.save()
            }
        }
    }
}
