import QtQuick
import QtQuick.Controls.macOS

MenuItem {
    id: control

    background: Rectangle {
        implicitWidth: 200
        implicitHeight: 25
        radius: 8

        color: control.palette.accent
        opacity: 0.7
        visible: control.down || control.highlighted
    }
}
