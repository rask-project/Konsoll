import QtQuick
import QtQuick.Shapes

// A small ring for the tab: a determinate progress arc (OSC 9 ; 4 states 1,
// 2 and 4). Nothing in it animates but the arc's growth: a busy tab shows a
// static icon instead of a spinner, so it never makes the window render.
Item {
    id: indicator

    // 0 - 100.
    property int value: 0
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
                sweepAngle: 360 * Math.max(0, Math.min(100, indicator.value)) / 100

                Behavior on sweepAngle {
                    NumberAnimation {
                        duration: 200
                        easing.type: Easing.OutCubic
                    }
                }
            }
        }
    }
}
