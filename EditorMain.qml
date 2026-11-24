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

    // The size of the last window closed, for the next one opened.
    Settings {
        id: editorGeometry

        category: "Editor"

        property int width: 900
        property int height: 700
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

    // A window running `command` in `directory`, cascaded from the last one.
    function openCommand(command: string, directory: string, title: string): EditorWindow {
        const previous = editor.lastActiveWindow;
        const w = windowTemplate.createObject(editor, {
            command: command,
            directory: directory,
            fallbackTitle: title,
            width: editorGeometry.width,
            height: editorGeometry.height
        }) as EditorWindow;
        if (previous && previous.visibility === Window.Windowed) {
            w.x = previous.x + 24;
            w.y = previous.y + 24;
        }
        editor.windows = editor.windows.concat([w]);
        editor.lastActiveWindow = w;
        w.show();
        w.raise();
        w.requestActivate();
        return w;
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
