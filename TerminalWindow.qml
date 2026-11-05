import QtQuick
import QtQuick.Controls
import QtCore

Window {
    id: appWindow

    // Back-references to the app-level manager root (Main.qml).
    required property var windowManager
    required property AppSettings settings
    required property AboutDialog aboutDialog
    required property SettingsDialog settingsDialog

    readonly property font terminalFont: Qt.font({
        family: appWindow.settings.fontFamily,
        pointSize: appWindow.settings.fontPointSize,
        weight: appWindow.settings.fontWeight,
        italic: appWindow.settings.fontItalic
    })

    visibility: Window.Hidden

    width: appWindow.settings.windowWidth
    height: appWindow.settings.windowHeight
    minimumWidth: 200
    minimumHeight: 250

    flags: Qt.Window | Qt.ExpandedClientAreaHint | Qt.NoTitleBarBackgroundHint
    color: "transparent"

    function requestQuit() {
        const count = sessionManager.sessionCount();
        const activeProcess = sessionManager.anySessionHasActiveProcess();
        if ((count > 1 || activeProcess) && !NativeDialogs.confirmQuit()) {
            return;
        }
        NativeDialogs.approveQuit();
        NativeDialogs.terminateApplication();
    }

    onClosing: close => {
        close.accepted = false;
        appWindow.requestQuit();
    }

    Connections {
        target: sessionManager
        function onQuitRequested() {
            appWindow.requestQuit();
        }
    }

    // Save only the normal window geometry. Maximized and fullscreen sizes are
    // monitor geometry and must not replace the user's restorable dimensions.
    onWidthChanged: {
        if (visibility === Window.Windowed)
            appWindow.settings.windowWidth = width;
    }
    onHeightChanged: {
        if (visibility === Window.Windowed)
            appWindow.settings.windowHeight = height;
    }

    onVisibilityChanged: {
        if (visibility === Window.FullScreen) {
            appWindow.settings.windowState = "fullscreen";
        } else if (visibility === Window.Maximized) {
            appWindow.settings.windowState = "maximized";
        } else if (visibility === Window.Windowed) {
            appWindow.settings.windowState = "windowed";
        }
    }

    onScreenChanged: appWindow.settings.windowScreenName = windowScreen.currentScreenName()

    Component.onCompleted: {
        windowScreen.restoreScreen(appWindow.settings.windowScreenName);
        if (appWindow.settings.windowState === "fullscreen")
            appWindow.visibility = Window.FullScreen;
        else if (appWindow.settings.windowState === "maximized")
            appWindow.visibility = Window.Maximized;
        else
            appWindow.visibility = Window.Windowed;
        sessionManager.start();
    }

    Shortcut {
        sequence: "Ctrl+T"
        context: Qt.WindowShortcut
        autoRepeat: false
        onActivated: sessionManager.addWorkspace()
    }

    Shortcut {
        sequence: "Meta+,"
        context: Qt.WindowShortcut
        onActivated: {
            appWindow.settingsDialog.translucency = translucency;
            appWindow.settingsDialog.open();
        }
    }

    Shortcut {
        enabled: sessionManager.workspaces.length > 0

        sequence: "Meta+Tab"
        context: Qt.WindowShortcut
        autoRepeat: false
        onActivated: sessionManager.navigateActivationHistory(1)
    }

    Shortcut {
        enabled: sessionManager.workspaces.length > 0

        sequence: "Meta+Shift+Tab"
        context: Qt.WindowShortcut
        autoRepeat: false
        onActivated: sessionManager.navigateActivationHistory(-1)
    }

    Shortcut {
        enabled: sessionManager.workspaces.length > 0

        sequence: "Ctrl+Tab"
        context: Qt.WindowShortcut
        autoRepeat: false
        onActivated: sessionManager.navigateActivationHistory(1)
    }

    Shortcut {
        enabled: sessionManager.workspaces.length > 0

        sequence: "Ctrl+Shift+Tab"
        context: Qt.WindowShortcut
        autoRepeat: false
        onActivated: sessionManager.navigateActivationHistory(-1)
    }

    Action {
        id: splitHorizontallyAction

        enabled: sessionManager.activeSession !== null

        text: qsTr("Split Horizontally")
        shortcut: "Ctrl+D"
        onTriggered: sessionManager.splitActive(Qt.Horizontal)
    }

    Action {
        id: splitVerticallyAction

        enabled: sessionManager.activeSession !== null

        text: qsTr("Split Vertically")
        shortcut: "Ctrl+Shift+D"
        onTriggered: sessionManager.splitActive(Qt.Vertical)
    }

    Action {
        id: closeSessionAction

        enabled: sessionManager.activeSession !== null

        text: qsTr("Close Session")
        shortcut: "Ctrl+W"
        onTriggered: {
            if (sessionManager.activeSession.commandRunning && !NativeDialogs.confirmClose()) {
                return;
            }
            sessionManager.closeSession(sessionManager.activeSession);
        }
    }

    Action {
        id: clearScreenAction

        text: qsTr("Clear")
        shortcut: "Meta+L"
        enabled: sessionManager.activeSession !== null
        onTriggered: sessionManager.activeSession.clearScreen()
    }

    Action {
        id: clearBufferAction

        enabled: sessionManager.activeSession !== null

        text: qsTr("Clear Buffer")
        shortcut: "Meta+K"
        onTriggered: sessionManager.activeSession.clearBuffer()
    }

    Action {
        id: copyAction

        enabled: sessionManager.activeSession !== null

        text: qsTr("Copy")
        shortcut: StandardKey.Copy
        onTriggered: sessionManager.activeSession.copySelection()
    }

    Action {
        id: pasteAction

        enabled: sessionManager.activeSession !== null

        text: qsTr("Paste")
        shortcut: StandardKey.Paste
        onTriggered: sessionManager.activeSession.paste()
    }

    Action {
        id: selectAllAction

        enabled: sessionManager.activeSession !== null

        text: qsTr("Select All")
        shortcut: StandardKey.SelectAll
        onTriggered: sessionManager.activeSession.selectAll()
    }

    Action {
        id: saveTextAction

        enabled: sessionManager.activeSession !== null

        text: qsTr("Save Buffer or Selection…")
        onTriggered: saveTextDialog.openFor(sessionManager.activeSession, appWindow.activeFocusItem)
    }

    Action {
        id: quitAction

        text: qsTr("Quit Konsoll")
        shortcut: StandardKey.Quit
        onTriggered: appWindow.requestQuit()
    }

    Action {
        id: newWindowAction

        text: qsTr("New Window")
        shortcut: "Ctrl+N"
        onTriggered: windowManager.openWindow()
    }

    Action {
        id: closeWindowAction

        text: qsTr("Close Window")
        shortcut: "Ctrl+Alt+W"
        onTriggered: windowManager.closeWindow(appWindow)
    }

    // On macOS (Qt >= 6.8, native menu bar) a Menu whose title equals the
    // application name is moved to the app menu (the bold first menu).
    MenuBar {
        Menu {
            title: Qt.application.name

            MenuItem {
                text: qsTr("About Konsoll")
                onTriggered: appWindow.aboutDialog.open()
            }

            MenuItem {
                text: qsTr("About Qt")
                onTriggered: appWindow.aboutDialog.openAboutQt()
            }

            MenuItem {
                text: qsTr("Settings…")
                onTriggered: {
                    appWindow.settingsDialog.translucency = translucency;
                    appWindow.settingsDialog.open();
                }
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
                enabled: sessionManager.activeWorkspaceIndex >= 0
                onTriggered: {
                    const workspace = sessionManager.workspaces[sessionManager.activeWorkspaceIndex];
                    if (sessionManager.workspaceHasRunningCommand(workspace) && !NativeDialogs.confirmClose()) {
                        return;
                    }
                    sessionManager.closeWorkspace(workspace);
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

        Menu {
            title: qsTr("Window")

            MenuItem {
                action: newWindowAction
            }

            MenuItem {
                action: closeWindowAction
            }
        }
    }

    SaveTextDialog {
        id: saveTextDialog
    }

    WindowTranslucency {
        id: translucency

        window: appWindow
        color: sessionManager.activeSession ? sessionManager.activeSession.backgroundColor : 0xff141414
        alpha: appWindow.settings.windowAlpha
        blurEnabled: appWindow.settings.blurEnabled
        fullScreen: appWindow.visibility === Window.FullScreen
    }

    WindowScreen {
        id: windowScreen
        window: appWindow
    }

    TerminalManager {
        id: sessionManager
        window: appWindow
    }

    TerminalWorkspace {
        sessionManager: sessionManager
        windowTranslucency: translucency
        fullScreen: appWindow.visibility === Window.FullScreen
        terminalFont: appWindow.terminalFont

        anchors.fill: parent
        safeTop: appWindow.SafeArea.margins.top
        safeLeft: appWindow.SafeArea.margins.left
        safeRight: appWindow.SafeArea.margins.right

        onSaveBufferRequested: session => saveTextDialog.openFor(session, appWindow.activeFocusItem)
    }
}
