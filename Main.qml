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
    // Commands running without a tab, saved commands and open sessions.
    readonly property BackgroundWindow backgroundWindow: BackgroundWindow {
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
    function moveWorkspace(workspace, source: TerminalWindow, target: TerminalWindow, index: int): TerminalWindow {
        const newWindow = target === null;
        if (newWindow)
            target = manager.openWindow("", true);
        const position = index < 0 ? target.terminalManager.workspaces.length : index;
        if (!source.terminalManager.moveWorkspaceTo(workspace, target.terminalManager, position)) {
            if (newWindow)
                target.close();
            return null;
        }
        target.raise();
        target.requestActivate();
        return target;
    }

    // A tab dragged out of `source` and released at a global point joins the
    // window under it, or tears off into a new window placed there.
    function dropWorkspace(workspace, source: TerminalWindow, globalX: real, globalY: real) {
        const target = manager.windows.find(w => w !== source && w.visible && w.visibility !== Window.Minimized
                                            && globalX >= w.x && globalX < w.x + w.width
                                            && globalY >= w.y && globalY < w.y + w.height) ?? null;
        if (target) {
            manager.moveWorkspace(workspace, source, target, target.workspaceDropIndex(globalX, globalY));
            return;
        }
        // A lone workspace torn off would just be its window again.
        if (source.terminalManager.workspaces.length < 2)
            return;
        const w = manager.moveWorkspace(workspace, source, null, 0);
        if (w && w.visibility === Window.Windowed) {
            w.width = source.width;
            w.height = source.height;
            w.x = globalX - 100;
            w.y = globalY - 18;
        }
    }

    // The window under a global point; `preferred` wins where windows overlap.
    function windowAtGlobal(globalX: real, globalY: real, preferred: TerminalWindow): TerminalWindow {
        const contains = w => w.visible && w.visibility !== Window.Minimized
                         && globalX >= w.x && globalX < w.x + w.width
                         && globalY >= w.y && globalY < w.y + w.height;
        if (preferred && contains(preferred))
            return preferred;
        return manager.windows.find(contains) ?? null;
    }

    // Where a pane dragged out of `source` to a global point lands: a drop
    // target of the window under it (see TerminalWorkspace.paneDropTarget).
    function paneDropAt(session, source: TerminalWindow, globalX: real, globalY: real) {
        const target = manager.windowAtGlobal(globalX, globalY, source);
        let drop = target ? target.paneDropTarget(globalX, globalY, session) : null;
        // A window's only pane already is its own workspace there.
        if (drop && drop.kind === "tab" && target === source && source.terminalManager.sessionCount() < 2)
            drop = null;
        return {
            window: target,
            drop: drop
        };
    }

    // Highlights the drop under a dragged pane, in whichever window it is.
    function hoverPane(session, source: TerminalWindow, globalX: real, globalY: real) {
        const at = manager.paneDropAt(session, source, globalX, globalY);
        manager.windows.forEach(w => w.showPaneDrop(w === at.window ? at.drop : null));
    }

    function clearPaneDrops() {
        manager.windows.forEach(w => w.showPaneDrop(null));
    }

    // A pane dragged out of `source` and released at a global point: beside
    // the pane under it (in this or another window), into a tab strip as a
    // workspace of its own, or torn off into a new window placed there.
    function dropPane(session, source: TerminalWindow, globalX: real, globalY: real) {
        manager.clearPaneDrops();
        const at = manager.paneDropAt(session, source, globalX, globalY);
        const sourceManager = source.terminalManager;
        if (at.window) {
            if (!at.drop)
                return;
            const targetManager = at.window.terminalManager;
            const moved = at.drop.kind === "pane"
                ? sourceManager.movePane(session, targetManager, at.drop.session, at.drop.edge)
                : sourceManager.movePaneToWorkspace(session, targetManager, at.drop.index);
            if (moved && at.window !== source) {
                at.window.raise();
                at.window.requestActivate();
            }
            return;
        }
        // A window's only pane torn off would just be its window again.
        if (sourceManager.sessionCount() < 2)
            return;
        const w = manager.openWindow("", true);
        if (!sourceManager.movePaneToWorkspace(session, w.terminalManager, 0)) {
            w.close();
            return;
        }
        if (w.visibility === Window.Windowed) {
            w.width = source.width;
            w.height = source.height;
            w.x = globalX - 100;
            w.y = globalY - 18;
        }
        w.raise();
        w.requestActivate();
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
                // Settings and Background Commands would otherwise keep the
                // app alive (quit on last window closed) with no terminal left.
                manager.settingsWindow.close();
                manager.backgroundWindow.close();
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
