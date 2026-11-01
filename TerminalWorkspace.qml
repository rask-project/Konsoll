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

        sessions: SessionManager.sessions
        currentIndex: SessionManager.activeIndex
        onActivateRequested: index => SessionManager.setActiveIndex(index)
        onCloseRequested: session => SessionManager.closeSession(session)
        onAddRequested: SessionManager.addSession()
    }

    StackLayout {
        id: terminalStack

        anchors.fill: parent
        anchors.topMargin: workspace.titleBarHeight + tabStrip.y
        currentIndex: SessionManager.activeIndex

        Repeater {
            model: SessionManager.sessions

            delegate: TerminalView {
                required property int index
                required property var modelData

                session: modelData
                active: index === SessionManager.activeIndex
            }
        }
    }
}
