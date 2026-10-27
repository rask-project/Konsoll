import QtQuick
import QtQuick.Controls.macOS
import QtQuick.Effects

Menu {
    id: control

    leftPadding: 6
    topPadding: 6
    rightPadding: 6
    bottomPadding: 6

    background: Item {
        implicitWidth: 200 - control.leftInset - control.rightInset
        implicitHeight: 20 - control.topInset - control.bottomInset

        MultiEffect {
            x: -control.leftInset
            y: -control.topInset

            width: source.width
            height: source.height

            source: Rectangle {
                visible: false

                width: control.background.width + control.leftInset + control.rightInset
                height: control.background.height + control.topInset + control.bottomInset

                radius: 16
                color: Application.styleHints.colorScheme === Qt.Light ? Qt.darker(control.palette.window, 1.04) : Qt.darker(control.palette.window, 1.2)
                border.color: Application.styleHints.colorScheme === Qt.Light ? Qt.darker(control.palette.window, 1.4) : Qt.lighter(control.palette.window, 2.0)
                border.width: 1
            }

            shadowScale: 1
            shadowOpacity: Application.styleHints.colorScheme === Qt.Light ? 0.15 : 0.2
            shadowColor: 'black'
            shadowEnabled: true
            shadowHorizontalOffset: 0
            shadowVerticalOffset: 6
        }
    }
}
