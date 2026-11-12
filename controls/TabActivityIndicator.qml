import QtQuick
import QtQuick.Shapes

// A small ring for the tab: a determinate progress arc (OSC 9 ; 4 states 1,
// 2 and 4) or a spinning partial arc (indeterminate progress, a busy pane).
Item {
    id: indicator

    // 0 - 100; ignored while `spinning`.
    property int value: 0
    property bool spinning: false
    property color color: "white"
    property color trackColor: Qt.rgba(color.r, color.g, color.b, 0.2)
    property real lineWidth: 2

    readonly property real radius: (Math.min(width, height) - lineWidth) / 2

    implicitWidth: 14
    implicitHeight: 14

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: "transparent"
            strokeColor: indicator.trackColor
            strokeWidth: indicator.lineWidth

            PathAngleArc {
                centerX: indicator.width / 2
                centerY: indicator.height / 2
                radiusX: indicator.radius
                radiusY: indicator.radius
                startAngle: 0
                sweepAngle: 360
            }
        }
    }

    Shape {
        id: arc

        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: "transparent"
            strokeColor: indicator.color
            strokeWidth: indicator.lineWidth
            capStyle: ShapePath.RoundCap

            PathAngleArc {
                centerX: indicator.width / 2
                centerY: indicator.height / 2
                radiusX: indicator.radius
                radiusY: indicator.radius
                startAngle: -90
                sweepAngle: indicator.spinning ? 100 : 360 * Math.max(0, Math.min(100, indicator.value)) / 100

                Behavior on sweepAngle {
                    enabled: !indicator.spinning
                    NumberAnimation {
                        duration: 200
                        easing.type: Easing.OutCubic
                    }
                }
            }
        }
    }

    // Spin in 45° steps, eight a second, rather than with a per-frame
    // animator: every step makes the whole window render, so a smooth spin
    // cost ~120 renders a second for as long as a job ran. In a window that
    // is not active the arc stands still (still a busy mark) and costs no
    // renders at all.
    Timer {
        running: indicator.spinning && indicator.visible && indicator.Window.active
        interval: 125
        repeat: true
        onTriggered: arc.rotation = (arc.rotation + 45) % 360
    }

    onSpinningChanged: if (!spinning)
        arc.rotation = 0
}
