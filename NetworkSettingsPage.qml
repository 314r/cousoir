pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import Cousoir

Item {
    id: networkPage

    required property LibraryShare share
    // Where the sharing's settings are kept: enabled, port and password.
    required property QtObject settings
    // The readers, whose wireless device server this page turns on.
    property DeviceLibrary devices: null

    implicitHeight: contentLayout.implicitHeight + 2 * Theme.panePadding

    ColumnLayout {
        id: contentLayout
        objectName: "networkForm"

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.panePadding
        spacing: Theme.pagePadding

        ColumnLayout {
            objectName: "networkSection"

            Layout.fillWidth: true
            spacing: Theme.gutter

            HeadingLabel {
                text: qsTr("Share the library on the local network")
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("E-readers and reading apps, KOReader, Thorium, Moon+ Reader and others, browse the library as an OPDS catalog and download its books; a browser, on a phone for instance, shows a page to search it. Nothing on the network can change the library.")
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }

            FlatButton {
                objectName: "networkShareButton"

                variant: "outline"
                selected: networkPage.share.running
                text: networkPage.settings.enabled ? qsTr("Stop sharing") : qsTr("Share the library")
                icon.source: "assets/icons/wifi.svg"
                onClicked: networkPage.settings.enabled = !networkPage.settings.enabled
            }

            Label {
                objectName: "networkError"

                Layout.fillWidth: true
                visible: networkPage.share.error !== ""
                text: networkPage.share.error
                color: Theme.danger
                wrapMode: Text.WordWrap
            }
        }

        ColumnLayout {
            objectName: "networkAddresses"

            Layout.fillWidth: true
            visible: networkPage.share.running
            spacing: Theme.gutter

            HeadingLabel {
                text: qsTr("Addresses")
            }

            Label {
                Layout.fillWidth: true
                visible: networkPage.share.urls.length === 0
                text: qsTr("This computer is on no network.")
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }

            Repeater {
                model: networkPage.share.urls

                delegate: RowLayout {
                    id: address

                    required property string modelData

                    Layout.fillWidth: true
                    spacing: Theme.pagePadding

                    // For a phone: its camera opens the web page.
                    Image {
                        objectName: "networkQrCode"

                        Layout.preferredWidth: Math.round(9 * Theme.rowHeight / 2)
                        Layout.preferredHeight: Layout.preferredWidth
                        source: networkPage.share.qrCode(address.modelData)
                        sourceSize.width: Layout.preferredWidth
                        sourceSize.height: Layout.preferredHeight
                        smooth: false
                        Accessible.name: qsTr("QR code of %1").arg(address.modelData)
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        spacing: Theme.gutter

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Web page")
                            color: Theme.textMuted
                            elide: Text.ElideRight
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gutter

                            Label {
                                Layout.fillWidth: true
                                text: address.modelData
                                elide: Text.ElideRight
                            }

                            FlatButton {
                                text: qsTr("Copy")
                                onClicked: networkPage.share.copy(address.modelData)
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("OPDS catalog, for reading apps")
                            color: Theme.textMuted
                            elide: Text.ElideRight
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gutter

                            Label {
                                Layout.fillWidth: true
                                text: address.modelData + networkPage.share.catalogPath
                                elide: Text.ElideRight
                            }

                            FlatButton {
                                text: qsTr("Copy")
                                onClicked: networkPage.share.copy(address.modelData + networkPage.share.catalogPath)
                            }
                        }
                    }
                }
            }

            FlatButton {
                text: qsTr("Look again")
                tip: qsTr("After joining another network")
                icon.source: "assets/icons/refresh.svg"
                onClicked: networkPage.share.refresh()
            }
        }

        ColumnLayout {
            objectName: "networkOptions"

            Layout.fillWidth: true
            spacing: Theme.gutter

            HeadingLabel {
                text: qsTr("Port and password")
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gutter

                Label {
                    text: qsTr("Port")
                }

                FormField {
                    objectName: "networkPortField"

                    Layout.preferredWidth: Math.round(8 * Theme.charWidth)
                    text: String(networkPage.settings.port)
                    validator: IntValidator { bottom: 1024; top: 65535 }
                    onEditingFinished: {
                        if (acceptableInput && Number(text) !== networkPage.settings.port)
                            networkPage.settings.port = Number(text)
                    }
                }

                Label {
                    text: qsTr("Password")
                }

                FormField {
                    objectName: "networkPasswordField"

                    Layout.preferredWidth: Math.round(20 * Theme.charWidth)
                    text: networkPage.settings.password
                    echoMode: TextInput.PasswordEchoOnEdit
                    placeholderText: qsTr("None")
                    onEditingFinished: {
                        if (text !== networkPage.settings.password)
                            networkPage.settings.password = text
                    }
                }

                Item { Layout.fillWidth: true }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Without a password, anyone on the network can download the books. With one, readers ask for it, with any user name. Sharing lasts while Cousoir is open; “cousoir-cli serve” shares a library without the application, on a server for instance.")
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }
        }

        ColumnLayout {
            objectName: "wirelessSection"

            Layout.fillWidth: true
            visible: networkPage.devices !== null
            spacing: Theme.gutter

            HeadingLabel {
                text: qsTr("Wireless readers")
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("KOReader, Calibre Companion and the other apps that connect to Calibre's wireless device server connect to Cousoir the same way: their books show among the readers, and books go to them with their metadata and covers. One app at a time; in the app, connect to Calibre.")
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }

            FlatButton {
                objectName: "wirelessButton"

                variant: "outline"
                selected: networkPage.devices !== null && networkPage.devices.wirelessListening
                text: networkPage.devices !== null && networkPage.devices.wirelessEnabled ? qsTr("Stop letting apps connect")
                                                                                         : qsTr("Let apps connect")
                icon.source: "assets/icons/wifi.svg"
                onClicked: networkPage.devices.setWireless(!networkPage.devices.wirelessEnabled,
                                                           networkPage.devices.wirelessPort,
                                                           networkPage.devices.wirelessPassword)
            }

            Label {
                objectName: "wirelessError"

                Layout.fillWidth: true
                visible: networkPage.devices !== null && networkPage.devices.wirelessError !== ""
                text: networkPage.devices !== null ? networkPage.devices.wirelessError : ""
                color: Theme.danger
                wrapMode: Text.WordWrap
            }

            Label {
                objectName: "wirelessAddresses"

                Layout.fillWidth: true
                visible: networkPage.devices !== null && networkPage.devices.wirelessListening
                text: networkPage.devices === null ? ""
                    : networkPage.devices.wirelessAddresses.length === 0
                        ? qsTr("This computer is on no network.")
                        : qsTr("Apps find Cousoir on the network by themselves; should one not, give it %1.")
                              .arg(networkPage.devices.wirelessAddresses.join(qsTr(" or ")))
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gutter

                Label {
                    text: qsTr("Port")
                }

                FormField {
                    objectName: "wirelessPortField"

                    Layout.preferredWidth: Math.round(8 * Theme.charWidth)
                    text: networkPage.devices !== null && networkPage.devices.wirelessPort > 0
                        ? String(networkPage.devices.wirelessPort) : ""
                    placeholderText: "9090"
                    validator: IntValidator { bottom: 1024; top: 65535 }
                    onEditingFinished: {
                        const port = text === "" ? 0 : Number(text)
                        if ((text === "" || acceptableInput) && port !== networkPage.devices.wirelessPort)
                            networkPage.devices.setWireless(networkPage.devices.wirelessEnabled, port,
                                                            networkPage.devices.wirelessPassword)
                    }
                }

                Label {
                    text: qsTr("Password")
                }

                FormField {
                    objectName: "wirelessPasswordField"

                    Layout.preferredWidth: Math.round(20 * Theme.charWidth)
                    text: networkPage.devices !== null ? networkPage.devices.wirelessPassword : ""
                    echoMode: TextInput.PasswordEchoOnEdit
                    placeholderText: qsTr("None")
                    onEditingFinished: {
                        if (text !== networkPage.devices.wirelessPassword)
                            networkPage.devices.setWireless(networkPage.devices.wirelessEnabled,
                                                            networkPage.devices.wirelessPort, text)
                    }
                }

                Item { Layout.fillWidth: true }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("With a password, the app must give the same. The port is Calibre's, 9090, or another free one when Calibre has it; set one when a firewall wants it.")
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }
        }
    }
}
