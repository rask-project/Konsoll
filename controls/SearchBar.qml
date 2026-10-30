pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts

Rectangle {
    id: root

    property bool regexSearch: false
    property int matchCount: 0
    property int currentMatch: 0

    signal closeRequested
    signal repaintRequested

    visible: false

    width: 520
    height: 48
    radius: 8

    color: Qt.rgba(appPalette.window.r, appPalette.window.g, appPalette.window.b, 0.90)
    border.color: appPalette.mid
    layer.enabled: true

    layer.effect: MultiEffect {
        shadowEnabled: true
        shadowColor: appPalette.shadow
        shadowOpacity: 0.4
        shadowBlur: 0.35
        blurMax: 16
        shadowVerticalOffset: 3
    }

    SystemPalette {
        id: appPalette
        colorGroup: SystemPalette.Active
    }

    function openSearch() {
        visible = true;
        searchField.forceActiveFocus();
    }

    function closeSearch() {
        visible = false;
        Backend.clearSearch();
        matchCount = 0;
        currentMatch = 0;
        closeRequested();
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 12

        CheckBox {
            Accessible.name: qsTr("Regular expression")

            text: ".*"
            checked: root.regexSearch
            onToggled: {
                root.regexSearch = checked;
                root.matchCount = Backend.search(searchField.text, checked);
                root.currentMatch = Backend.searchCurrentIndex();
                root.repaintRequested();
            }
        }

        TextField {
            id: searchField

            Layout.fillWidth: true

            placeholderText: qsTr("Type here")

            onTextChanged: {
                root.matchCount = Backend.search(text, root.regexSearch);
                root.currentMatch = Backend.searchCurrentIndex();
                root.repaintRequested();
            }

            Keys.onPressed: event => {
                if (event.key === Qt.Key_Escape) {
                    root.closeSearch();
                    event.accepted = true;
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    root.matchCount = Backend.searchNext((event.modifiers & Qt.ShiftModifier) !== 0);
                    root.currentMatch = Backend.searchCurrentIndex();
                    root.repaintRequested();
                    event.accepted = true;
                }
            }
        }

        Label {
            visible: root.matchCount > 0
            text: root.matchCount < 0 ? qsTr("Invalid regex") : qsTr("%1/%2").arg(root.currentMatch).arg(root.matchCount > 0 ? root.matchCount : 0)
            verticalAlignment: Text.AlignVCenter
        }

        ToolButton {
            Accessible.name: qsTr("Previous match")

            enabled: root.matchCount > 0

            icon.name: "arrow-prev"
            icon.color: appPalette.buttonText

            onClicked: {
                root.matchCount = Backend.searchNext(true);
                root.currentMatch = Backend.searchCurrentIndex();
                root.repaintRequested();
            }
        }

        ToolButton {
            Accessible.name: qsTr("Next match")

            enabled: root.matchCount > 0

            icon.name: "arrow-next"
            icon.color: appPalette.buttonText

            onClicked: {
                root.matchCount = Backend.searchNext(false);
                root.currentMatch = Backend.searchCurrentIndex();
                root.repaintRequested();
            }
        }

        ToolButton {
            Accessible.name: qsTr("Close find")

            icon.name: "close"
            icon.color: appPalette.buttonText

            onClicked: root.closeSearch()
        }
    }
}
