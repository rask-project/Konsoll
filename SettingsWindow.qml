pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Dialogs
import QtQuick.Layouts

// App-wide settings window laid out after macOS System Settings: a full-height
// translucent sidebar (the native blur from WindowTranslucency) under the
// traffic lights, an opaque content pane with grouped form boxes, and values
// applied live to every terminal window (they bind to `preferences`).
Window {
    id: control

    required property Main windowManager
    required property var preferences
    property int selectedSection: 0

    readonly property var sections: [
        {
            title: qsTr("Appearance"),
            iconName: "appearance"
        },
        {
            title: qsTr("Window"),
            iconName: "window"
        },
        {
            title: qsTr("Security"),
            iconName: "security"
        }
    ]

    // Appearance tokens approximating AppKit's semantic colors.
    readonly property bool dark: Qt.styleHints.colorScheme === Qt.ColorScheme.Dark
    readonly property int sidebarWidth: 210
    readonly property color contentColor: palette.window
    readonly property color groupColor: dark ? Qt.rgba(1, 1, 1, 0.05) : "#ffffff"
    readonly property color separatorColor: dark ? Qt.rgba(1, 1, 1, 0.1) : Qt.rgba(0, 0, 0, 0.1)
    readonly property color secondaryTextColor: dark ? Qt.rgba(1, 1, 1, 0.55) : Qt.rgba(0, 0, 0, 0.5)

    // Height of the title bar + empty toolbar the content extends under.
    readonly property real titleBarHeight: Math.max(control.SafeArea.margins.top, 28)

    function present() {
        control.show();
        control.raise();
        control.requestActivate();
    }

    // Feeds Mission Control and the window switcher; WindowTranslucency hides
    // it from the title bar, where the section name is drawn by QML instead.
    title: control.sections[control.selectedSection].title

    // Full-size content under a transparent title bar; close and minimize only
    // (no zoom), and a fixed size, like a native settings window.
    flags: Qt.Window | Qt.CustomizeWindowHint | Qt.WindowTitleHint | Qt.WindowCloseButtonHint | Qt.WindowMinimizeButtonHint | Qt.ExpandedClientAreaHint | Qt.NoTitleBarBackgroundHint

    // Fixed size; the limits use constants (binding them to width/height
    // loops when the platform adjusts the size).
    readonly property int fixedWidth: 720
    readonly property int fixedHeight: 480
    width: fixedWidth
    height: fixedHeight
    minimumWidth: fixedWidth
    maximumWidth: fixedWidth
    minimumHeight: fixedHeight
    maximumHeight: fixedHeight

    // The sidebar shows the native blur through the window; only the content
    // pane paints an opaque background.
    color: "transparent"

    WindowTranslucency {
        window: control
        color: control.dark ? 0xff1e1e1e : 0xffe8e8e8
        alpha: 0.9
        blurEnabled: true
    }

    // While this window is key, AppKit uses its menu bar, so Cmd+W and the
    // other key equivalents act on Settings instead of a terminal window.
    Action {
        id: closeAction

        text: qsTr("Close")
        shortcut: StandardKey.Close
        onTriggered: control.close()
    }

    Action {
        id: minimizeAction

        text: qsTr("Minimize")
        shortcut: "Ctrl+M"
        onTriggered: control.showMinimized()
    }

    Action {
        id: newWindowAction

        text: qsTr("New Window")
        shortcut: "Ctrl+N"
        onTriggered: control.windowManager.openWindow()
    }

    Action {
        id: quitAction

        text: qsTr("Quit Konsoll")
        shortcut: StandardKey.Quit
        onTriggered: {
            control.windowManager.saveLastActiveWindow();
            NativeDialogs.terminateApplication();
        }
    }

    MenuBar {
        Menu {
            title: Qt.application.name

            MenuItem {
                text: qsTr("About Konsoll")
                onTriggered: control.windowManager.aboutDialog.open()
            }

            MenuItem {
                text: qsTr("About Qt")
                onTriggered: control.windowManager.aboutDialog.openAboutQt()
            }

            MenuSeparator {}

            MenuItem {
                action: quitAction
            }
        }
    }

    // Opaque content pane right of the sidebar, separated by a hairline.
    Rectangle {
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        width: parent.width - control.sidebarWidth

        color: control.contentColor

        Rectangle {
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 1

            color: control.separatorColor
        }
    }

    RowLayout {
        anchors.fill: parent

        spacing: 0

        // Sidebar: edge to edge, transparent so the window blur shows through.
        ColumnLayout {
            // Its rows fill width, which would make the column stretch too;
            // pin it so it ends exactly at the content pane's hairline.
            Layout.fillWidth: false
            Layout.minimumWidth: control.sidebarWidth - 20
            Layout.maximumWidth: control.sidebarWidth - 20
            Layout.fillHeight: true
            Layout.topMargin: control.titleBarHeight + 6
            Layout.leftMargin: 10
            Layout.rightMargin: 10

            spacing: 2

            Repeater {
                model: control.sections

                delegate: SidebarItem {}
            }

            Item {
                Layout.fillHeight: true
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.bottomMargin: 24

            spacing: 0

            // Section title, vertically centered with the traffic lights.
            Label {
                Layout.fillWidth: true
                Layout.preferredHeight: control.titleBarHeight
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                Layout.bottomMargin: 16

                text: control.sections[control.selectedSection].title
                font.pointSize: 15
                font.weight: Font.Bold
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24

                currentIndex: control.selectedSection

                ColumnLayout {
                    spacing: 6

                    FormGroup {
                        FormRow {
                            label: qsTr("Font")
                            detail: `${control.preferences.fontFamily} ${control.preferences.fontPointSize} pt`
                            showSeparator: false

                            Button {
                                text: qsTr("Change…")
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
                    }

                    Item {
                        Layout.fillHeight: true
                    }
                }

                ColumnLayout {
                    spacing: 6

                    FormGroup {
                        FormRow {
                            label: qsTr("Background blur")
                            showSeparator: false

                            Switch {
                                checked: control.preferences.blurEnabled
                                onToggled: control.preferences.blurEnabled = checked
                            }
                        }

                        FormRow {
                            label: qsTr("Background opacity")

                            Slider {
                                Layout.preferredWidth: 180

                                from: 0.2
                                to: 1.0
                                stepSize: 0.01
                                value: control.preferences.windowAlpha
                                onMoved: control.preferences.windowAlpha = value
                            }

                            Label {
                                Layout.preferredWidth: 36

                                text: Math.round(control.preferences.windowAlpha * 100) + "%"
                                horizontalAlignment: Text.AlignRight
                                font.features: {
                                    "tnum": 1
                                }
                                color: control.secondaryTextColor
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10

                        text: qsTr("These settings apply to all windows and are saved automatically.")
                        font.pointSize: 11
                        color: control.secondaryTextColor
                        wrapMode: Text.Wrap
                    }

                    Item {
                        Layout.fillHeight: true
                    }
                }

                // What programs running in the terminal may do.
                ColumnLayout {
                    spacing: 6

                    FormGroup {
                        FormRow {
                            label: qsTr("Read the clipboard")
                            detail: qsTr("OSC 52 paste requests")
                            showSeparator: false

                            ComboBox {
                                Layout.preferredWidth: 120

                                model: [qsTr("Never"), qsTr("Ask"), qsTr("Always")]
                                currentIndex: control.preferences.clipboardRead
                                onActivated: index => control.preferences.clipboardRead = index
                            }
                        }

                        FormRow {
                            label: qsTr("Read the window title")
                            detail: qsTr("Title reports (CSI 20 t / 21 t)")

                            Switch {
                                checked: control.preferences.allowTitleReport
                                onToggled: control.preferences.allowTitleReport = checked
                            }
                        }

                        FormRow {
                            label: qsTr("Move and resize the window")
                            detail: qsTr("Window operations (CSI t)")

                            Switch {
                                checked: control.preferences.allowWindowOps
                                onToggled: control.preferences.allowWindowOps = checked
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10

                        text: qsTr("A program could misuse these to read what you copied or to inject text. Writing to the clipboard (copy from the terminal) is always allowed.")
                        font.pointSize: 11
                        color: control.secondaryTextColor
                        wrapMode: Text.Wrap
                    }

                    Item {
                        Layout.fillHeight: true
                    }
                }
            }
        }
    }

    // Sidebar row: themed icon badge + 13 pt title; accent pill when selected.
    component SidebarItem: ItemDelegate {
        id: sidebarItem

        required property int index
        required property var modelData

        readonly property bool selected: control.selectedSection === index

        Layout.fillWidth: true

        implicitHeight: 30
        leftPadding: 6
        rightPadding: 8
        spacing: 8

        onClicked: control.selectedSection = index

        // The theme SVGs carry their own badge colors, so the icon is not tinted.
        contentItem: IconLabel {
            spacing: sidebarItem.spacing
            display: AbstractButton.TextBesideIcon
            alignment: Qt.AlignLeft | Qt.AlignVCenter

            icon.name: sidebarItem.modelData.iconName
            icon.width: 20
            icon.height: 20

            text: sidebarItem.modelData.title
            font: sidebarItem.font
            color: sidebarItem.selected ? "white" : sidebarItem.palette.text
        }

        background: Rectangle {
            radius: 7
            // Read the delegate's palette explicitly: it is the item-view palette
            // (selectedContentBackgroundColor). An unqualified `palette` here can
            // resolve to the generic one (selectedControlColor, a duller blue).
            color: sidebarItem.selected ? sidebarItem.palette.highlight : sidebarItem.down ? control.separatorColor : "transparent"
            border.width: Qt.styleHints.accessibility.contrastPreference === Qt.HighContrast ? 1 : 0
            border.color: sidebarItem.palette.text
        }
    }

    // Rounded "inset grouped" box holding a column of FormRows.
    component FormGroup: Rectangle {
        default property alias rows: rowColumn.data

        Layout.fillWidth: true
        implicitHeight: rowColumn.implicitHeight

        radius: 10
        color: control.groupColor
        border.width: 1
        border.color: control.separatorColor

        ColumnLayout {
            id: rowColumn

            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 0
        }
    }

    // Title (and optional secondary detail) on the left, controls on the right,
    // with a hairline separator above unless it is the group's first row.
    component FormRow: Item {
        id: formRow

        property string label
        property string detail
        property bool showSeparator: true
        default property alias trailing: trailingRow.data

        Layout.fillWidth: true
        implicitHeight: Math.max(40, rowLayout.implicitHeight + 16)

        Rectangle {
            visible: formRow.showSeparator

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            height: 1

            color: control.separatorColor
        }

        RowLayout {
            id: rowLayout

            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 12
            anchors.rightMargin: 12

            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1

                Label {
                    Layout.fillWidth: true
                    text: formRow.label
                    elide: Text.ElideRight
                }

                Label {
                    Layout.fillWidth: true
                    visible: formRow.detail !== ""
                    text: formRow.detail
                    font.pointSize: 11
                    color: control.secondaryTextColor
                    elide: Text.ElideRight
                }
            }

            RowLayout {
                id: trailingRow

                spacing: 8
            }
        }
    }

    FontDialog {
        id: fontDialog

        function applySelectedFont() {
            control.preferences.fontFamily = selectedFont.family;
            if (selectedFont.pointSize > 0)
                control.preferences.fontPointSize = selectedFont.pointSize;
            control.preferences.fontWeight = selectedFont.weight;
            control.preferences.fontItalic = selectedFont.italic;
        }

        title: qsTr("Choose a Monospaced Font")

        options: FontDialog.MonospacedFonts

        // macOS presents this as a live font panel without an Open button.
        // Apply each selection as the native panel changes it.
        onSelectedFontChanged: {
            if (visible && selectedFont.family !== "")
                applySelectedFont();
        }
        onAccepted: applySelectedFont()
    }
}
