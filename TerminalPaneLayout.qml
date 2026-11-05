pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic
import "controls" as Controls

Item {
    id: pane

    required property var sessionManager
    required property var node
    required property font terminalFont

    signal saveBufferRequested(var session)

    Loader {
        id: contentLoader

        property var node: pane.node

        anchors.fill: parent
        sourceComponent: node ? (node.leaf ? leafComponent : splitComponent) : null
        onLoaded: item.node = node
    }

    Component {
        id: leafComponent

        Item {
            id: leafItem
            property var node: null
            property var session: node ? node.session : null
            readonly property bool active: session && session === sessionManager.activeSession

            SystemPalette {
                id: systemPalette
                colorGroup: SystemPalette.Active
            }

            Column {
                anchors.fill: parent
                spacing: 5
                topPadding: 5

                Rectangle {
                    width: parent.width - 10
                    height: 32
                    anchors.horizontalCenter: parent.horizontalCenter

                    color: Qt.rgba(systemPalette.window.r, systemPalette.window.g, systemPalette.window.b, 0.50)

                    border.width: 1
                    border.color: leafItem.active ? systemPalette.accent : Qt.rgba(systemPalette.windowText.r, systemPalette.windowText.g, systemPalette.windowText.b, 0.15)
                    radius: 8

                    TapHandler {
                        enabled: leafItem.session !== null
                        onTapped: sessionManager.activateSession(leafItem.session)
                    }

                    Controls.Menu {
                        id: sessionContextMenu

                        Controls.MenuItem {
                            text: qsTr("Close tab")
                            onTriggered: {
                                if (leafItem.session.commandRunning && !NativeDialogs.confirmClose())
                                    return;
                                sessionManager.closeSession(leafItem.session);
                            }
                        }

                        Controls.MenuItem {
                            text: qsTr("Divide horizontal")
                            onTriggered: {
                                sessionManager.activateSession(leafItem.session);
                                sessionManager.splitActive(Qt.Horizontal);
                            }
                        }

                        Controls.MenuItem {
                            text: qsTr("Divide vertical")
                            onTriggered: {
                                sessionManager.activateSession(leafItem.session);
                                sessionManager.splitActive(Qt.Vertical);
                            }
                        }

                        Controls.MenuItem {
                            text: qsTr("Save Buffer")
                            onTriggered: {
                                sessionManager.activateSession(leafItem.session);
                                pane.saveBufferRequested(leafItem.session);
                            }
                        }
                    }

                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: {
                            sessionManager.activateSession(leafItem.session);
                            sessionContextMenu.popup();
                        }
                    }

                    Row {
                        x: 4
                        y: 0

                        width: parent.width - 9
                        height: parent.height
                        spacing: 4

                        Controls.ToolButton {
                            enabled: leafItem.session !== null

                            Accessible.name: qsTr("Close sub-tab")

                            width: 28
                            height: 28
                            anchors.verticalCenter: parent.verticalCenter

                            icon.name: "close"
                            onClicked: {
                                if (leafItem.session.commandRunning && !NativeDialogs.confirmClose()) {
                                    return;
                                }
                                sessionManager.closeSession(leafItem.session);
                            }
                        }

                        Text {
                            width: Math.max(0, parent.width - 36)
                            height: parent.height

                            text: leafItem.session ? (leafItem.session.currentDirectory !== "" ? leafItem.session.currentDirectory : leafItem.session.title) : ""
                            color: systemPalette.windowText

                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideMiddle
                        }
                    }
                }

                Controls.TerminalView {
                    width: parent.width
                    height: Math.max(0, parent.height - 42)

                    sessionManager: sessionManager
                    session: leafItem.session
                    active: leafItem.active
                    terminalFont: pane.terminalFont
                }
            }
        }
    }

    Component {
        id: splitComponent

        SplitView {
            id: splitItem
            property var node: null

            SystemPalette {
                id: dividerPalette
                colorGroup: SystemPalette.Active
            }

            anchors.fill: parent
            orientation: node ? node.orientation : Qt.Horizontal

            handle: Item {
                z: 10
                implicitWidth: 2
                implicitHeight: 2

                Rectangle {
                    x: splitItem.orientation === Qt.Horizontal ? Math.floor(parent.width / 2) - 1 : 0
                    y: splitItem.orientation === Qt.Vertical ? Math.floor(parent.height / 2) - 1 : 0
                    width: splitItem.orientation === Qt.Horizontal ? 1 : parent.width
                    height: splitItem.orientation === Qt.Vertical ? 1 : parent.height
                    color: Qt.rgba(dividerPalette.dark.r, dividerPalette.dark.g, dividerPalette.dark.b, 0.15)
                }

                Rectangle {
                    x: splitItem.orientation === Qt.Horizontal ? Math.floor(parent.width / 2) : 0
                    y: splitItem.orientation === Qt.Vertical ? Math.floor(parent.height / 2) : 0
                    width: splitItem.orientation === Qt.Horizontal ? 1 : parent.width
                    height: splitItem.orientation === Qt.Vertical ? 1 : parent.height
                    color: Qt.rgba(dividerPalette.midlight.r, dividerPalette.midlight.g, dividerPalette.midlight.b, 0.75)
                }
            }

            Loader {
                id: firstLoader

                property var node: splitItem.node ? splitItem.node.first : null

                sourceComponent: node ? (node.leaf ? leafComponent : splitComponent) : null
                onLoaded: item.node = node

                SplitView.minimumWidth: 120
                SplitView.minimumHeight: 80
                SplitView.preferredWidth: splitItem.width / 2
                SplitView.preferredHeight: splitItem.height / 2
            }

            Loader {
                id: secondLoader

                property var node: splitItem.node ? splitItem.node.second : null

                sourceComponent: node ? (node.leaf ? leafComponent : splitComponent) : null
                onLoaded: item.node = node

                SplitView.minimumWidth: 120
                SplitView.minimumHeight: 80
                SplitView.fillWidth: true
                SplitView.fillHeight: true
            }
        }
    }
}
