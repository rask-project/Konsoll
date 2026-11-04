import QtQuick
import QtQuick.Controls
import QtCore

Window {
    id: appWindow

    visibility: Window.Hidden

    width: appPreferences.windowWidth
    height: appPreferences.windowHeight
    minimumWidth: 200
    minimumHeight: 250

    flags: Qt.Window | Qt.ExpandedClientAreaHint | Qt.NoTitleBarBackgroundHint
    color: "transparent"

    function requestQuit() {
        const count = SessionManager.sessionCount()
        const activeProcess = SessionManager.anySessionHasActiveProcess()
        if ((count > 1 || activeProcess) && !CloseConfirmDialog.confirmQuit()) {
            return
        }
        CloseConfirmDialog.approveQuit()
        Qt.quit()
    }

    onClosing: close => {
        close.accepted = false
        appWindow.requestQuit()
    }

    Settings {
        id: appPreferences

        category: "General"

        property real fontPointSize: 15
        property string fontFamily: "Menlo"
        property int fontWeight: 400
        property bool fontItalic: false
        property real windowAlpha: 0.85
        property bool blurEnabled: true
        property int windowWidth: 1024
        property int windowHeight: 768
        property string windowState: "windowed"
        property string windowScreenName: ""
    }

    // Save only the normal window geometry. Maximized and fullscreen sizes are
    // monitor geometry and must not replace the user's restorable dimensions.
    onWidthChanged: {
        if (visibility === Window.Windowed)
            appPreferences.windowWidth = width
    }
    onHeightChanged: {
        if (visibility === Window.Windowed)
            appPreferences.windowHeight = height
    }

    onVisibilityChanged: {
        if (visibility === Window.FullScreen) {
            appPreferences.windowState = "fullscreen"
        } else if (visibility === Window.Maximized) {
            appPreferences.windowState = "maximized"
        } else if (visibility === Window.Windowed) {
            appPreferences.windowState = "windowed"
        }
    }

    onScreenChanged: appPreferences.windowScreenName = windowScreen.currentScreenName()

    Component.onCompleted: {
        windowScreen.restoreScreen(appPreferences.windowScreenName)
        if (appPreferences.windowState === "fullscreen")
            visibility = Window.FullScreen
        else if (appPreferences.windowState === "maximized")
            visibility = Window.Maximized
        else
            visibility = Window.Windowed
        SessionManager.start()
    }

    readonly property font terminalFont: Qt.font({
        family: appPreferences.fontFamily,
        pointSize: appPreferences.fontPointSize,
        weight: appPreferences.fontWeight,
        italic: appPreferences.fontItalic
    })

    Shortcut {
        sequence: "Ctrl+T"
        context: Qt.WindowShortcut
        autoRepeat: false
        onActivated: SessionManager.addWorkspace()
    }

    Shortcut {
        sequence: "Meta+,"
        context: Qt.WindowShortcut
        onActivated: settingsDialog.open()
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
        onTriggered: {
            if (SessionManager.activeSession.commandRunning && !CloseConfirmDialog.confirmClose()) {
                return
            }
            SessionManager.closeSession(SessionManager.activeSession)
        }
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

    Action {
        id: quitAction

        text: qsTr("Quit Konsoll")
        shortcut: StandardKey.Quit
        onTriggered: appWindow.requestQuit()
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
                text: qsTr("Settings…")
                onTriggered: settingsDialog.open()
            }

            MenuSeparator {}

            MenuItem {
                action: quitAction
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
                onTriggered: {
                    const workspace = SessionManager.workspaces[SessionManager.activeWorkspaceIndex]
                    if (SessionManager.workspaceHasRunningCommand(workspace) && !CloseConfirmDialog.confirmClose()) {
                        return
                    }
                    SessionManager.closeWorkspace(workspace)
                }
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

            MenuSeparator {}

            MenuItem {
                action: saveTextAction
            }
        }
    }

    AboutDialog {
        id: aboutDialog
    }

    SaveTextDialog {
        id: saveTextDialog
    }

    SettingsDialog {
        id: settingsDialog
        preferences: appPreferences
        translucency: translucency
    }

    WindowTranslucency {
        id: translucency

        window: appWindow
        color: SessionManager.activeSession ? SessionManager.activeSession.backgroundColor : 0xff141414
        alpha: appPreferences.windowAlpha
        blurEnabled: appPreferences.blurEnabled
        fullScreen: appWindow.visibility === Window.FullScreen
    }

    WindowScreen {
        id: windowScreen
        window: appWindow
    }

    TerminalWorkspace {
        anchors.fill: parent
        windowTranslucency: translucency
        fullScreen: appWindow.visibility === Window.FullScreen
        terminalFont: appWindow.terminalFont

        safeTop: appWindow.SafeArea.margins.top
        safeLeft: appWindow.SafeArea.margins.left
        safeRight: appWindow.SafeArea.margins.right

        onSaveBufferRequested: session => saveTextDialog.openFor(session, appWindow.activeFocusItem)
    }

    Connections {
        target: SessionManager
        function onQuitRequested() {
            Qt.quit();
        }
    }
}
