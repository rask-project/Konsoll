import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// Sidebar row of a System Settings-style window (Settings, Background
// Commands): themed icon badge + 13 pt title, accent pill when selected and
// an optional count badge on the right.
ItemDelegate {
    id: sidebarItem

    property string title
    property string iconName
    property bool selected: false
    // Shown in a rounded badge when not empty (e.g. running commands).
    property string badge

    readonly property bool dark: Qt.styleHints.colorScheme === Qt.ColorScheme.Dark
    readonly property color separatorColor: dark ? Qt.rgba(1, 1, 1, 0.1) : Qt.rgba(0, 0, 0, 0.1)

    Layout.fillWidth: true

    implicitHeight: 30
    leftPadding: 6
    rightPadding: 8
    spacing: 8

    // The theme SVGs carry their own badge colors, so the icon is not tinted.
    contentItem: RowLayout {
        spacing: sidebarItem.spacing

        IconLabel {
            Layout.fillWidth: true

            spacing: sidebarItem.spacing
            display: AbstractButton.TextBesideIcon
            alignment: Qt.AlignLeft | Qt.AlignVCenter

            icon.name: sidebarItem.iconName
            icon.width: 20
            icon.height: 20

            text: sidebarItem.title
            font: sidebarItem.font
            color: sidebarItem.selected ? "white" : sidebarItem.palette.text
        }

        Label {
            visible: sidebarItem.badge !== ""

            text: sidebarItem.badge
            font.pointSize: 11
            font.features: {
                "tnum": 1
            }
            color: sidebarItem.selected ? "white" : sidebarItem.palette.text
            opacity: 0.7
        }
    }

    background: Rectangle {
        radius: 7
        // Read the delegate's palette explicitly: it is the item-view palette
        // (selectedContentBackgroundColor). An unqualified `palette` here can
        // resolve to the generic one (selectedControlColor, a duller blue).
        color: sidebarItem.selected ? sidebarItem.palette.highlight : sidebarItem.down ? sidebarItem.separatorColor : "transparent"
        border.width: Qt.styleHints.accessibility.contrastPreference === Qt.HighContrast ? 1 : 0
        border.color: sidebarItem.palette.text
    }
}
