pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "controls" as Controls

Item {
    id: workspace

    signal saveBufferRequested(var session)

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
        tabs: SessionManager.workspaces
        currentIndex: SessionManager.activeWorkspaceIndex

        onActivateRequested: index => SessionManager.setActiveWorkspace(index)
        onCloseRequested: workspace => {
            if (SessionManager.workspaceHasRunningCommand(workspace) && !NativeDialogs.confirmClose()) {
                return;
            }
            SessionManager.closeWorkspace(workspace);
        }
        onReorderRequested: (fromIndex, toIndex) => SessionManager.moveWorkspace(fromIndex, toIndex)
        onAddRequested: SessionManager.addWorkspace()
    }

    StackLayout {
        anchors.fill: parent
        anchors.topMargin: workspace.titleBarHeight + tabStrip.y
        currentIndex: SessionManager.activeWorkspaceIndex

        Repeater {
            model: SessionManager.workspaces

            delegate: Item {
                id: item

                required property var modelData

                Controls.TerminalPaneLayout {
                    anchors.fill: parent
                    node: item.modelData.rootNode
                    terminalFont: workspace.terminalFont
                    onSaveBufferRequested: session => workspace.saveBufferRequested(session)
                }
            }
        }
    }
}
