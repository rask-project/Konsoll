import QtQuick
import QtQuick.Controls
import QtCore

Item {
    id: manager

    Settings {
        id: appPreferences

        category: "General"

        property real fontPointSize: 15
        property string fontFamily: "Menlo"
        property int fontWeight: 400
        property bool fontItalic: false
        property real windowAlpha: 0.85
        property bool blurEnabled: true
        property int windowWidth: 1024
        property int windowHeight: 768
        property string windowState: "windowed"
        property string windowScreenName: ""
    }

    AboutDialog {
        id: aboutDialog
    }

    SettingsDialog {
        id: settingsDialog
        preferences: appPreferences
    }

    property var windows: []

    Component {
        id: windowTemplate

        TerminalWindow {
            windowManager: manager
            appPreferences: manager.appPreferences
            aboutDialog: manager.aboutDialog
            settingsDialog: manager.settingsDialog
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
