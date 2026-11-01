import QtQuick
import QtQuick.Controls.Basic as Basic

Basic.ToolButton {
    id: control

    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    background: Rectangle {
        radius: Math.min(width, height) / 2
        color: control.hovered && control.enabled ? Qt.rgba(systemPalette.windowText.r, systemPalette.windowText.g, systemPalette.windowText.b, control.down ? 0.18 : 0.12) : "transparent"
    }
}
