pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "controls" as Controls

Item {
    id: itemWorkspace

    signal saveBufferRequested(var session)
    signal workspaceDropRequested(var workspace, real globalX, real globalY)
    // A pane header dragged to a global point (hover) or released there.
    signal paneDragHovered(var session, real globalX, real globalY)
    signal paneDropRequested(var session, real globalX, real globalY)
    signal paneDragEnded

    function tabInsertionIndexAtGlobal(globalX, globalY) {
        return tabStrip.insertionIndexAtGlobal(globalX, globalY);
    }

    // What a pane dragged to a global point lands on in this window: the tab
    // strip (a workspace of its own at `index`) or a pane, split on the
    // `edge` nearest the pointer. Null over the dragged pane or elsewhere.
    function paneDropTarget(globalX, globalY, session) {
        const point = itemWorkspace.mapFromGlobal(globalX, globalY);
        if (point.x < 0 || point.y < 0 || point.x >= itemWorkspace.width || point.y >= itemWorkspace.height)
            return null;
        if (point.y < paneStack.y) {
            const index = tabStrip.insertionIndexAtGlobal(globalX, globalY);
            return {
                kind: "tab",
                index: index,
                rect: tabStrip.insertionMarkerRect(index)
            };
        }
        const page = workspaceRepeater.itemAt(itemWorkspace.sessionManager.activeWorkspaceIndex);
        const leaf = page ? page.paneLayout.leafAtGlobal(globalX, globalY) : null;
        if (!leaf || !leaf.session || leaf.session === session)
            return null;

        const local = leaf.mapFromGlobal(globalX, globalY);
        const dx = local.x / leaf.width - 0.5;
        const dy = local.y / leaf.height - 0.5;
        let edge;
        if (Math.abs(dx) > Math.abs(dy))
            edge = dx < 0 ? TerminalManager.PaneEdgeLeft : TerminalManager.PaneEdgeRight;
        else
            edge = dy < 0 ? TerminalManager.PaneEdgeTop : TerminalManager.PaneEdgeBottom;

        const area = leaf.mapToItem(itemWorkspace, 0, 0, leaf.width, leaf.height);
        const halfWidth = area.width / 2;
        const halfHeight = area.height / 2;
        let rect;
        switch (edge) {
        case TerminalManager.PaneEdgeLeft:
            rect = Qt.rect(area.x, area.y, halfWidth, area.height);
            break;
        case TerminalManager.PaneEdgeRight:
            rect = Qt.rect(area.x + halfWidth, area.y, halfWidth, area.height);
            break;
        case TerminalManager.PaneEdgeTop:
            rect = Qt.rect(area.x, area.y, area.width, halfHeight);
            break;
        default:
            rect = Qt.rect(area.x, area.y + halfHeight, area.width, halfHeight);
            break;
        }
        return {
            kind: "pane",
            session: leaf.session,
            edge: edge,
            rect: rect
        };
    }

    // Highlights where a pane drop lands (null hides it).
    function showPaneDrop(drop) {
        paneDropIndicator.drop = drop;
    }

    required property TerminalManager sessionManager
    required property font terminalFont
    required property int cursorShape
    required property bool cursorBlink

    property real safeTop: 0
    property real safeLeft: 0
    property real safeRight: 0
    property var windowTranslucency: null
    property bool fullScreen: false
    readonly property real titleBarHeight: tabStrip.requiredHeight

    // The pane header being dragged from this window, drawn under the pointer.
    property var paneDragSession: null
    property string paneDragTitle: ""
    property real paneDragGrabX: 0
    property real paneDragGrabY: 0
    property real paneDragWidth: 0
    property real paneDragGlobalX: 0
    property real paneDragGlobalY: 0

    function endPaneDrag() {
        itemWorkspace.paneDragSession = null;
        itemWorkspace.paneDragEnded();
    }

    Controls.TerminalTabStrip {
        id: tabStrip

        x: 0
        y: 0

        width: itemWorkspace.width
        height: itemWorkspace.titleBarHeight

        baseSafeTop: itemWorkspace.safeTop
        leadingInset: itemWorkspace.fullScreen ? 0 : Math.max(84, itemWorkspace.safeLeft)
        trailingInset: itemWorkspace.safeRight

        windowTranslucency: itemWorkspace.windowTranslucency
        sessionManager: itemWorkspace.sessionManager
        tabs: itemWorkspace.sessionManager.workspaces
        currentIndex: itemWorkspace.sessionManager.activeWorkspaceIndex

        onActivateRequested: index => itemWorkspace.sessionManager.setActiveWorkspace(index)
        onCloseRequested: workspace => {
            if (itemWorkspace.sessionManager.workspaceNeedsCloseConfirmation(workspace) && !NativeDialogs.confirmClose()) {
                return;
            }
            itemWorkspace.sessionManager.closeWorkspace(workspace);
        }
        onReorderRequested: (fromIndex, toIndex) => itemWorkspace.sessionManager.moveWorkspace(fromIndex, toIndex)
        onAddRequested: itemWorkspace.sessionManager.addWorkspace()
        onWindowDropRequested: (workspace, globalX, globalY) => itemWorkspace.workspaceDropRequested(workspace, globalX, globalY)
    }

    StackLayout {
        id: paneStack

        anchors.fill: parent
        anchors.topMargin: itemWorkspace.titleBarHeight + tabStrip.y
        currentIndex: itemWorkspace.sessionManager.activeWorkspaceIndex

        Repeater {
            id: workspaceRepeater

            model: itemWorkspace.sessionManager.workspaces

            delegate: Item {
                id: item

                required property var modelData
                readonly property alias paneLayout: paneLayout

                Controls.TerminalPaneLayout {
                    id: paneLayout

                    anchors.fill: parent
                    sessionManager: itemWorkspace.sessionManager
                    node: item.modelData.rootNode
                    terminalFont: itemWorkspace.terminalFont
                    cursorShape: itemWorkspace.cursorShape
                    cursorBlink: itemWorkspace.cursorBlink
                    onSaveBufferRequested: session => itemWorkspace.saveBufferRequested(session)
                    onPaneDragStarted: (session, title, grabX, grabY, headerWidth) => {
                        itemWorkspace.paneDragTitle = title;
                        itemWorkspace.paneDragGrabX = grabX;
                        itemWorkspace.paneDragGrabY = grabY;
                        itemWorkspace.paneDragWidth = headerWidth;
                        itemWorkspace.paneDragSession = session;
                    }
                    onPaneDragMoved: (globalX, globalY) => {
                        itemWorkspace.paneDragGlobalX = globalX;
                        itemWorkspace.paneDragGlobalY = globalY;
                        if (itemWorkspace.paneDragSession)
                            itemWorkspace.paneDragHovered(itemWorkspace.paneDragSession, globalX, globalY);
                    }
                    onPaneDragFinished: (session, globalX, globalY) => {
                        itemWorkspace.endPaneDrag();
                        itemWorkspace.paneDropRequested(session, globalX, globalY);
                    }
                    onPaneDragCanceled: itemWorkspace.endPaneDrag()
                }
            }
        }
    }

    SystemPalette {
        id: workspacePalette
        colorGroup: SystemPalette.Active
    }

    // Where a dragged pane lands: a tinted half of the target pane, or a bar
    // between the tabs for a workspace of its own.
    Rectangle {
        id: paneDropIndicator

        property var drop: null
        readonly property bool overTabs: drop !== null && drop.kind === "tab"

        visible: drop !== null
        z: 20

        x: drop ? drop.rect.x : 0
        y: drop ? drop.rect.y : 0
        width: drop ? drop.rect.width : 0
        height: drop ? drop.rect.height : 0

        radius: overTabs ? width / 2 : 10
        color: overTabs ? workspacePalette.accent : Qt.rgba(workspacePalette.accent.r, workspacePalette.accent.g, workspacePalette.accent.b, 0.18)
        border.width: overTabs ? 0 : 2
        border.color: workspacePalette.accent

        Behavior on x {
            enabled: !paneDropIndicator.overTabs
            NumberAnimation {
                duration: 90
                easing.type: Easing.OutCubic
            }
        }
        Behavior on y {
            enabled: !paneDropIndicator.overTabs
            NumberAnimation {
                duration: 90
                easing.type: Easing.OutCubic
            }
        }
        Behavior on width {
            enabled: !paneDropIndicator.overTabs
            NumberAnimation {
                duration: 90
                easing.type: Easing.OutCubic
            }
        }
        Behavior on height {
            enabled: !paneDropIndicator.overTabs
            NumberAnimation {
                duration: 90
                easing.type: Easing.OutCubic
            }
        }
    }

    // The dragged header follows the pointer in a frameless window of its
    // own, so it shows over other windows and the desktop too.
    Loader {
        active: itemWorkspace.paneDragSession !== null

        sourceComponent: Window {
            flags: Qt.ToolTip | Qt.FramelessWindowHint | Qt.WindowTransparentForInput | Qt.WindowDoesNotAcceptFocus
            color: "transparent"
            visible: true

            x: itemWorkspace.paneDragGlobalX - Math.min(itemWorkspace.paneDragGrabX, width - 16)
            y: itemWorkspace.paneDragGlobalY - itemWorkspace.paneDragGrabY
            width: Math.max(120, Math.min(itemWorkspace.paneDragWidth, 320))
            height: 32

            Rectangle {
                anchors.fill: parent
                radius: height / 2
                color: Qt.rgba(workspacePalette.window.r, workspacePalette.window.g, workspacePalette.window.b, 0.92)
                border.width: 1
                border.color: workspacePalette.accent
            }

            Text {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14

                text: itemWorkspace.paneDragTitle
                color: workspacePalette.windowText
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideMiddle
            }
        }
    }
}
