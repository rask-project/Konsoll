pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Dialogs
import QtQuick.Layouts

import "controls" as Controls

// Background Commands: commands running without a tab, the saved commands
// ready to start, the shell functions Konsoll defines in every shell, and
// the sessions open in every window. Laid out like the
// Settings window (System Settings sidebar, grouped form boxes).
Window {
    id: control

    required property Main windowManager
    required property var preferences
    property int selectedSection: 0
    // The job whose output the Running section shows.
    property BackgroundJob selectedJob: null

    // Saved-command editor: open for a new command (-1) or the saved command
    // at `editingIndex`.
    property bool editorOpen: false
    property int editingIndex: -1

    // Function editor, like the saved-command one; `functionError` is the
    // reason the last save was refused.
    property bool functionEditorOpen: false
    property int functionEditingIndex: -1
    property string functionError

    readonly property var sections: [
        {
            title: qsTr("Running"),
            iconName: "background-running"
        },
        {
            title: qsTr("Saved Commands"),
            iconName: "background-saved"
        },
        {
            title: qsTr("Functions"),
            iconName: "background-functions"
        },
        {
            title: qsTr("Sessions"),
            iconName: "background-sessions"
        },
        {
            title: qsTr("AI Agents"),
            iconName: "agents"
        }
    ]
    readonly property int runningSection: 0
    readonly property int savedSection: 1
    readonly property int functionsSection: 2
    readonly property int sessionsSection: 3
    readonly property int agentsSection: 4

    // Appearance tokens approximating AppKit's semantic colors (as Settings).
    readonly property bool dark: Qt.styleHints.colorScheme === Qt.ColorScheme.Dark
    readonly property int sidebarWidth: 210
    readonly property color contentColor: palette.window
    readonly property color separatorColor: dark ? Qt.rgba(1, 1, 1, 0.1) : Qt.rgba(0, 0, 0, 0.1)
    readonly property color secondaryTextColor: dark ? Qt.rgba(1, 1, 1, 0.55) : Qt.rgba(0, 0, 0, 0.5)
    readonly property color runningColor: dark ? "#30d158" : "#28cd41"
    readonly property color failedColor: dark ? "#ff453a" : "#ff3b30"
    readonly property color stoppedColor: dark ? "#ff9f0a" : "#ff9500"

    readonly property real titleBarHeight: Math.max(control.SafeArea.margins.top, 28)
    readonly property string homePath: StandardPaths.writableLocation(StandardPaths.HomeLocation).toString().replace(/^file:\/\//, "")

    // Ticks while something runs, so the elapsed times stay current.
    property date now: new Date()

    function present(section) {
        if (section !== undefined)
            control.selectedSection = section;
        control.show();
        control.raise();
        control.requestActivate();
    }

    // Every pane running an AI agent, in every window, the ones that need
    // the user first (waiting, failed), then working, done, unreported.
    function agentSessions(): var {
        const urgency = {
            waiting: 4,
            error: 3,
            working: 2,
            idle: 1
        };
        const rows = [];
        control.windowManager.windows.forEach(window => {
            const manager = window.terminalManager;
            manager.workspaces.forEach(workspace => {
                if (!workspace.rootNode)
                    return;
                manager.workspaceSessions(workspace).forEach(session => {
                    if (session.agentState !== "" || session.processKind === "agent")
                        rows.push({
                            window: window,
                            manager: manager,
                            workspace: workspace,
                            session: session,
                            urgency: urgency[session.agentState] || 0
                        });
                });
            });
        });
        return rows.sort((a, b) => b.urgency - a.urgency);
    }

    function agentStateText(state: string): string {
        switch (state) {
        case "waiting":
            return qsTr("Waiting for you");
        case "error":
            return qsTr("Failed");
        case "working":
            return qsTr("Working");
        case "idle":
            return qsTr("Done");
        }
        return qsTr("Running");
    }

    function agentStateColor(state: string): color {
        switch (state) {
        case "waiting":
            return control.stoppedColor;
        case "error":
            return control.failedColor;
        case "working":
            return control.palette.accent;
        }
        return control.palette.windowText;
    }

    function shortPath(path: string): string {
        if (path === control.homePath)
            return "~";
        if (path.startsWith(control.homePath + "/"))
            return "~" + path.slice(control.homePath.length);
        return path;
    }

    function formatDuration(ms: real): string {
        const total = Math.max(0, Math.floor(ms / 1000));
        const hours = Math.floor(total / 3600);
        const minutes = Math.floor((total % 3600) / 60);
        const seconds = total % 60;
        if (hours > 0)
            return qsTr("%1h %2m").arg(hours).arg(String(minutes).padStart(2, "0"));
        if (minutes > 0)
            return qsTr("%1m %2s").arg(minutes).arg(String(seconds).padStart(2, "0"));
        return qsTr("%1s").arg(seconds);
    }

    // A multi-line command is shown by its first line.
    function firstLine(text: string): string {
        const lines = text.split("\n").filter(line => line.trim() !== "");
        if (lines.length === 0)
            return "";
        return lines.length > 1 ? lines[0].trim() + " …" : lines[0].trim();
    }

    function jobStatus(job: BackgroundJob): string {
        const end = job.running ? control.now : job.finishedAt;
        const elapsed = control.formatDuration(end - job.startedAt);
        switch (job.state) {
        case "running":
            return qsTr("Running for %1").arg(elapsed);
        case "finished":
            return job.command !== "" ? qsTr("Finished after %1").arg(elapsed) : qsTr("Ended after %1").arg(elapsed);
        case "failed":
            return job.exitCode >= 0 ? qsTr("Failed with exit status %1 after %2").arg(job.exitCode).arg(elapsed) : qsTr("Failed after %1").arg(elapsed);
        default:
            return qsTr("Stopped after %1").arg(elapsed);
        }
    }

    function jobDetail(job: BackgroundJob): string {
        const parts = [control.jobStatus(job)];
        if (job.command !== "" && job.name !== "")
            parts.push(control.firstLine(job.command));
        if (job.directory !== "")
            parts.push(control.shortPath(job.directory));
        return parts.join("  ·  ");
    }

    function stateColor(state: string): color {
        switch (state) {
        case "running":
            return control.runningColor;
        case "failed":
            return control.failedColor;
        case "stopped":
            return control.stoppedColor;
        default:
            return control.secondaryTextColor;
        }
    }

    // A terminal color (0xAARRGGBB) as a QML color.
    function terminalColor(argb): color {
        return Qt.rgba(((argb >> 16) & 255) / 255, ((argb >> 8) & 255) / 255, (argb & 255) / 255, 1);
    }

    function runCommand(command: string, directory: string, title: string) {
        const job = BackgroundJobs.run(command, directory, title);
        if (job) {
            control.selectedJob = job;
            control.selectedSection = control.runningSection;
        }
    }

    function runSaved(index: int) {
        const job = BackgroundJobs.runSaved(index);
        if (job) {
            control.selectedJob = job;
            control.selectedSection = control.runningSection;
        }
    }

    // Shows a running job as a tab of the last active window (a new window
    // when there is none) and brings that window forward.
    function openInTab(job: BackgroundJob) {
        let target = control.windowManager.lastActiveWindow();
        const created = target === null;
        if (created)
            target = control.windowManager.openWindow("", true);
        if (!BackgroundJobs.openInWindow(job, target.terminalManager)) {
            if (created)
                target.close();
            return;
        }
        target.raise();
        target.requestActivate();
    }

    function openEditor(index: int, name: string, command: string, directory: string) {
        control.editingIndex = index;
        nameField.text = name;
        commandField.text = command;
        directoryField.text = directory;
        control.editorOpen = true;
        control.selectedSection = control.savedSection;
        commandField.focusEditor();
    }

    function saveEditor(): bool {
        const command = commandField.text;
        if (command.trim() === "")
            return false;
        if (control.editingIndex >= 0)
            BackgroundJobs.updateSavedCommand(control.editingIndex, nameField.text, command, directoryField.text);
        else
            BackgroundJobs.saveCommand(nameField.text, command, directoryField.text);
        control.editorOpen = false;
        return true;
    }

    function openFunctionEditor(index: int, name: string, description: string, body: string) {
        control.functionEditingIndex = index;
        functionNameField.text = name;
        functionDescriptionField.text = description;
        functionBodyField.text = body;
        control.functionError = "";
        control.functionEditorOpen = true;
        control.selectedSection = control.functionsSection;
        if (name === "")
            functionNameField.forceActiveFocus();
        else
            functionBodyField.focusEditor();
    }

    function saveFunctionEditor(): bool {
        control.functionError = ShellFunctions.saveFunction(control.functionEditingIndex, functionNameField.text, functionDescriptionField.text, functionBodyField.text);
        if (control.functionError !== "")
            return false;
        control.functionEditorOpen = false;
        return true;
    }

    title: control.sections[control.selectedSection].title

    // Full-size content under a transparent title bar, like Settings, but
    // resizable: the output pane wants the room.
    flags: Qt.Window | Qt.CustomizeWindowHint | Qt.WindowTitleHint | Qt.WindowCloseButtonHint | Qt.WindowMinimizeButtonHint | Qt.WindowMaximizeButtonHint | Qt.ExpandedClientAreaHint | Qt.NoTitleBarBackgroundHint

    width: 900
    height: 620
    minimumWidth: 760
    minimumHeight: 500

    color: "transparent"

    onSelectedSectionChanged: {
        if (control.selectedSection !== control.savedSection)
            control.editorOpen = false;
        if (control.selectedSection !== control.functionsSection)
            control.functionEditorOpen = false;
    }

    Timer {
        interval: 1000
        repeat: true
        running: control.visible && BackgroundJobs.runningCount > 0
        onTriggered: control.now = new Date()
    }

    WindowTranslucency {
        window: control
        color: control.dark ? 0xff1e1e1e : 0xffe8e8e8
        alpha: 0.9
        blurEnabled: true
    }

    // While this window is key, AppKit uses its menu bar, so the key
    // equivalents act on it instead of a terminal window.
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
        id: settingsAction

        text: qsTr("Settings…")
        shortcut: "Ctrl+,"
        onTriggered: control.windowManager.settingsWindow.present()
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

            MenuItem {
                action: settingsAction
            }

            MenuSeparator {}

            MenuItem {
                action: quitAction
            }
        }

        Menu {
            title: qsTr("Window")

            MenuItem {
                action: newWindowAction
            }

            MenuItem {
                action: minimizeAction
            }

            MenuItem {
                action: closeAction
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
                    badge: index === control.runningSection && BackgroundJobs.runningCount > 0 ? String(BackgroundJobs.runningCount) : ""
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
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: control.titleBarHeight
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                Layout.bottomMargin: 16

                Label {
                    Layout.fillWidth: true

                    text: control.sections[control.selectedSection].title
                    font.pointSize: 15
                    font.weight: Font.Bold
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }

                Button {
                    visible: control.selectedSection === control.runningSection && BackgroundJobs.jobs.length > BackgroundJobs.runningCount

                    text: qsTr("Remove Ended")
                    flat: true
                    onClicked: BackgroundJobs.removeEnded()
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24

                currentIndex: control.selectedSection

                // Running: start a command, the jobs, the selected one's output.
                ColumnLayout {
                    spacing: 10

                    Controls.FormGroup {
                        Item {
                            Layout.fillWidth: true
                            implicitHeight: quickRow.implicitHeight + 16

                            RowLayout {
                                id: quickRow

                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10

                                spacing: 8

                                // Return runs; Shift-Return starts another line.
                                TextArea {
                                    id: quickCommand

                                    function submit(event) {
                                        if (event.modifiers & Qt.ShiftModifier) {
                                            event.accepted = false;
                                            return;
                                        }
                                        event.accepted = true;
                                        runButton.clicked();
                                    }

                                    Layout.fillWidth: true

                                    placeholderText: qsTr("Command to run in the background (⇧↩ for a new line)")
                                    font.family: "Menlo"
                                    wrapMode: TextEdit.NoWrap
                                    Keys.onReturnPressed: event => quickCommand.submit(event)
                                    Keys.onEnterPressed: event => quickCommand.submit(event)
                                }

                                Button {
                                    text: qsTr("Save…")
                                    enabled: quickCommand.text.trim() !== ""
                                    onClicked: control.openEditor(-1, "", quickCommand.text, "")
                                }

                                Button {
                                    id: runButton

                                    text: qsTr("Run")
                                    highlighted: true
                                    enabled: quickCommand.text.trim() !== ""
                                    onClicked: {
                                        if (!enabled)
                                            return;
                                        control.runCommand(quickCommand.text, "", "");
                                        quickCommand.clear();
                                    }
                                }
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10
                        visible: BackgroundJobs.jobs.length === 0

                        text: qsTr("Nothing runs in the background. Run a command above, start a saved one, or send a tab here with “Move to Background” in its menu.")
                        font.pointSize: 11
                        color: control.secondaryTextColor
                        wrapMode: Text.Wrap
                    }

                    Controls.FormGroup {
                        visible: BackgroundJobs.jobs.length > 0

                        ListView {
                            id: jobList

                            Layout.fillWidth: true
                            Layout.preferredHeight: Math.min(contentHeight, control.selectedJob ? 160 : 400)

                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            model: BackgroundJobs.jobs

                            ScrollBar.vertical: ScrollBar {}

                            delegate: Controls.FormRow {
                                id: jobRow

                                required property BackgroundJob modelData
                                required property int index

                                width: ListView.view.width

                                // Null while a removed job's row is torn down.
                                label: modelData ? modelData.title : ""
                                detail: modelData ? control.jobDetail(modelData) : ""
                                showSeparator: index > 0
                                selectable: true
                                highlighted: control.selectedJob === modelData
                                onClicked: control.selectedJob = control.selectedJob === modelData ? null : modelData

                                leading: [
                                    StatusDot {
                                        jobState: jobRow.modelData ? jobRow.modelData.state : ""
                                    }
                                ]

                                Button {
                                    visible: jobRow.modelData !== null && jobRow.modelData.running

                                    text: qsTr("Stop")
                                    flat: true
                                    onClicked: BackgroundJobs.stop(jobRow.modelData)
                                }

                                Button {
                                    visible: jobRow.modelData !== null && !jobRow.modelData.running

                                    text: qsTr("Remove")
                                    flat: true
                                    onClicked: BackgroundJobs.remove(jobRow.modelData)
                                }
                            }
                        }
                    }

                    // The selected job's live terminal, read-only.
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: control.selectedJob !== null

                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true

                            spacing: 8

                            Label {
                                Layout.fillWidth: true

                                text: control.selectedJob ? control.selectedJob.title : ""
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }

                            Button {
                                visible: control.selectedJob !== null && control.selectedJob.running

                                text: qsTr("Interrupt")
                                ToolTip.visible: hovered
                                ToolTip.delay: 600
                                ToolTip.text: qsTr("Send Control-C to the program")
                                onClicked: BackgroundJobs.interrupt(control.selectedJob)
                            }

                            Button {
                                visible: control.selectedJob !== null && !control.selectedJob.running && control.selectedJob.command !== ""

                                text: qsTr("Run Again")
                                onClicked: {
                                    const job = control.selectedJob;
                                    control.runCommand(job.command, job.directory, job.name);
                                }
                            }

                            Button {
                                enabled: outputPane.session !== null

                                text: qsTr("Save Output…")
                                onClicked: saveOutputDialog.openFor(outputPane.session, null)
                            }

                            Button {
                                enabled: control.selectedJob !== null && control.selectedJob.running

                                text: qsTr("Open in Tab")
                                highlighted: true
                                onClicked: control.openInTab(control.selectedJob)
                            }
                        }

                        Rectangle {
                            id: outputPane

                            readonly property var session: control.selectedJob ? control.selectedJob.session : null

                            // Fits the session to the pane while it is shown,
                            // like a tab would.
                            function fitSession() {
                                if (!outputPane.session || !control.selectedJob.running || !outputPane.visible || renderer.cellWidth <= 0 || renderer.cellHeight <= 0)
                                    return;
                                const columns = Math.floor(renderer.width / renderer.cellWidth);
                                const rows = Math.floor(renderer.height / renderer.cellHeight);
                                if (columns >= 20 && rows >= 5)
                                    outputPane.session.resize(columns, rows);
                            }

                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            radius: 8
                            clip: true
                            color: session ? control.terminalColor(session.backgroundColor) : "black"
                            border.width: 1
                            border.color: control.separatorColor

                            onWidthChanged: fitTimer.restart()
                            onHeightChanged: fitTimer.restart()
                            onSessionChanged: fitTimer.restart()
                            onVisibleChanged: fitTimer.restart()

                            Timer {
                                id: fitTimer

                                interval: 80
                                onTriggered: outputPane.fitSession()
                            }

                            TerminalRenderer {
                                id: renderer

                                anchors.fill: parent
                                anchors.margins: 8

                                session: outputPane.session
                                font: Qt.font({
                                    family: control.preferences.fontFamily,
                                    pointSize: 12,
                                    weight: control.preferences.fontWeight
                                })
                                defaultCursorShape: control.preferences.cursorShape
                                defaultCursorBlink: false
                                accentColor: control.palette.highlight
                                selectedTextColor: control.palette.highlightedText
                                hoverRow: -1

                                onCellSizeChanged: fitTimer.restart()
                            }

                            WheelHandler {
                                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                                onWheel: event => {
                                    if (!outputPane.session)
                                        return;
                                    const steps = event.pixelDelta.y !== 0 ? event.pixelDelta.y / Math.max(1, renderer.cellHeight) : event.angleDelta.y / 40;
                                    const lines = steps > 0 ? Math.ceil(steps) : Math.floor(steps);
                                    if (lines !== 0)
                                        outputPane.session.scrollBy(lines);
                                }
                            }
                        }
                    }

                    Item {
                        Layout.fillHeight: true
                        visible: control.selectedJob === null
                    }
                }

                // Saved Commands: ready to start with one click.
                ScrollView {
                    id: savedScroll

                    contentWidth: availableWidth

                    ColumnLayout {
                        width: savedScroll.availableWidth

                        spacing: 10

                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: 10
                            Layout.rightMargin: 10
                            visible: BackgroundJobs.savedCommands.length === 0 && !control.editorOpen

                            text: qsTr("Save the commands you start often — a dev server, a build watcher, a tunnel — and run them in the background with one click.")
                            font.pointSize: 11
                            color: control.secondaryTextColor
                            wrapMode: Text.Wrap
                        }

                        Controls.FormGroup {
                            visible: BackgroundJobs.savedCommands.length > 0

                            Repeater {
                                model: BackgroundJobs.savedCommands

                                delegate: Controls.FormRow {
                                    id: savedRow

                                    required property var modelData
                                    required property int index

                                    label: modelData.name !== "" ? modelData.name : control.firstLine(modelData.command)
                                    detail: [modelData.name !== "" ? control.firstLine(modelData.command) : "", control.shortPath(modelData.directory !== "" ? modelData.directory : "~")].filter(part => part !== "").join("  ·  ")
                                    monospaceDetail: true
                                    showSeparator: index > 0
                                    highlighted: control.editorOpen && control.editingIndex === index

                                    Button {
                                        text: qsTr("Edit")
                                        flat: true
                                        onClicked: control.openEditor(savedRow.index, savedRow.modelData.name, savedRow.modelData.command, savedRow.modelData.directory)
                                    }

                                    Button {
                                        text: qsTr("Delete")
                                        flat: true
                                        onClicked: {
                                            if (control.editorOpen && control.editingIndex === savedRow.index)
                                                control.editorOpen = false;
                                            BackgroundJobs.removeSavedCommand(savedRow.index);
                                        }
                                    }

                                    Button {
                                        text: qsTr("Run")
                                        highlighted: true
                                        onClicked: control.runSaved(savedRow.index)
                                    }
                                }
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: 10
                            Layout.topMargin: 6
                            visible: control.editorOpen

                            text: control.editingIndex >= 0 ? qsTr("Edit Command") : qsTr("New Command")
                            font.weight: Font.DemiBold
                        }

                        Controls.FormGroup {
                            visible: control.editorOpen

                            Controls.FormRow {
                                label: qsTr("Name")
                                detail: qsTr("Optional; the command is shown when empty")
                                showSeparator: false

                                TextField {
                                    id: nameField

                                    Layout.preferredWidth: 300
                                    placeholderText: qsTr("Dev server")
                                    onAccepted: control.saveEditor()
                                }
                            }

                            CodeField {
                                id: commandField

                                label: qsTr("Command")
                                detail: qsTr("One or more lines, run by your shell (%1) in the background; ⌘↩ saves").arg(ShellFunctions.shellName)
                                placeholderText: "cd ~/project\nnpm install\nnpm run dev"
                                lines: 5
                                onSubmitted: control.saveEditor()
                            }

                            Controls.FormRow {
                                label: qsTr("Directory")
                                detail: qsTr("Where it starts; your home folder when empty")

                                TextField {
                                    id: directoryField

                                    Layout.preferredWidth: 210
                                    placeholderText: "~"
                                    onAccepted: control.saveEditor()
                                }

                                Button {
                                    text: qsTr("Choose…")
                                    onClicked: folderDialog.open()
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true

                            spacing: 8

                            Button {
                                visible: !control.editorOpen

                                text: qsTr("Add Command…")
                                onClicked: control.openEditor(-1, "", "", "")
                            }

                            Item {
                                Layout.fillWidth: true
                            }

                            Button {
                                visible: control.editorOpen

                                text: qsTr("Cancel")
                                onClicked: control.editorOpen = false
                            }

                            Button {
                                visible: control.editorOpen
                                enabled: commandField.text.trim() !== ""

                                text: qsTr("Save and Run")
                                onClicked: {
                                    const command = commandField.text;
                                    const directory = directoryField.text;
                                    const name = nameField.text.trim();
                                    if (control.saveEditor())
                                        control.runCommand(command, directory, name);
                                }
                            }

                            Button {
                                visible: control.editorOpen
                                enabled: commandField.text.trim() !== ""

                                text: qsTr("Save")
                                highlighted: true
                                onClicked: control.saveEditor()
                            }
                        }
                    }
                }

                // Functions: shell functions defined here instead of an rc
                // file, available in every tab and background command.
                ScrollView {
                    id: functionsScroll

                    contentWidth: availableWidth

                    ColumnLayout {
                        width: functionsScroll.availableWidth

                        spacing: 10

                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: 10
                            Layout.rightMargin: 10
                            visible: ShellFunctions.functions.length === 0 && !control.functionEditorOpen

                            text: qsTr("Define shell functions here instead of in your shell's startup file. Every tab can call them — open ones from their next prompt — and so can background and saved commands.")
                            font.pointSize: 11
                            color: control.secondaryTextColor
                            wrapMode: Text.Wrap
                        }

                        Controls.FormGroup {
                            visible: ShellFunctions.functions.length > 0

                            Repeater {
                                model: ShellFunctions.functions

                                delegate: Controls.FormRow {
                                    id: functionRow

                                    required property var modelData
                                    required property int index

                                    label: modelData.name
                                    detail: modelData.description !== "" ? modelData.description : control.firstLine(modelData.body)
                                    monospaceDetail: modelData.description === ""
                                    showSeparator: index > 0
                                    highlighted: control.functionEditorOpen && control.functionEditingIndex === index

                                    Button {
                                        text: qsTr("Edit")
                                        flat: true
                                        onClicked: control.openFunctionEditor(functionRow.index, functionRow.modelData.name, functionRow.modelData.description, functionRow.modelData.body)
                                    }

                                    Button {
                                        text: qsTr("Delete")
                                        flat: true
                                        onClicked: {
                                            if (control.functionEditorOpen && control.functionEditingIndex === functionRow.index)
                                                control.functionEditorOpen = false;
                                            control.functionError = ShellFunctions.removeFunction(functionRow.index);
                                        }
                                    }

                                    Button {
                                        text: qsTr("Run in Background")
                                        highlighted: true
                                        onClicked: control.runCommand(functionRow.modelData.name, "", "")
                                    }
                                }
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: 10
                            Layout.topMargin: 6
                            visible: control.functionEditorOpen

                            text: control.functionEditingIndex >= 0 ? qsTr("Edit Function") : qsTr("New Function")
                            font.weight: Font.DemiBold
                        }

                        Controls.FormGroup {
                            visible: control.functionEditorOpen

                            Controls.FormRow {
                                label: qsTr("Name")
                                detail: qsTr("Call it by this name in any tab")
                                showSeparator: false

                                TextField {
                                    id: functionNameField

                                    Layout.preferredWidth: 300
                                    placeholderText: "my_function"
                                    font.family: "Menlo"
                                    onTextEdited: control.functionError = ""
                                }
                            }

                            Controls.FormRow {
                                label: qsTr("Description")
                                detail: qsTr("Optional; shown in this list")

                                TextField {
                                    id: functionDescriptionField

                                    Layout.preferredWidth: 300
                                }
                            }

                            CodeField {
                                id: functionBodyField

                                label: qsTr("Body")
                                detail: qsTr("%1 code; the arguments are $1, $2… and $@. ⌘↩ saves").arg(ShellFunctions.shellName)
                                lines: 12
                                onSubmitted: control.saveFunctionEditor()
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: 10
                            Layout.rightMargin: 10
                            visible: control.functionError !== ""

                            text: control.functionError
                            font.pointSize: 11
                            color: control.failedColor
                            wrapMode: Text.Wrap
                        }

                        RowLayout {
                            Layout.fillWidth: true

                            spacing: 8

                            Button {
                                visible: !control.functionEditorOpen

                                text: qsTr("Add Function…")
                                onClicked: control.openFunctionEditor(-1, "", "", "")
                            }

                            Item {
                                Layout.fillWidth: true
                            }

                            Button {
                                visible: control.functionEditorOpen

                                text: qsTr("Cancel")
                                onClicked: {
                                    control.functionEditorOpen = false;
                                    control.functionError = "";
                                }
                            }

                            Button {
                                visible: control.functionEditorOpen
                                enabled: functionNameField.text.trim() !== ""

                                text: qsTr("Save")
                                highlighted: true
                                onClicked: control.saveFunctionEditor()
                            }
                        }
                    }
                }

                // Sessions: every pane of every window, to find one or send
                // it to the background.
                ScrollView {
                    id: sessionsScroll

                    contentWidth: availableWidth

                    ColumnLayout {
                        width: sessionsScroll.availableWidth

                        spacing: 6

                        Repeater {
                            model: control.windowManager.windows

                            delegate: ColumnLayout {
                                id: windowGroup

                                required property TerminalWindow modelData
                                required property int index
                                readonly property TerminalManager manager: modelData.terminalManager

                                Layout.fillWidth: true
                                Layout.bottomMargin: 10

                                spacing: 6

                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 10

                                    text: qsTr("Window %1").arg(windowGroup.index + 1)
                                    font.pointSize: 11
                                    font.weight: Font.DemiBold
                                    color: control.secondaryTextColor
                                    elide: Text.ElideRight
                                }

                                Controls.FormGroup {
                                    Repeater {
                                        model: windowGroup.manager.workspaces

                                        delegate: ColumnLayout {
                                            id: workspaceGroup

                                            required property var modelData
                                            required property int index

                                            Layout.fillWidth: true

                                            spacing: 0

                                            Repeater {
                                                // rootNode makes the list follow splits and closed panes.
                                                model: workspaceGroup.modelData.rootNode ? windowGroup.manager.workspaceSessions(workspaceGroup.modelData) : []

                                                delegate: Controls.FormRow {
                                                    id: sessionRow

                                                    required property var modelData
                                                    required property int index

                                                    label: modelData.displayTitle
                                                    // The directory only when the title is not already it.
                                                    detail: [workspaceGroup.modelData.customTitle, modelData.currentDirectory !== modelData.displayTitle ? control.shortPath(modelData.currentDirectory) : ""].filter(part => part !== "").join("  ·  ")
                                                    showSeparator: workspaceGroup.index > 0 || index > 0

                                                    leading: [
                                                        IconImage {
                                                            name: sessionRow.modelData.processKind !== "" ? "kind-" + sessionRow.modelData.processKind : "kind-shell"
                                                            sourceSize: Qt.size(16, 16)
                                                            color: control.palette.windowText
                                                        }
                                                    ]

                                                    Button {
                                                        text: qsTr("Show")
                                                        flat: true
                                                        onClicked: {
                                                            windowGroup.manager.activateSession(sessionRow.modelData);
                                                            windowGroup.modelData.raise();
                                                            windowGroup.modelData.requestActivate();
                                                        }
                                                    }

                                                    Button {
                                                        text: qsTr("Move to Background")
                                                        onClicked: {
                                                            const manager = windowGroup.manager;
                                                            const session = sessionRow.modelData;
                                                            // Deferred: the move rebuilds this list.
                                                            Qt.callLater(() => BackgroundJobs.sendSession(manager, session));
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: 10
                            Layout.rightMargin: 10

                            text: qsTr("A session moved to the background keeps running; open it again as a tab from Running.")
                            font.pointSize: 11
                            color: control.secondaryTextColor
                            wrapMode: Text.Wrap
                        }
                    }
                }

                // AI agents: every pane running one, what it is doing, and
                // a way to jump to it.
                ScrollView {
                    id: agentsScroll

                    contentWidth: availableWidth

                    ColumnLayout {
                        width: agentsScroll.availableWidth

                        spacing: 6

                        Controls.FormGroup {
                            visible: agentRepeater.count > 0

                            Repeater {
                                id: agentRepeater

                                // Re-evaluated as panes, states and windows change.
                                model: control.agentSessions()

                                delegate: Controls.FormRow {
                                    id: agentRow

                                    required property var modelData
                                    required property int index

                                    readonly property string agentState: modelData.session.agentState

                                    label: control.agentStateText(agentState) + (modelData.session.agentName !== "" ? "  ·  " + modelData.session.agentName : "  ·  " + modelData.session.foregroundProcess)
                                    detail: [modelData.workspace.title, modelData.session.agentMessage].filter(part => part !== "").join("  ·  ")
                                    showSeparator: index > 0

                                    leading: [
                                        IconImage {
                                            name: agentRow.agentState === "" || agentRow.agentState === "working" ? "kind-agent" : "agent-" + agentRow.agentState
                                            sourceSize: Qt.size(16, 16)
                                            color: control.agentStateColor(agentRow.agentState)
                                        }
                                    ]

                                    Button {
                                        text: qsTr("Show")
                                        highlighted: agentRow.agentState === "waiting"
                                        onClicked: {
                                            agentRow.modelData.manager.activateSession(agentRow.modelData.session);
                                            agentRow.modelData.window.raise();
                                            agentRow.modelData.window.requestActivate();
                                        }
                                    }
                                }
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: 10
                            Layout.rightMargin: 10

                            visible: agentRepeater.count === 0
                            text: qsTr("No AI agent is running in Konsoll.")
                            color: control.secondaryTextColor
                            wrapMode: Text.Wrap
                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: 10
                            Layout.rightMargin: 10

                            text: qsTr("Agents connected in Settings ▸ AI Agents report whether they are working, waiting for you, done or failed; others show as running.")
                            font.pointSize: 11
                            color: control.secondaryTextColor
                            wrapMode: Text.Wrap
                        }

                        Button {
                            Layout.leftMargin: 6

                            text: qsTr("Open AI Agents Settings…")
                            flat: true
                            onClicked: control.windowManager.settingsWindow.present(control.windowManager.settingsWindow.agentsSection)
                        }
                    }
                }
            }
        }
    }

    // A labelled multi-line code editor row for a FormGroup (monospace, no
    // wrapping, Tab indents); Cmd+Return emits `submitted`.
    component CodeField: Item {
        id: codeField

        property alias text: codeArea.text
        property alias placeholderText: codeArea.placeholderText
        property string label
        property string detail
        property int lines: 6

        signal submitted

        function focusEditor() {
            codeArea.forceActiveFocus();
        }

        Layout.fillWidth: true
        implicitHeight: codeColumn.implicitHeight + 20

        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            height: 1

            color: control.separatorColor
        }

        FontMetrics {
            id: codeMetrics

            font: codeArea.font
        }

        ColumnLayout {
            id: codeColumn

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 10
            anchors.leftMargin: 12
            anchors.rightMargin: 12

            spacing: 6

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1

                Label {
                    Layout.fillWidth: true
                    text: codeField.label
                    elide: Text.ElideRight
                }

                Label {
                    Layout.fillWidth: true
                    visible: codeField.detail !== ""
                    text: codeField.detail
                    font.pointSize: 11
                    color: control.secondaryTextColor
                    elide: Text.ElideRight
                }
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: codeField.lines * codeMetrics.lineSpacing + codeArea.topPadding + codeArea.bottomPadding

                TextArea {
                    id: codeArea

                    font.family: "Menlo"
                    font.pointSize: 12
                    wrapMode: TextEdit.NoWrap
                    tabStopDistance: 4 * codeMetrics.advanceWidth(" ")
                    Keys.onReturnPressed: event => {
                        if (event.modifiers & Qt.ControlModifier) {
                            event.accepted = true;
                            codeField.submitted();
                        } else {
                            event.accepted = false;
                        }
                    }
                }
            }
        }
    }

    // Colored status mark of a job.
    component StatusDot: Rectangle {
        property string jobState

        Layout.alignment: Qt.AlignVCenter
        implicitWidth: 8
        implicitHeight: 8
        radius: 4

        color: control.stateColor(jobState)
    }

    SaveTextDialog {
        id: saveOutputDialog

        title: qsTr("Save Command Output")
    }

    FolderDialog {
        id: folderDialog

        title: qsTr("Choose the Command's Directory")
        onAccepted: directoryField.text = control.shortPath(decodeURIComponent(selectedFolder.toString().replace(/^file:\/\//, "")))
    }
}
