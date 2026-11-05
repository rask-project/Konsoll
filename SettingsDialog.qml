pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Templates as T
import QtQuick.Layouts

T.Dialog {
    id: control

    required property var preferences
    property var translucency: null
    property int selectedSection: 0

    title: qsTr("Settings")
    modal: true

    anchors.centerIn: Overlay.overlay

    width: Math.min(680, parent ? parent.width - 40 : 680)
    height: Math.min(480, parent ? parent.height - 40 : 480)
    padding: 0

    header: Label {
        visible: parent?.parent === Overlay.overlay && control.title

        padding: 18

        text: control.title
        font.bold: true
        font.pixelSize: 22
        elide: Label.ElideRight
    }

    background: Rectangle {
        color: systemPalette.window
        border.color: systemPalette.mid
        radius: 18
    }

    SystemPalette {
        id: systemPalette

        colorGroup: SystemPalette.Active
    }

    contentItem: RowLayout {
        spacing: 0

        ColumnLayout {
            Layout.preferredWidth: 190
            Layout.fillHeight: true

            spacing: 5

            Repeater {
                model: [qsTr("Appearance"), qsTr("Window")]

                delegate: ItemDelegate {
                    id: controlItemDelegate

                    required property int index
                    required property string modelData

                    Layout.fillWidth: true
                    Layout.leftMargin: 10
                    Layout.rightMargin: 8

                    text: modelData
                    highlighted: control.selectedSection === index
                    onClicked: control.selectedSection = index

                    leftPadding: 12
                    font.bold: highlighted

                    background: Rectangle {
                        visible: controlItemDelegate.down || controlItemDelegate.highlighted || controlItemDelegate.visualFocus

                        implicitWidth: 100
                        implicitHeight: 32

                        color: systemPalette.accent

                        border.width: Qt.styleHints.accessibility.contrastPreference === Qt.HighContrast ? 1 : 0
                        border.color: controlItemDelegate.highlighted ? systemPalette.highlight : systemPalette.text
                        radius: 8
                    }
                }
            }

            Item {
                Layout.fillHeight: true
            }
        }

        Rectangle {
            Layout.fillHeight: true
            Layout.maximumWidth: 1

            color: systemPalette.mid
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 28
            Layout.topMargin: 0

            currentIndex: control.selectedSection

            ColumnLayout {
                spacing: 18

                TitleSetting {
                    text: qsTr("Appearance")
                }

                Label {
                    Layout.fillWidth: true

                    text: qsTr("Font family")
                    font.bold: true
                }

                RowLayout {
                    Layout.fillWidth: true

                    Label {
                        Layout.fillWidth: true
                        text: `${control.preferences.fontFamily}, ${control.preferences.fontPointSize}pt, ${control.preferences.fontWeight}`
                        elide: Text.ElideRight
                        opacity: 0.75
                    }

                    Button {
                        text: qsTr("Choose…")
                        onClicked: {
                            fontDialog.currentFont = Qt.font({
                                family: control.preferences.fontFamily,
                                pointSize: control.preferences.fontPointSize,
                                weight: control.preferences.fontWeight,
                                italic: control.preferences.fontItalic
                            });
                            fontDialog.open();
                        }
                    }
                }

                Item {
                    Layout.fillHeight: true
                }
            }

            ColumnLayout {
                spacing: 18

                TitleSetting {
                    text: qsTr("Window")
                }

                CheckBox {
                    text: qsTr("Enable background blur")
                    checked: control.preferences.blurEnabled

                    onToggled: {
                        control.preferences.blurEnabled = checked;
                        control.translucency.blurEnabled = checked;
                    }
                }

                Label {
                    text: qsTr("Background opacity")
                    font.bold: true
                }

                RowLayout {
                    Slider {
                        Layout.fillWidth: true

                        from: 0.2
                        to: 1.0
                        stepSize: 0.01
                        value: control.preferences.windowAlpha
                        onMoved: {
                            control.preferences.windowAlpha = value;
                            control.translucency.alpha = value;
                        }
                    }

                    Label {
                        Layout.preferredWidth: 48

                        text: Math.round(control.preferences.windowAlpha * 100) + "%"
                    }
                }

                Label {
                    text: qsTr("These settings apply to the current window and are saved automatically.")
                    opacity: 0.7
                    wrapMode: Text.Wrap
                }

                Item {
                    Layout.fillHeight: true
                }
            }
        }
    }

    component TitleSetting: Label {
        font.pixelSize: 18
        font.bold: true
    }

    FontDialog {
        id: fontDialog

        title: qsTr("Choose a Monospaced Font")

        options: FontDialog.MonospacedFonts

        // macOS presents this as a live font panel without an Open button.
        // Apply each selection as the native panel changes it.
        onSelectedFontChanged: {
            if (visible && selectedFont.family !== "") {
                control.preferences.fontFamily = selectedFont.family;
                if (selectedFont.pointSize > 0)
                    control.preferences.fontPointSize = selectedFont.pointSize;
                control.preferences.fontWeight = selectedFont.weight;
                control.preferences.fontItalic = selectedFont.italic;
            }
        }
        onAccepted: {
            control.preferences.fontFamily = selectedFont.family;
            if (selectedFont.pointSize > 0)
                control.preferences.fontPointSize = selectedFont.pointSize;
            control.preferences.fontWeight = selectedFont.weight;
            control.preferences.fontItalic = selectedFont.italic;
        }
    }

    T.Overlay.modal: Rectangle {
        color: Color.transparent(systemPalette.shadow, 0.5)
    }

    T.Overlay.modeless: Rectangle {
        color: Color.transparent(systemPalette.shadow, 0.12)
    }
}
