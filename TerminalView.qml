import QtQuick
import QtQuick.Controls
import QtCanvas2D

import "controls" as Controls

Item {
    id: view

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

    // Accumulates wheel/trackpad deltas so a few small deltas add up to a whole
    // line. 40 units = 1 line (a mouse notch is 120 units, so 1 notch = 3 lines).
    property real scrollAccumulator: 0

    // Whole history (scrollback + visible screen) for the scrollbar mapping.
    readonly property int totalLines: Backend.maxScrollOffset + Backend.rows

    // True while syncScrollBar() drives the bar, so onPositionChanged ignores
    // our imperative sets and only reacts to real user drags.
    property bool _syncingScrollBar: false

    onColsChanged: Backend.resize(cols, rows)
    onRowsChanged: Backend.resize(cols, rows)

    // Invisible focus target that receives composed text from the macOS text
    // input system (dead keys / accents). Special keys and Cmd/Option/Ctrl
    // combos are intercepted here and sent to the backend; printable
    // characters are let through so the TextInput composes them, then
    // onTextChanged forwards and clears.
    TextInput {
        id: input

        x: 0
        y: 0
        width: 1
        height: 1
        color: "transparent"
        cursorVisible: false
        focus: true

        property bool _clearing: false

        readonly property var specialKeys: [
            Qt.Key_Return, Qt.Key_Enter, Qt.Key_Backspace, Qt.Key_Tab,
            Qt.Key_Escape, Qt.Key_Up, Qt.Key_Down, Qt.Key_Left, Qt.Key_Right,
            Qt.Key_Home, Qt.Key_End, Qt.Key_Delete, Qt.Key_PageUp, Qt.Key_PageDown
        ]

        Keys.onPressed: event => {
            const hasCmd = (event.modifiers & Qt.ControlModifier) !== 0
            const hasOption = (event.modifiers & Qt.AltModifier) !== 0
            const hasCtrl = (event.modifiers & Qt.MetaModifier) !== 0
            const isSpecialKey = specialKeys.includes(event.key)
            if (hasCmd || hasOption || hasCtrl || isSpecialKey) {
                Backend.keyPress(event.key, event.text, event.modifiers);
                event.accepted = true;
            }
        }

        onTextChanged: {
            if (_clearing)
                return;
            if (text.length > 0) {
                Backend.writeText(text);
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
                if (Backend.mouseTracking) {
                    const p = selectionArea.mapFromItem(view, event.x, event.y);
                    const cell = selectionArea.cellAt(p.x, p.y);
                    Backend.mouseWheel(dy > 0, cell[1], cell[0], event.modifiers);
                } else {
                    view.scrollAccumulator += dy;
                    const step = Math.trunc(view.scrollAccumulator / 40);
                    if (step !== 0) {
                        Backend.scrollBy(step);
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
            const data = Backend.snapshot();
            const cw = view.cellWidth;
            const ch = view.cellHeight;
            // |0 coerces to signed 32-bit so it matches the signed ints the
            // snapshot sends (C++ static_cast<int> of 0xAARRGGBB).
            const bg = Backend.backgroundColor | 0;
            const cols = view.cols;
            const rows = view.rows;
            ctx.textAlign = "left";
            ctx.textBaseline = "top";
            ctx.font = view.fontSize + "px " + view.fontFamily;

            // The snapshot is a flat, row-major list of cells:
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
                // Selection highlight: drawn over cell backgrounds, under text.
                // Per-row range (iTerm2-style): start line clipped at the press
                // column, middle lines full, end line clipped at the release
                // column. -1 means this row is not part of the selection.
                if (Backend.selectionActive) {
                    const l = Backend.selRowLeft(r);
                    const rr = Backend.selRowRight(r);
                    if (l >= 0 && rr >= l) {
                        ctx.fillStyle = "rgba(70, 130, 180, 0.45)";
                        ctx.fillRect(l * cw, y, (rr - l + 1) * cw, ch);
                    }
                }
                let buf = "";
                let bufCells = 0;
                let start = 0;
                let fg = data[rowBase]["fg"];
                let flags = data[rowBase]["flags"];
                for (let c = 0; c < cols; ++c) {
                    const cell = data[rowBase + c];
                    if (cell["fg"] !== fg || cell["flags"] !== flags) {
                        view.drawRun(ctx, buf, start, y, fg, flags, bufCells);
                        buf = "";
                        bufCells = 0;
                        start = c;
                        fg = cell["fg"];
                        flags = cell["flags"];
                    }
                    buf += cell["ch"];
                    ++bufCells;
                }
                view.drawRun(ctx, buf, start, y, fg, flags, bufCells);
            }

            // The cursor is at the live position (bottom of the main screen),
            // which is out of view when scrolled up.
            if (Backend.cursorVisible() && Backend.scrollOffset === 0) {
                ctx.fillStyle = Qt.rgba(0.88, 0.88, 0.88, 1);
                ctx.fillRect(Backend.cursorCol() * cw, Backend.cursorRow() * ch, cw, ch);
            }
        }
    }

    // Mouse selection: press starts, drag extends, a click (no drag) clears,
    // double-click selects the word. Declared before the ScrollBar so the bar
    // stays on top in its right-edge strip.
    MouseArea {
        id: selectionArea

        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
        hoverEnabled: Backend.mouseMotionTracking

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

        onPressed: mouse => {
            if (Backend.mouseTracking) {
                const cell = selectionArea.cellAt(mouse.x, mouse.y);
                Backend.mousePress(selectionArea.buttonCode(mouse.button), cell[1], cell[0], mouse.modifiers);
                mouse.accepted = true;
            } else if (mouse.button === Qt.LeftButton) {
                const cell = selectionArea.cellAt(mouse.x, mouse.y);
                Backend.selPress(cell[1], cell[0]);
            } else if (mouse.button === Qt.RightButton) {
                // popup(x, y) positions the menu relative to its parent item,
                // so map the click into that parent's space (not global screen).
                const p = selectionArea.mapToItem(view, mouse.x, mouse.y);
                contextMenu.popup(p.x, p.y);
            }
        }

        onPositionChanged: mouse => {
            if (Backend.mouseTracking && (mouse.buttons !== Qt.NoButton || Backend.mouseMotionTracking)) {
                const button = (mouse.buttons & Qt.LeftButton) ? Qt.LeftButton
                    : (mouse.buttons & Qt.MiddleButton) ? Qt.MiddleButton
                    : (mouse.buttons & Qt.RightButton) ? Qt.RightButton : Qt.NoButton;
                const cell = selectionArea.cellAt(mouse.x, mouse.y);
                Backend.mouseMove(selectionArea.buttonCode(button), cell[1], cell[0], mouse.modifiers);
            } else if (!Backend.mouseTracking && (mouse.buttons & Qt.LeftButton)) {
                const cell = selectionArea.cellAt(mouse.x, mouse.y);
                Backend.selMove(cell[1], cell[0]);
            }
        }

        onReleased: mouse => {
            if (Backend.mouseTracking) {
                const cell = selectionArea.cellAt(mouse.x, mouse.y);
                Backend.mouseRelease(selectionArea.buttonCode(mouse.button), cell[1], cell[0], mouse.modifiers);
            } else if (mouse.button === Qt.LeftButton) {
                Backend.selRelease();
            }
        }

        onDoubleClicked: mouse => {
            if (!Backend.mouseTracking && mouse.button === Qt.LeftButton) {
                const cell = selectionArea.cellAt(mouse.x, mouse.y);
                Backend.selWord(cell[1], cell[0]);
            }
        }
    }

    Controls.Menu {
        id: contextMenu

        Controls.MenuItem {
            text: qsTr("Copy")
            onTriggered: Backend.copySelection()
        }

        Controls.MenuItem {
            text: qsTr("Paste")
            onTriggered: Backend.paste()
        }

        Controls.MenuItem {
            text: qsTr("Select All")
            onTriggered: Backend.selectAll()
        }

        Controls.MenuItem {
            text: qsTr("Clear Buffer")
            onTriggered: Backend.clearBuffer()
        }
    }

    // Thin scrollbar on the right edge, matching the canvas vertically.
    // size and position are driven imperatively from the view (see
    // syncScrollBar below) so 0 = oldest line and 1 - size = the live cursor
    // at the bottom. The style keeps it thin and fatter on hover; it only
    // shows when there is scrollback.
    ScrollBar {
        id: scrollBar

        visible: Backend.maxScrollOffset > 0

        anchors.right: parent.right
        anchors.top: canvas.top
        anchors.bottom: canvas.bottom

        orientation: Qt.Vertical
        onPositionChanged: {
            if (view._syncingScrollBar)
                return;
            const offset = Backend.maxScrollOffset - position * view.totalLines;
            const target = Math.max(0, Math.min(Backend.maxScrollOffset, Math.round(offset)));
            if (target !== Backend.scrollOffset)
                Backend.scrollTo(target);
        }
    }

    // Drive the scrollbar size (visible fraction) and position from the view.
    // Both are set imperatively (not bound) so a window resize can't re-trigger
    // a binding loop through a re-evaluated size binding.
    function syncScrollBar() {
        _syncingScrollBar = true;
        scrollBar.size = Backend.rows / view.totalLines;
        scrollBar.position = (Backend.maxScrollOffset - Backend.scrollOffset) / view.totalLines;
        _syncingScrollBar = false;
    }

    Connections {
        target: Backend

        function onFrameReady() {
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
