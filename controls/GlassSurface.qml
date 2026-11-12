import QtQuick

// A glass-looking surface drawn in QML, no native view and nothing animated:
// a translucent body brighter at the top, an optional tint, a soft sheen
// over the upper half and a thin light rim, kept faint like a native glass
// pill. It reads as glass over the window's own translucency and blur.
Item {
    id: glass

    property real radius: height / 2
    // Colored wash over the body (the accent for the active tab).
    property color tint: "transparent"
    property real tintStrength: 0
    property color borderColor: glass.dark ? Qt.rgba(1, 1, 1, 0.5) : Qt.rgba(0, 0, 0, 0.05)
    property real borderWidth: 1
    // How opaque the body is; 1 is the default glass.
    property real density: 1

    readonly property bool dark: Application.styleHints.colorScheme === Qt.Dark

    // Body: a white veil, denser at the top like light catching a pane.
    Rectangle {
        radius: glass.radius

        gradient: Gradient {
            GradientStop {
                position: 0
                color: Qt.rgba(1, 1, 1, (glass.dark ? 0.055 : 0.22) * glass.density)
            }

            GradientStop {
                position: 0.5
                color: Qt.rgba(1, 1, 1, (glass.dark ? 0.075 : 0.32) * glass.density)
            }

            GradientStop {
                position: 1
                color: Qt.rgba(1, 1, 1, (glass.dark ? 0.025 : 0.12) * glass.density)
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: glass.radius
        visible: glass.tintStrength > 0
        color: Qt.rgba(glass.tint.r, glass.tint.g, glass.tint.b, glass.tintStrength)
    }

    // Sheen: a highlight fading out over the upper half.
    Rectangle {
        x: glass.borderWidth
        y: glass.borderWidth
        width: parent.width - 2 * glass.borderWidth
        height: parent.height * 0.5
        radius: Math.max(0, glass.radius - glass.borderWidth)
        gradient: Gradient {
            GradientStop {
                position: 0
                color: Qt.rgba(1, 1, 1, glass.dark ? 0.035 : 0.18)
            }
            GradientStop {
                position: 1
                color: Qt.rgba(1, 1, 1, 0)
            }
        }
    }

    // Specular line along the top edge, where the rim catches the light.
    Rectangle {
        visible: glass.borderWidth > 0

        x: Math.min(glass.radius * 0.7, glass.width / 2)
        y: glass.borderWidth

        width: Math.max(0, glass.width - 2 * x)
        height: 1

        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop {
                position: 0
                color: Qt.rgba(1, 1, 1, 0)
            }
            GradientStop {
                position: 0.5
                color: Qt.rgba(1, 1, 1, glass.dark ? 0.16 : 0.6)
            }
            GradientStop {
                position: 1
                color: Qt.rgba(1, 1, 1, 0)
            }
        }
    }

    // Rim.
    Rectangle {
        anchors.fill: parent
        radius: glass.radius
        color: "transparent"
        border.width: glass.borderWidth
        border.color: glass.borderColor
    }
}
