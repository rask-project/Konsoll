import QtQuick
import QtQuick.Controls.macOS

MenuItem {
    id: control

    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    background: Rectangle {
        implicitWidth: 200
        implicitHeight: 25
        radius: 8

        color: systemPalette.accent
        opacity: 0.7
        visible: control.down || control.highlighted
    }
}
