import QtQuick
import QtQuick.Controls

Window {
    id: appWindow

    visible: true

    width: 1024
    height: 768
    minimumWidth: 200
    minimumHeight: 250

    flags: Qt.Window | Qt.ExpandedClientAreaHint | Qt.NoTitleBarBackgroundHint
    color: "transparent"

    // On macOS (Qt >= 6.8, native menu bar) a Menu whose title equals the
    // application name is moved to the app menu (the bold first menu).
    MenuBar {
        Menu {
            title: Qt.application.name

            MenuItem {
                text: qsTr("About Konsoll")
                onTriggered: aboutDialog.open()
            }

            MenuItem {
                text: qsTr("About Qt")
                onTriggered: aboutDialog.openAboutQt()
            }
        }
    }

    AboutDialog {
        id: aboutDialog
    }

    WindowTranslucency {
        id: translucency

        window: appWindow
        color: SessionManager.activeSession ? SessionManager.activeSession.backgroundColor : 0xff141414
        alpha: 0.85
    }

    TerminalWorkspace {
        anchors.fill: parent

        safeTop: appWindow.SafeArea.margins.top
        safeLeft: appWindow.SafeArea.margins.left
        safeRight: appWindow.SafeArea.margins.right
    }

    Component.onCompleted: SessionManager.start()

    Connections {
        target: SessionManager
        function onQuitRequested() {
            Qt.quit();
        }
    }
}
