pragma ComponentBehavior: Bound

import QtQuick
import QtCore

// The root of Neovim (Konsoll) (EditorMode): a window per file or folder
// opened, each running nvim. Settings are Konsoll's.
Item {
    id: editor

    readonly property AboutDialog aboutDialog: AboutDialog {}
    property AppSettings settings: AppSettings {}

    property var windows: []
    property EditorWindow lastActiveWindow: null

    // The geometry of the last window closed, for the next one opened.
    Settings {
        id: editorGeometry

        category: "Editor"

        property int width: 900
        property int height: 700
        property int x: 0
        property int y: 0
        // Set once a windowed window closed; until then x/y are meaningless
        // and a new window opens centered.
        property bool hasPosition: false
    }

    Component {
        id: windowTemplate

        EditorWindow {
            windowManager: editor
            settings: editor.settings
            aboutDialog: editor.aboutDialog
        }
    }

    // A window running nvim on `path` (a file, a folder, or empty).
    function openPath(path: string): EditorWindow {
        return editor.openCommand(EditorMode.commandFor(path), EditorMode.directoryFor(path), EditorMode.titleFor(path));
    }

    // A window running `command` in `directory`: cascaded from the last open
    // one, or back where the last closed window was (when that screen is
    // still connected).
    function openCommand(command: string, directory: string, title: string): EditorWindow {
        const previous = editor.lastActiveWindow;
        const w = windowTemplate.createObject(editor, {
            command: command,
            directory: directory,
            fallbackTitle: title
        }) as EditorWindow;
        // Sized once created: the native window is made while the component
        // completes, and the transparent title bar then grows a size given
        // as an initial property by its height, so each close would save a
        // taller window than the one opened.
        w.width = editorGeometry.width;
        w.height = editorGeometry.height;
        if (previous && previous.visibility === Window.Windowed) {
            w.x = previous.x + 24;
            w.y = previous.y + 24;
        } else if (editorGeometry.hasPosition && editor.positionOnScreen(editorGeometry.x, editorGeometry.y, editorGeometry.width, editorGeometry.height)) {
            w.x = editorGeometry.x;
            w.y = editorGeometry.y;
        }
        editor.windows = editor.windows.concat([w]);
        editor.lastActiveWindow = w;
        w.show();
        w.raise();
        w.requestActivate();
        return w;
    }

    // Whether a saved window rectangle still reaches a connected screen:
    // a position parked on a monitor that went away would open the window
    // out of sight.
    function positionOnScreen(x: int, y: int, width: int, height: int): bool {
        return Qt.application.screens.some(screen => x < screen.virtualX + screen.width && x + width > screen.virtualX && y < screen.virtualY + screen.height && y + height > screen.virtualY);
    }

    // File ▸ Open…: a window for each file or folder chosen.
    function openChosen() {
        EditorMode.choosePaths().forEach(path => editor.openPath(path));
    }

    function noteActiveWindow(w: EditorWindow) {
        editor.lastActiveWindow = w;
    }

    function windowClosed(w: EditorWindow) {
        if (w.visibility === Window.Windowed) {
            editorGeometry.width = w.width;
            editorGeometry.height = w.height;
            editorGeometry.x = w.x;
            editorGeometry.y = w.y;
            editorGeometry.hasPosition = true;
        }
        editor.windows = editor.windows.filter(window => window !== w);
        if (editor.lastActiveWindow === w)
            editor.lastActiveWindow = editor.windows.length > 0 ? editor.windows[editor.windows.length - 1] : null;
        Qt.callLater(() => w.destroy());
    }

    Connections {
        target: EditorMode

        function onOpenRequested(path: string) {
            editor.openPath(path);
        }

        // A launch to open files waits for them; a plain launch opens an
        // empty buffer.
        function onLaunched(openWindow: bool) {
            if (openWindow)
                editor.openPath("");
            else
                launchFallback.start();
        }

        function onQuitRequested() {
            editor.windows.forEach(w => w.quitEditor());
        }
    }

    // Launched to open something that never came (a file that went away):
    // an empty buffer rather than an app without windows.
    Timer {
        id: launchFallback

        interval: 2000
        onTriggered: {
            if (editor.windows.length === 0)
                editor.openPath("");
        }
    }

    // The Dock icon's menu: New Window.
    Connections {
        target: DockMenu

        function onNewWindowRequested() {
            editor.openPath("");
        }
    }
}
