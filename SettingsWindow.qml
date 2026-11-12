pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import "controls" as Controls

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
            title: qsTr("Terminal"),
            iconName: "terminal"
        },
        {
            title: qsTr("Keyboard"),
            iconName: "keyboard"
        },
        {
            title: qsTr("Security"),
            iconName: "security"
        },
        {
            title: qsTr("AI Agents"),
            iconName: "kind-agent"
        }
    ]
    readonly property int agentsSection: 5
    // Why the last Connect / Remove failed (empty when it worked).
    property string agentError

    // Appearance tokens approximating AppKit's semantic colors.
    readonly property bool dark: Qt.styleHints.colorScheme === Qt.ColorScheme.Dark
    readonly property int sidebarWidth: 210
    readonly property color contentColor: palette.window
    readonly property color separatorColor: dark ? Qt.rgba(1, 1, 1, 0.1) : Qt.rgba(0, 0, 0, 0.1)
    readonly property color secondaryTextColor: dark ? Qt.rgba(1, 1, 1, 0.55) : Qt.rgba(0, 0, 0, 0.5)

    // Height of the title bar + empty toolbar the content extends under.
    readonly property real titleBarHeight: Math.max(control.SafeArea.margins.top, 28)

    function present(section) {
        if (section !== undefined)
            control.selectedSection = section;
        AgentIntegrations.refresh();
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

                delegate: Controls.SettingsSidebarItem {
                    required property int index
                    required property var modelData

                    title: modelData.title
                    iconName: modelData.iconName
                    selected: control.selectedSection === index
                    onClicked: control.selectedSection = index
                }
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

                    Controls.FormGroup {
                        Controls.FormRow {
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

                    Controls.FormGroup {
                        Controls.FormRow {
                            label: qsTr("Cursor style")
                            showSeparator: false

                            ComboBox {
                                Layout.preferredWidth: 150

                                model: [qsTr("Block"), qsTr("Underline"), qsTr("Bar"), qsTr("Hollow block")]
                                currentIndex: control.preferences.cursorShape
                                onActivated: index => control.preferences.cursorShape = index
                            }
                        }

                        Controls.FormRow {
                            label: qsTr("Blinking cursor")

                            Switch {
                                checked: control.preferences.cursorBlink
                                onToggled: control.preferences.cursorBlink = checked
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10

                        text: qsTr("Programs such as Neovim can still change the cursor while they run (DECSCUSR).")
                        font.pointSize: 11
                        color: control.secondaryTextColor
                        wrapMode: Text.Wrap
                    }

                    Item {
                        Layout.fillHeight: true
                    }
                }

                ColumnLayout {
                    spacing: 6

                    Controls.FormGroup {
                        Controls.FormRow {
                            label: qsTr("Background blur")
                            showSeparator: false

                            Switch {
                                checked: control.preferences.blurEnabled
                                onToggled: control.preferences.blurEnabled = checked
                            }
                        }

                        Controls.FormRow {
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

                // Terminal: how much history each session keeps.
                ColumnLayout {
                    spacing: 6

                    Controls.FormGroup {
                        Controls.FormRow {
                            label: qsTr("Unlimited scrollback")
                            detail: qsTr("Keep every line of output")
                            showSeparator: false

                            Switch {
                                checked: ScrollbackSettings.maxLines === 0
                                onToggled: ScrollbackSettings.maxLines = checked ? 0 : control.preferences.scrollbackLimit
                            }
                        }

                        Controls.FormRow {
                            label: qsTr("Scrollback lines")
                            detail: qsTr("Older lines are dropped past the limit")
                            enabled: ScrollbackSettings.maxLines !== 0
                            opacity: enabled ? 1 : 0.5

                            SpinBox {
                                Layout.preferredWidth: 150

                                from: 100
                                to: 10000000
                                stepSize: 1000
                                editable: true
                                value: control.preferences.scrollbackLimit
                                onValueModified: {
                                    control.preferences.scrollbackLimit = value;
                                    ScrollbackSettings.maxLines = value;
                                }
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10

                        text: qsTr("History is stored compressed: a line of plain text takes about as many bytes as it has characters, so unlimited scrollback stays affordable. Clear Buffer frees it.")
                        font.pointSize: 11
                        color: control.secondaryTextColor
                        wrapMode: Text.Wrap
                    }

                    Item {
                        Layout.fillHeight: true
                    }
                }

                // Keyboard: how modifier keys reach programs.
                ColumnLayout {
                    spacing: 6

                    Controls.FormGroup {
                        Controls.FormRow {
                            label: qsTr("Use Option as Meta key")
                            detail: qsTr("Option+key sends Esc+key instead of typing special characters")
                            showSeparator: false

                            Switch {
                                checked: control.preferences.optionAsMeta
                                onToggled: control.preferences.optionAsMeta = checked
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10

                        text: qsTr("Turn on for Alt shortcuts in Emacs, tmux, readline and Neovim. Programs can also ask for it (xterm metaSendsEscape).")
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

                    Controls.FormGroup {
                        Controls.FormRow {
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

                        Controls.FormRow {
                            label: qsTr("Read the window title")
                            detail: qsTr("Title reports (CSI 20 t / 21 t)")

                            Switch {
                                checked: control.preferences.allowTitleReport
                                onToggled: control.preferences.allowTitleReport = checked
                            }
                        }

                        Controls.FormRow {
                            label: qsTr("Show notifications")
                            detail: qsTr("Desktop notifications (OSC 9 / OSC 777)")

                            Switch {
                                checked: control.preferences.allowNotifications
                                onToggled: control.preferences.allowNotifications = checked
                            }
                        }

                        Controls.FormRow {
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

                // AI agents: connect their hooks to the tab status, and
                // Konsoll's MCP server. Taller than the window: scrolls.
                ScrollView {
                    id: agentsScroll

                    contentWidth: availableWidth

                    ColumnLayout {
                        width: agentsScroll.availableWidth
                        spacing: 6

                        Controls.FormGroup {
                            Repeater {
                                model: AgentIntegrations.agents

                                delegate: Controls.FormRow {
                                    id: agentRow

                                    required property var modelData
                                    required property int index

                                    label: modelData.name
                                    detail: modelData.viaPlugin ? qsTr("Connected through the Konsoll plugin") : modelData.installed ? qsTr("Connected: %1").arg(modelData.path) : modelData.available ? qsTr("Not connected") : qsTr("Not found on this Mac")
                                    showSeparator: index > 0

                                    Button {
                                        visible: !agentRow.modelData.viaPlugin
                                        enabled: agentRow.modelData.installed || agentRow.modelData.available
                                        text: agentRow.modelData.installed ? qsTr("Remove") : qsTr("Connect")
                                        onClicked: control.agentError = agentRow.modelData.installed ? AgentIntegrations.uninstall(agentRow.modelData.id) : AgentIntegrations.install(agentRow.modelData.id)
                                    }
                                }
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: 10
                            Layout.rightMargin: 10

                            visible: control.agentError !== ""
                            text: control.agentError
                            font.pointSize: 11
                            color: control.dark ? "#ff453a" : "#ff3b30"
                            wrapMode: Text.Wrap
                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: 10
                            Layout.rightMargin: 10

                            text: qsTr("Connecting adds hooks to the agent's settings file (the previous file is kept next to it as .konsoll-backup); opencode gets a plugin file. The tab then shows whether the agent is working, waiting for you, done or failed. Agents already running pick it up when restarted, and Codex runs new hooks only after you trust them with /hooks.")
                            font.pointSize: 11
                            color: control.secondaryTextColor
                            wrapMode: Text.Wrap
                        }

                        // Konsoll's MCP server: tools for the agents.
                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: 12
                            Layout.leftMargin: 10

                            text: qsTr("Konsoll tools for agents (MCP)")
                            font.pointSize: 11
                            font.weight: Font.DemiBold
                            color: control.secondaryTextColor
                        }

                        Controls.FormGroup {
                            Repeater {
                                model: AgentIntegrations.agents

                                delegate: Controls.FormRow {
                                    id: mcpRow

                                    required property var modelData
                                    required property int index

                                    label: modelData.name
                                    detail: modelData.id === "codex" || modelData.id === "opencode" ? qsTr("Copy the configuration and add it to the agent's settings") : qsTr("Copy the command and run it once in a terminal")
                                    showSeparator: index > 0

                                    Button {
                                        text: qsTr("Copy")
                                        onClicked: AgentIntegrations.copyToClipboard(AgentIntegrations.mcpSetup(mcpRow.modelData.id))
                                    }
                                }
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: 10
                            Layout.rightMargin: 10

                            text: qsTr("With Konsoll's MCP server an agent can open a pane next to its own for a dev server or a watcher, and run background commands it can check on later. It only works for agents running in Konsoll, and only for the pane they run in.")
                            font.pointSize: 11
                            color: control.secondaryTextColor
                            wrapMode: Text.Wrap
                        }

                        Controls.FormGroup {
                            Layout.topMargin: 12

                            Controls.FormRow {
                                label: qsTr("Notify when an agent needs you")
                                detail: qsTr("When it waits for you, finishes or fails while you look elsewhere")
                                showSeparator: false

                                Switch {
                                    checked: control.preferences.agentNotifications
                                    onToggled: control.preferences.agentNotifications = checked
                                }
                            }
                        }
                    }
                }
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
