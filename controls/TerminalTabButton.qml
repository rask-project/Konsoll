import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic

TabButton {
    id: tab

    required property var session
    required property bool active

    readonly property string tabTitle: tab.session ? (tab.session.currentDirectory !== "" ? tab.session.currentDirectory : tab.session.title) : "Konsoll"
    readonly property color systemAccent: systemPalette.accent

    signal closeRequested

    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    width: active ? 250 : 150
    height: 32
    leftPadding: 10
    rightPadding: 4

    ToolTip {
        id: titleToolTip

        visible: tab.hovered && tabLabel.implicitWidth > tabLabel.width

        x: (tab.width - width) / 2
        y: tab.height + 6

        parent: tab

        delay: 600
        timeout: -1

        horizontalPadding: 12
        verticalPadding: 8

        text: tab.tabTitle
        font.pixelSize: 13

        contentItem: Text {
            text: titleToolTip.text
            color: systemPalette.windowText
            font: titleToolTip.font
        }

        background: Rectangle {
            radius: 8

            color: Qt.rgba(systemPalette.window.r, systemPalette.window.g, systemPalette.window.b, 0.85)
            border.width: 1
            border.color: Qt.rgba(systemPalette.windowText.r, systemPalette.windowText.g, systemPalette.windowText.b, Application.styleHints.colorScheme === Qt.Dark ? 0.95 : 0.15)
        }
    }

    background: Rectangle {
        radius: height / 2
        color: tab.active ? Qt.rgba(tab.systemAccent.r, tab.systemAccent.g, tab.systemAccent.b, 0.05) : Qt.rgba(systemPalette.window.r, systemPalette.window.g, systemPalette.window.b, 0.85)

        border.width: tab.active ? 1 : 0
        border.color: tab.active ? Qt.rgba(tab.systemAccent.r, tab.systemAccent.g, tab.systemAccent.b, 0.8) : "transparent"
    }

    contentItem: Row {
        spacing: 6

        ToolButton {
            id: closeButton

            Accessible.name: qsTr("Close tab")

            visible: tab.active || tab.hovered

            width: 22
            height: 22
            anchors.verticalCenter: parent.verticalCenter
            padding: 3

            icon.name: "close"
            icon.color: systemPalette.windowText
            display: AbstractButton.IconOnly

            background: Rectangle {
                radius: width / 2
                color: closeButton.hovered ? Qt.rgba(systemPalette.windowText.r, systemPalette.windowText.g, systemPalette.windowText.b, 0.10) : "transparent"
            }

            onClicked: tab.closeRequested()
        }

        Text {
            id: tabLabel

            width: Math.max(0, tab.width - tab.leftPadding - tab.rightPadding - closeButton.width - parent.spacing)
            height: parent.height

            text: tab.tabTitle
            color: systemPalette.windowText

            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }
}
