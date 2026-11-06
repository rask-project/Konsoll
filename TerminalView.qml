import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtCanvas2D

import "controls" as Controls

Item {
    id: view

    required property font terminalFont
    required property TerminalManager sessionManager

    readonly property real terminalFontSize: terminalFont.pixelSize > 0 ? terminalFont.pixelSize : terminalFont.pointSize * (4 / 3)
    readonly property real cellWidth: terminalFontSize * 0.6
    readonly property real cellHeight: terminalFontSize * 1.2
    readonly property real canvasInset: 5
    readonly property string nerdFont: "'JetBrainsMono Nerd Font Mono'"
    readonly property string fontFamily: "'" + terminalFont.family.replace(/'/g, "\\'") + "', " + nerdFont
    readonly property int cols: Math.max(2, Math.floor((width - 2 * canvasInset) / cellWidth))
    readonly property int rows: Math.max(2, Math.floor((height - 2 * canvasInset) / cellHeight))
    readonly property color systemBackground: appPalette.window
    readonly property color systemForeground: appPalette.windowText
    readonly property int totalLines: view.session ? view.session.maxScrollOffset + view.session.rows : 0

    property var session: null
    property bool active: false
    property string transferError: ""
    property real scrollAccumulator: 0
    property bool _syncingScrollBar: false

    onSystemBackgroundChanged: applySystemColors()
    onSystemForegroundChanged: applySystemColors()

    onColsChanged: scheduleSessionResize()
    onRowsChanged: scheduleSessionResize()
    onTerminalFontChanged: canvas.requestPaint()
    onSessionChanged: {
        if (session) {
            syncSessionSize();
            applySystemColors();
        }
    }
    onVisibleChanged: scheduleSessionResize()
    onActiveChanged: focusInputIfActive()

    SystemPalette {
        id: appPalette
        colorGroup: SystemPalette.Active
    }

    Timer {
        id: resizeTimer

        interval: 0
        repeat: false
        onTriggered: view.syncSessionSize()
    }

    Component.onCompleted: focusInputIfActive()

    function focusInputIfActive() {
        if (active) {
            Qt.callLater(() => {
                if (view.active)
                    input.forceActiveFocus();
            });
        }
    }

    function applySystemColors() {
        if (session)
            session.setSystemColors(systemBackground, systemForeground);
    }

    function syncSessionSize() {
        if (session && width >= cellWidth * 2 + 2 * canvasInset && height >= cellHeight * 2 + 2 * canvasInset)
            session.resize(cols, rows);
    }

    function scheduleSessionResize() {
        if (session && visible)
            resizeTimer.restart();
    }

    // Invisible focus target that receives composed text from the macOS text
    // input system (dead keys / accents). Special keys and Cmd/Option/Ctrl
    // combos are intercepted here and sent to the backend; printable
    // characters are let through so the TextInput composes them, then
    // onTextChanged forwards and clears.
    TextInput {
        id: input

        visible: false

        x: 0
        y: 0

        width: 1
        height: 1

        color: "transparent"
        cursorVisible: false
        focus: view.active

        property bool _clearing: false

        readonly property var specialKeys: [Qt.Key_Return, Qt.Key_Enter, Qt.Key_Backspace, Qt.Key_Tab, Qt.Key_Escape, Qt.Key_Up, Qt.Key_Down, Qt.Key_Left, Qt.Key_Right, Qt.Key_Home, Qt.Key_End, Qt.Key_Delete, Qt.Key_PageUp, Qt.Key_PageDown]

        Keys.onPressed: event => {
            let linkModifiers = event.modifiers;
            if (event.key === Qt.Key_Control)
                linkModifiers |= Qt.ControlModifier;
            if (event.key === Qt.Key_Meta)
                linkModifiers |= Qt.MetaModifier;
            selectionArea.updateLinkHover(linkModifiers);
            if (!view.active) {
                event.accepted = true;
                return;
            }
            const hasCommand = (event.modifiers & Qt.ControlModifier) !== 0;
            const hasOption = (event.modifiers & Qt.AltModifier) !== 0;
            const hasPhysicalControl = (event.modifiers & Qt.MetaModifier) !== 0;
            const isSpecialKey = specialKeys.includes(event.key);
            if (hasCommand || hasOption || hasPhysicalControl || isSpecialKey) {
                if (hasCommand && event.key === Qt.Key_F) {
                    searchBar.openSearch();
                    event.accepted = true;
                    return;
                }
                if (view.session)
                    view.sessionManager.routeKeyPress(view.session, event.key, event.text, event.modifiers);
                event.accepted = true;
            }
        }

        Keys.onReleased: event => {
            let linkModifiers = event.modifiers;
            if (event.key === Qt.Key_Control)
                linkModifiers &= ~Qt.ControlModifier;
            if (event.key === Qt.Key_Meta)
                linkModifiers &= ~Qt.MetaModifier;
            selectionArea.updateLinkHover(linkModifiers);
        }

        onTextChanged: {
            if (_clearing)
                return;
            if (!view.active) {
                _clearing = true;
                text = "";
                _clearing = false;
                return;
            }
            if (text.length > 0) {
                if (view.session)
                    view.sessionManager.routeTextInput(view.session, text);
                _clearing = true;
                text = "";
                _clearing = false;
            }
        }
    }

    DropArea {
        anchors.fill: parent

        onDropped: drop => {
            if (!view.session || !drop.hasUrls)
                return;
            for (let i = 0; i < drop.urls.length; ++i)
                view.session.transferFile(drop.urls[i]);
            drop.acceptProposedAction();
        }
    }

    Connections {
        target: view.session

        function onTransferFailed(message) {
            view.transferError = message;
            transferErrorTimer.restart();
        }
    }

    Rectangle {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 14
        z: 10
        visible: view.transferError.length > 0
        radius: 6
        color: "#cc7f1d1d"
        implicitWidth: transferErrorText.implicitWidth + 24
        implicitHeight: transferErrorText.implicitHeight + 16

        Text {
            id: transferErrorText
            anchors.centerIn: parent
            text: view.transferError
            color: "white"
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
        }
    }

    Timer {
        id: transferErrorTimer
        interval: 6000
        onTriggered: view.transferError = ""
    }

    WheelHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad

        onWheel: event => {
            // Trackpads report high-resolution pixel deltas; mouse wheels
            // generally report angle deltas. Prefer pixels when available.
            const pixelDelta = event.pixelDelta.y;
            const delta = pixelDelta !== 0 ? pixelDelta / view.cellHeight : event.angleDelta.y / 40;
            if (delta !== 0 && view.session) {
                if (view.session.mouseTracking) {
                    const p = selectionArea.mapFromItem(view, event.x, event.y);
                    const cell = selectionArea.cellAt(p.x, p.y);
                    view.session.mouseWheel(delta > 0, cell[1], cell[0], event.modifiers);
                } else {
                    view.scrollAccumulator += delta;
                    const step = Math.trunc(view.scrollAccumulator);
                    if (step !== 0) {
                        view.session.scrollBy(step);
                        view.scrollAccumulator -= step;
                    }
                }
            }
            event.accepted = true;
        }
    }

    // `cells` is the number of terminal cells in the run (not text.length,
    // which counts UTF-16 units and over-counts supplementary-plane glyphs
    // like Nerd Font icons that occupy one cell each).
    function drawRun(ctx, text, startCol, y, fg, flags, cells) {
        if (!text || (flags & 32) !== 0)
            return;
        ctx.fillStyle = Qt.rgba(((fg >> 16) & 255) / 255, ((fg >> 8) & 255) / 255, (fg & 255) / 255, 1);
        const weight = (flags & 1) ? Math.max(700, view.terminalFont.weight) : view.terminalFont.weight;
        ctx.font = (view.terminalFont.italic ? "italic " : "") + weight + " " + view.terminalFontSize + "px " + view.fontFamily;
        ctx.fillText(text, startCol * view.cellWidth, y);
        if (flags & 4)
            ctx.fillRect(startCol * view.cellWidth, y + view.cellHeight - 1, cells * view.cellWidth, 1);
        if (flags & 8)
            ctx.fillRect(startCol * view.cellWidth, y + Math.floor(view.cellHeight / 2), cells * view.cellWidth, 1);
    }

    Canvas2D {
        id: canvas

        anchors.fill: parent
        anchors.margins: view.canvasInset
        fillColor: "transparent"
        alphaBlending: true

        Component.onCompleted: requestPaint()

        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()

        onPaint: {
            const ctx = getContext("2d");
            if (!view.session)
                return;
            const data = view.session.snapshot();
            const cw = view.cellWidth;
            const ch = view.cellHeight;
            // |0 coerces to signed 32-bit so it matches the signed ints the
            // snapshot sends (C++ static_cast<int> of 0xAARRGGBB).
            const bg = view.session.backgroundColor | 0;
            const cols = view.session.cols;
            const rows = view.session.rows;
            const selectedText = appPalette.highlightedText;
            const selectedForeground = (Math.round(selectedText.r * 255) << 16) | (Math.round(selectedText.g * 255) << 8) | Math.round(selectedText.b * 255);
            ctx.textAlign = "left";
            ctx.textBaseline = "top";
            ctx.font = view.terminalFontSize + "px " + view.fontFamily;

            // The snapshot is a flat, row-major cell list:
            // cell(r, c) === data[r * cols + c].
            for (let r = 0; r < rows; ++r) {
                const y = r * ch;
                const rowBase = r * cols;
                for (let c = 0; c < cols; ++c) {
                    const cell = data[rowBase + c];
                    if (cell["bg"] !== bg) {
                        ctx.fillStyle = Qt.rgba(((cell["bg"] >> 16) & 255) / 255, ((cell["bg"] >> 8) & 255) / 255, (cell["bg"] & 255) / 255, 1);
                        ctx.fillRect(c * cw, y, cw, ch);
                    }
                }
                const matchLeft = view.session.searchRowLeft(r);
                const matchRight = view.session.searchRowRight(r);
                if (matchLeft >= 0 && matchRight >= matchLeft) {
                    const highlight = appPalette.accent;
                    ctx.fillStyle = Qt.rgba(highlight.r, highlight.g, highlight.b, 0.55);
                    ctx.fillRect(matchLeft * cw, y, (matchRight - matchLeft + 1) * cw, ch);
                }
                // Selection highlight: drawn over cell backgrounds, under text.
                // Per-row range (iTerm2-style): start line clipped at the press
                // column, middle lines full, end line clipped at the release
                // column. -1 means this row is not part of the selection.
                if (view.session.selectionActive) {
                    const l = view.session.selRowLeft(r);
                    const rr = view.session.selRowRight(r);
                    if (l >= 0 && rr >= l) {
                        const accent = appPalette.accent;
                        ctx.fillStyle = Qt.rgba(accent.r, accent.g, accent.b, 0.82);
                        ctx.fillRect(l * cw, y, (rr - l + 1) * cw, ch);
                    }
                }
                // Command highlight (iTerm2-style), drawn under text with a
                // distinct amber color from the text selection.
                if (view.session.highlightActive) {
                    const first = view.session.highlightStartRow;
                    const last = view.session.highlightEndRow;
                    if (first >= 0 && r === first && last >= first) {
                        const width = cols * cw;
                        const height = (last - first + 1) * ch;
                        ctx.save();
                        ctx.beginPath();
                        ctx.roundRect(0.5, y + 0.5, width - 1, height - 1, Math.min(6, height / 2));
                        const accent = appPalette.accent;
                        ctx.fillStyle = Qt.rgba(accent.r, accent.g, accent.b, 0.0175);
                        ctx.fill();
                        ctx.lineWidth = 1;
                        ctx.strokeStyle = Qt.rgba(accent.r, accent.g, accent.b, 0.65);
                        ctx.stroke();
                        ctx.restore();
                    }
                }
                let buf = "";
                let bufCells = 0;
                let start = 0;
                let fg = data[rowBase]["fg"];
                let flags = data[rowBase]["flags"];
                let selected = false;
                for (let c = 0; c < cols; ++c) {
                    const cell = data[rowBase + c];
                    const selLeft = view.session.selectionActive ? view.session.selRowLeft(r) : -1;
                    const selRight = view.session.selectionActive ? view.session.selRowRight(r) : -1;
                    const cellSelected = selLeft >= 0 && c >= selLeft && c <= selRight;
                    if (cell["fg"] !== fg || cell["flags"] !== flags || cellSelected !== selected) {
                        view.drawRun(ctx, buf, start, y, selected ? selectedForeground : fg, flags, bufCells);
                        buf = "";
                        bufCells = 0;
                        start = c;
                        fg = cell["fg"];
                        flags = cell["flags"];
                        selected = cellSelected;
                    }
                    buf += cell["ch"];
                    ++bufCells;
                }
                view.drawRun(ctx, buf, start, y, selected ? selectedForeground : fg, flags, bufCells);
                if (selectionArea._linkModifierDown && selectionArea._hoverRow === r && selectionArea._hoverLinkStart >= 0 && selectionArea._hoverLinkEnd > selectionArea._hoverLinkStart) {
                    const accent = appPalette.accent;
                    ctx.fillStyle = Qt.rgba(accent.r, accent.g, accent.b, 1);
                    ctx.fillRect(selectionArea._hoverLinkStart * cw, y + ch - 2, (selectionArea._hoverLinkEnd - selectionArea._hoverLinkStart) * cw, 1);
                }
            }

            // The cursor is at the live position (bottom of the main screen),
            // which is out of view when scrolled up.
            if (view.session.cursorVisible() && view.session.scrollOffset === 0) {
                const curCol = view.session.cursorCol();
                const curRow = view.session.cursorRow();
                const cursor = view.session.foregroundColor | 0;
                ctx.fillStyle = Qt.rgba(((cursor >> 16) & 255) / 255, ((cursor >> 8) & 255) / 255, (cursor & 255) / 255, 1);
                ctx.fillRect(curCol * cw, curRow * ch, cw, ch);
                // Invert the glyph under the block (standard block-cursor
                // behavior) so it stays readable against the light block.
                const curCell = data[curRow * cols + curCol];
                if (curCell["ch"] && curCell["ch"] !== " " && (curCell["flags"] & 32) === 0) {
                    const weight = (curCell["flags"] & 1) ? Math.max(700, view.terminalFont.weight) : view.terminalFont.weight;
                    ctx.font = (view.terminalFont.italic ? "italic " : "") + weight + " " + view.terminalFontSize + "px " + view.fontFamily;
                    ctx.fillStyle = Qt.rgba(((bg >> 16) & 255) / 255, ((bg >> 8) & 255) / 255, (bg & 255) / 255, 1);
                    ctx.fillText(curCell["ch"], curCol * cw, curRow * ch);
                }
            }
        }
    }

    Rectangle {
        anchors.fill: canvas
        color: appPalette.windowText
        opacity: view.active ? 0 : 0.025
        visible: !view.active
        Accessible.ignored: true
    }

    // Mouse selection: press starts, drag extends, a click (no drag) clears,
    // double-click selects the word. Declared before the ScrollBar so the bar
    // stays on top in its right-edge strip.
    MouseArea {
        id: selectionArea

        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
        hoverEnabled: true
        cursorShape: _linkModifierDown && _hoverRow >= 0 ? Qt.PointingHandCursor : Qt.IBeamCursor

        anchors.top: canvas.top
        anchors.left: canvas.left
        anchors.right: canvas.right
        anchors.bottom: canvas.bottom

        function cellAt(x, y) {
            const col = Math.max(0, Math.min(view.cols - 1, Math.floor(x / view.cellWidth)));
            const row = Math.max(0, Math.min(view.rows - 1, Math.floor(y / view.cellHeight)));
            return [col, row];
        }

        function buttonCode(button) {
            if (button === Qt.LeftButton)
                return 0;
            if (button === Qt.MiddleButton)
                return 1;
            if (button === Qt.RightButton)
                return 2;
            return -1;
        }

        // Triple-click detection. onDoubleClicked fires on the 2nd press and
        // suppresses that press/release, so the 3rd press is a regular
        // onPressed: a press within _tapInterval of the last double-click is
        // the triple-click (select the whole line). _inTriple keeps the
        // following release from collapsing the line selection back to a point.
        property real _doubleAt: -1
        property bool _inTriple: false
        property bool _inDouble: false
        property bool _dragging: false
        property bool _cancelOnly: false
        property bool _activationClick: false
        property bool _middlePaste: false
        property bool _pointerInside: false
        property bool _linkModifierDown: false
        property int _hoverRow: -1
        property int _hoverLinkStart: -1
        property int _hoverLinkEnd: -1
        property real _pointerX: 0
        property real _pointerY: 0
        readonly property real _tapInterval: 350

        property int _pressRow: -1
        property int _pressCol: -1
        property real _pressX: 0
        property real _pressY: 0
        property int _pendingRow: -1

        function updateLinkHover(modifiers) {
            _linkModifierDown = (modifiers & (Qt.ControlModifier | Qt.MetaModifier)) !== 0;
            refreshLinkHover();
        }

        function refreshLinkHover() {
            const cell = _pointerInside ? cellAt(_pointerX, _pointerY) : [-1, -1];
            let link = ({});
            if (_linkModifierDown && view.session && cell[1] >= 0)
                link = view.session.linkAt(cell[1], cell[0]);

            const start = link && link.start !== undefined ? link.start : -1;
            const end = link && link.end !== undefined ? link.end : -1;
            const row = start >= 0 ? cell[1] : -1;
            if (_hoverRow !== row || _hoverLinkStart !== start || _hoverLinkEnd !== end) {
                _hoverRow = row;
                _hoverLinkStart = start;
                _hoverLinkEnd = end;
                canvas.requestPaint();
            }
        }

        onEntered: {
            _pointerInside = true;
            refreshLinkHover();
        }

        onExited: {
            _pointerInside = false;
            refreshLinkHover();
        }

        Timer {
            id: singleClickTimer

            interval: selectionArea._tapInterval
            onTriggered: {
                const row = selectionArea._pendingRow;
                const shouldHighlight = !selectionArea._cancelOnly;
                selectionArea._pendingRow = -1;
                selectionArea._cancelOnly = false;
                view.session.selClear();
                view.session.clearHighlight();
                if (row >= 0 && shouldHighlight) {
                    view.session.highlightCommandAt(row);
                }
            }
        }

        onPressed: mouse => {
            if (!view.session)
                return;
            const wasActive = view.session === view.sessionManager.activeSession;
            if (!wasActive) {
                selectionArea._activationClick = true;
                view.sessionManager.activateSession(view.session);
                mouse.accepted = true;
                return;
            }
            if (mouse.button === Qt.LeftButton && (mouse.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) !== 0) {
                const cell = selectionArea.cellAt(mouse.x, mouse.y);
                if (view.session.openLinkAt(cell[1], cell[0])) {
                    mouse.accepted = true;
                    return;
                }
            }
            if (mouse.button === Qt.MiddleButton) {
                selectionArea._middlePaste = true;
                view.session.pasteSelectionOrClipboard();
                mouse.accepted = true;
                return;
            }
            if (view.session.mouseTracking) {
                const cell = selectionArea.cellAt(mouse.x, mouse.y);
                view.session.mousePress(selectionArea.buttonCode(mouse.button), cell[1], cell[0], mouse.modifiers);
                mouse.accepted = true;
            } else if (mouse.button === Qt.LeftButton) {
                const cell = selectionArea.cellAt(mouse.x, mouse.y);
                selectionArea._pressRow = cell[1];
                selectionArea._pressCol = cell[0];
                selectionArea._pressX = mouse.x;
                selectionArea._pressY = mouse.y;
                selectionArea._dragging = false;
                selectionArea._cancelOnly = view.session.selectionActive || view.session.highlightActive;
                view.session.selClear();
                if (Date.now() - selectionArea._doubleAt <= selectionArea._tapInterval) {
                    // The third press selects the whole line.
                    selectionArea._inTriple = true;
                    selectionArea._inDouble = false;
                    selectionArea._cancelOnly = false;
                    selectionArea._doubleAt = -1;
                    singleClickTimer.stop();
                    selectionArea._pendingRow = -1;
                    view.session.selClear();
                    view.session.clearHighlight();
                    view.session.selLine(cell[1]);
                } else {
                    selectionArea._inTriple = false;
                }
            } else if (mouse.button === Qt.RightButton) {
                view.sessionManager.activateSession(view.session);
                // popup(x, y) positions the menu relative to its parent item,
                // so map the click into that parent's space (not global screen).
                const p = selectionArea.mapToItem(view, mouse.x, mouse.y);
                contextMenu.popup(p.x, p.y);
            }
        }

        onPositionChanged: mouse => {
            selectionArea._pointerX = mouse.x;
            selectionArea._pointerY = mouse.y;
            selectionArea._pointerInside = true;
            selectionArea.updateLinkHover(mouse.modifiers);
            if (selectionArea._activationClick || selectionArea._middlePaste)
                return;
            if (view.session.mouseTracking && (mouse.buttons !== Qt.NoButton || view.session.mouseMotionTracking)) {
                const button = (mouse.buttons & Qt.LeftButton) ? Qt.LeftButton : (mouse.buttons & Qt.MiddleButton) ? Qt.MiddleButton : (mouse.buttons & Qt.RightButton) ? Qt.RightButton : Qt.NoButton;
                const cell = selectionArea.cellAt(mouse.x, mouse.y);
                view.session.mouseMove(selectionArea.buttonCode(button), cell[1], cell[0], mouse.modifiers);
            } else if (!view.session.mouseTracking && (mouse.buttons & Qt.LeftButton)) {
                const dx = Math.abs(mouse.x - selectionArea._pressX);
                const dy = Math.abs(mouse.y - selectionArea._pressY);
                if (!selectionArea._dragging && dx < 4 && dy < 4) {
                    return;
                }
                if (!selectionArea._dragging) {
                    singleClickTimer.stop();
                    selectionArea._pendingRow = -1;
                    selectionArea._cancelOnly = false;
                    view.session.clearHighlight();
                    view.session.selPress(selectionArea._pressRow, selectionArea._pressCol);
                    selectionArea._dragging = true;
                }
                const cell = selectionArea.cellAt(mouse.x, mouse.y);
                view.session.selMove(cell[1], cell[0]);
            }
        }

        onReleased: mouse => {
            if (mouse.button === Qt.MiddleButton) {
                selectionArea._middlePaste = false;
                mouse.accepted = true;
                return;
            }
            if (selectionArea._activationClick) {
                selectionArea._activationClick = false;
                mouse.accepted = true;
                return;
            }
            if (view.session.mouseTracking) {
                const cell = selectionArea.cellAt(mouse.x, mouse.y);
                view.session.mouseRelease(selectionArea.buttonCode(mouse.button), cell[1], cell[0], mouse.modifiers);
            } else if (mouse.button === Qt.LeftButton) {
                if (selectionArea._inTriple) {
                    // The triple press already selected the line; don't let
                    // the release collapse it back to a point.
                    selectionArea._inTriple = false;
                } else if (selectionArea._inDouble) {
                    selectionArea._inDouble = false;
                } else if (selectionArea._dragging) {
                    view.session.selRelease();
                    selectionArea._dragging = false;
                } else {
                    const cell = selectionArea.cellAt(mouse.x, mouse.y);
                    selectionArea._pendingRow = cell[1];
                    singleClickTimer.restart();
                }
            }
        }

        onDoubleClicked: mouse => {
            if (!selectionArea._activationClick && !view.session.mouseTracking && mouse.button === Qt.LeftButton) {
                const cell = selectionArea.cellAt(mouse.x, mouse.y);
                singleClickTimer.stop();
                selectionArea._pendingRow = -1;
                selectionArea._inDouble = true;
                selectionArea._cancelOnly = false;
                view.session.selClear();
                // A double-click is a selection gesture, so clear the pending
                // highlight (the user is switching to "selection mode").
                view.session.clearHighlight();
                view.session.selWord(cell[1], cell[0]);
                selectionArea._doubleAt = Date.now();
            }
        }

        onCanceled: {
            selectionArea._activationClick = false;
            selectionArea._middlePaste = false;
        }
    }

    Controls.Menu {
        id: contextMenu

        Controls.MenuItem {
            text: qsTr("Split Side by Side")
            onTriggered: {
                view.sessionManager.activateSession(view.session);
                view.sessionManager.splitActive(Qt.Horizontal);
            }
        }

        Controls.MenuItem {
            text: qsTr("Split Top and Bottom")
            onTriggered: {
                view.sessionManager.activateSession(view.session);
                view.sessionManager.splitActive(Qt.Vertical);
            }
        }

        MenuSeparator {}

        Controls.MenuItem {
            text: qsTr("Save Buffer or Selection…")
            onTriggered: saveTextDialog.openFor(view.session, input)
        }

        MenuSeparator {}

        Controls.MenuItem {
            text: qsTr("Copy")
            onTriggered: view.session.copySelection()
        }

        Controls.MenuItem {
            text: qsTr("Paste")
            onTriggered: view.session.paste()
        }

        Controls.MenuItem {
            text: qsTr("Select All")
            onTriggered: view.session.selectAll()
        }

        Controls.MenuItem {
            text: qsTr("Clear Buffer")
            onTriggered: view.session.clearBuffer()
        }
    }

    Controls.SaveTextDialog {
        id: saveTextDialog
    }

    Controls.SearchBar {
        id: searchBar

        session: view.session
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 12
        z: 10
        onCloseRequested: input.forceActiveFocus()
        onRepaintRequested: canvas.requestPaint()
    }

    // Thin scrollbar on the right edge, matching the canvas vertically.
    // size and position are driven imperatively from the view (see
    // syncScrollBar below) so 0 = oldest line and 1 - size = the live cursor
    // at the bottom. The style keeps it thin and fatter on hover; it only
    // shows when there is scrollback.
    Basic.ScrollBar {
        id: scrollBar

        visible: view.session && view.session.maxScrollOffset > 0
        width: 10
        policy: ScrollBar.AlwaysOn
        z: 20

        anchors.right: parent.right
        anchors.rightMargin: 5
        anchors.top: canvas.top
        anchors.bottom: canvas.bottom

        orientation: Qt.Vertical
        onPositionChanged: {
            if (view._syncingScrollBar)
                return;
            const offset = view.session.maxScrollOffset - position * view.totalLines;
            const target = Math.max(0, Math.min(view.session.maxScrollOffset, Math.round(offset)));
            if (target !== view.session.scrollOffset)
                view.session.scrollTo(target);
        }

        background: Rectangle {
            color: Qt.rgba(appPalette.windowText.r, appPalette.windowText.g, appPalette.windowText.b, 0.10)
            radius: width / 2
        }

        contentItem: Rectangle {
            implicitWidth: 6
            implicitHeight: 24
            radius: width / 2
            color: scrollBar.pressed ? Qt.rgba(appPalette.windowText.r, appPalette.windowText.g, appPalette.windowText.b, 0.72) : scrollBar.hovered ? Qt.rgba(appPalette.windowText.r, appPalette.windowText.g, appPalette.windowText.b, 0.55) : Qt.rgba(appPalette.windowText.r, appPalette.windowText.g, appPalette.windowText.b, 0.35)
        }
    }

    // Drive the scrollbar size (visible fraction) and position from the view.
    // Both are set imperatively (not bound) so a window resize can't re-trigger
    // a binding loop through a re-evaluated size binding.
    function syncScrollBar() {
        _syncingScrollBar = true;
        scrollBar.size = view.session.rows / view.totalLines;
        scrollBar.position = (view.session.maxScrollOffset - view.session.scrollOffset) / view.totalLines;
        _syncingScrollBar = false;
    }

    Connections {
        target: view.session

        function onFrameReady() {
            canvas.requestPaint();
        }

        function onSelectionChanged() {
            canvas.requestPaint();
        }

        function onHighlightChanged() {
            canvas.requestPaint();
        }

        function onScrollOffsetChanged() {
            view.syncScrollBar();
        }

        function onSizeChanged() {
            view.syncScrollBar();
        }
    }
}
