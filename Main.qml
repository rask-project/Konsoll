import QtQuick

Window {
    id: appWindow

    width: 720
    height: 480
    minimumWidth: 200
    minimumHeight: 250

    visible: true

    title: Backend.title
    color: "transparent"

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
