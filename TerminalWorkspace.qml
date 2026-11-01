import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "controls" as Controls

Item {
    id: workspace

    property real safeTop: 0
    property real safeLeft: 0
    property real safeRight: 0
    readonly property real titleBarHeight: tabStrip.requiredHeight

    Controls.TerminalTabStrip {
        id: tabStrip

        x: 0
        y: 3
        width: workspace.width
        height: workspace.titleBarHeight
        baseSafeTop: workspace.safeTop
        leadingInset: Math.max(84, workspace.safeLeft)
        trailingInset: workspace.safeRight
        tabs: SessionManager.workspaces
        currentIndex: SessionManager.activeWorkspaceIndex
        onActivateRequested: index => SessionManager.setActiveWorkspace(index)
        onCloseRequested: workspace => SessionManager.closeWorkspace(workspace)
        onAddRequested: SessionManager.addWorkspace()
    }

    StackLayout {
        anchors.fill: parent
        anchors.topMargin: workspace.titleBarHeight + tabStrip.y
        currentIndex: SessionManager.activeWorkspaceIndex

        Repeater {
            model: SessionManager.workspaces

            delegate: Item {
                required property var modelData

                TerminalPaneLayout {
                    anchors.fill: parent
                    node: modelData.rootNode
                }
            }
        }
    }
}
