pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// The books selected, edited together as Calibre's bulk metadata edit edits
// them: each field filled sets that value for every book, an empty one
// keeps each book's own, and × clears it for all. Tags are added and
// removed rather than set; a series numbers its books in the list's order,
// from a number or after the series' last one. Apply saves it all in one
// change, which Undo puts back.
FocusScope {
    id: pane

    required property BookLibrary library
    // The books, in the list's order.
    property var bookIds: []
    readonly property bool editable: library.writable && bookIds.length > 1
    readonly property var ratingChoices: [{ value: -1, label: qsTr("Keep") }].concat(
        [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10].map(rating => ({
            value: rating,
            label: rating === 0 ? qsTr("None") : "★".repeat(Math.floor(rating / 2)) + (rating % 2 ? "½" : "")
        })))

    signal failed(string message)

    // The changes the fields hold, as updateBooks() takes them.
    function changes() {
        const result = {}
        const text = field => field.text.trim()
        if (text(authorsField))
            result.authors = text(authorsField)
        if (seriesRow.cleared)
            result.series = ""
        else if (text(seriesField)) {
            result.series = text(seriesField)
            if (numberingBox.currentValue !== "none")
                result.numberSeries = true
            if (numberingBox.currentValue === "from")
                result.seriesStart = Number(text(startField).replace(",", "."))
        }
        if (removeAllRow.cleared)
            result.removeAllTags = true
        if (text(addTagsField))
            result.addTags = text(addTagsField)
        if (text(removeTagsField) && !removeAllRow.cleared)
            result.removeTags = text(removeTagsField)
        if (ratingBox.currentValue >= 0)
            result.rating = ratingBox.currentValue
        if (publisherRow.cleared)
            result.publisher = ""
        else if (text(publisherField))
            result.publisher = text(publisherField)
        if (languagesRow.cleared)
            result.languages = ""
        else if (text(languagesField))
            result.languages = text(languagesField)
        if (pubdateRow.cleared)
            result.pubdate = ""
        else if (text(pubdateField))
            result.pubdate = text(pubdateField)
        return result
    }

    function reset() {
        for (const field of [authorsField, seriesField, addTagsField, removeTagsField, publisherField,
                             languagesField, pubdateField])
            field.text = ""
        for (const row of [seriesRow, removeAllRow, publisherRow, languagesRow, pubdateRow])
            row.cleared = false
        numberingBox.currentIndex = 0
        startField.text = "1"
        ratingBox.currentIndex = 0
    }

    function apply() {
        const values = pane.changes()
        if (Object.keys(values).length === 0)
            return
        const error = pane.library.updateBooks(pane.bookIds, values)
        if (error)
            pane.failed(error)
        else
            pane.reset()
    }

    // What was typed for other books is not kept for the next ones.
    onVisibleChanged: {
        if (!visible)
            reset()
    }

    readonly property bool valid: (!pubdateField.text.trim() || pubdateRow.cleared || library.isDate(pubdateField.text))
        && (numberingBox.currentValue !== "from" || (startField.text.trim().length > 0
            && isFinite(Number(startField.text.trim().replace(",", ".")))
            && Number(startField.text.trim().replace(",", ".")) >= 0))

    // A label, its field, and × to clear the value for every book when it
    // can be.
    component BulkRow: RowLayout {
        id: row

        property string label
        property bool clearable: false
        property bool cleared: false
        default property alias content: slot.data

        Layout.fillWidth: true
        spacing: Theme.rowPadding

        Label {
            Layout.preferredWidth: Math.round(11 * Theme.charWidth)
            Layout.minimumWidth: Layout.preferredWidth
            text: row.label
            color: Theme.textMuted
            elide: Text.ElideRight
        }

        Item {
            id: slot

            Layout.fillWidth: true
            implicitHeight: Theme.rowHeight
        }

        FlatButton {
            objectName: "clear_" + row.label
            visible: row.clearable
            iconOnly: true
            selected: row.cleared
            enabled: pane.editable
            text: row.cleared ? qsTr("Keep %1").arg(row.label.toLowerCase())
                              : qsTr("Clear %1 for all").arg(row.label.toLowerCase())
            icon.source: "assets/icons/close.svg"
            icon.color: row.cleared ? Theme.danger : Theme.textMuted
            onClicked: row.cleared = !row.cleared
        }
    }

    component BulkField: FormField {
        property bool cleared: false

        anchors.fill: parent
        enabled: pane.editable && !cleared
        placeholderText: cleared ? qsTr("Cleared for all") : qsTr("Keep")
        Keys.onReturnPressed: pane.apply()
        Keys.onEnterPressed: pane.apply()
    }

    ScrollView {
        id: paneScroll

        anchors.fill: parent
        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            x: Theme.panePadding
            width: paneScroll.availableWidth - 2 * Theme.panePadding
            spacing: Math.round(6 * Theme.spacingScale)

            Item { Layout.preferredHeight: Theme.pagePadding - Theme.rowPadding }

            HeadingLabel {
                objectName: "bulkTitle"

                Layout.fillWidth: true
                text: qsTr("%1 books").arg(pane.bookIds.length)
            }

            Label {
                Layout.fillWidth: true
                Layout.bottomMargin: Theme.gutter
                text: pane.library.writable
                    ? qsTr("A value filled in goes to every book; one left empty keeps each book's own.")
                    : qsTr("The library cannot be changed right now.")
                color: Theme.textMuted
                font.pointSize: Theme.fontPointSizeCaption
                wrapMode: Text.Wrap
            }

            BulkRow {
                label: qsTr("Authors")

                BulkField {
                    id: authorsField
                    objectName: "bulk_authors"
                }
            }

            BulkRow {
                id: seriesRow

                label: qsTr("Series")
                clearable: true

                BulkField {
                    id: seriesField
                    objectName: "bulk_series"
                    cleared: seriesRow.cleared
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Math.round(11 * Theme.charWidth) + Theme.rowPadding
                visible: seriesField.text.trim().length > 0 && !seriesRow.cleared
                spacing: Theme.rowPadding

                // As Calibre's bulk edit numbers a series: each book at 1,
                // from a number in the list's order, or after its last.
                ChoiceBox {
                    id: numberingBox
                    objectName: "bulk_numbering"

                    Layout.fillWidth: true
                    model: [
                        { value: "none", label: qsTr("Each at 1") },
                        { value: "from", label: qsTr("Numbered from") },
                        { value: "after", label: qsTr("After its last") }
                    ]
                    enabled: pane.editable
                    Accessible.name: qsTr("Numbering")
                }

                FormField {
                    id: startField
                    objectName: "bulk_seriesStart"

                    Layout.preferredWidth: Math.round(7 * Theme.charWidth)
                    visible: numberingBox.currentValue === "from"
                    text: "1"
                    horizontalAlignment: TextInput.AlignRight
                    enabled: pane.editable
                    invalid: !pane.valid
                }

            }

            BulkRow {
                label: qsTr("Add tags")

                BulkField {
                    id: addTagsField
                    objectName: "bulk_addTags"
                    placeholderText: qsTr("tag, tag")
                }
            }

            BulkRow {
                id: removeAllRow

                label: qsTr("Remove tags")
                clearable: true

                BulkField {
                    id: removeTagsField
                    objectName: "bulk_removeTags"
                    cleared: removeAllRow.cleared
                    placeholderText: removeAllRow.cleared ? qsTr("Every tag") : qsTr("tag, tag")
                }
            }

            BulkRow {
                label: qsTr("Rating")

                ChoiceBox {
                    id: ratingBox
                    objectName: "bulk_rating"

                    width: Math.round(14 * Theme.charWidth)
                    height: parent.height
                    model: pane.ratingChoices
                    enabled: pane.editable
                    Accessible.name: qsTr("Rating")
                }
            }

            BulkRow {
                id: publisherRow

                label: qsTr("Publisher")
                clearable: true

                BulkField {
                    id: publisherField
                    objectName: "bulk_publisher"
                    cleared: publisherRow.cleared
                }
            }

            BulkRow {
                id: languagesRow

                label: qsTr("Languages")
                clearable: true

                BulkField {
                    id: languagesField
                    objectName: "bulk_languages"
                    cleared: languagesRow.cleared
                }
            }

            BulkRow {
                id: pubdateRow

                label: qsTr("Published")
                clearable: true

                BulkField {
                    id: pubdateField
                    objectName: "bulk_pubdate"
                    cleared: pubdateRow.cleared
                    placeholderText: cleared ? qsTr("Cleared for all") : qsTr("Keep, or YYYY-MM-DD")
                    invalid: !pubdateRow.cleared && text.trim().length > 0 && !pane.library.isDate(text)
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.gutter
                spacing: Theme.gutter

                FlatButton {
                    objectName: "bulkApplyButton"

                    variant: "primary"
                    text: qsTr("Apply to %1 books").arg(pane.bookIds.length)
                    enabled: pane.editable && pane.valid && Object.keys(pane.changes()).length > 0
                    onClicked: pane.apply()
                }

                FlatButton {
                    objectName: "bulkUndoButton"

                    visible: pane.library.bulkUndoCount > 0
                    enabled: pane.library.writable && !pane.library.saving
                    variant: "outline"
                    text: qsTr("Undo")
                    tip: qsTr("Put the %1 books of the last change back as they were").arg(pane.library.bulkUndoCount)
                    onClicked: pane.library.undoBulkEdit()
                }

                Item { Layout.fillWidth: true }
            }

            Item { Layout.preferredHeight: Theme.pagePadding }
        }
    }
}
