pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

// Cousoir's version, its credits, Calibre's first, and its license.
Item {
    id: aboutPage

    implicitHeight: contentLayout.implicitHeight + 2 * Theme.panePadding

    ColumnLayout {
        id: contentLayout
        objectName: "aboutForm"

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.panePadding
        spacing: Theme.pagePadding

        ColumnLayout {
            objectName: "cousoirSection"

            Layout.fillWidth: true
            spacing: Theme.gutter

            HeadingLabel {
                text: qsTr("Cousoir")
            }

            Label {
                objectName: "versionLabel"

                Layout.fillWidth: true
                text: qsTr("Version %1").arg(Qt.application.version)
                font.weight: Font.Bold
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("An ebook manager for Calibre libraries.")
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }
        }

        ColumnLayout {
            objectName: "calibreCreditSection"

            Layout.fillWidth: true
            spacing: Theme.gutter

            HeadingLabel {
                text: qsTr("Built on Calibre")
            }

            Label {
                objectName: "calibreCreditLabel"

                Layout.fillWidth: true
                text: qsTr("Cousoir would not exist without Calibre, the free and open-source ebook manager created by Kovid Goyal and built with the Calibre contributors. Much of Cousoir is ported from Calibre's own code: its title and author sorts, its folder and file names, the way it writes a library, its metadata readers and the way it finds books on a reader, from the Kobo driver of Timothy Legge, Kovid Goyal and David Forrester. And Calibre itself converts books and sends them to readers.")
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Cousoir is an independent project, not made, endorsed or supported by the Calibre project.")
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }

            Flow {
                Layout.fillWidth: true
                spacing: Math.round(6 * Theme.spacingScale)

                FlatButton {
                    objectName: "calibreWebsiteButton"

                    variant: "outline"
                    text: qsTr("calibre-ebook.com")
                    icon.source: "assets/icons/external.svg"
                    onClicked: Qt.openUrlExternally("https://calibre-ebook.com")
                }

                FlatButton {
                    objectName: "supportCalibreButton"

                    variant: "outline"
                    text: qsTr("Support Calibre")
                    icon.source: "assets/icons/external.svg"
                    onClicked: Qt.openUrlExternally("https://calibre-ebook.com/donate")
                }
            }
        }

        ColumnLayout {
            objectName: "thanksSection"

            Layout.fillWidth: true
            spacing: Theme.gutter

            HeadingLabel {
                text: qsTr("Thanks also to")
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Hiroshi Miura and John Schember for Unihandecode, and Sean M. Burke for Text::Unidecode, which it grew from; kbextract and JolieBulle, where Cousoir's structure, theming and settings come from; Omarchy for the look; Lucide for a few icons; and Qt, SQLite, libzip, libmtp and poppler.")
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }
        }

        ColumnLayout {
            objectName: "licenseSection"

            Layout.fillWidth: true
            spacing: Theme.gutter

            HeadingLabel {
                text: qsTr("License")
            }

            Label {
                objectName: "licenseLabel"

                Layout.fillWidth: true
                text: qsTr("Cousoir is free software: you can redistribute it and modify it under the terms of the GNU General Public License, version 3, as it contains code ported from Calibre. It comes with absolutely no warranty.")
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("The theme files from kbextract and JolieBulle also keep their MIT notice, and the icons adapted from Lucide their ISC notice.")
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }

            Flow {
                Layout.fillWidth: true
                spacing: Math.round(6 * Theme.spacingScale)

                FlatButton {
                    objectName: "licenseButton"

                    variant: "outline"
                    text: qsTr("GNU GPL v3")
                    icon.source: "assets/icons/external.svg"
                    onClicked: Qt.openUrlExternally("https://www.gnu.org/licenses/gpl-3.0.html")
                }

                FlatButton {
                    objectName: "sourceCodeButton"

                    variant: "outline"
                    text: qsTr("Source code")
                    icon.source: "assets/icons/external.svg"
                    onClicked: Qt.openUrlExternally("https://github.com/314r/cousoir")
                }
            }
        }
    }
}
