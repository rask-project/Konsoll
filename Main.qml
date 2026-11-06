pragma ComponentBehavior: Bound

import QtQuick
import QtCore

Item {
    id: manager

    readonly property AboutDialog aboutDialog: AboutDialog {}
    readonly property SettingsDialog settingsDialog: SettingsDialog {
        preferences: manager.settings
    }
    property AppSettings settings: AppSettings {}

    property var windows: []
    property TerminalWindow createdWindow: null

    Component {
        id: windowTemplate

        TerminalWindow {
            windowManager: manager
            settings: manager.settings
            aboutDialog: manager.aboutDialog
            settingsDialog: manager.settingsDialog
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
        manager.persistCollection();
        w.show();
        return w;
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
        manager.persistCollection();
    }

    function windowClosed(w: TerminalWindow) {
        manager.windows = manager.windows.filter(window => window !== w);
        if (!WindowStateStore.terminating()) {
            w.windowState.removeState();
            manager.persistCollection();
        }
        Qt.callLater(() => w.destroy());
    }

    Component.onCompleted: {
        const ids = WindowStateStore.listWindowIds();
        if (ids.length > 0) {
            for (const id of ids)
                manager.openWindow(id);
        } else {
            manager.openWindow();
        }
    }
}
