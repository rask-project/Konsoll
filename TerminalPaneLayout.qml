pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic
import "controls" as Controls

Item {
    id: pane

    required property TerminalManager sessionManager
    required property var node
    required property font terminalFont
    required property int cursorShape
    required property bool cursorBlink

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

            readonly property bool active: session && session === pane.sessionManager.activeSession
            property var node: null
            property var session: node ? node.session : null

            SystemPalette {
                id: systemPalette
                colorGroup: SystemPalette.Active
            }

            Item {
                anchors.fill: parent

                Rectangle {
                    id: sessionHeader

                    width: parent.width - 10
                    height: 32
                    y: 5
                    anchors.horizontalCenter: parent.horizontalCenter

                    color: Qt.rgba(systemPalette.window.r, systemPalette.window.g, systemPalette.window.b, 0.50)

                    border.width: 1
                    border.color: leafItem.active ? systemPalette.accent : Qt.rgba(systemPalette.windowText.r, systemPalette.windowText.g, systemPalette.windowText.b, 0.15)
                    radius: height / 2

                    TapHandler {
                        enabled: leafItem.session !== null
                        onTapped: pane.sessionManager.activateSession(leafItem.session)
                    }

                    Controls.Menu {
                        id: sessionContextMenu

                        Controls.MenuItem {
                            text: qsTr("Close tab")
                            onTriggered: {
                                if (leafItem.session.commandRunning && !NativeDialogs.confirmClose())
                                    return;
                                pane.sessionManager.closeSession(leafItem.session);
                            }
                        }

                        Controls.MenuItem {
                            text: qsTr("Divide horizontal")
                            onTriggered: {
                                pane.sessionManager.activateSession(leafItem.session);
                                pane.sessionManager.splitActive(Qt.Horizontal);
                            }
                        }

                        Controls.MenuItem {
                            text: qsTr("Divide vertical")
                            onTriggered: {
                                pane.sessionManager.activateSession(leafItem.session);
                                pane.sessionManager.splitActive(Qt.Vertical);
                            }
                        }

                        Controls.MenuItem {
                            text: qsTr("Save Buffer")
                            onTriggered: {
                                pane.sessionManager.activateSession(leafItem.session);
                                pane.saveBufferRequested(leafItem.session);
                            }
                        }
                    }

                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: {
                            pane.sessionManager.activateSession(leafItem.session);
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
                                pane.sessionManager.closeSession(leafItem.session);
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
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: sessionHeader.bottom
                    anchors.topMargin: 5
                    anchors.bottom: parent.bottom

                    sessionManager: pane.sessionManager
                    session: leafItem.session
                    active: leafItem.active
                    terminalFont: pane.terminalFont
                    cursorShape: pane.cursorShape
                    cursorBlink: pane.cursorBlink
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
