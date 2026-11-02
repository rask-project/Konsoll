import QtQuick
import QtQuick.Controls

Window {
    id: appWindow

    visible: true

    width: 1024
    height: 768
    minimumWidth: 200
    minimumHeight: 250

    flags: Qt.Window | Qt.ExpandedClientAreaHint | Qt.NoTitleBarBackgroundHint
    color: "transparent"

    Shortcut {
        sequence: "Ctrl+T"
        context: Qt.WindowShortcut
        autoRepeat: false
        onActivated: SessionManager.addWorkspace()
    }

    Shortcut {
        sequence: "Meta+Tab"
        context: Qt.WindowShortcut
        autoRepeat: false
        enabled: SessionManager.workspaces.length > 0
        onActivated: SessionManager.navigateActivationHistory(1)
    }

    Shortcut {
        sequence: "Meta+Shift+Tab"
        context: Qt.WindowShortcut
        autoRepeat: false
        enabled: SessionManager.workspaces.length > 0
        onActivated: SessionManager.navigateActivationHistory(-1)
    }

    Shortcut {
        sequence: "Ctrl+Tab"
        context: Qt.WindowShortcut
        autoRepeat: false
        enabled: SessionManager.workspaces.length > 0
        onActivated: SessionManager.navigateActivationHistory(1)
    }

    Shortcut {
        sequence: "Ctrl+Shift+Tab"
        context: Qt.WindowShortcut
        autoRepeat: false
        enabled: SessionManager.workspaces.length > 0
        onActivated: SessionManager.navigateActivationHistory(-1)
    }

    Action {
        id: splitHorizontallyAction

        text: qsTr("Split Horizontally")
        shortcut: "Ctrl+D"
        enabled: SessionManager.activeSession !== null
        onTriggered: SessionManager.splitActive(Qt.Horizontal)
    }

    Action {
        id: splitVerticallyAction

        text: qsTr("Split Vertically")
        shortcut: "Ctrl+Shift+D"
        enabled: SessionManager.activeSession !== null
        onTriggered: SessionManager.splitActive(Qt.Vertical)
    }

    Action {
        id: closeSessionAction

        text: qsTr("Close Session")
        shortcut: "Ctrl+W"
        enabled: SessionManager.activeSession !== null
        onTriggered: SessionManager.closeSession(SessionManager.activeSession)
    }

    Action {
        id: clearScreenAction

        text: qsTr("Clear")
        shortcut: "Meta+L"
        enabled: SessionManager.activeSession !== null
        onTriggered: SessionManager.activeSession.clearScreen()
    }

    Action {
        id: clearBufferAction

        text: qsTr("Clear Buffer")
        shortcut: "Meta+K"
        enabled: SessionManager.activeSession !== null
        onTriggered: SessionManager.activeSession.clearBuffer()
    }

    Action {
        id: copyAction

        text: qsTr("Copy")
        shortcut: StandardKey.Copy
        enabled: SessionManager.activeSession !== null
        onTriggered: SessionManager.activeSession.copySelection()
    }

    Action {
        id: pasteAction

        text: qsTr("Paste")
        shortcut: StandardKey.Paste
        enabled: SessionManager.activeSession !== null
        onTriggered: SessionManager.activeSession.paste()
    }

    Action {
        id: selectAllAction

        text: qsTr("Select All")
        shortcut: StandardKey.SelectAll
        enabled: SessionManager.activeSession !== null
        onTriggered: SessionManager.activeSession.selectAll()
    }

    Action {
        id: saveTextAction

        text: qsTr("Save Buffer or Selection…")
        enabled: SessionManager.activeSession !== null
        onTriggered: saveTextDialog.openFor(SessionManager.activeSession, appWindow.activeFocusItem)
    }

    // On macOS (Qt >= 6.8, native menu bar) a Menu whose title equals the
    // application name is moved to the app menu (the bold first menu).
    MenuBar {
        Menu {
            title: Qt.application.name

            MenuItem {
                text: qsTr("About Konsoll")
                onTriggered: aboutDialog.open()
            }

            MenuItem {
                text: qsTr("About Qt")
                onTriggered: aboutDialog.openAboutQt()
            }

            MenuSeparator {}

            MenuItem {
                action: saveTextAction
            }
        }

        Menu {
            title: qsTr("Shell")

            MenuItem {
                action: splitHorizontallyAction
            }

            MenuItem {
                action: splitVerticallyAction
            }

            MenuSeparator {}

            MenuItem {
                action: clearScreenAction
            }

            MenuItem {
                action: clearBufferAction
            }

            MenuSeparator {}

            MenuItem {
                action: closeSessionAction
            }

            MenuItem {
                text: qsTr("Close Workspace")
                enabled: SessionManager.activeWorkspaceIndex >= 0
                onTriggered: SessionManager.closeWorkspace(SessionManager.workspaces[SessionManager.activeWorkspaceIndex])
            }
        }

        Menu {
            title: qsTr("Edit")

            MenuItem {
                action: copyAction
            }

            MenuItem {
                action: pasteAction
            }

            MenuItem {
                action: selectAllAction
            }
        }
    }

    AboutDialog {
        id: aboutDialog
    }

    SaveTextDialog {
        id: saveTextDialog
    }

    WindowTranslucency {
        id: translucency

        window: appWindow
        color: SessionManager.activeSession ? SessionManager.activeSession.backgroundColor : 0xff141414
        alpha: 0.85
        blurEnabled: true
    }

    TerminalWorkspace {
        anchors.fill: parent
        windowTranslucency: translucency
        fullScreen: appWindow.visibility === Window.FullScreen

        safeTop: appWindow.SafeArea.margins.top
        safeLeft: appWindow.SafeArea.margins.left
        safeRight: appWindow.SafeArea.margins.right
    }

    Component.onCompleted: SessionManager.start()

    Connections {
        target: SessionManager
        function onQuitRequested() {
            Qt.quit();
        }
    }
}
