import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic

Item {
    id: pane

    required property var node

    Loader {
        id: contentLoader

        anchors.fill: parent
        property var node: pane.node
        sourceComponent: node ? (node.leaf ? leafComponent : splitComponent) : null
        onLoaded: item.node = node
    }

    Component {
        id: leafComponent

        Item {
            id: leafItem
            property var node: null
            property var session: node ? node.session : null
            readonly property bool active: session && session === SessionManager.activeSession

            SystemPalette {
                id: palette
                colorGroup: SystemPalette.Active
            }

            Column {
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    width: parent.width
                    height: 30
                    color: Qt.rgba(palette.window.r, palette.window.g, palette.window.b, 0.88)
                    border.width: 1
                    border.color: leafItem.active ? palette.accent : Qt.rgba(palette.windowText.r, palette.windowText.g, palette.windowText.b, 0.16)

                    TapHandler {
                        enabled: leafItem.session !== null
                        onTapped: SessionManager.activateSession(leafItem.session)
                    }

                    Row {
                        x: 4
                        y: 0
                        width: parent.width - 9
                        height: parent.height
                        spacing: 4

                        ToolButton {
                            width: 22
                            height: 24
                            padding: 3
                            text: "×"
                            enabled: leafItem.session !== null
                            Accessible.name: qsTr("Close sub-tab")
                            onClicked: SessionManager.closeSession(leafItem.session)
                        }

                        Text {
                            width: Math.max(0, parent.width - 36)
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideMiddle
                            color: palette.windowText
                            text: leafItem.session
                                  ? (leafItem.session.currentDirectory !== ""
                                     ? leafItem.session.currentDirectory : leafItem.session.title)
                                  : ""
                        }

                    }
                }

                TerminalView {
                    width: parent.width
                    height: Math.max(0, parent.height - 30)
                    session: leafItem.session
                    active: leafItem.active
                }
            }
        }
    }

    Component {
        id: splitComponent

        SplitView {
            id: splitItem
            property var node: null

            anchors.fill: parent
            orientation: node ? node.orientation : Qt.Horizontal
            handle: Rectangle {
                implicitWidth: 5
                implicitHeight: 5
                color: Qt.rgba(1, 1, 1, 0.14)
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
