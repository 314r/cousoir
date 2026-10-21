pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// What a library collects over time, as the check finds it: the same book
// twice, an author under several names, books whose files are gone and files
// the database does not know. Each finding comes with what fixes it, or
// Ignore; a row's menu (Enter, Menu or Shift+F10) has the same.
Rectangle {
    id: page

    required property BookLibrary library
    required property ConcurrencyGuard guard
    readonly property var rows: library.check.rows || []
    readonly property bool canFix: library.writable && !library.saving
    readonly property var groupTitles: ({
        "duplicates": qsTr("Duplicate books"),
        "authors": qsTr("Author name variants"),
        "missing": qsTr("Books without files"),
        "outside": qsTr("Files outside the database")
    })

    signal bookRequested(int bookId)
    signal failed(string message)

    color: Theme.canvas

    function focusList() {
        checkList.forceActiveFocus()
    }

    function moveCurrent(delta) {
        checkList.currentIndex = Math.max(0, Math.min(checkList.count - 1, checkList.currentIndex + delta))
        checkList.forceActiveFocus()
    }

    function plural(count, one, many) {
        return (count === 1 ? one : many).arg(count)
    }

    function report(error) {
        if (error)
            page.failed(error)
    }

    function reason(row) {
        if (row.kind === "duplicates") {
            const reasons = row.sameTitle ? [qsTr("Same title and authors")] : []
            return reasons.concat(row.isbns.map(isbn => qsTr("Same ISBN %1").arg(isbn))).join(" · ")
        }
        if (row.kind === "authors")
            return row.initials ? qsTr("Possibly the same person") : qsTr("Same name written differently")
        if (row.kind === "missing") {
            if (row.folderMissing)
                return qsTr("Its folder is missing")
            if (row.noFormats)
                return qsTr("It has no file")
            return row.formats.length === 1 ? qsTr("The %1 file is missing").arg(row.formats[0])
                                            : qsTr("The %1 files are missing").arg(row.formats.join(", "))
        }
        return row.folder ? qsTr("Folder") : row.size
    }

    // The row's actions, for its menu.
    function actions(row) {
        let list = []
        if (row.kind === "duplicates") {
            for (const book of row.books) {
                if (page.canFix)
                    list.push({ key: "keep:" + book.id, title: qsTr("Keep “%1” (book %2)").arg(book.title).arg(book.id) })
                list.push({ key: "open:" + book.id, title: qsTr("Open book %1").arg(book.id) })
            }
        } else if (row.kind === "authors") {
            if (page.canFix && !page.library.check.caseDuplicates) {
                for (const author of row.authors)
                    list.push({ key: "use:" + author.id, title: qsTr("Use “%1”").arg(author.name) })
            }
        } else if (row.kind === "missing") {
            if (page.canFix && row.formats.length > 0)
                list.push({ key: "remove", title: qsTr("Remove missing entries") })
            list.push({ key: "open:" + row.bookId, title: qsTr("Open book") })
        } else {
            list.push({ key: "show", title: qsTr("Show in folder") })
        }
        list.push({ key: "ignore", title: qsTr("Ignore") })
        return list
    }

    function runAction(key, row) {
        const [action, value] = key.split(":")
        const id = Number(value)
        if (action === "keep")
            askKeep(row, row.books.find(book => book.id === id))
        else if (action === "open")
            page.bookRequested(id)
        else if (action === "use")
            askMerge(row, row.authors.find(author => author.id === id))
        else if (action === "remove")
            askRemove(row)
        else if (action === "show")
            page.report(page.library.showInFolder(row.path))
        else if (action === "ignore")
            page.library.ignoreFinding(row.key)
    }

    function askKeep(row, book) {
        const others = row.books.filter(other => other.id !== book.id)
        keepDialog.keepId = book.id
        keepDialog.otherIds = others.map(other => other.id)
        keepDialog.title = others.length === 1
            ? qsTr("Keep “%1” and move the other copy to the Trash?").arg(book.title)
            : qsTr("Keep “%1” and move the %2 other copies to the Trash?").arg(book.title).arg(others.length)
        const copied = book.gains.length === 0 ? ""
            : (book.gains.length === 1 ? qsTr("The %1 file it lacks is copied to it first.").arg(book.gains[0])
                                       : qsTr("The %1 files it lacks are copied to it first.").arg(book.gains.join(", ")))
              + " "
        keepDialog.message = copied + plural(page.library.trashExpiryDays,
            qsTr("Restore from the Trash to undo; Calibre deletes what is there after %1 day."),
            qsTr("Restore from the Trash to undo; Calibre deletes what is there after %1 days."))
        keepDialog.ask()
    }

    function askMerge(row, author) {
        const others = row.authors.filter(other => other.id !== author.id)
        const books = others.reduce((count, other) => count + other.count, 0)
        mergeDialog.authorIds = row.ids
        mergeDialog.keepId = author.id
        mergeDialog.title = books === 1 ? qsTr("Use “%1” on 1 book?").arg(author.name)
                                        : qsTr("Use “%1” on %2 books?").arg(author.name).arg(books)
        mergeDialog.message = qsTr("%1 replaced, and the books' folders move to match. This cannot be undone here: edit the books' authors again to change them back.")
            .arg(others.length === 1 ? qsTr("“%1” is").arg(others[0].name)
                                     : qsTr("%1 are").arg(others.map(other => "“" + other.name + "”").join(", ")))
        mergeDialog.ask()
    }

    function askRemove(row) {
        removeDialog.bookId = row.bookId
        removeDialog.title = row.formats.length === 1
            ? qsTr("Remove the missing %1 entry of “%2”?").arg(row.formats[0]).arg(row.title)
            : qsTr("Remove the missing %1 entries of “%2”?").arg(row.formats.join(", ")).arg(row.title)
        removeDialog.ask()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.pagePadding
            Layout.rightMargin: Theme.pagePadding
            Layout.topMargin: Theme.gutter
            Layout.bottomMargin: Theme.gutter
            spacing: Theme.gutter

            Label {
                objectName: "checkState"

                Layout.fillWidth: true
                text: page.library.checking ? qsTr("Checking the library…")
                    : page.library.check.checked ? qsTr("Checked at %1").arg(page.library.check.checked) : ""
                color: Theme.textMuted
                elide: Text.ElideRight
            }

            FlatButton {
                objectName: "showIgnoredButton"

                visible: (page.library.check.ignored || 0) > 0
                text: qsTr("Show %1 ignored").arg(page.library.check.ignored || 0)
                onClicked: page.library.showIgnored()
            }

            FlatButton {
                objectName: "recheckButton"

                variant: "outline"
                enabled: !page.library.checking
                text: qsTr("Check again")
                icon.source: "assets/icons/refresh.svg"
                onClicked: page.library.recheck()
            }
        }

        Label {
            objectName: "checkReadOnly"

            Layout.fillWidth: true
            visible: !page.library.writable && page.rows.length > 0
            leftPadding: Theme.pagePadding
            rightPadding: Theme.pagePadding
            bottomPadding: Theme.gutter
            text: qsTr("%1. The fixes wait until the library can be changed.").arg(page.guard.readOnlyReason)
            color: Theme.textMuted
            wrapMode: Text.Wrap
        }

        Divider {
            Layout.fillWidth: true
        }

        ListView {
            id: checkList
            objectName: "checkList"

            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            focus: true
            keyNavigationEnabled: true
            model: page.rows
            boundsBehavior: Flickable.StopAtBounds
            Accessible.name: qsTr("Check")

            ScrollBar.vertical: ThinScrollBar { }

            onCountChanged: {
                if (count > 0 && (currentIndex < 0 || currentIndex >= count))
                    currentIndex = Math.min(Math.max(currentIndex, 0), count - 1)
            }

            function openMenu() {
                if (!currentItem)
                    return
                rowMenu.row = page.rows[currentIndex]
                rowMenu.popup(currentItem, Theme.pagePadding, Math.round(currentItem.height / 2))
            }

            Keys.onReturnPressed: openMenu()
            Keys.onEnterPressed: openMenu()
            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && event.modifiers & Qt.ShiftModifier)) {
                    openMenu()
                    event.accepted = true
                }
            }

            delegate: Rectangle {
                id: finding

                required property var modelData
                required property int index
                readonly property bool firstOfKind: index === 0 || page.rows[index - 1].kind !== modelData.kind

                objectName: "check_" + modelData.key
                width: ListView.view.width
                implicitHeight: content.implicitHeight + 2 * Theme.gutter
                color: ListView.isCurrentItem && checkList.activeFocus ? Theme.selected
                    : hover.hovered ? Theme.hover : "transparent"

                HoverHandler {
                    id: hover
                }

                TapHandler {
                    onTapped: checkList.currentIndex = finding.index
                }

                ColumnLayout {
                    id: content

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.pagePadding
                    anchors.rightMargin: Theme.pagePadding
                    spacing: Math.round(4 * Theme.spacingScale)

                    HeadingLabel {
                        Layout.fillWidth: true
                        Layout.topMargin: finding.index > 0 ? Theme.gutter : 0
                        visible: finding.firstOfKind
                        text: page.groupTitles[finding.modelData.kind] + " · "
                            + page.rows.filter(row => row.kind === finding.modelData.kind).length
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gutter

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Label {
                                Layout.fillWidth: true
                                visible: finding.modelData.kind === "missing" || finding.modelData.kind === "outside"
                                text: finding.modelData.kind === "missing" ? finding.modelData.title
                                    : finding.modelData.path || ""
                                font.weight: Font.Bold
                                elide: Text.ElideMiddle
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: finding.modelData.kind === "missing"
                                text: finding.modelData.authors || ""
                                color: Theme.textMuted
                                elide: Text.ElideRight
                            }

                            Label {
                                objectName: "reason"

                                Layout.fillWidth: true
                                text: page.reason(finding.modelData)
                                color: finding.modelData.kind === "missing" ? Theme.danger : Theme.textMuted
                                font.pointSize: Theme.fontPointSizeCaption
                                elide: Text.ElideRight
                            }
                        }

                        FlatButton {
                            objectName: "remove_" + finding.modelData.key

                            visible: finding.modelData.kind === "missing" && finding.modelData.formats.length > 0
                            variant: "outline"
                            enabled: page.canFix
                            text: qsTr("Remove missing entries")
                            onClicked: page.askRemove(finding.modelData)
                        }

                        FlatButton {
                            visible: finding.modelData.kind === "missing"
                            variant: "outline"
                            text: qsTr("Open book")
                            onClicked: page.bookRequested(finding.modelData.bookId)
                        }

                        FlatButton {
                            objectName: "show_" + finding.modelData.key

                            visible: finding.modelData.kind === "outside"
                            variant: "outline"
                            text: qsTr("Show in folder")
                            icon.source: "assets/icons/folder.svg"
                            onClicked: page.report(page.library.showInFolder(finding.modelData.path))
                        }

                        FlatButton {
                            objectName: "ignore_" + finding.modelData.key

                            text: qsTr("Ignore")
                            tip: qsTr("Leave it out until it changes")
                            onClicked: page.library.ignoreFinding(finding.modelData.key)
                        }
                    }

                    // The copies of a book, each one the one to keep.
                    Repeater {
                        model: finding.modelData.kind === "duplicates" ? finding.modelData.books : []

                        delegate: RowLayout {
                            id: copy

                            required property var modelData
                            readonly property int thumbWidth: Theme.rowHeight

                            Layout.fillWidth: true
                            spacing: Theme.gutter

                            Rectangle {
                                Layout.preferredWidth: copy.thumbWidth
                                Layout.preferredHeight: Math.round(copy.thumbWidth * 1.5)
                                color: Theme.fill
                                border.width: 1
                                border.color: Theme.divider

                                Image {
                                    anchors.fill: parent
                                    anchors.margins: 1
                                    visible: copy.modelData.cover.length > 0
                                    source: copy.modelData.cover
                                    sourceSize.width: copy.thumbWidth * 2
                                    fillMode: Image.PreserveAspectCrop
                                    asynchronous: true
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2

                                Label {
                                    Layout.fillWidth: true
                                    text: copy.modelData.title
                                    font.weight: Font.Bold
                                    elide: Text.ElideRight

                                    TapHandler {
                                        onTapped: page.bookRequested(copy.modelData.id)
                                    }

                                    HoverHandler {
                                        cursorShape: Qt.PointingHandCursor
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: [copy.modelData.authors, qsTr("Book %1").arg(copy.modelData.id),
                                           copy.modelData.formats.length > 0 ? copy.modelData.formats.join(", ") : qsTr("No file"),
                                           qsTr("added %1").arg(copy.modelData.added)].join(" · ")
                                    color: Theme.textMuted
                                    font.pointSize: Theme.fontPointSizeCaption
                                    elide: Text.ElideRight
                                }
                            }

                            FlatButton {
                                objectName: "keep_" + copy.modelData.id

                                variant: "outline"
                                enabled: page.canFix
                                text: qsTr("Keep this one")
                                tip: copy.modelData.gains.length > 0
                                    ? qsTr("Takes %1 from the other copy").arg(copy.modelData.gains.join(", ")) : ""
                                onClicked: page.askKeep(finding.modelData, copy.modelData)
                            }
                        }
                    }

                    // The names of an author, each one the one to keep.
                    Repeater {
                        model: finding.modelData.kind === "authors" ? finding.modelData.authors : []

                        delegate: RowLayout {
                            id: nameRow

                            required property var modelData

                            Layout.fillWidth: true
                            spacing: Theme.gutter

                            Label {
                                Layout.fillWidth: true
                                text: nameRow.modelData.name
                                font.weight: Font.Bold
                                elide: Text.ElideRight
                            }

                            Label {
                                text: page.plural(nameRow.modelData.count, qsTr("%1 book"), qsTr("%1 books"))
                                color: Theme.textMuted
                            }

                            FlatButton {
                                objectName: "useName_" + nameRow.modelData.id

                                variant: "outline"
                                enabled: page.canFix && !page.library.check.caseDuplicates
                                text: qsTr("Use this name")
                                tip: page.library.check.caseDuplicates
                                    ? qsTr("Two author names differ only by case; Calibre merges them the next time it saves an author")
                                    : ""
                                onClicked: page.askMerge(finding.modelData, nameRow.modelData)
                            }
                        }
                    }
                }
            }
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * Theme.panePadding, Math.round(420 * Theme.spacingScale))
        visible: page.rows.length === 0
        spacing: Theme.gutter

        Label {
            objectName: "checkEmpty"

            Layout.fillWidth: true
            text: page.library.checking ? qsTr("Checking the library…") : qsTr("Nothing to fix")
            font.weight: Font.Bold
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }

        Label {
            Layout.fillWidth: true
            visible: !page.library.checking
            text: qsTr("No duplicates, author name variants, missing files or unknown files.")
            color: Theme.textMuted
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
    }

    ActionMenu {
        id: rowMenu
        objectName: "checkMenu"

        property var row: ({})

        actions: page.actions(rowMenu.row)
        onChosen: function(key) { page.runAction(key, rowMenu.row) }
        onClosed: {
            if (page.visible)
                checkList.forceActiveFocus()
        }
    }

    ConfirmDialog {
        id: keepDialog
        objectName: "keepDialog"

        property int keepId: 0
        property var otherIds: []

        acceptText: qsTr("Move to Trash")
        acceptVariant: "danger"
        onAccepted: page.report(page.library.keepDuplicate(keepDialog.keepId, keepDialog.otherIds))
    }

    ConfirmDialog {
        id: mergeDialog
        objectName: "mergeDialog"

        property var authorIds: []
        property int keepId: 0

        acceptText: qsTr("Merge")
        onAccepted: page.report(page.library.mergeAuthors(mergeDialog.authorIds, mergeDialog.keepId))
    }

    ConfirmDialog {
        id: removeDialog
        objectName: "removeDialog"

        property int bookId: 0

        message: qsTr("Nothing is deleted from the disk. If the library is shared, wait until the other computers' changes have arrived: a file still syncing looks missing.")
        acceptText: qsTr("Remove")
        acceptVariant: "danger"
        onAccepted: page.report(page.library.removeMissingFormats(removeDialog.bookId))
    }
}
