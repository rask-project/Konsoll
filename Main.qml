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

    function openWindow() {
        manager.createdWindow = windowTemplate.createObject(manager);
        const w = manager.createdWindow;
        manager.windows = manager.windows.concat([w]);
        w.show();
        return w;
    }

    function closeWindow(w: TerminalWindow) {
        if (w)
            w.close();
    }

    function saveWindow(window: TerminalWindow) {
        window.windowState.save();
    }

    function isActiveWindow(window: TerminalWindow): bool {
        return window.active;
    }

    function saveAllOpenWindows() {
        const focusedWindow = manager.windows.find(manager.isActiveWindow);
        const windowToSave = focusedWindow ?? manager.windows[manager.windows.length - 1];
        if (windowToSave)
            manager.saveWindow(windowToSave);
    }

    function windowClosed(w: TerminalWindow) {
        manager.saveWindow(w);
        manager.windows = manager.windows.filter(window => window !== w);
        Qt.callLater(() => w.destroy());
    }

    Component.onCompleted: {
        manager.openWindow();
    }
}
