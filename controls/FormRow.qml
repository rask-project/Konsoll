import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Title (and optional secondary detail) on the left, controls on the right,
// with a hairline separator above unless it is the group's first row.
// `leading` holds an optional icon or status mark before the title; a
// `selectable` row reports clicks and shows `highlighted` with a tint.
Item {
    id: formRow

    property string label
    property string detail
    property bool showSeparator: true
    property bool monospaceDetail: false
    property bool selectable: false
    property bool highlighted: false
    default property alias trailing: trailingRow.data
    property alias leading: leadingRow.data

    signal clicked

    readonly property bool dark: Qt.styleHints.colorScheme === Qt.ColorScheme.Dark
    readonly property color separatorColor: dark ? Qt.rgba(1, 1, 1, 0.1) : Qt.rgba(0, 0, 0, 0.1)
    readonly property color secondaryTextColor: dark ? Qt.rgba(1, 1, 1, 0.55) : Qt.rgba(0, 0, 0, 0.5)

    Layout.fillWidth: true
    implicitHeight: Math.max(40, rowLayout.implicitHeight + 16)

    Rectangle {
        visible: formRow.highlighted

        anchors.fill: parent
        anchors.margins: 4
        radius: 6

        color: formRow.palette.highlight
        opacity: 0.18
    }

    Rectangle {
        visible: formRow.showSeparator

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        height: 1

        color: formRow.separatorColor
    }

    TapHandler {
        enabled: formRow.selectable
        onTapped: formRow.clicked()
    }

    RowLayout {
        id: rowLayout

        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 12
        anchors.rightMargin: 12

        spacing: 12

        RowLayout {
            id: leadingRow

            visible: children.length > 0
            spacing: 8
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1

            Label {
                id: titleLabel

                Layout.fillWidth: true
                text: formRow.label
                elide: Text.ElideRight
            }

            Label {
                Layout.fillWidth: true
                visible: formRow.detail !== ""
                text: formRow.detail
                font.pointSize: 11
                font.family: formRow.monospaceDetail ? "Menlo" : titleLabel.font.family
                color: formRow.secondaryTextColor
                elide: Text.ElideRight
            }
        }

        RowLayout {
            id: trailingRow

            spacing: 8
        }
    }
}
