import QtQuick
import QtQuick.Controls
import QtCore

Window {
    id: appWindow

    // Back-references to the app-level manager root (Main.qml).
    required property Main windowManager
    required property AppSettings settings
    required property AboutDialog aboutDialog
    property string windowId: ""
    readonly property WindowState windowState: windowStateObject

    readonly property font terminalFont: Qt.font({
        family: appWindow.settings.fontFamily,
        pointSize: appWindow.settings.fontPointSize,
        weight: appWindow.settings.fontWeight,
        italic: appWindow.settings.fontItalic
    })

    visibility: Window.Hidden

    width: 1024
    height: 768
    minimumWidth: 200
    minimumHeight: 250

    flags: Qt.Window | Qt.ExpandedClientAreaHint | Qt.NoTitleBarBackgroundHint
    color: "transparent"

    function requestQuit() {
        // Persist the last active window before the AppKit terminate delegate
        // aggregates sessions and owns the confirmation flow.
        appWindow.windowManager.saveLastActiveWindow();
        NativeDialogs.terminateApplication();
    }

    onClosing: close => {
        // Ask before closing when the window holds more than one session/tab,
        // or when its single session is blocked by a running command. A lone
        // idle tab closes without prompting. Only this (active) window's
        // manager is consulted, so the other windows are left untouched.
        const needsConfirm = sessionManager.sessionCount() > 1
                || sessionManager.anySessionHasRunningCommand();
        if (!WindowStateStore.terminating() && needsConfirm
                && !NativeDialogs.confirmClose()) {
            close.accepted = false;
            return;
        }
        close.accepted = true;
        appWindow.windowManager.windowClosed(appWindow);
    }

    Connections {
        target: sessionManager
        function onWindowShouldClose() {
            appWindow.windowManager.closeWindow(appWindow);
        }
    }

    onActiveChanged: {
        if (active)
            appWindow.windowManager.noteActiveWindow(appWindow);
    }

    // Geometry is committed once it settles: maximizing/fullscreen resizes the
    // window before `visibility` changes, so recording on every change would
    // store the maximized size as the normal one. The timer records the normal
    // geometry only if the window is still windowed when it fires, and saves
    // the state (including maximized/fullscreen) if this is the last active
    // window (Main.qml keeps just that one).
    onWidthChanged: geometrySaveTimer.restart()
    onHeightChanged: geometrySaveTimer.restart()
    onXChanged: geometrySaveTimer.restart()
    onYChanged: geometrySaveTimer.restart()
    onVisibilityChanged: geometrySaveTimer.restart()

    Timer {
        id: geometrySaveTimer
        interval: 500
        onTriggered: {
            if (appWindow.visibility === Window.Windowed) {
                windowState.x = appWindow.x;
                windowState.y = appWindow.y;
                windowState.width = appWindow.width;
                windowState.height = appWindow.height;
            }
            if (appWindow.visibility !== Window.Hidden
                    && appWindow.windowManager.lastActiveWindowId === appWindow.windowId)
                appWindow.windowManager.saveLastActiveWindow();
        }
    }

    // Show the window the way it was saved: the normal geometry is applied in
    // Component.onCompleted first, so leaving maximized/fullscreen returns to it.
    function showSaved() {
        if (windowState.state === "maximized")
            appWindow.showMaximized();
        else if (windowState.state === "fullscreen")
            appWindow.showFullScreen();
        else
            appWindow.show();
    }

    Component.onCompleted: {
        // Restore only the window geometry (size + location). Sessions always
        // start fresh — the saved layout is never restored.
        if (windowState.load()) {
            appWindow.width = windowState.width;
            appWindow.height = windowState.height;
            appWindow.x = windowState.x;
            appWindow.y = windowState.y;
        }
        if (sessionManager.workspaces.length === 0) {
            sessionManager.start();
        }
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
        onActivated: settingsDialog.open()
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
        onTriggered: appWindow.windowManager.openWindow()
    }

    Action {
        id: closeWindowAction

        text: qsTr("Close Window")
        shortcut: "Ctrl+Alt+W"
        onTriggered: appWindow.windowManager.closeWindow(appWindow)
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
                enabled: sessionManager.activeWorkspaceIndex >= 0
                onTriggered: {
                    const workspace = sessionManager.workspaces[sessionManager.activeWorkspaceIndex];
                    if (sessionManager.workspaceNeedsCloseConfirmation(workspace) && !NativeDialogs.confirmClose()) {
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

    // A Popup opens only inside a window, so each window owns its dialog; the
    // preferences object itself is shared app-wide (Main.qml).
    SettingsDialog {
        id: settingsDialog
        preferences: appWindow.settings
        translucency: translucency
    }

    WindowTranslucency {
        id: translucency

        window: appWindow
        color: sessionManager.activeSession ? sessionManager.activeSession.backgroundColor : 0xff141414
        alpha: appWindow.settings.windowAlpha
        blurEnabled: appWindow.settings.blurEnabled
        fullScreen: appWindow.visibility === Window.FullScreen
    }

    WindowState {
        id: windowStateObject
        window: appWindow
        windowId: appWindow.windowId
        manager: sessionManager
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
