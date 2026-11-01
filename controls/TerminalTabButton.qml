import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic
import "." as Controls

TabButton {
    id: tab

    required property var workspace
    required property bool active

    property bool dragging: false
    property bool dragged: false
    property bool pointerOverTab: false
    property var windowTranslucency: null
    property var tabStrip: null

    readonly property string tabTitle: tab.workspace ? (tab.workspace.title || "Konsoll") : "Konsoll"
    readonly property color systemAccent: systemPalette.accent

    signal closeRequested
    signal dragStarted(real sceneX, real sceneY)
    signal dragMoved(real sceneX, real sceneY)
    signal dragFinished(real sceneX, real sceneY)
    signal dragCanceled

    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    HoverHandler {
        onHoveredChanged: {
            tab.pointerOverTab = hovered;
            if (tab.tabStrip)
                tab.tabStrip.updateWindowMovable();
        }
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

        Controls.ToolButton {
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

            MouseArea {
                id: dragArea

                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                preventStealing: true

                property real pressSceneX: 0
                property real pressSceneY: 0
                property bool movedBeforeHold: false
                property bool holdActivated: false

                onPressed: mouse => {
                    const point = dragArea.mapToItem(null, mouse.x, mouse.y);
                    pressSceneX = point.x;
                    pressSceneY = point.y;
                    movedBeforeHold = false;
                    holdActivated = false;
                    if (tab.tabStrip) {
                        tab.tabStrip.tabPointerPressed = true;
                        tab.tabStrip.updateWindowMovable();
                    } else if (tab.windowTranslucency) {
                        tab.windowTranslucency.setWindowMovable(false);
                    }
                    mouse.accepted = true;
                }

                onPressAndHold: mouse => {
                    if (!pressed || movedBeforeHold)
                        return;
                    const point = dragArea.mapToItem(null, mouse.x, mouse.y);
                    holdActivated = true;
                    tab.dragging = true;
                    tab.dragged = true;
                    tab.dragStarted(pressSceneX, pressSceneY);
                    tab.dragMoved(point.x, point.y);
                }

                onPositionChanged: mouse => {
                    if (!pressed)
                        return;
                    const point = dragArea.mapToItem(null, mouse.x, mouse.y);
                    const dx = point.x - pressSceneX;
                    const dy = point.y - pressSceneY;
                    if (!holdActivated) {
                        if (Math.hypot(dx, dy) >= Application.styleHints.startDragDistance)
                            movedBeforeHold = true;
                        return;
                    }
                    tab.dragMoved(point.x, point.y);
                }

                onReleased: mouse => {
                    if (holdActivated) {
                        const point = dragArea.mapToItem(null, mouse.x, mouse.y);
                        tab.dragFinished(point.x, point.y);
                        tab.dragging = false;
                        Qt.callLater(() => tab.dragged = false);
                    } else if (!movedBeforeHold) {
                        tab.click();
                    }
                    holdActivated = false;
                    movedBeforeHold = false;
                    if (tab.tabStrip) {
                        tab.tabStrip.tabPointerPressed = false;
                        tab.tabStrip.updateWindowMovable();
                    } else if (tab.windowTranslucency) {
                        tab.windowTranslucency.setWindowMovable(true);
                    }
                    mouse.accepted = true;
                }

                onCanceled: {
                    if (holdActivated)
                        tab.dragCanceled();
                    tab.dragging = false;
                    holdActivated = false;
                    movedBeforeHold = false;
                    if (tab.tabStrip) {
                        tab.tabStrip.tabPointerPressed = false;
                        tab.tabStrip.updateWindowMovable();
                    } else if (tab.windowTranslucency) {
                        tab.windowTranslucency.setWindowMovable(true);
                    }
                }
            }
        }
    }
}
