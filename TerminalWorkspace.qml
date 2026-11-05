pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "controls" as Controls

Item {
    id: workspace

    signal saveBufferRequested(var session)

    required property var sessionManager
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
        y: 3

        width: workspace.width
        height: workspace.titleBarHeight

        baseSafeTop: workspace.safeTop
        leadingInset: workspace.fullScreen ? 0 : Math.max(84, workspace.safeLeft)
        trailingInset: workspace.safeRight

        windowTranslucency: workspace.windowTranslucency
        sessionManager: sessionManager
        tabs: sessionManager.workspaces
        currentIndex: sessionManager.activeWorkspaceIndex

        onActivateRequested: index => sessionManager.setActiveWorkspace(index)
        onCloseRequested: workspace => {
            if (sessionManager.workspaceHasRunningCommand(workspace) && !NativeDialogs.confirmClose()) {
                return;
            }
            sessionManager.closeWorkspace(workspace);
        }
        onReorderRequested: (fromIndex, toIndex) => sessionManager.moveWorkspace(fromIndex, toIndex)
        onAddRequested: sessionManager.addWorkspace()
    }

    StackLayout {
        anchors.fill: parent
        anchors.topMargin: workspace.titleBarHeight + tabStrip.y
        currentIndex: sessionManager.activeWorkspaceIndex

        Repeater {
            model: sessionManager.workspaces

            delegate: Item {
                id: item

                required property var modelData

                Controls.TerminalPaneLayout {
                    anchors.fill: parent
                    sessionManager: sessionManager
                    node: item.modelData.rootNode
                    terminalFont: workspace.terminalFont
                    onSaveBufferRequested: session => workspace.saveBufferRequested(session)
                }
            }
        }
    }
}
