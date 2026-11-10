pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "controls" as Controls

Item {
    id: itemWorkspace

    signal saveBufferRequested(var session)
    signal workspaceDropRequested(var workspace, real globalX, real globalY)

    function tabInsertionIndexAtGlobal(globalX, globalY) {
        return tabStrip.insertionIndexAtGlobal(globalX, globalY);
    }

    required property TerminalManager sessionManager
    required property font terminalFont

    property real safeTop: 0
    property real safeLeft: 0
    property real safeRight: 0
    property var windowTranslucency: null
    property bool fullScreen: false
    readonly property real titleBarHeight: tabStrip.requiredHeight

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
        anchors.fill: parent
        anchors.topMargin: itemWorkspace.titleBarHeight + tabStrip.y
        currentIndex: itemWorkspace.sessionManager.activeWorkspaceIndex

        Repeater {
            model: itemWorkspace.sessionManager.workspaces

            delegate: Item {
                id: item

                required property var modelData

                Controls.TerminalPaneLayout {
                    anchors.fill: parent
                    sessionManager: itemWorkspace.sessionManager
                    node: item.modelData.rootNode
                    terminalFont: itemWorkspace.terminalFont
                    onSaveBufferRequested: session => itemWorkspace.saveBufferRequested(session)
                }
            }
        }
    }
}
