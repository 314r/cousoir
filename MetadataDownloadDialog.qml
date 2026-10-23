pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Controls.impl
import QtQuick.Layouts

import Cousoir

// Download metadata: what Calibre's sources, or Open Library, find on the
// web for the open book. The books found on the left; the one picked beside
// the book's own values, each field ticked to take it, the ones that differ
// ticked first. Covers shows the covers found, the book's own first and kept
// unless another is picked; Save takes the fields ticked and the cover
// picked as one change, which Revert undoes. Space ticks a field, the arrows
// pick a cover, Enter saves or presses the button it is on; Esc closes and
// stops the download.
Popup {
    id: dialog

    required property MetadataDownloader downloader
    // The book found picked, its fields and the ones ticked by key.
    property int result: 0
    property var rows: []
    property var ticked: ({})
    // The cover picked by its address, which stays as covers come in and
    // are sorted; empty for the book's own.
    property string coverUrl: ""
    // The book found whose covers were asked for.
    property int coversResult: -1
    readonly property bool coverPicked: coversResult === result && coverUrl !== ""
    // The covers, kept as they come so that the grid only adds them.
    property bool syncingCovers: false
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

    // The covers model follows the downloader's list: new covers added at
    // its end, the whole of it again when it changes otherwise.
    function syncCovers() {
        const covers = dialog.downloader.covers
        dialog.syncingCovers = true
        let same = covers.length >= coverModel.count
        for (let i = 0; same && i < coverModel.count; ++i)
            same = coverModel.get(i).url === covers[i].url
        if (!same)
            coverModel.clear()
        for (let i = coverModel.count; i < covers.length; ++i) {
            const cover = covers[i]
            coverModel.append({ name: cover.source, pixelWidth: cover.width, pixelHeight: cover.height,
                                url: cover.url, own: cover.own })
        }
        coverGrid.currentIndex = dialog.coverIndex()
        dialog.syncingCovers = false
    }

    // The row of the cover picked in the model, the book's own's for none.
    function coverIndex() {
        for (let i = 0; i < coverModel.count; ++i) {
            if (dialog.coverUrl !== "" ? coverModel.get(i).url === dialog.coverUrl : coverModel.get(i).own)
                return i
        }
        return coverModel.count > 0 ? 0 : -1
    }

    function pickCover(index) {
        if (index < 0 || index >= coverModel.count)
            return
        const cover = coverModel.get(index)
        dialog.coverUrl = cover.own ? "" : cover.url
        coverGrid.currentIndex = index
    }

    // Enter: the button it is on, else Save.
    function pressEnter() {
        const item = dialog.contentItem.Window.activeFocusItem
        if (item && item.checkable !== undefined && item.clicked !== undefined)
            item.clicked()
        else
            dialog.save()
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
        const error = dialog.downloader.apply(dialog.result, fields, dialog.coverPicked ? dialog.coverUrl : "")
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
            // Over elsewhere, when another library opens.
            else if (dialog.downloader.state === "idle" && dialog.opened)
                dialog.close()
        }

        function onCoversChanged() {
            dialog.syncCovers()
        }
    }

    ListModel {
        id: coverModel
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

        Keys.onReturnPressed: dialog.pressEnter()
        Keys.onEnterPressed: dialog.pressEnter()

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
                text: dialog.downloader.source === "openlibrary" ? qsTr("from Open Library")
                                                                  : qsTr("from Calibre's sources")
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
                    : dialog.downloader.source === "openlibrary" ? qsTr("Asking Open Library about the book…")
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
                activeFocusOnTab: true
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
                keyNavigationEnabled: true
                activeFocusOnTab: true
                highlightMoveDuration: 0
                ScrollBar.vertical: ThinScrollBar {}
                onActiveFocusChanged: {
                    if (activeFocus && currentIndex < 0 && count > 0)
                        currentIndex = 0
                }
                Keys.onSpacePressed: {
                    if (currentIndex >= 0)
                        dialog.toggle(dialog.rows[currentIndex].key)
                }

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
                    border.width: fieldList.activeFocus && ListView.isCurrentItem ? 1 : 0
                    border.color: Theme.accent

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
                        onClicked: {
                            fieldList.currentIndex = fieldRow.index
                            dialog.toggle(fieldRow.modelData.key)
                        }
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
                visible: dialog.downloader.searchingCovers || coverModel.count <= 1 || dialog.downloader.coversError

                BusyIndicator {
                    Layout.preferredWidth: Theme.rowHeight
                    Layout.preferredHeight: Theme.rowHeight
                    visible: dialog.downloader.searchingCovers
                    running: visible
                }

                Label {
                    objectName: "downloadCoversStatus"

                    Layout.fillWidth: true
                    text: dialog.downloader.searchingCovers ? qsTr("Looking for covers…")
                        : dialog.downloader.coversError ? dialog.downloader.coversError
                        : qsTr("No cover was found.")
                    color: dialog.downloader.coversError && !dialog.downloader.searchingCovers ? Theme.danger : Theme.textMuted
                    wrapMode: Text.Wrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
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
                model: coverModel
                boundsBehavior: Flickable.StopAtBounds
                keyNavigationEnabled: true
                highlightMoveDuration: 0
                ScrollBar.vertical: ThinScrollBar {}
                // The arrows pick, as a click does.
                onCurrentIndexChanged: {
                    if (activeFocus && !dialog.syncingCovers && currentIndex >= 0)
                        dialog.pickCover(currentIndex)
                }

                delegate: Item {
                    id: coverCell

                    required property string name
                    required property int pixelWidth
                    required property int pixelHeight
                    required property string url
                    required property bool own
                    required property int index
                    readonly property bool isCurrent: dialog.coverUrl !== "" ? url === dialog.coverUrl : own

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
                            source: coverCell.url
                            sourceSize.width: coverGrid.coverWidth * 2
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            // A file the download replaces as it goes.
                            cache: false
                        }

                        Label {
                            Layout.fillWidth: true
                            text: coverCell.name
                            color: coverCell.isCurrent ? Theme.textBright : Theme.text
                            font.pointSize: Theme.fontPointSizeCaption
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                        }

                        Label {
                            Layout.fillWidth: true
                            text: coverCell.pixelWidth > 0
                                ? qsTr("%1 × %2").arg(coverCell.pixelWidth).arg(coverCell.pixelHeight) : ""
                            color: Theme.textFaint
                            font.pointSize: Theme.fontPointSizeLabel
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }

                    MouseArea {
                        id: coverMouse

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: dialog.pickCover(coverCell.index)
                        onDoubleClicked: {
                            dialog.pickCover(coverCell.index)
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
                    ? (dialog.coverPicked ? qsTr("Fields to take: %1 of %2, and the cover picked")
                                          : qsTr("Fields to take: %1 of %2")).arg(dialog.tickedCount).arg(dialog.rows.length)
                    : dialog.coverPicked ? qsTr("This cover replaces the book's")
                                         : qsTr("The cover stays as it is")
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
                enabled: dialog.tickedCount > 0 || dialog.coverPicked
                variant: "primary"
                text: qsTr("Save")
                onClicked: dialog.save()
            }
        }
    }
}
