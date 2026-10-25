import QtQuick
import QtQuick.Controls
import QtCanvas2D

Item {
    id: view
    focus: true

    // Menlo advance is 0.6em, so 15px -> exactly 9px per column.
    readonly property int fontSize: 15
    readonly property real cellWidth: 9
    readonly property real cellHeight: 18
    readonly property int cols: Math.max(2, Math.floor(canvas.width / cellWidth))
    readonly property int rows: Math.max(2, Math.floor(canvas.height / cellHeight))

    // Accumulates wheel/trackpad deltas so a few small deltas add up to a whole
    // line. 40 units = 1 line (a mouse notch is 120 units, so 1 notch = 3 lines).
    property real scrollAccumulator: 0

    // Whole history (scrollback + visible screen) for the scrollbar mapping.
    readonly property int totalLines: Backend.maxScrollOffset + Backend.rows

    onColsChanged: Backend.resize(cols, rows)
    onRowsChanged: Backend.resize(cols, rows)

    Keys.onPressed: event => {
        Backend.keyPress(event.key, event.text, event.modifiers);
        event.accepted = true;
    }

    WheelHandler {
        onWheel: event => {
            const dy = event.angleDelta.y; // positive = up, negative = down
            if (dy !== 0) {
                view.scrollAccumulator += dy;
                const step = Math.trunc(view.scrollAccumulator / 40);
                if (step !== 0) {
                    Backend.scrollBy(step);
                    view.scrollAccumulator -= step * 40;
                }
            }
            event.accepted = true;
        }
    }

    function drawRun(ctx, text, startCol, y, fg, flags) {
        if (!text || (flags & 32) !== 0)
            return;
        ctx.fillStyle = Qt.rgba(((fg >> 16) & 255) / 255, ((fg >> 8) & 255) / 255, (fg & 255) / 255, 1);
        ctx.font = (flags & 1) ? "bold " + view.fontSize + "px monospace" : view.fontSize + "px monospace";
        ctx.fillText(text, startCol * view.cellWidth, y);
        if (flags & 4)
            ctx.fillRect(startCol * view.cellWidth, y + view.cellHeight - 1, text.length * view.cellWidth, 1);
        if (flags & 8)
            ctx.fillRect(startCol * view.cellWidth, y + Math.floor(view.cellHeight / 2), text.length * view.cellWidth, 1);
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
            ctx.font = view.fontSize + "px monospace";

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
                let start = 0;
                let fg = data[rowBase]["fg"];
                let flags = data[rowBase]["flags"];
                for (let c = 0; c < cols; ++c) {
                    const cell = data[rowBase + c];
                    if (cell["fg"] !== fg || cell["flags"] !== flags) {
                        view.drawRun(ctx, buf, start, y, fg, flags);
                        buf = "";
                        start = c;
                        fg = cell["fg"];
                        flags = cell["flags"];
                    }
                    buf += cell["ch"];
                }
                view.drawRun(ctx, buf, start, y, fg, flags);
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

        acceptedButtons: Qt.LeftButton | Qt.RightButton

        anchors.top: canvas.top
        anchors.left: canvas.left
        anchors.right: canvas.right
        anchors.bottom: canvas.bottom

        function cellAt(x, y) {
            const col = Math.max(0, Math.min(view.cols - 1, Math.floor(x / view.cellWidth)));
            const row = Math.max(0, Math.min(view.rows - 1, Math.floor(y / view.cellHeight)));
            return [col, row];
        }

        onPressed: mouse => {
            if (mouse.button === Qt.LeftButton) {
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
            if (mouse.buttons & Qt.LeftButton) {
                const cell = selectionArea.cellAt(mouse.x, mouse.y);
                Backend.selMove(cell[1], cell[0]);
            }
        }

        onReleased: mouse => {
            if (mouse.button === Qt.LeftButton)
                Backend.selRelease();
        }

        onDoubleClicked: mouse => {
            if (mouse.button === Qt.LeftButton) {
                const cell = selectionArea.cellAt(mouse.x, mouse.y);
                Backend.selWord(cell[1], cell[0]);
            }
        }
    }

    Menu {
        id: contextMenu

        MenuItem {
            text: "Copiar"
            onTriggered: Backend.copySelection()
        }

        MenuItem {
            text: "Colar"
            onTriggered: Backend.paste()
        }

        MenuItem {
            text: "Selecionar tudo"
            onTriggered: Backend.selectAll()
        }
    }

    // Thin scrollbar on the right edge, matching the canvas vertically.
    // size = visible fraction of the history; position is driven imperatively
    // from the scroll offset (see onScrollOffsetChanged below) so 0 = oldest
    // line and 1 - size = the live cursor at the bottom. The style keeps it
    // thin and fatter on hover; it only shows when there is scrollback.
    ScrollBar {
        id: scrollBar

        visible: Backend.maxScrollOffset > 0

        anchors.right: parent.right
        anchors.top: canvas.top
        anchors.bottom: canvas.bottom
        size: Backend.rows / view.totalLines

        orientation: Qt.Vertical
        onPositionChanged: {
            const offset = Backend.maxScrollOffset - position * view.totalLines;
            const target = Math.max(0, Math.min(Backend.maxScrollOffset, Math.round(offset)));
            if (target !== Backend.scrollOffset)
                Backend.scrollTo(target);
        }
    }

    Connections {
        target: Backend

        function onFrameReady() {
            canvas.requestPaint();
        }

        // Drive the scrollbar position from the view. Set imperatively (not
        // bound) so onPositionChanged can't re-trigger itself through a
        // re-evaluated binding.
        function onScrollOffsetChanged() {
            scrollBar.position = (Backend.maxScrollOffset - Backend.scrollOffset) / view.totalLines;
        }
    }
}
