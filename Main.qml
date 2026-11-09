pragma ComponentBehavior: Bound

import QtQuick
import QtCore

Item {
    id: manager

    readonly property AboutDialog aboutDialog: AboutDialog {}
    property AppSettings settings: AppSettings {}

    property var windows: []
    property TerminalWindow createdWindow: null

    // The ID of the most recently active window; its geometry is the only
    // per-window state restored on the next launch.
    property string lastActiveWindowId: ""

    Component {
        id: windowTemplate

        TerminalWindow {
            windowManager: manager
            settings: manager.settings
            aboutDialog: manager.aboutDialog
        }
    }

    function openWindow(windowId) {
        if (!windowId)
            windowId = WindowStateStore.generateWindowId();
        manager.createdWindow = windowTemplate.createObject(manager, {
            windowId: windowId
        });
        const w = manager.createdWindow;
        manager.windows = manager.windows.concat([w]);
        manager.lastActiveWindowId = w.windowId;
        manager.persistCollection();
        w.show();
        return w;
    }

    function noteActiveWindow(w: TerminalWindow) {
        manager.lastActiveWindowId = w.windowId;
    }

    function closeWindow(w: TerminalWindow) {
        if (w)
            w.close();
    }

    function persistCollection() {
        WindowStateStore.saveCollection(manager.windows.map(w => w.windowId));
    }

    function saveAllOpenWindows() {
        for (const w of manager.windows)
            w.windowState.save();
        if (manager.lastActiveWindowId)
            WindowStateStore.saveLastWindowId(manager.lastActiveWindowId);
        manager.persistCollection();
    }

    function windowClosed(w: TerminalWindow) {
        manager.windows = manager.windows.filter(window => window !== w);
        // If the closed window was the last active one, point the restoration
        // target at a remaining window (or none) so we never persist a stale ID.
        if (manager.lastActiveWindowId === w.windowId) {
            manager.lastActiveWindowId = manager.windows.length > 0
                    ? manager.windows[0].windowId
                    : "";
        }
        if (!WindowStateStore.terminating()) {
            w.windowState.removeState();
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
