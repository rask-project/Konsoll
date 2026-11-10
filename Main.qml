pragma ComponentBehavior: Bound

import QtQuick
import QtCore

Item {
    id: manager

    readonly property AboutDialog aboutDialog: AboutDialog {}
    property AppSettings settings: AppSettings {}
    // One Settings window for the whole app, like a native macOS app.
    readonly property SettingsWindow settingsWindow: SettingsWindow {
        windowManager: manager
        preferences: manager.settings
    }

    property var windows: []
    property TerminalWindow createdWindow: null

    // The ID of the most recently active window. Its geometry (size, position,
    // screen) is the only window state kept on disk and is restored on the next
    // launch; it is saved whenever it changes, so every way the app ends (last
    // window closed, Quit menu, Dock quit, logout) leaves it up to date.
    property string lastActiveWindowId: ""

    Component {
        id: windowTemplate

        TerminalWindow {
            windowManager: manager
            settings: manager.settings
            aboutDialog: manager.aboutDialog
        }
    }

    // `startEmpty` skips the window's first workspace: it is about to receive
    // one moved from another window.
    function openWindow(windowId, startEmpty) {
        if (!windowId)
            windowId = WindowStateStore.generateWindowId();
        manager.createdWindow = windowTemplate.createObject(manager, {
            windowId: windowId,
            startEmpty: startEmpty === true
        });
        const w = manager.createdWindow;
        manager.windows = manager.windows.concat([w]);
        manager.lastActiveWindowId = w.windowId;
        manager.persistCollection();
        w.showSaved();
        return w;
    }

    // Moves `workspace` from `source` to `target` (a new window when null) at
    // `index` (the end when negative). Its shells keep running; a source left
    // empty closes.
    function moveWorkspace(workspace, source: TerminalWindow, target: TerminalWindow, index: int) {
        const newWindow = target === null;
        if (newWindow)
            target = manager.openWindow("", true);
        const position = index < 0 ? target.terminalManager.workspaces.length : index;
        if (!source.terminalManager.moveWorkspaceTo(workspace, target.terminalManager, position)) {
            if (newWindow)
                target.close();
            return;
        }
        target.raise();
        target.requestActivate();
    }

    function noteActiveWindow(w: TerminalWindow) {
        if (manager.lastActiveWindowId !== w.windowId) {
            // Only the last active window's state is kept on disk.
            const previous = manager.lastActiveWindow();
            if (previous)
                previous.windowState.removeState();
            manager.lastActiveWindowId = w.windowId;
        }
        manager.saveLastActiveWindow();
    }

    function lastActiveWindow(): TerminalWindow {
        return manager.windows.find(w => w.windowId === manager.lastActiveWindowId) ?? null;
    }

    function saveLastActiveWindow() {
        const w = manager.lastActiveWindow();
        if (!w)
            return;
        w.windowState.save();
        WindowStateStore.saveLastWindowId(w.windowId);
    }

    function closeWindow(w: TerminalWindow) {
        if (w)
            w.close();
    }

    function persistCollection() {
        WindowStateStore.saveCollection(manager.windows.map(w => w.windowId));
    }

    function windowClosed(w: TerminalWindow) {
        const wasLastActive = manager.lastActiveWindowId === w.windowId;
        manager.windows = manager.windows.filter(window => window !== w);
        // A confirmed quit already saved the last active window; the windows
        // closing during termination must not overwrite that choice.
        if (!WindowStateStore.terminating()) {
            if (manager.windows.length === 0) {
                // Settings would otherwise keep the app alive (quit on last
                // window closed) with no terminal left.
                manager.settingsWindow.close();
                // Closing the last window ends the app: it is the window to
                // restore on the next launch, so save it instead of dropping it.
                manager.lastActiveWindowId = w.windowId;
                w.windowState.save();
                WindowStateStore.saveLastWindowId(w.windowId);
            } else {
                w.windowState.removeState();
                if (wasLastActive) {
                    manager.lastActiveWindowId = manager.windows[0].windowId;
                    manager.saveLastActiveWindow();
                }
            }
            manager.persistCollection();
        }
        Qt.callLater(() => w.destroy());
    }

    Component.onCompleted: {
        // Reopen a single window. Only the last active window's geometry is
        // restored (via its saved state); sessions always start fresh.
        const lastId = WindowStateStore.lastWindowId();
        if (lastId) {
            manager.openWindow(lastId);
        } else {
            manager.openWindow();
        }
    }
}
