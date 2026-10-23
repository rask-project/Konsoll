#include "TerminalBackend.h"

#include <QColor>
#include <QFileInfo>
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

void TerminalBackend::keyPress(
    int key,
    const QString &text,
    int modifiers
) // NOLINT: keyboard dispatch is inherently branching
{
    const bool ctrl = (modifiers & static_cast<int>(Qt::ControlModifier)) != 0;
    const bool shift = (modifiers & static_cast<int>(Qt::ShiftModifier)) != 0;
    const bool meta = (modifiers & static_cast<int>(Qt::MetaModifier)) != 0;

    if (meta && key == static_cast<int>(Qt::Key_Q)) {
        emit quitRequested();
        return;
    }

    QByteArray out;
    if (ctrl) {
        if (!text.isEmpty()) {
            const QChar c = text.at(0);
            if (c.unicode() < 0x20) {
                out.append(static_cast<char>(c.unicode()));
            } else if (shift) {
                const QChar u = c.toUpper();
                if (u >= 'A' && u <= 'Z') {
                    out.append(static_cast<char>((u.unicode() - 'A' + 1) & 0x1f));
                }
            } else {
                const QChar l = c.toLower();
                if (l >= 'a' && l <= 'z') {
                    out.append(static_cast<char>((l.unicode() - 'a' + 1) & 0x1f));
                }
            }
        }
    } else {
        switch (key) {
        case static_cast<int>(Qt::Key_Return):
        case static_cast<int>(Qt::Key_Enter):
            out.append(static_cast<char>(0x0d));
            break;
        case static_cast<int>(Qt::Key_Backspace):
            out.append(static_cast<char>(0x7f));
            break;
        case static_cast<int>(Qt::Key_Tab):
            out.append(static_cast<char>(0x09));
            break;
        case static_cast<int>(Qt::Key_Escape):
            out.append(static_cast<char>(0x1b));
            break;
        case static_cast<int>(Qt::Key_Up):
            out = "\x1b[A";
            break;
        case static_cast<int>(Qt::Key_Down):
            out = "\x1b[B";
            break;
        case static_cast<int>(Qt::Key_Right):
            out = "\x1b[C";
            break;
        case static_cast<int>(Qt::Key_Left):
            out = "\x1b[D";
            break;
        case static_cast<int>(Qt::Key_Home):
            out = "\x1b[H";
            break;
        case static_cast<int>(Qt::Key_End):
            out = "\x1b[F";
            break;
        case static_cast<int>(Qt::Key_Delete):
            out = "\x1b[3~";
            break;
        case static_cast<int>(Qt::Key_PageUp):
            out = "\x1b[5~";
            break;
        case static_cast<int>(Qt::Key_PageDown):
            out = "\x1b[6~";
            break;
        default:
            if (!text.isEmpty()) {
                out = text.toUtf8();
            }
            break;
        }
    }
    if (!out.isEmpty() && m_pty) {
        // Temporary input trace: hex preserves control keys and UTF-8 without
        // truncating at NUL or interpreting terminal escape sequences.
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
    emit sizeChanged();
    scheduleFrame();
}

QVariantList TerminalBackend::snapshot() const
{
    QVariantList rowsOut;
    if (!m_terminal) {
        return rowsOut;
    }
    const Screen &s = m_terminal->screen();
    for (int r = 0; r < s.rows(); ++r) {
        QVariantList row;
        for (int c = 0; c < s.cols(); ++c) {
            const Cell &cell = s.cellAt(r, c);
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
