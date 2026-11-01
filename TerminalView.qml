import QtQuick
import QtQuick.Controls
import QtCanvas2D

import "controls" as Controls

Item {
    id: view

    property var session: null
    property bool active: false

    // Menlo advance is 0.6em, so 15px -> exactly 9px per column.
    readonly property int fontSize: 15
    readonly property real cellWidth: 9
    readonly property real cellHeight: 18
    // Primary font for regular text. The Nerd Font is the CSS fallback for
    // supplementary-plane glyphs (Nerd Font icons, U+F0xxx) that the primary
    // font lacks: the CSS font-family list does per-glyph fallback, so icons
    // render from the Nerd Font while regular text stays in the primary font.
    // The "Mono" variant guarantees every glyph (icons included) is one cell
    // wide, so the icon column grid stays aligned.
    readonly property string primaryFont: "monospace"
    readonly property string nerdFont: "'JetBrainsMono Nerd Font Mono'"
    readonly property string fontFamily: primaryFont + ", " + nerdFont
    readonly property int cols: Math.max(2, Math.floor(canvas.width / cellWidth))
    readonly property int rows: Math.max(2, Math.floor(canvas.height / cellHeight))

    SystemPalette {
        id: appPalette
        colorGroup: SystemPalette.Active
    }

    readonly property color systemBackground: appPalette.window
    readonly property color systemForeground: appPalette.windowText

    onSystemBackgroundChanged: applySystemColors()
    onSystemForegroundChanged: applySystemColors()

    // Accumulates wheel/trackpad deltas so a few small deltas add up to a whole
    // line. 40 units = 1 line (a mouse notch is 120 units, so 1 notch = 3 lines).
    property real scrollAccumulator: 0

    // Whole history (scrollback + visible screen) for the scrollbar mapping.
    readonly property int totalLines: view.session ? view.session.maxScrollOffset + view.session.rows : 0

    // True while syncScrollBar() drives the bar, so onPositionChanged ignores
    // our imperative sets and only reacts to real user drags.
    property bool _syncingScrollBar: false

    onColsChanged: syncSessionSize()
    onRowsChanged: syncSessionSize()
    onSessionChanged: {
        if (session) {
            syncSessionSize();
            applySystemColors();
        }
    }
    onVisibleChanged: syncSessionSize()
    onActiveChanged: focusInputIfActive()

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
        if (session && canvas.width >= cellWidth * 2 && canvas.height >= cellHeight * 2)
            session.resize(cols, rows);
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
                    SessionManager.routeKeyPress(view.session, event.key, event.text, event.modifiers);
                event.accepted = true;
            }
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
                    SessionManager.routeTextInput(view.session, text);
                _clearing = true;
                text = "";
                _clearing = false;
            }
        }
    }

    WheelHandler {
        onWheel: event => {
            const dy = event.angleDelta.y; // positive = up, negative = down
            if (dy !== 0) {
                if (view.session.mouseTracking) {
                    const p = selectionArea.mapFromItem(view, event.x, event.y);
                    const cell = selectionArea.cellAt(p.x, p.y);
                    view.session.mouseWheel(dy > 0, cell[1], cell[0], event.modifiers);
                } else {
                    view.scrollAccumulator += dy;
                    const step = Math.trunc(view.scrollAccumulator / 40);
                    if (step !== 0) {
                        view.session.scrollBy(step);
                        view.scrollAccumulator -= step * 40;
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
        ctx.font = ((flags & 1) ? "bold " : "") + view.fontSize + "px " + view.fontFamily;
        ctx.fillText(text, startCol * view.cellWidth, y);
        if (flags & 4)
            ctx.fillRect(startCol * view.cellWidth, y + view.cellHeight - 1, cells * view.cellWidth, 1);
        if (flags & 8)
            ctx.fillRect(startCol * view.cellWidth, y + Math.floor(view.cellHeight / 2), cells * view.cellWidth, 1);
    }

    Canvas2D {
        id: canvas

        anchors.fill: parent
        anchors.margins: 5
        fillColor: "transparent"

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
            const cols = view.cols;
            const rows = view.rows;
            const selectedText = appPalette.highlightedText;
            const selectedForeground = (Math.round(selectedText.r * 255) << 16) | (Math.round(selectedText.g * 255) << 8) | Math.round(selectedText.b * 255);
            ctx.textAlign = "left";
            ctx.textBaseline = "top";
            ctx.font = view.fontSize + "px " + view.fontFamily;

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
                    ctx.font = ((curCell["flags"] & 1) ? "bold " : "") + view.fontSize + "px " + view.fontFamily;
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
        hoverEnabled: view.session ? view.session.mouseMotionTracking : false

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
        readonly property real _tapInterval: 350

        property int _pressRow: -1
        property int _pressCol: -1
        property real _pressX: 0
        property real _pressY: 0
        property int _pendingRow: -1

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
            const wasActive = view.session === SessionManager.activeSession;
            if (!wasActive) {
                selectionArea._activationClick = true;
                SessionManager.activateSession(view.session);
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
                SessionManager.activateSession(view.session);
                // popup(x, y) positions the menu relative to its parent item,
                // so map the click into that parent's space (not global screen).
                const p = selectionArea.mapToItem(view, mouse.x, mouse.y);
                contextMenu.popup(p.x, p.y);
            }
        }

        onPositionChanged: mouse => {
            if (selectionArea._activationClick)
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

        onCanceled: selectionArea._activationClick = false
    }

    Controls.Menu {
        id: contextMenu

        Controls.MenuItem {
            text: qsTr("Split Side by Side")
            onTriggered: {
                SessionManager.activateSession(view.session);
                SessionManager.splitActive(Qt.Horizontal);
            }
        }

        Controls.MenuItem {
            text: qsTr("Split Top and Bottom")
            onTriggered: {
                SessionManager.activateSession(view.session);
                SessionManager.splitActive(Qt.Vertical);
            }
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
    ScrollBar {
        id: scrollBar

        visible: view.session && view.session.maxScrollOffset > 0

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

        background: Item {}
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
