#include "TerminalBackend.h"

#include <QClipboard>
#include <QColor>
#include <QFileInfo>
#include <QGuiApplication>
#include <QtGui>

#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <utility>

using namespace std::chrono_literals;

namespace konsoll
{

namespace
{
// Standard xterm 256-color palette (0x00RRGGBB).
std::array<QRgb, 256> buildPalette()
{
    std::array<QRgb, 256> p{};
    p[0] = qRgb(0, 0, 0);
    p[1] = qRgb(194, 54, 33);
    p[2] = qRgb(151, 185, 15);
    p[3] = qRgb(181, 138, 4);
    p[4] = qRgb(52, 55, 143);
    p[5] = qRgb(140, 83, 194);
    p[6] = qRgb(38, 139, 172);
    p[7] = qRgb(203, 204, 205);
    p[8] = qRgb(42, 42, 42);
    p[9] = qRgb(234, 92, 53);
    p[10] = qRgb(181, 192, 55);
    p[11] = qRgb(253, 177, 44);
    p[12] = qRgb(72, 118, 255);
    p[13] = qRgb(214, 108, 255);
    p[14] = qRgb(52, 189, 244);
    p[15] = qRgb(242, 242, 242);
    int i = 0;

    for (int r = 0; r < 6; ++r) {
        for (int g = 0; g < 6; ++g) {
            for (int b = 0; b < 6; ++b) {
                p[16 + i] =
                    qRgb((r != 0) ? (r * 40) + 55 : 0, (g != 0) ? (g * 40) + 55 : 0, (b != 0) ? (b * 40) + 55 : 0);
                ++i;
            }
        }
    }

    for (int i2 = 0; i2 < 24; ++i2) {
        const int v = (i2 * 10) + 8;
        p[232 + i2] = qRgb(v, v, v);
    }

    return p;
}

// Encode a Ctrl+<key> press into the control byte the shell expects, or an
// empty byte array if the key has no Ctrl meaning.
QByteArray ctrlKeyByte(const QString &text, bool shift)
{
    if (text.isEmpty()) {
        return {};
    }
    const QChar c = text.at(0);
    if (c.unicode() < 0x20) { // 0x20: below are ASCII control chars; pass through raw
        return QByteArray(1, static_cast<char>(c.unicode()));
    }
    if (shift) {
        const QChar u = c.toUpper();
        if (u >= 'A' && u <= 'Z') {
            return QByteArray(1, static_cast<char>((u.unicode() - 'A' + 1) & 0x1fu));
        }
    } else {
        const QChar l = c.toLower();
        if (l >= 'a' && l <= 'z') {
            return QByteArray(1, static_cast<char>((l.unicode() - 'a' + 1) & 0x1fu));
        }
    }
    return {};
}

// Map a plain (unmodified) special key to its byte sequence, or an empty
// byte array if the key has no special meaning.
QByteArray specialKeyByte(int key)
{
    switch (key) {
    case static_cast<int>(Qt::Key_Return):
    case static_cast<int>(Qt::Key_Enter):
        return QByteArray(1, static_cast<char>(0x0d));
    case static_cast<int>(Qt::Key_Backspace):
        return QByteArray(1, static_cast<char>(0x7f));
    case static_cast<int>(Qt::Key_Tab):
        return QByteArray(1, static_cast<char>(0x09));
    case static_cast<int>(Qt::Key_Escape):
        return QByteArray(1, static_cast<char>(0x1b));
    case static_cast<int>(Qt::Key_Up):
        return "\x1b[A";
    case static_cast<int>(Qt::Key_Down):
        return "\x1b[B";
    case static_cast<int>(Qt::Key_Right):
        return "\x1b[C";
    case static_cast<int>(Qt::Key_Left):
        return "\x1b[D";
    case static_cast<int>(Qt::Key_Home):
        return "\x1bOH";
    case static_cast<int>(Qt::Key_End):
        return "\x1bOF";
    case static_cast<int>(Qt::Key_Delete):
        return "\x1b[3~";
    case static_cast<int>(Qt::Key_PageUp):
        return "\x1b[5~";
    case static_cast<int>(Qt::Key_PageDown):
        return "\x1b[6~";
    default:
        return {};
    }
}

// Map an Option-modified key to its byte sequence (word movement for the
// shell line editor), or an empty byte array if the key has no Option meaning.
QByteArray optionKeyByte(int key)
{
    switch (key) {
    case static_cast<int>(Qt::Key_Left):
        return "\033b";
    case static_cast<int>(Qt::Key_Right):
        return "\033f";
    default:
        return {};
    }
}
} // namespace

TerminalBackend::TerminalBackend(QObject *parent) : QObject(parent)
{
    m_frameTimer.setTimerType(Qt::CoarseTimer);
    m_frameTimer.setInterval(16ms);
    connect(&m_frameTimer, &QTimer::timeout, this, &TerminalBackend::onFrameTimer);
}

void TerminalBackend::start()
{
    if (m_pty) {
        return; // Already running.
    }

    m_terminal = std::make_unique<Terminal>(m_rows, m_cols);
    m_pty = std::make_unique<PtyProcess>();
    connect(m_pty.get(), &PtyProcess::dataReceived, this, &TerminalBackend::onData);
    connect(m_pty.get(), &PtyProcess::processExited, this, &TerminalBackend::onExited);
    const QString shell = detectDefaultShell();
    if (!m_pty->start(shell, m_cols, m_rows)) {
        qWarning("Konsoll: failed to start the PTY");
    }
}

void TerminalBackend::onData(const QByteArray &data)
{
    m_terminal->write(data);
    if (m_terminal->daPending()) {
        m_pty->write(Terminal::daResponse());
        m_terminal->clearDaPending();
    }

    if (m_terminal->titleDirty()) {
        m_title = m_terminal->title();
        emit titleChanged();
        m_terminal->clearTitleDirty();
    }

    if (m_terminal->clearPending()) {
        // The screen was erased (e.g. Ctrl+L / clear); a selection over the
        // now-blank lines is stale, so drop it.
        m_selAnchor = {.line = -1, .col = 0};
        m_selCurrent = {.line = -1, .col = 0};
        emit selectionChanged();
        m_terminal->clearClearPending();
    }

    checkScrollOffsetChanged();
    scheduleFrame();
}

void TerminalBackend::onExited(int code)
{
    emit processExited(code);
}

void TerminalBackend::onFrameTimer()
{
    emit frameReady();
}

void TerminalBackend::scheduleFrame()
{
    if (!m_frameTimer.isActive()) {
        m_frameTimer.start();
    }
}

void TerminalBackend::keyPress(int key, const QString &text, int modifiers)
{
    const unsigned int mods = static_cast<unsigned int>(modifiers);
    const bool meta = (mods & static_cast<unsigned int>(Qt::ControlModifier)) != 0;
    const bool ctrl = (mods & static_cast<unsigned int>(Qt::MetaModifier)) != 0;
    const bool option = (mods & static_cast<unsigned int>(Qt::AltModifier)) != 0;
    const bool shift = (mods & static_cast<unsigned int>(Qt::ShiftModifier)) != 0;

    // Editor shortcuts: copy / paste / select-all.
    if (meta) {
        editorShortcuts(key);
        return;
    }

    // Typing while scrolled up snaps the view back to the live cursor.
    if (m_terminal && m_terminal->screen().scrollOffset() > 0) {
        scrollToBottom();
    }

    QByteArray out;
    if (ctrl) {
        // Physical Ctrl+letter: send the control byte (e.g. Ctrl+C -> 0x03).
        // The event text may be empty for a Ctrl-modified key, so derive the
        // letter from the key code if needed.
        QString ctrlText = text;
        if (ctrlText.isEmpty() && key >= static_cast<int>(Qt::Key_A) && key <= static_cast<int>(Qt::Key_Z)) {
            ctrlText = QChar(static_cast<char>('a' + (key - static_cast<int>(Qt::Key_A))));
        }
        out = ctrlKeyByte(ctrlText, shift);
    } else {
        out = specialKeyByte(key);
        if (option) {
            // Option+arrows: word-by-word movement, handled by the shell
            // line editor (zsh: \eb/\ef -> backward/forward-word).
            const QByteArray opt = optionKeyByte(key);
            if (!opt.isEmpty()) {
                out = opt;
            }
        }
        if (out.isEmpty() && !text.isEmpty()) {
            out = text.toUtf8();
        }
    }

    if (!out.isEmpty() && m_pty) {
        m_pty->write(out);
    }
}

void TerminalBackend::writeText(const QString &text)
{
    if (m_pty && !text.isEmpty()) {
        m_pty->write(text.toUtf8());
    }
}

void TerminalBackend::resize(int cols, int rows)
{
    if (cols < 2 || rows < 2 || (cols == m_cols && rows == m_rows)) {
        return;
    }

    m_cols = cols;
    m_rows = rows;

    if (m_terminal) {
        // Terminal/Screen take (rows, cols); the PTY takes (cols, rows).
        m_terminal->resize(rows, cols);
    }

    if (m_pty) {
        m_pty->resize(cols, rows);
    }

    // The reflow re-wraps the history, so absolute selection anchors are stale.
    m_selAnchor = {.line = -1, .col = 0};
    m_selCurrent = {.line = -1, .col = 0};

    emit selectionChanged();
    emit sizeChanged();

    checkScrollOffsetChanged();
    scheduleFrame();
}

void TerminalBackend::scrollBy(int delta)
{
    if (!m_terminal) {
        return;
    }

    m_terminal->screen().scrollBy(delta);
    checkScrollOffsetChanged();
    scheduleFrame();
}

void TerminalBackend::scrollTo(int offset)
{
    if (!m_terminal) {
        return;
    }

    m_terminal->screen().scrollTo(offset);
    checkScrollOffsetChanged();
    scheduleFrame();
}

void TerminalBackend::scrollToBottom()
{
    if (!m_terminal) {
        return;
    }

    m_terminal->screen().scrollToBottom();
    checkScrollOffsetChanged();
    scheduleFrame();
}

void TerminalBackend::checkScrollOffsetChanged()
{
    if (m_terminal) {
        emit scrollOffsetChanged();
    }
}

int TerminalBackend::visibleStart() const
{
    if (!m_terminal) {
        return 0;
    }

    const Screen &s = m_terminal->screen();
    return s.totalLines() - s.rows() - s.scrollOffset();
}

void TerminalBackend::selPress(int row, int col)
{
    if (!m_terminal) {
        return;
    }

    const int absLine = visibleStart() + row;
    const int c = std::clamp(col, 0, m_terminal->screen().cols() - 1);
    m_selAnchor = {.line = absLine, .col = c};
    m_selCurrent = {.line = absLine, .col = c};

    emit selectionChanged();
}

void TerminalBackend::selMove(int row, int col)
{
    if (!m_terminal || m_selAnchor.line < 0) {
        return;
    }

    Screen &s = m_terminal->screen();
    int absLine = visibleStart() + row;
    const int c = std::clamp(col, 0, s.cols() - 1);
    // Rubber-band auto-scroll: dragging into the top/bottom edge reveals more
    // history, like selecting text in any editor. The selection follows the
    // mouse in absolute coordinates, so it stays on the same text.
    bool scrolled = false;
    if (row <= 0 && s.scrollOffset() < s.maxScrollOffset()) {
        s.scrollBy(1); // up = older lines
        absLine = visibleStart();
        scrolled = true;
    } else if (row >= s.rows() - 1 && s.scrollOffset() > 0) {
        s.scrollBy(-1); // down = newer lines
        absLine = visibleStart() + (s.rows() - 1);
        scrolled = true;
    }

    m_selCurrent = {.line = absLine, .col = c};
    emit selectionChanged();

    if (scrolled) {
        checkScrollOffsetChanged();
        scheduleFrame();
    }
}

void TerminalBackend::selRelease()
{
    if (m_selAnchor.line < 0) {
        return;
    }

    // A click that didn't drag collapses to a point: clear the selection.
    if (m_selAnchor.line == m_selCurrent.line && m_selAnchor.col == m_selCurrent.col) {
        m_selAnchor = {.line = -1, .col = 0};
        m_selCurrent = {.line = -1, .col = 0};
    }

    emit selectionChanged();
}

void TerminalBackend::selClear()
{
    if (m_selAnchor.line < 0) {
        return;
    }

    m_selAnchor = {.line = -1, .col = 0};
    m_selCurrent = {.line = -1, .col = 0};
    emit selectionChanged();
}

void TerminalBackend::selWord(int row, int col)
{
    if (!m_terminal) {
        return;
    }

    Screen const &s = m_terminal->screen();
    const int absLine = visibleStart() + row;
    const Line &line = s.lineAt(absLine);
    if (col < 0 || col >= static_cast<int>(line.size())) {
        m_selAnchor = {.line = -1, .col = 0};
        m_selCurrent = {.line = -1, .col = 0};
        emit selectionChanged();
        return;
    }

    auto const isWordChar = [](QChar ch) { return ch.isLetterOrNumber() || ch == '_' || ch == '-'; };
    int startCol = col;
    int endCol = col;
    while (startCol > 0 && isWordChar(line[startCol - 1].ch)) {
        --startCol;
    }

    while (endCol + 1 < static_cast<int>(line.size()) && isWordChar(line[endCol + 1].ch)) {
        ++endCol;
    }

    m_selAnchor = {.line = absLine, .col = startCol};
    m_selCurrent = {.line = absLine, .col = endCol};
    emit selectionChanged();
}

void TerminalBackend::selectAll()
{
    if (!m_terminal) {
        return;
    }

    Screen const &s = m_terminal->screen();
    const int start = visibleStart();
    m_selAnchor = {.line = start, .col = 0};
    m_selCurrent = {.line = start + s.rows() - 1, .col = s.cols() - 1};
    emit selectionChanged();
}

void TerminalBackend::copySelection()
{
    if (!m_terminal || m_selAnchor.line < 0) {
        return;
    }

    Screen const &s = m_terminal->screen();
    // Reading-order selection: the earlier point starts the selection (its
    // line runs from that column to the end of the text), the later point
    // ends it (its line runs from column 0 to that column). Middle lines are
    // whole. Symmetric in drag direction, like selecting text in an editor.
    const int sLine = m_selAnchor.line;
    const int eLine = m_selCurrent.line;
    const int sCol = m_selAnchor.col;
    const int eCol = m_selCurrent.col;
    const int top = std::min(sLine, eLine);
    const int bot = std::max(sLine, eLine);
    const int topCol = (sLine == top) ? sCol : eCol;
    const int botCol = (eLine == bot) ? eCol : sCol;
    QString text;

    for (int line = top; line <= bot; ++line) {
        const Line &l = s.lineAt(line);
        int c0 = 0;
        int c1 = 0;
        // c1 is an exclusive end for the copy loop below; the selection points
        // are inclusive columns, so each bound gets +1.
        if (top == bot) {
            c0 = std::min(sCol, eCol);
            c1 = std::max(sCol, eCol) + 1;
        } else if (line == top) {
            c0 = topCol;
            c1 = lineTextEnd(line) + 1;
        } else if (line == bot) {
            c0 = 0;
            c1 = botCol + 1;
        } else {
            c0 = 0;
            c1 = lineTextEnd(line) + 1;
        }
        c1 = std::min(c1, static_cast<int>(l.size()));
        while (c1 > c0 && l[c1 - 1].ch == ' ') {
            --c1;
        }
        for (int c = c0; c < c1; ++c) {
            text.append(l[c].ch);
        }
        if (line < bot) {
            text.append('\n');
        }
    }
    QGuiApplication::clipboard()->setText(text);
}

void TerminalBackend::paste()
{
    if (!m_pty) {
        return;
    }
    const QString text = QGuiApplication::clipboard()->text();
    if (!text.isEmpty()) {
        m_pty->write(text.toUtf8());
    }
}

int TerminalBackend::lineTextEnd(int line) const
{
    const Line &l = m_terminal->screen().lineAt(line);
    int end = static_cast<int>(l.size()) - 1;
    while (end >= 0 && l[end].ch == ' ') {
        --end;
    }
    if (end < 0) {
        return m_terminal->screen().cols() - 1; // blank line: full width
    }
    return end;
}

void TerminalBackend::editorShortcuts(int key)
{
    if (key == static_cast<int>(Qt::Key_C)) {
        copySelection();
        return;
    }

    if (key == static_cast<int>(Qt::Key_V)) {
        paste();
        return;
    }

    if (key == static_cast<int>(Qt::Key_A)) {
        selectAll();
        return;
    }
}

int TerminalBackend::selRowLeft(int visibleRow) const
{
    if (!m_terminal || m_selAnchor.line < 0) {
        return -1;
    }
    const int sLine = m_selAnchor.line;
    const int eLine = m_selCurrent.line;
    const int sCol = m_selAnchor.col;
    const int eCol = m_selCurrent.col;
    const int topLine = std::min(sLine, eLine);
    const int botLine = std::max(sLine, eLine);
    const int absLine = visibleStart() + visibleRow;
    if (absLine < topLine || absLine > botLine) {
        return -1;
    }
    if (topLine == botLine) {
        return std::min(sCol, eCol);
    }
    if (absLine == topLine) {
        // The top line is clipped at the column of the point that sits on it.
        return (sLine == topLine) ? sCol : eCol;
    }
    return 0;
}

int TerminalBackend::selRowRight(int visibleRow) const
{
    if (!m_terminal || m_selAnchor.line < 0) {
        return -1;
    }
    const int sLine = m_selAnchor.line;
    const int eLine = m_selCurrent.line;
    const int sCol = m_selAnchor.col;
    const int eCol = m_selCurrent.col;
    const int topLine = std::min(sLine, eLine);
    const int botLine = std::max(sLine, eLine);
    const int absLine = visibleStart() + visibleRow;
    if (absLine < topLine || absLine > botLine) {
        return -1;
    }
    if (topLine == botLine) {
        return std::max(sCol, eCol);
    }
    if (absLine == botLine) {
        // The bottom line is clipped at the column of the point that sits on it.
        return (eLine == botLine) ? eCol : sCol;
    }
    return lineTextEnd(absLine);
}

QVariantList TerminalBackend::snapshot() const
{
    QVariantList rowsOut;
    if (!m_terminal) {
        return rowsOut;
    }
    const Screen &s = m_terminal->screen();
    // The visible window is the last `rows` lines, shifted up by the scroll
    // offset (0 = at the bottom).
    const int total = s.totalLines();
    const int start = total - s.rows() - s.scrollOffset();
    static const Cell kBlank{};
    for (int r = 0; r < s.rows(); ++r) {
        const Line &line = s.lineAt(start + r);
        QVariantList row;
        for (int c = 0; c < s.cols(); ++c) {
            const Cell &cell = (c < static_cast<int>(line.size())) ? line[c] : kBlank;
            unsigned int fg = resolveColor(cell.fg, m_defaultFg);
            unsigned int bg = resolveColor(cell.bg, m_defaultBg);

            if ((cell.flags & CellFlag::Reverse) != 0) {
                std::swap(fg, bg);
            }

            QVariantMap m;
            m["ch"] = QString(cell.ch);
            m["fg"] = static_cast<int>(fg);
            m["bg"] = static_cast<int>(bg);
            m["flags"] = static_cast<int>(cell.flags);
            row.append(m);
        }
        rowsOut.append(row);
    }
    return rowsOut;
}

int TerminalBackend::cursorRow() const
{
    return m_terminal ? m_terminal->screen().cursorRow() : 0;
}

int TerminalBackend::cursorCol() const
{
    return m_terminal ? m_terminal->screen().cursorCol() : 0;
}

bool TerminalBackend::cursorVisible() const
{
    return m_terminal ? m_terminal->screen().cursorVisible() : true;
}

QString TerminalBackend::detectDefaultShell()
{
    struct passwd pwd{};
    struct passwd *result = nullptr;
    std::array<char, 4096> buf;
    if (getpwuid_r(getuid(), &pwd, buf.data(), buf.size(), &result) == 0 && (result != nullptr)) {
        const QString shell = QString::fromLocal8Bit(result->pw_shell);
        if (!shell.isEmpty() && QFileInfo(shell).isExecutable()) {
            return shell;
        }
    }
    if (QFileInfo::exists("/bin/zsh")) {
        return "/bin/zsh";
    }
    return "/bin/sh";
}

QRgb TerminalBackend::resolveColor(
    unsigned int encoded,
    int defaultColor
) // NOLINT: encoded/defaultColor are inherently swappable
{
    if (encoded == 0) {
        return static_cast<QRgb>(defaultColor);
    }
    if (encoded <= 256) {
        static const std::array<QRgb, 256> palette = buildPalette();
        return static_cast<QRgb>(0xff000000u | palette[encoded - 1]);
    }
    if ((encoded & 0x80000000u) != 0u) {
        const unsigned int rgb = encoded & 0xffffff;
        return static_cast<QRgb>(0xff000000u | rgb);
    }
    return static_cast<QRgb>(defaultColor);
}

} // namespace konsoll
