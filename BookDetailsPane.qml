pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

import Reliure

// A book's metadata as Calibre keeps it, and its files, each of which opens
// in the desktop's reader for it, goes to the library's trash, or is added
// from a file. While the library may be written the
// metadata edits in place: a field saves when you leave it, with Enter or
// Tab; Esc puts the stored value back; a value the field cannot take shows
// in red and is not saved.
FocusScope {
    id: pane

    required property BookLibrary library
    property var book: ({})
    readonly property bool editable: library.writable && (book.id || 0) > 0
    // A field has the focus.
    readonly property bool editing: activeFocus && Window.activeFocusItem?.detailField === true
    readonly property var ratingChoices: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10].map(rating => ({
        value: rating,
        label: rating === 0 ? qsTr("None") : "★".repeat(Math.floor(rating / 2)) + (rating % 2 ? "½" : "")
    }))
    readonly property var facts: [
        { key: "title", label: qsTr("Title"), value: book.title || "" },
        // The sorts only when they differ from what they sort.
        { key: "sort", label: qsTr("Title sort"), value: book.sort !== book.title ? book.sort || "" : "" },
        { key: "authors", label: qsTr("Authors"), value: (book.authors || []).join(" & ") },
        { key: "authorSort", label: qsTr("Author sort"),
          value: book.authorSort !== (book.authors || []).join(" & ") ? book.authorSort || "" : "" },
        { key: "series", label: qsTr("Series"), value: book.seriesText || "" },
        { key: "tags", label: qsTr("Tags"), value: (book.tags || []).join(", ") },
        { key: "rating", label: qsTr("Rating"), value: book.rating || "" },
        { key: "publisher", label: qsTr("Publisher"), value: book.publisher || "" },
        { key: "languages", label: qsTr("Languages"), value: (book.languages || []).join(", ") },
        { key: "published", label: qsTr("Published"), value: book.published || "" },
        { key: "added", label: qsTr("Added"), value: book.added || "" },
        { key: "modified", label: qsTr("Modified"), value: book.modified || "" }
    ]

    signal failed(string message)

    function open(format) {
        const error = pane.library.openFormat(pane.book.id, format)
        if (error)
            pane.failed(error)
    }

    // A file as the book's format of its extension, after asking when the
    // book has one already.
    function addFormat(url) {
        const name = decodeURIComponent(url.toString().split("/").pop())
        const format = name.includes(".") ? name.split(".").pop().toUpperCase() : ""
        if ((pane.book.formats || []).some(existing => existing.format === format)) {
            replaceDialog.url = url
            replaceDialog.title = qsTr("Replace the book's %1 file?").arg(format)
            replaceDialog.ask()
            return
        }
        const error = pane.library.addFormat(pane.book.id, url.toString())
        if (error)
            pane.failed(error)
    }

    function removeFormat(format) {
        removeDialog.format = format
        removeDialog.title = qsTr("Move the %1 file to the trash?").arg(format)
        removeDialog.ask()
    }

    function save(key, value) {
        const changes = {}
        changes[key] = value
        const error = pane.library.updateBook(pane.book.id, changes)
        if (error)
            pane.failed(error)
    }

    // Leaves the field being edited, saving it.
    function finishEditing() {
        if (pane.editing)
            pane.forceActiveFocus()
    }

    // A metadata field. `value` is the stored value as the field shows it.
    component DetailField: RowLayout {
        id: detail

        property string label
        property string key
        property string value
        property string placeholder
        property bool number: false
        property bool allowEmpty: true
        // A check of the typed text beyond emptiness.
        property var check: null
        readonly property bool valid: accepts(field.text)

        function accepts(text) {
            if (!allowEmpty && text.trim().length === 0)
                return false
            if (number)
                return text.trim().length > 0 && isFinite(Number(text.trim().replace(",", "."))) && Number(text.trim().replace(",", ".")) >= 0
            return check ? check(text) : true
        }

        function revert() {
            field.text = Qt.binding(() => detail.value)
        }

        function commit() {
            if (field.text.trim() !== detail.value.trim() && valid)
                pane.save(detail.key, detail.number ? Number(field.text.trim().replace(",", ".")) : field.text)
            // The saved value once it is in, or the old one.
            revert()
        }

        Layout.fillWidth: true
        spacing: Theme.rowPadding

        Label {
            Layout.preferredWidth: Math.round(11 * Theme.charWidth)
            Layout.minimumWidth: Layout.preferredWidth
            text: detail.label
            color: Theme.textMuted
            elide: Text.ElideRight
        }

        FormField {
            id: field
            objectName: "field_" + detail.key
            readonly property bool detailField: true

            Layout.fillWidth: !detail.number
            Layout.preferredWidth: detail.number ? Math.round(8 * Theme.charWidth) : 1
            text: detail.value
            placeholderText: detail.placeholder
            horizontalAlignment: detail.number ? TextInput.AlignRight : TextInput.AlignLeft
            invalid: !detail.valid
            enabled: pane.editable
            Accessible.name: detail.label

            onEditingFinished: detail.commit()
            // Esc reverts here instead of closing the book.
            Keys.onShortcutOverride: function(event) {
                event.accepted = event.key === Qt.Key_Escape
            }
            Keys.onEscapePressed: {
                detail.revert()
                pane.forceActiveFocus()
            }
        }

        Item {
            Layout.fillWidth: true
            visible: detail.number
        }
    }

    ScrollView {
        id: paneScroll

        anchors.fill: parent
        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            x: Theme.panePadding
            width: paneScroll.availableWidth - 2 * Theme.panePadding
            spacing: Theme.rowPadding

            Item { Layout.preferredHeight: Theme.pagePadding - Theme.rowPadding }

            HeadingLabel {
                Layout.fillWidth: true
                text: qsTr("Book")
            }

            ColumnLayout {
                objectName: "bookFacts"

                Layout.fillWidth: true
                visible: !pane.editable
                spacing: Math.round(6 * Theme.spacingScale)

                Repeater {
                    model: pane.facts

                    delegate: MetaRow {
                        required property var modelData

                        objectName: "fact_" + modelData.key
                        visible: modelData.value.length > 0
                        label: modelData.label
                        value: modelData.value
                    }
                }

                Repeater {
                    model: pane.book.identifiers || []

                    delegate: MetaRow {
                        required property var modelData

                        objectName: "identifier_" + modelData.type
                        label: modelData.label
                        value: modelData.value
                        link: modelData.url
                    }
                }
            }

            ColumnLayout {
                objectName: "bookFields"

                Layout.fillWidth: true
                visible: pane.editable
                spacing: Math.round(6 * Theme.spacingScale)

                DetailField {
                    key: "title"
                    label: qsTr("Title")
                    value: pane.book.title || ""
                    allowEmpty: false
                }

                DetailField {
                    key: "sort"
                    label: qsTr("Title sort")
                    value: pane.book.sort || ""
                    allowEmpty: false
                }

                DetailField {
                    key: "authors"
                    label: qsTr("Authors")
                    value: pane.book.authorsText || ""
                    placeholder: qsTr("Name & name")
                    allowEmpty: false
                }

                DetailField {
                    key: "authorSort"
                    label: qsTr("Author sort")
                    value: pane.book.authorSort || ""
                }

                DetailField {
                    key: "series"
                    label: qsTr("Series")
                    value: pane.book.series || ""
                }

                DetailField {
                    key: "seriesIndex"
                    label: qsTr("Number")
                    value: pane.book.seriesIndexText || "1"
                    number: true
                    visible: (pane.book.series || "").length > 0
                }

                DetailField {
                    key: "tags"
                    label: qsTr("Tags")
                    value: pane.book.tagsText || ""
                    placeholder: qsTr("tag, tag")
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.rowPadding

                    Label {
                        Layout.preferredWidth: Math.round(11 * Theme.charWidth)
                        text: qsTr("Rating")
                        color: Theme.textMuted
                    }

                    ChoiceBox {
                        id: ratingBox
                        objectName: "field_rating"
                        readonly property bool detailField: true

                        Layout.preferredWidth: Math.round(14 * Theme.charWidth)
                        model: pane.ratingChoices
                        currentIndex: pane.book.ratingValue || 0
                        enabled: pane.editable
                        Accessible.name: qsTr("Rating")
                        onActivated: function(index) {
                            if (index !== (pane.book.ratingValue || 0))
                                pane.save("rating", index)
                            currentIndex = Qt.binding(() => pane.book.ratingValue || 0)
                        }
                    }

                    Item { Layout.fillWidth: true }
                }

                DetailField {
                    key: "publisher"
                    label: qsTr("Publisher")
                    value: pane.book.publisher || ""
                }

                DetailField {
                    key: "languages"
                    label: qsTr("Languages")
                    value: pane.book.languagesText || ""
                    placeholder: qsTr("French, English")
                }

                DetailField {
                    key: "pubdate"
                    label: qsTr("Published")
                    value: pane.book.pubdateText || ""
                    placeholder: qsTr("YYYY-MM-DD")
                    check: text => pane.library.isDate(text)
                }

                DetailField {
                    key: "identifiers"
                    label: qsTr("Identifiers")
                    value: pane.book.identifiersText || ""
                    placeholder: qsTr("isbn:…, goodreads:…")
                }

                MetaRow {
                    Layout.topMargin: Math.round(Theme.gutter / 2)
                    label: qsTr("Added")
                    value: pane.book.added || ""
                }

                MetaRow {
                    label: qsTr("Modified")
                    value: pane.book.modified || ""
                }
            }

            HeadingLabel {
                Layout.fillWidth: true
                Layout.topMargin: Theme.gutter
                text: qsTr("Files")
            }

            Label {
                Layout.fillWidth: true
                visible: (pane.book.formats || []).length === 0
                text: qsTr("This book has no file.")
                color: Theme.textMuted
                wrapMode: Text.Wrap
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Repeater {
                    model: pane.book.formats || []

                    delegate: RowLayout {
                        id: formatRow

                        required property var modelData

                        Layout.fillWidth: true
                        spacing: Theme.gutter

                        FlatButton {
                            objectName: "format_" + formatRow.modelData.format

                            enabled: !formatRow.modelData.missing
                            text: formatRow.modelData.format
                            tip: qsTr("Open in reader")
                            icon.source: "assets/icons/external.svg"
                            onClicked: pane.open(formatRow.modelData.format)
                        }

                        Label {
                            Layout.fillWidth: true
                            text: formatRow.modelData.missing ? qsTr("File missing") : formatRow.modelData.size
                            color: formatRow.modelData.missing ? Theme.danger : Theme.textMuted
                            font.pointSize: Theme.fontPointSizeCaption
                            horizontalAlignment: Text.AlignRight
                            elide: Text.ElideRight
                        }

                        FlatButton {
                            objectName: "removeFormat_" + formatRow.modelData.format

                            visible: pane.editable
                            iconOnly: true
                            text: qsTr("Move the %1 file to the trash").arg(formatRow.modelData.format)
                            icon.source: "assets/icons/trash.svg"
                            icon.width: Math.round(Theme.iconSize * 0.85)
                            icon.height: Math.round(Theme.iconSize * 0.85)
                            onClicked: pane.removeFormat(formatRow.modelData.format)
                        }
                    }
                }
            }

            FlatButton {
                objectName: "addFormatButton"

                visible: pane.editable
                variant: "outline"
                text: qsTr("Add file…")
                icon.source: "assets/icons/plus.svg"
                onClicked: formatDialog.open()
            }

            FlatButton {
                objectName: "showFolderButton"

                variant: "outline"
                text: qsTr("Show in folder")
                icon.source: "assets/icons/folder.svg"
                onClicked: {
                    const error = pane.library.openFolder(pane.book.id)
                    if (error)
                        pane.failed(error)
                }
            }

            Label {
                objectName: "bookPath"

                Layout.fillWidth: true
                text: pane.book.path || ""
                color: Theme.textFaint
                font.pointSize: Theme.fontPointSizeCaption
                wrapMode: Text.WrapAnywhere
            }

            Item { Layout.preferredHeight: Theme.panePadding }
        }
    }

    FileDialog {
        id: formatDialog

        title: qsTr("Add a file to the book")
        nameFilters: [qsTr("Books (*.epub *.kepub *.azw3 *.azw *.mobi *.prc *.pdf *.fb2 *.cbz *.cbr *.djvu *.docx *.rtf *.txt)"),
                      qsTr("All files (*)")]
        onAccepted: pane.addFormat(selectedFile)
    }

    ConfirmDialog {
        id: replaceDialog
        objectName: "replaceFormatDialog"

        property url url

        message: qsTr("The file it has now goes away.")
        acceptText: qsTr("Replace")
        acceptVariant: "danger"
        onAccepted: {
            const error = pane.library.addFormat(pane.book.id, url.toString())
            if (error)
                pane.failed(error)
        }
    }

    ConfirmDialog {
        id: removeDialog
        objectName: "removeFormatDialog"

        property string format

        message: qsTr("Reliure and Calibre can restore it from the library's trash.")
        acceptText: qsTr("Move to Trash")
        acceptVariant: "danger"
        onAccepted: {
            const error = pane.library.removeFormat(pane.book.id, format)
            if (error)
                pane.failed(error)
        }
    }
}
