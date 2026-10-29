import QtQuick
import QtQuick.Controls

Window {
    id: appWindow

    width: 1024
    height: 768
    minimumWidth: 200
    minimumHeight: 250

    visible: true

    title: Backend.title
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
        color: Backend.backgroundColor
        alpha: 0.85
    }

    TerminalView {
        anchors.fill: parent
    }

    Component.onCompleted: Backend.start()

    Connections {
        target: Backend

        function onProcessExited(code) {
            Qt.quit();
        }

        function onQuitRequested() {
            Qt.quit();
        }
    }
}
