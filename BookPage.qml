pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window

import Cousoir

// A book: its cover, title, authors and description, beside the details
// pane with the rest of its metadata and its files. While the library may be
// written, an image dropped on the page replaces the cover, another file is
// added as a format, the Cover menu picks, pastes or removes a cover, and
// Edit opens the description as text: Ctrl+Enter saves it, Esc puts it back,
// and closing the book saves it too.
Rectangle {
    id: page

    required property BookLibrary library
    property ConversionService conversions: null
    property DeviceLibrary devices: null
    property var book: ({})
    property bool detailsVisible: true
    readonly property int coverWidth: Math.round(180 * Theme.spacingScale)
    // What the details pane would say, in one line while it is hidden.
    readonly property var facts: [
        book.published || "",
        (book.languages || []).join(", "),
        (book.formats || []).map(format => format.format).join(", ")
    ].filter(fact => fact.length > 0)

    // The toolbar asks before undoing every change since the book opened.
    property bool confirmingRevert: false
    // The description is open as text, for the book of that id.
    property int descriptionBookId: 0
    readonly property bool editingDescription: descriptionBookId > 0 && descriptionBookId === (book.id || 0)
    readonly property bool editing: details.editing || editingDescription
    readonly property var coverActions: [
        { key: "choose", title: qsTr("Choose image…") },
        { key: "paste", title: qsTr("Paste image") },
        { key: "remove", title: qsTr("Remove cover") }
    ].filter(action => action.key !== "remove" || page.book.hasCover)

    signal failed(string message)
    // Convert, chosen in the details pane.
    signal convertRequested(string format)
    // Send to a Kobo, chosen in the details pane.
    signal sendRequested(string root)
    // Something done, for the status bar.
    signal noticed(string message)

    // What the readers connected know of the book's reading.
    readonly property var reading: page.devices && page.devices.rows !== undefined && page.book.id
        ? page.devices.reading(page.book.id) : []
    // The copy whose notes the save dialog writes: { root, lpath }.
    property var savingNotes: ({})

    color: Theme.canvas

    function setCover(url) {
        const error = page.library.setCover(page.book.id, url.toString())
        if (error)
            page.failed(error)
    }

    // An image becomes the cover, anything else one of the book's files.
    function dropFile(url) {
        const name = url.toString().toLowerCase()
        if (/\.(jpe?g|png|webp|gif|bmp)$/.test(name))
            page.setCover(url)
        else
            details.addFormat(url)
    }

    function pasteCover() {
        const error = page.library.pasteCover(page.book.id)
        if (error)
            page.failed(error)
    }

    function runCoverAction(key) {
        if (key === "choose") {
            coverDialog.open()
            return
        }
        const error = key === "paste" ? page.library.pasteCover(page.book.id) : page.library.removeCover(page.book.id)
        if (error)
            page.failed(error)
    }

    function finishEditing() {
        details.finishEditing()
        if (page.editingDescription)
            page.saveDescription()
    }

    function editDescription() {
        descriptionArea.text = page.book.descriptionText || ""
        page.descriptionBookId = page.book.id || 0
        descriptionArea.forceActiveFocus()
        descriptionArea.cursorPosition = descriptionArea.length
    }

    function saveDescription() {
        const error = page.library.saveDescription(page.descriptionBookId, descriptionArea.text)
        page.descriptionBookId = 0
        page.forceActiveFocus()
        if (error)
            page.failed(error)
    }

    function cancelDescription() {
        page.descriptionBookId = 0
        page.forceActiveFocus()
    }

    function revertChanges() {
        page.confirmingRevert = false
        details.finishEditing()
        page.library.revertCurrentBook()
        page.forceActiveFocus()
    }

    onBookChanged: {
        if (!page.library.canRevert)
            confirmingRevert = false
    }

    // Anywhere on the page; the cover shows that it takes the drop.
    DropArea {
        id: pageDrop

        anchors.fill: parent
        enabled: page.library.writable
        onEntered: function(drag) {
            drag.accepted = drag.hasUrls
        }
        onDropped: function(drop) {
            if (drop.hasUrls)
                page.dropFile(drop.urls[0])
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        ScrollView {
            id: bookScroll

            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                id: bookContent

                x: Theme.pagePadding
                width: bookScroll.availableWidth - 2 * Theme.pagePadding
                spacing: Theme.pagePadding

                Item { Layout.preferredHeight: 0 }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.pagePadding

                    ColumnLayout {
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: page.coverWidth
                        spacing: Theme.gutter

                        Rectangle {
                            objectName: "bookCover"

                            Layout.preferredWidth: page.coverWidth
                            Layout.preferredHeight: cover.status === Image.Ready && cover.implicitWidth > 0
                                ? Math.round(page.coverWidth * cover.implicitHeight / cover.implicitWidth) + 2
                                : Math.round(page.coverWidth * 1.5)
                            color: pageDrop.containsDrag ? Theme.hover : Theme.fill
                            border.width: 1
                            border.color: pageDrop.containsDrag ? Theme.accent : Theme.divider

                            Image {
                                id: cover
                                objectName: "bookCoverImage"

                                anchors.fill: parent
                                anchors.margins: 1
                                visible: status === Image.Ready
                                source: page.book.cover || ""
                                sourceSize.width: Math.round(page.coverWidth * 2 * Screen.devicePixelRatio)
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                            }

                            Label {
                                anchors.centerIn: parent
                                width: parent.width - 2 * Theme.gutter
                                visible: cover.status !== Image.Ready
                                text: page.library.writable ? qsTr("No cover\nDrop an image here") : qsTr("No cover")
                                color: Theme.textFaint
                                font.pointSize: Theme.fontPointSizeCaption
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.Wrap
                            }
                        }

                        FlatButton {
                            id: coverButton
                            objectName: "coverButton"

                            visible: page.library.writable
                            variant: "outline"
                            text: qsTr("Cover")
                            icon.source: "assets/icons/image.svg"
                            onClicked: coverMenu.popup(coverButton, 0, coverButton.height)
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: Math.round(6 * Theme.spacingScale)

                        Label {
                            objectName: "bookTitle"

                            Layout.fillWidth: true
                            text: page.book.title || ""
                            color: Theme.textBright
                            font.pointSize: Theme.fontPointSizeTitle
                            font.weight: Font.Bold
                            wrapMode: Text.Wrap
                        }

                        Label {
                            objectName: "bookAuthors"

                            Layout.fillWidth: true
                            text: (page.book.authors || []).join(" & ")
                            wrapMode: Text.Wrap
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: page.book.seriesText || ""
                            color: Theme.textMuted
                            wrapMode: Text.Wrap
                        }

                        Label {
                            objectName: "bookRating"

                            visible: text.length > 0
                            text: page.book.rating || ""
                            color: Theme.textMuted
                        }

                        Label {
                            objectName: "factsLine"

                            Layout.fillWidth: true
                            Layout.topMargin: Theme.gutter
                            visible: !page.detailsVisible && page.facts.length > 0
                            text: page.facts.join("  ·  ")
                            color: Theme.textMuted
                            wrapMode: Text.Wrap
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.gutter

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gutter

                        HeadingLabel {
                            Layout.fillWidth: true
                            text: qsTr("Description")
                        }

                        FlatButton {
                            objectName: "editDescriptionButton"

                            visible: page.library.writable && !page.editingDescription
                            text: qsTr("Edit")
                            tip: qsTr("Edit the description")
                            onClicked: page.editDescription()
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: page.editingDescription
                        spacing: Theme.gutter

                        TextArea {
                            id: descriptionArea
                            objectName: "descriptionEditor"

                            Layout.fillWidth: true
                            Layout.minimumHeight: 6 * Theme.rowHeight
                            wrapMode: TextArea.Wrap
                            textFormat: TextEdit.PlainText
                            color: Theme.text
                            selectionColor: Theme.textSelection
                            selectedTextColor: Theme.textBright
                            placeholderText: qsTr("No description.")
                            placeholderTextColor: Theme.textFaint
                            padding: Theme.controlInset
                            Accessible.name: qsTr("Description")

                            background: Rectangle {
                                color: Theme.fill
                                border.width: 1
                                border.color: descriptionArea.activeFocus ? Theme.accent : Theme.controlBorder
                            }

                            // Esc puts it back here instead of closing the book.
                            Keys.onShortcutOverride: function(event) {
                                event.accepted = event.key === Qt.Key_Escape
                            }
                            Keys.onEscapePressed: page.cancelDescription()
                            Keys.onPressed: function(event) {
                                if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                    && (event.modifiers & Qt.ControlModifier)) {
                                    page.saveDescription()
                                    event.accepted = true
                                }
                            }
                        }

                        Label {
                            objectName: "descriptionLoss"

                            Layout.fillWidth: true
                            visible: page.book.descriptionLosesFormatting === true
                            text: qsTr("This description holds more than text, a table or an image: saving it from here leaves that out.")
                            color: Theme.danger
                            font.pointSize: Theme.fontPointSizeCaption
                            wrapMode: Text.Wrap
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("**bold**   *italics*   [link](https://…)   - list   1. list   # heading")
                            color: Theme.textMuted
                            font.pointSize: Theme.fontPointSizeCaption
                            wrapMode: Text.Wrap
                        }

                        RowLayout {
                            spacing: Theme.gutter

                            FlatButton {
                                objectName: "saveDescriptionButton"

                                variant: "primary"
                                text: qsTr("Save")
                                tip: qsTr("Save (Ctrl+Enter)")
                                onClicked: page.saveDescription()
                            }

                            FlatButton {
                                objectName: "cancelDescriptionButton"

                                variant: "outline"
                                text: qsTr("Cancel")
                                tip: qsTr("Cancel (Esc)")
                                onClicked: page.cancelDescription()
                            }
                        }
                    }

                    Label {
                        objectName: "bookDescription"

                        Layout.fillWidth: true
                        visible: !page.editingDescription
                        text: page.book.description || qsTr("No description.")
                        textFormat: page.book.description ? Text.RichText : Text.PlainText
                        color: page.book.description ? Theme.text : Theme.textMuted
                        linkColor: Theme.accent
                        wrapMode: Text.Wrap
                        onLinkActivated: function(link) { Qt.openUrlExternally(link) }

                        HoverHandler {
                            cursorShape: parent.hoveredLink ? Qt.PointingHandCursor : Qt.ArrowCursor
                        }
                    }
                }

                // Each reader's copy of the book: how far it is read, when,
                // and the highlights and notes made in it.
                Repeater {
                    model: page.reading

                    delegate: ColumnLayout {
                        id: copy

                        required property var modelData
                        readonly property bool reading: copy.modelData.status === 1 && copy.modelData.percent >= 0

                        objectName: "readingOn_" + copy.modelData.name
                        Layout.fillWidth: true
                        spacing: Theme.gutter

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.gutter
                            spacing: Theme.gutter

                            HeadingLabel {
                                Layout.fillWidth: true
                                text: qsTr("On %1").arg(copy.modelData.name)
                            }

                            FlatButton {
                                objectName: "copyNotesButton"

                                visible: copy.modelData.notes.length > 0
                                text: qsTr("Copy")
                                tip: qsTr("Copy the highlights and notes as Markdown")
                                onClicked: {
                                    page.devices.copyNotes(copy.modelData.root, copy.modelData.lpath)
                                    page.noticed(qsTr("Highlights and notes copied as Markdown"))
                                }
                            }

                            FlatButton {
                                objectName: "saveNotesButton"

                                visible: copy.modelData.notes.length > 0
                                text: qsTr("Save…")
                                tip: qsTr("Save the highlights and notes as a Markdown file")
                                onClicked: {
                                    page.savingNotes = { root: copy.modelData.root, lpath: copy.modelData.lpath }
                                    notesDialog.selectedFile = notesDialog.currentFolder + "/"
                                        + (page.book.title || "notes").replace(/[\\/:*?"<>|]/g, "_") + ".md"
                                    notesDialog.open()
                                }
                            }
                        }

                        Label {
                            objectName: "readingState"

                            Layout.fillWidth: true
                            text: [copy.modelData.state,
                                   copy.modelData.lastRead ? qsTr("last read %1").arg(copy.modelData.lastRead) : "",
                                   copy.modelData.timeSpent,
                                   copy.modelData.notes.length > 0
                                       ? (copy.modelData.notes.length === 1 ? qsTr("1 note")
                                                                            : qsTr("%1 notes").arg(copy.modelData.notes.length))
                                       : ""].filter(part => part.length > 0).join("  ·  ")
                            color: Theme.textMuted
                            wrapMode: Text.Wrap
                        }

                        // How far, as a hairline.
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Math.max(2, Math.round(2 * Theme.spacingScale))
                            visible: copy.reading
                            color: Theme.divider

                            Rectangle {
                                width: parent.width * Math.min(100, copy.modelData.percent) / 100
                                height: parent.height
                                color: Theme.accent
                            }
                        }

                        Repeater {
                            model: copy.modelData.notes

                            delegate: ColumnLayout {
                                id: note

                                required property var modelData
                                required property int index
                                readonly property bool bookmark: note.modelData.type === "Bookmark"

                                Layout.fillWidth: true
                                Layout.topMargin: note.modelData.newChapter ? Theme.gutter : 0
                                spacing: Math.round(Theme.gutter / 2)

                                Label {
                                    Layout.fillWidth: true
                                    visible: note.modelData.newChapter && note.modelData.chapter.length > 0
                                    text: note.modelData.chapter
                                    font.weight: Font.Bold
                                    wrapMode: Text.Wrap
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.gutter

                                    // A quote's bar; none for a bookmark.
                                    Rectangle {
                                        Layout.preferredWidth: Math.max(2, Math.round(2 * Theme.spacingScale))
                                        Layout.fillHeight: true
                                        color: note.bookmark ? "transparent" : Theme.controlBorder
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: Math.round(Theme.gutter / 2)

                                        Label {
                                            objectName: "noteText"

                                            Layout.fillWidth: true
                                            visible: text.length > 0
                                            text: note.bookmark ? qsTr("Bookmark") : note.modelData.text
                                            color: note.bookmark ? Theme.textMuted : Theme.text
                                            wrapMode: Text.Wrap
                                        }

                                        Label {
                                            objectName: "noteAnnotation"

                                            Layout.fillWidth: true
                                            visible: note.modelData.annotation.length > 0
                                            text: note.modelData.annotation
                                            color: Theme.textBright
                                            font.italic: true
                                            wrapMode: Text.Wrap
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: [qsTr("%1 % into the chapter").arg(note.modelData.progress),
                                                   note.modelData.created].filter(part => part.length > 0).join("  ·  ")
                                            color: Theme.textFaint
                                            font.pointSize: Theme.fontPointSizeCaption
                                            wrapMode: Text.Wrap
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Item { Layout.preferredHeight: Theme.pagePadding }
            }
        }

        Divider {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            visible: details.visible
        }

        BookDetailsPane {
            id: details
            objectName: "detailsPane"

            Layout.preferredWidth: Theme.paneWidth
            Layout.fillHeight: true
            visible: page.detailsVisible
            library: page.library
            conversions: page.conversions
            devices: page.devices
            book: page.book
            onFailed: function(message) { page.failed(message) }
            onConvertRequested: function(format) { page.convertRequested(format) }
            onSendRequested: function(root) { page.sendRequested(root) }
        }
    }

    ActionMenu {
        id: coverMenu
        objectName: "coverMenu"

        actions: page.coverActions
        onChosen: function(key) { page.runCoverAction(key) }
    }

    FileDialog {
        id: notesDialog

        title: qsTr("Save the highlights and notes")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "md"
        nameFilters: [qsTr("Markdown (*.md)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: {
            const error = page.devices.saveNotes(page.savingNotes.root, page.savingNotes.lpath, selectedFile)
            if (error)
                page.failed(error)
            else
                page.noticed(qsTr("Highlights and notes saved"))
        }
    }

    FileDialog {
        id: coverDialog

        title: qsTr("Choose a cover")
        nameFilters: [qsTr("Images (*.jpg *.jpeg *.png *.webp *.gif *.bmp)")]
        onAccepted: page.setCover(selectedFile)
    }
}
