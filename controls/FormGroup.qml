import QtQuick
import QtQuick.Layouts

// Rounded "inset grouped" box holding a column of FormRows, as in macOS
// System Settings.
Rectangle {
    default property alias rows: rowColumn.data

    readonly property bool dark: Qt.styleHints.colorScheme === Qt.ColorScheme.Dark

    Layout.fillWidth: true
    implicitHeight: rowColumn.implicitHeight

    radius: 10
    color: dark ? Qt.rgba(1, 1, 1, 0.05) : "#ffffff"
    border.width: 1
    border.color: dark ? Qt.rgba(1, 1, 1, 0.1) : Qt.rgba(0, 0, 0, 0.1)

    ColumnLayout {
        id: rowColumn

        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 0
    }
}
