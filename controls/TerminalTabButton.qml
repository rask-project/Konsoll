import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import "." as Controls

TabButton {
    id: tab

    required property TerminalManager sessionManager
    required property var workspace
    required property bool active

    property bool dragging: false
    property bool dragged: false
    property bool pointerOverTab: false
    property var windowTranslucency: null
    property var tabStrip: null

    // The TerminalWindow showing this tab (its manager's window).
    readonly property var hostWindow: sessionManager ? sessionManager.window : null
    readonly property string tabTitle: tab.workspace ? (tab.workspace.title || "Konsoll") : "Konsoll"
    readonly property color systemAccent: systemPalette.accent

    // Tab indicators (TerminalWorkspaceModel): the leading slot shows OSC 9;4
    // progress, else the kind of program running, else a spinner while a job
    // runs, else a plain shell icon at the prompt; the trailing badge says
    // why a background tab wants a look.
    readonly property int progressState: tab.workspace ? tab.workspace.progressState : 0
    readonly property int progressValue: tab.workspace ? tab.workspace.progressValue : 0
    readonly property string processKind: tab.workspace ? tab.workspace.processKind : ""
    readonly property bool busy: tab.workspace ? tab.workspace.busy : false
    readonly property string attention: tab.workspace ? tab.workspace.attention : ""
    // Quick commands finish before the spinner would flash.
    property bool spinnerDue: false
    readonly property string leadingMode: tab.progressState > 0 ? "progress" : (tab.processKind !== "" ? "kind" : (tab.busy && tab.spinnerDue ? "spinner" : "shell"))

    readonly property color successColor: "#30d158"
    readonly property color failureColor: "#ff453a"
    readonly property color warningColor: "#ffb020"
    readonly property var kindNames: ({
            remote: qsTr("Remote session"),
            editor: qsTr("Editor"),
            monitor: qsTr("System monitor"),
            container: qsTr("Containers"),
            repl: qsTr("Interactive interpreter"),
            database: qsTr("Database client"),
            privileged: qsTr("Elevated privileges"),
            pager: qsTr("Pager"),
            git: qsTr("Git"),
            build: qsTr("Build"),
            multiplexer: qsTr("Terminal multiplexer")
        })
    readonly property var attentionNames: ({
            activity: qsTr("New output"),
            success: qsTr("Command finished"),
            failure: qsTr("Command failed"),
            bell: qsTr("Bell")
        })

    onBusyChanged: if (!busy)
        spinnerDue = false

    Timer {
        interval: 400
        running: tab.busy && !tab.spinnerDue
        onTriggered: tab.spinnerDue = true
    }

    signal closeRequested
    signal dragStarted(real sceneX, real sceneY)
    signal dragMoved(real sceneX, real sceneY)
    signal dragFinished(real sceneX, real sceneY)
    signal dragCanceled

    // Deferred: the move rebuilds this window's tabs, destroying this tab and
    // its menu while the triggered handler is still running.
    function moveToWindow(target, index) {
        const workspace = tab.workspace;
        const source = tab.hostWindow;
        Qt.callLater(() => source.windowManager.moveWorkspace(workspace, source, target, index));
    }

    Controls.Menu {
        id: contextMenu

        Controls.MenuItem {
            text: qsTr("New workspace")
            onTriggered: tab.sessionManager.addWorkspace()
        }

        Controls.MenuItem {
            text: qsTr("Rename workspace")
            onTriggered: {
                const result = NativeDialogs.promptWorkspaceRename(tab.workspace.customTitle);
                if (result.accepted)
                    sessionManager.renameWorkspace(tab.workspace, result.title);
            }
        }

        Controls.MenuItem {
            text: qsTr("Close workspace")
            onTriggered: {
                if (sessionManager.workspaceNeedsCloseConfirmation(tab.workspace) && !NativeDialogs.confirmClose())
                    return;
                sessionManager.closeWorkspace(tab.workspace);
            }
        }

        MenuSeparator {}

        Controls.MenuItem {
            text: qsTr("Move to new window")
            // A lone workspace already is its own window.
            enabled: tab.hostWindow !== null && tab.sessionManager.workspaces.length > 1
            onTriggered: tab.moveToWindow(null, 0)
        }

        Controls.Menu {
            id: moveMenu

            title: qsTr("Move to window")
            enabled: count > 0

            // Every other window, listed by name like the Window menu.
            Instantiator {
                model: tab.hostWindow ? tab.hostWindow.windowManager.windows.filter(w => w !== tab.hostWindow) : []

                delegate: Controls.MenuItem {
                    required property var modelData

                    text: modelData.title
                    onTriggered: tab.moveToWindow(modelData, -1)
                }

                onObjectAdded: (index, object) => moveMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => moveMenu.removeItem(object)
            }
        }
    }

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

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: contextMenu.popup()
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
        color: tab.active ? Qt.rgba(tab.systemAccent.r, tab.systemAccent.g, tab.systemAccent.b, 0.05) : Qt.rgba(systemPalette.window.r, systemPalette.window.g, systemPalette.window.b, 0.25)

        border.color: tab.attention === "bell" ? tab.warningColor : tab.attention === "failure" ? tab.failureColor : tab.attention === "success" ? tab.successColor : (tab.active ? Qt.rgba(tab.systemAccent.r, tab.systemAccent.g, tab.systemAccent.b, 0.8) : Qt.rgba(systemPalette.windowText.r, systemPalette.windowText.g, systemPalette.windowText.b, 0.05))
        border.width: 1
    }

    contentItem: Row {
        spacing: 6

        Controls.ToolButton {
            id: closeButton

            Accessible.name: qsTr("Close tab")

            width: 22
            height: 22
            anchors.verticalCenter: parent.verticalCenter
            padding: 3

            icon.name: "close"
            icon.color: systemPalette.windowText
            display: AbstractButton.IconOnly

            onClicked: tab.closeRequested()
        }

        Item {
            visible: !closeButton.visible
            width: parent.spacing
            height: closeButton.height
        }

        Item {
            id: leadingIndicator

            width: 16
            height: 16
            anchors.verticalCenter: parent.verticalCenter

            Accessible.role: Accessible.Indicator
            Accessible.name: tab.leadingMode === "kind" ? (tab.kindNames[tab.processKind] || "") : tab.leadingMode === "progress" ? qsTr("Progress %1%").arg(tab.progressValue) : tab.leadingMode === "spinner" ? qsTr("Running") : qsTr("Shell")

            IconImage {
                anchors.fill: parent
                visible: tab.leadingMode === "kind" || tab.leadingMode === "shell"
                name: tab.leadingMode === "shell" ? "kind-shell" : (tab.processKind !== "" ? "kind-" + tab.processKind : "")
                sourceSize: Qt.size(16, 16)
                color: tab.processKind === "privileged" ? tab.warningColor : systemPalette.windowText
            }

            Controls.TabActivityIndicator {
                anchors.centerIn: parent
                width: 13
                height: 13
                visible: tab.leadingMode === "progress" || tab.leadingMode === "spinner"
                spinning: tab.leadingMode === "spinner" || tab.progressState === 3
                value: tab.progressValue
                // ConEmu states: 2 error, 4 paused.
                color: tab.progressState === 2 ? tab.failureColor : (tab.progressState === 4 ? tab.warningColor : (tab.leadingMode === "spinner" ? systemPalette.windowText : tab.systemAccent))
                trackColor: Qt.rgba(systemPalette.windowText.r, systemPalette.windowText.g, systemPalette.windowText.b, 0.15)
            }
        }

        Text {
            id: tabLabel

            width: Math.max(0, tab.width - tab.leftPadding - tab.rightPadding - closeButton.width - parent.spacing - leadingIndicator.width - parent.spacing - (attentionBadge.visible ? attentionBadge.width + parent.spacing : 0))
            height: parent.height

            text: tab.tabTitle
            color: systemPalette.windowText

            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight

            MouseArea {
                id: dragArea

                property real pressSceneX: 0
                property real pressSceneY: 0
                property bool movedBeforeHold: false
                property bool holdActivated: false

                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                preventStealing: true

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

        Item {
            id: attentionBadge

            visible: tab.attention !== ""
            width: 14
            height: 14
            anchors.verticalCenter: parent.verticalCenter

            Accessible.role: Accessible.Indicator
            Accessible.name: tab.attentionNames[tab.attention] || ""

            Rectangle {
                anchors.centerIn: parent
                visible: tab.attention === "activity"
                width: 7
                height: 7
                radius: 3.5
                color: tab.systemAccent
            }

            IconImage {
                anchors.fill: parent
                visible: tab.attention !== "activity"
                name: tab.attention !== "" && tab.attention !== "activity" ? "status-" + tab.attention : ""
                sourceSize: Qt.size(14, 14)
                color: tab.attention === "failure" ? tab.failureColor : (tab.attention === "bell" ? tab.warningColor : tab.successColor)
            }
        }
    }
}
