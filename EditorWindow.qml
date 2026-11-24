import QtQuick
import QtQuick.Controls

// A Neovim (Konsoll) window (EditorMain.qml): one session running nvim, with
// no tabs, splits or shell. Closing it asks nvim to quit, so nvim asks about
// unsaved buffers; the window closes when nvim exits.
Window {
    id: editorWindow

    required property EditorMain windowManager
    required property AppSettings settings
    required property AboutDialog aboutDialog
    // What the session runs and where, and the title until nvim sets one.
    required property string command
    required property string directory
    required property string fallbackTitle
    // When the window last asked nvim to quit (ms since the epoch, 0 never):
    // closing again soon after, with nvim still running, offers to force it.
    property double quitRequestedAt: 0

    readonly property TerminalManager terminalManager: sessionManager
    readonly property font terminalFont: Qt.font({
        family: editorWindow.settings.fontFamily,
        pointSize: editorWindow.settings.fontPointSize,
        weight: editorWindow.settings.fontWeight,
        italic: editorWindow.settings.fontItalic
    })

    visibility: Window.Hidden

    width: 900
    height: 700
    minimumWidth: 200
    minimumHeight: 150

    flags: Qt.Window | Qt.ExpandedClientAreaHint | Qt.NoTitleBarBackgroundHint
    color: "transparent"

    // nvim's title (`set title`), else the file or folder name ("Konsoll" is
    // a session's title until the program sets one).
    readonly property string sessionTitle: sessionManager.activeSession ? sessionManager.activeSession.title : ""
    title: sessionTitle !== "" && sessionTitle !== "Konsoll" ? sessionTitle : editorWindow.fallbackTitle

    // Asks nvim to quit (`:confirm qa`).
    function quitEditor() {
        const session = sessionManager.activeSession;
        if (!session)
            return;
        editorWindow.quitRequestedAt = Date.now();
        session.writeText(EditorMode.quitKeys());
    }

    onClosing: close => {
        if (sessionManager.sessionCount() === 0 || WindowStateStore.terminating()) {
            close.accepted = true;
            editorWindow.windowManager.windowClosed(editorWindow);
            return;
        }
        close.accepted = false;
        if (Date.now() - editorWindow.quitRequestedAt < 3000) {
            // nvim did not quit: end it when the user agrees.
            if (NativeDialogs.confirmClose())
                sessionManager.closeSession(sessionManager.activeSession);
            return;
        }
        editorWindow.quitEditor();
    }

    onActiveChanged: {
        if (active)
            editorWindow.windowManager.noteActiveWindow(editorWindow);
    }

    Component.onCompleted: sessionManager.addCommandWorkspace(editorWindow.command, editorWindow.directory)

    TerminalManager {
        id: sessionManager
        window: editorWindow

        // nvim exited (or was ended): the window goes with it.
        onWindowShouldClose: editorWindow.close()
    }

    WindowTranslucency {
        id: translucency

        window: editorWindow
        color: sessionManager.activeSession ? sessionManager.activeSession.backgroundColor : 0xff141414
        alpha: editorWindow.settings.windowAlpha
        blurEnabled: editorWindow.settings.blurEnabled
        fullScreen: editorWindow.visibility === Window.FullScreen
    }

    SystemPalette {
        id: titlePalette
        colorGroup: editorWindow.active ? SystemPalette.Active : SystemPalette.Inactive
    }

    // The title in the transparent title bar (WindowTranslucency hides
    // AppKit's), clear of the window buttons.
    Text {
        id: titleText

        visible: editorWindow.visibility !== Window.FullScreen
        x: Math.max(editorWindow.SafeArea.margins.left, 84)
        width: parent.width - 2 * x
        height: editorWindow.SafeArea.margins.top

        text: editorWindow.title
        color: titlePalette.windowText
        opacity: editorWindow.active ? 0.85 : 0.5
        font.weight: Font.DemiBold
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideMiddle
    }

    TerminalView {
        anchors.fill: parent
        anchors.topMargin: titleText.visible ? titleText.height : 0

        sessionManager: sessionManager
        session: sessionManager.activeSession
        active: editorWindow.active
        singleSession: true
        terminalFont: editorWindow.terminalFont
        cursorShape: editorWindow.settings.cursorShape
        cursorBlink: editorWindow.settings.cursorBlink

        onEditorWindowRequested: (directory, command) => editorWindow.windowManager.openCommand(command, directory, EditorMode.titleFor(directory))
    }

    Action {
        id: newWindowAction

        text: qsTr("New Window")
        shortcut: "Ctrl+N"
        onTriggered: editorWindow.windowManager.openPath("")
    }

    Action {
        id: openAction

        text: qsTr("Open…")
        shortcut: StandardKey.Open
        onTriggered: editorWindow.windowManager.openChosen()
    }

    Action {
        id: closeWindowAction

        text: qsTr("Close Window")
        shortcut: "Ctrl+W"
        onTriggered: editorWindow.close()
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
        id: quitAction

        text: qsTr("Quit")
        shortcut: StandardKey.Quit
        onTriggered: NativeDialogs.terminateApplication()
    }

    // The app menu takes About, Settings-like and Quit items by their role
    // (see TerminalWindow.qml); anything else belongs in another menu.
    MenuBar {
        Menu {
            title: Qt.application.name

            MenuItem {
                text: qsTr("About Konsoll")
                onTriggered: editorWindow.aboutDialog.open()
            }

            MenuItem {
                text: qsTr("About Qt")
                onTriggered: editorWindow.aboutDialog.openAboutQt()
            }

            MenuSeparator {}

            MenuItem {
                action: quitAction
            }
        }

        Menu {
            title: qsTr("File")

            MenuItem {
                action: newWindowAction
            }

            MenuItem {
                action: openAction
            }

            MenuSeparator {}

            MenuItem {
                text: qsTr("Use as Default Editor for Text Files")
                onTriggered: {
                    if (!EditorMode.makeDefaultEditor())
                        NativeDialogs.showWarning(qsTr("Could not change the default editor"), qsTr("Choose this app in the Finder with Get Info ▸ Open with ▸ Change All."));
                }
            }

            MenuSeparator {}

            MenuItem {
                action: closeWindowAction
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

        Menu {
            id: windowMenu

            title: qsTr("Window")

            // Every open window by title; the active one is checked. An
            // Instantiator inserts each at its index (see TerminalWindow.qml).
            Instantiator {
                model: editorWindow.windowManager.windows

                delegate: MenuItem {
                    id: windowEntry

                    required property Window modelData

                    text: modelData.title
                    checkable: true
                    checked: modelData === editorWindow.windowManager.lastActiveWindow
                    onTriggered: {
                        windowEntry.checked = Qt.binding(() => windowEntry.modelData === editorWindow.windowManager.lastActiveWindow);
                        windowEntry.modelData.raise();
                        windowEntry.modelData.requestActivate();
                    }
                }

                onObjectAdded: (index, object) => windowMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => windowMenu.removeItem(object)
            }
        }
    }
}
