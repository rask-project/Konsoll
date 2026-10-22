import QtQuick
import QtCanvas2D

Item {
    id: view
    focus: true

    // Menlo advance is 0.6em, so 15px -> exactly 9px per column.
    readonly property int fontSize: 15
    readonly property real cellWidth: 9
    readonly property real cellHeight: 18
    readonly property int cols: Math.max(2, Math.floor(width / cellWidth))
    readonly property int rows: Math.max(2, Math.floor(height / cellHeight))

    onColsChanged: Backend.resize(cols, rows)
    onRowsChanged: Backend.resize(cols, rows)

    Keys.onPressed: event => {
        Backend.keyPress(event.key, event.text, event.modifiers);
        event.accepted = true;
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

            if (Backend.cursorVisible()) {
                ctx.fillStyle = Qt.rgba(0.88, 0.88, 0.88, 1);
                ctx.fillRect(Backend.cursorCol() * cw, Backend.cursorRow() * ch, cw, ch);
            }
        }
    }

    Connections {
        target: Backend

        function onFrameReady() {
            canvas.requestPaint();
        }
    }
}
