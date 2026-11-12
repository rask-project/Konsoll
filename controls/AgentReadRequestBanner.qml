pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts

// In an agent's pane: the agent asks to read another pane (its MCP tool
// read_pane). Nothing is read until the user allows it; the notification's
// Allow / Deny answer the same request.
Rectangle {
    id: root

    property var session: null

    visible: session !== null && session.readRequest !== ""

    width: Math.min(460, parent ? parent.width - 24 : 460)
    implicitHeight: content.implicitHeight + 24
    radius: 8

    color: Qt.rgba(appPalette.window.r, appPalette.window.g, appPalette.window.b, 0.98)
    border.color: appPalette.mid
    layer.enabled: true

    layer.effect: MultiEffect {
        shadowEnabled: true
        shadowColor: appPalette.shadow
        shadowOpacity: 0.4
        shadowBlur: 0.35
        blurMax: 16
        shadowVerticalOffset: 3
    }

    SystemPalette {
        id: appPalette
        colorGroup: SystemPalette.Active
    }

    ColumnLayout {
        id: content

        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Label {
            Layout.fillWidth: true
            text: root.session ? root.session.readRequest : ""
            font.bold: true
            color: appPalette.windowText
            wrapMode: Text.WordWrap
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("It would see what that pane shows, which may include secrets printed there.")
            color: appPalette.windowText
            opacity: 0.75
            font.pointSize: 11
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 8

            Button {
                text: qsTr("Deny")
                onClicked: root.session.answerReadRequest(false)
            }

            Button {
                text: qsTr("Allow Once")
                highlighted: true
                onClicked: root.session.answerReadRequest(true)
            }
        }
    }
}
