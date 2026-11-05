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

    Component {
        id: windowTemplate

        TerminalWindow {
            windowManager: manager
            settings: manager.settings
            aboutDialog: manager.aboutDialog
            settingsDialog: manager.settingsDialog
        }
    }

    Connections {
        target: AppController
        function onWindowOpenRequested() {
            manager.openWindow();
        }
    }

    function openWindow() {
        const w = windowTemplate.createObject(manager);
        manager.windows.push(w);
        return w;
    }

    function closeWindow(w) {
        const i = manager.windows.indexOf(w);
        if (i >= 0)
            manager.windows.splice(i, 1);
        w.destroy();
    }

    Component.onCompleted: {
        const w = manager.openWindow();
        w.show();
    }
}
