#include "Terminal.h"

#include <QUrl>

#include <algorithm>

namespace konsoll
{

Terminal::Terminal(int rows, int cols) : m_screen(rows, cols) {}

void Terminal::write(const QByteArray &data)
{
    for (const char b : data) {
        processByte(static_cast<unsigned char>(b));
    }
}

void Terminal::resize(int rows, int cols)
{
    m_screen.resize(rows, cols);
    clearCommandBlocks();
}

void Terminal::clearCommandBlocks()
{
    m_commandBlocks.clear();
    m_commandStartLine = -1;
}

QByteArray Terminal::daResponse()
{
    // Minimal "vt220-ish" primary device attributes reply.
    return "\x1b[?62c";
}

QByteArray Terminal::colorQueryResponse(unsigned char query)
{
    // Match the default colors exposed by TerminalBackend (foreground
    // #e0e0e0, background #141414), in the OSC 10/11 rgb: format.
    if (query == 10) {
        return "\x1b]10;rgb:e0e0/e0e0/e0e0\x1b\\";
    }
    return "\x1b]11;rgb:1414/1414/1414\x1b\\";
}

QByteArray Terminal::dsrResponse() const
{
    if (m_dsrQuery == 6) {
        return QByteArray("\x1b[") + QByteArray::number(m_screen.cursorRow() + 1) + ";" +
               QByteArray::number(m_screen.cursorCol() + 1) + "R";
    }
    return "\x1b[0n";
}

QByteArray Terminal::mouseReport(int button, int row, int col, int event, unsigned int modifiers) const
{
    const bool anyMotion = (m_mouseModes & 0x04u) != 0;
    const bool buttonMotion = (m_mouseModes & 0x02u) != 0;
    const bool motion = event == 2;
    const bool release = event == 1;
    const bool wheel = event == 3 || event == 4;
    if (!mouseTracking() || (motion && !anyMotion && (!buttonMotion || button < 0))) {
        return {};
    }

    int code = wheel ? (event == 3 ? 64 : 65) : ((button < 0) ? 3 : button);
    if (motion) {
        code |= 32;
    }
    if ((modifiers & 1u) != 0) {
        code |= 4;
    }
    if ((modifiers & 2u) != 0) {
        code |= 8;
    }
    if ((modifiers & 4u) != 0) {
        code |= 16;
    }

    const int x = std::clamp(col + 1, 1, m_screen.cols());
    const int y = std::clamp(row + 1, 1, m_screen.rows());
    if (m_sgrMouse) {
        QByteArray report = "\x1b[<" + QByteArray::number(code) + ";" + QByteArray::number(x) + ";" +
                            QByteArray::number(y);
        report.append(release ? 'm' : 'M');
        return report;
    }

    const int legacyCode = (release ? 3 | (code & 28) : code) + 32;
    if (x > 223 || y > 223 || legacyCode > 255) {
        return {};
    }
    return QByteArray("\x1b[M") + QByteArray(1, static_cast<char>(legacyCode)) +
           QByteArray(1, static_cast<char>(x + 32)) + QByteArray(1, static_cast<char>(y + 32));
}

void Terminal::processByte(unsigned char c)
{
    if (m_state == State::Ground && m_utf8Remaining != 0) {
        if (c >= 0x80 && c <= 0xbf) {
            m_utf8CodePoint = (m_utf8CodePoint << 6) | (c & 0x3fu);
            --m_utf8Remaining;
            if (m_utf8Remaining == 0) {
                putCodePoint(m_utf8CodePoint < m_utf8Minimum ? 0xfffd : m_utf8CodePoint);
                m_utf8Minimum = 0;
            }
            return;
        }
        putCodePoint(0xfffd);
        m_utf8Remaining = 0;
        m_utf8Minimum = 0;
    }

    switch (m_state) {
    case State::Ground:
        if (c == 0x1b) {
            m_state = State::Escape;
            m_csiBody.clear();
            m_csiPrivate = false;
        } else if (c == '\r') {
            m_screen.carriageReturn();
        } else if (c == '\n') {
            m_screen.lineFeed();
        } else if (c == '\b') {
            m_screen.backspace();
        } else if (c == '\t') {
            m_screen.tab();
        } else if (c == 0x07 || c == 0x08 || c == 0x09) {
            // BEL / SO / SI: charset shift and bell are ignored.
        } else if (c == 0x00) {
            // NUL: dropped.
        } else if (c < 0x80) {
            m_screen.putChar(c, m_fg, m_bg, m_flags);
        } else if (c >= 0xc2 && c <= 0xdf) {
            m_utf8CodePoint = c & 0x1fu;
            m_utf8Minimum = 0x80;
            m_utf8Remaining = 1;
        } else if (c >= 0xe0 && c <= 0xef) {
            m_utf8CodePoint = c & 0x0fu;
            m_utf8Minimum = 0x800;
            m_utf8Remaining = 2;
        } else if (c >= 0xf0 && c <= 0xf4) {
            m_utf8CodePoint = c & 0x07u;
            m_utf8Minimum = 0x10000;
            m_utf8Remaining = 3;
        } else {
            putCodePoint(0xfffd);
        }
        break;

    case State::Escape: // NOLINT: escape dispatch is inherently branching
        if (c == '[') {
            m_state = State::Csi;
            m_csiBody.clear();
            m_csiPrivate = false;
        } else if (c == '?') {
            m_state = State::Csi;
            m_csiBody.clear();
            m_csiPrivate = true;
        } else if (c == ']') {
            m_state = State::Osc;
            m_oscBody.clear();
        } else if (c >= 0x20 && c <= 0x2f) {
            // ESC sequences with an intermediate byte (for example ESC ( B
            // to select the ASCII character set) end with a separate final
            // byte. Consume both; otherwise that final byte leaks as text.
            m_state = State::EscapeIntermediate;
        } else {
            handleEsc(static_cast<char>(c));
            m_state = State::Ground;
        }
        break;

    case State::EscapeIntermediate:
        if (c >= 0x30 && c <= 0x7e) {
            m_state = State::Ground;
        }
        break;

    case State::Csi:
        if (c >= 0x40 && c <= 0x7e) {
            handleCsi(static_cast<char>(c), m_csiPrivate);
            m_state = State::Ground;
            m_csiBody.clear();
            m_csiPrivate = false;
        } else if (c == '?' && m_csiBody.isEmpty()) {
            // DEC private modes follow '[' directly: CSI ? 1049 h.
            m_csiPrivate = true;
        } else {
            m_csiBody.append(static_cast<char>(c));
        }
        break;

    case State::Osc:
        if (c == 0x07 || c == 0x9c) {
            handleOsc(m_oscBody);
            m_state = State::Ground;
            m_oscBody.clear();
        } else if (c == 0x1b) {
            // ST (ESC \) terminates the string.
            handleOsc(m_oscBody);
            m_state = State::Escape;
            m_oscBody.clear();
        } else {
            m_oscBody.append(static_cast<char>(c));
        }
        break;
    }
}

void Terminal::putCodePoint(unsigned int codePoint)
{
    if (codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff) || codePoint < 0x80) {
        codePoint = 0xfffd;
    }

    // Supplementary-plane characters (e.g. Nerd Font icons, U+F0xxx) occupy
    // one cell holding the full code point; the renderer encodes them as a
    // surrogate-pair string.
    m_screen.putChar(codePoint, m_fg, m_bg, m_flags);
}

std::vector<int> Terminal::parseParams(const QByteArray &body)
{
    std::vector<int> out;
    int cur = 0;
    bool has = false;
    for (const char c : body) {
        if (c >= '0' && c <= '9') {
            cur = (cur * 10) + (c - '0');
            has = true;
        } else if (c == ';' || c == ':') {
            // In colon-form SGR, an empty color-space field is optional
            // (for example 38:2::r:g:b), so omit empty colon fields. Keep
            // empty semicolon fields as zero-valued default parameters.
            if (has || c == ';') {
                out.push_back(has ? cur : 0);
            }
            cur = 0;
            has = false;
        }
        // Other intermediate bytes are ignored here.
    }
    out.push_back(has ? cur : 0);
    return out;
}

int Terminal::paramAt(const std::vector<int> &p, int i, int def) // NOLINT: i/def are inherently swappable
{
    if (i >= static_cast<int>(p.size()) || p[i] == 0) {
        return def;
    }
    return p[i];
}

void Terminal::handleCsi(char final, bool privateMode)
{
    if (privateMode) {
        handleDec(final, parseParams(m_csiBody));
        return;
    }

    const std::vector<int> p = parseParams(m_csiBody);

    switch (final) {
    case 'A':
        cursorMove(-paramAt(p, 0, 1), 0);
        break;
    case 'B':
        cursorMove(paramAt(p, 0, 1), 0);
        break;
    case 'C':
        cursorMove(0, paramAt(p, 0, 1));
        break;
    case 'D':
        cursorMove(0, -paramAt(p, 0, 1));
        break;
    case 'G':
        m_screen.moveCursor(m_screen.cursorRow(), paramAt(p, 0, 1) - 1);
        break;
    case 'd':
        m_screen.moveCursor(paramAt(p, 0, 1) - 1, m_screen.cursorCol());
        break;
    case 'H':
    case 'f':
        m_screen.moveCursor(paramAt(p, 0, 1) - 1, paramAt(p, 1, 1) - 1);
        break;
    case 'J': // NOLINT: similar erase branches
        m_screen.eraseInDisplay(paramAt(p, 0, 0));
        if (paramAt(p, 0, 0) == 2) {
            m_clearPending = true; // full-screen erase: selection now sits over blanks.
        }
        break;
    case 'K': // NOLINT: similar erase branches
        m_screen.eraseInLine(paramAt(p, 0, 0));
        break;
    case 'm':
        handleSgr(p);
        break;
    case 'P':
        m_screen.deleteColumns(paramAt(p, 0, 1));
        break;
    case '@':
        m_screen.insertColumns(paramAt(p, 0, 1));
        break;
    case 'r':
        if (p.size() >= 2) {
            m_screen.setScrollRegion(paramAt(p, 0, 1), paramAt(p, 1, m_screen.rows()));
        } else {
            m_screen.scrollUp(paramAt(p, 0, 1));
        }
        break;
    case 'M':
        m_screen.scrollDown(paramAt(p, 0, 1));
        break;
    case 'c':
        m_daPending = true;
        break; // Primary DA query.
    case 'n':
        if (paramAt(p, 0, 0) == 5 || paramAt(p, 0, 0) == 6) {
            m_dsrQuery = static_cast<unsigned char>(paramAt(p, 0, 0));
        }
        break; // Device status / cursor position report.
    case 'h':  // NOLINT: similar no-op branches
    case 'l':
        break; // Non-private modes: not needed for the MVP.
    default:   // NOLINT: similar no-op branches
        break;
    }
}

void Terminal::handleDec(char final, const std::vector<int> &params)
{
    const bool set = (final == 'h');
    for (const int mode : params) {
        switch (mode) {
        case 25:
            m_screen.showCursor(set);
            break;
        case 1049:
            clearCommandBlocks();
            if (set) {
                m_screen.enterAltScreen();
            } else {
                m_screen.leaveAltScreen();
            }
            break;
        case 1000:
            m_mouseModes = set ? static_cast<unsigned char>(m_mouseModes | 0x01u)
                               : static_cast<unsigned char>(m_mouseModes & ~0x01u);
            break;
        case 1002:
            m_mouseModes = set ? static_cast<unsigned char>(m_mouseModes | 0x02u)
                               : static_cast<unsigned char>(m_mouseModes & ~0x02u);
            break;
        case 1003:
            m_mouseModes = set ? static_cast<unsigned char>(m_mouseModes | 0x04u)
                               : static_cast<unsigned char>(m_mouseModes & ~0x04u);
            break;
        case 1006:
            m_sgrMouse = set;
            break;
        case 2004:
            break; // Bracketed paste: accepted, unused in the MVP.
        case 6: // Origin mode: ignored.
        default:
            break;
        }
    }
}

void Terminal::handleSgr(const std::vector<int> &p) // NOLINT: SGR dispatch is inherently branching
{
    if (p.empty()) {
        m_fg = 0;
        m_bg = 0;
        m_flags = CellFlag::None;
        return;
    }

    for (std::size_t i = 0; i < p.size(); ++i) {
        const int n = p[i];
        switch (n) {
        case 0:
            m_fg = 0;
            m_bg = 0;
            m_flags = CellFlag::None;
            break;
        case 1:
            m_flags |= CellFlag::Bold;
            break;
        case 2:
            m_flags |= CellFlag::Dim;
            break;
        case 4:
            m_flags |= CellFlag::Underline;
            break;
        case 7:
            m_flags |= CellFlag::Reverse;
            break;
        case 9:
            m_flags |= CellFlag::Strike;
            break;
        case 22:
            m_flags &= ~(CellFlag::Bold | CellFlag::Dim);
            break;
        case 23:
            m_flags &= ~CellFlag::Strike;
            break;
        case 24:
            m_flags &= ~CellFlag::Underline;
            break;
        case 27:
            m_flags &= ~CellFlag::Reverse;
            break;
        case 29:
            m_flags &= ~CellFlag::Strike;
            break;
        case 39:
            m_fg = 0;
            break;
        case 49:
            m_bg = 0;
            break;
        case 38: {
            if (i + 1 < p.size() && p[i + 1] == 5 && i + 2 < p.size()) {
                m_fg = static_cast<unsigned int>(p[i + 2] + 1); // 256 palette -> 1..256
                i += 2;
            } else if (i + 1 < p.size() && p[i + 1] == 2 && i + 4 < p.size()) {
                const int r = std::clamp(p[i + 2], 0, 255);
                const int g = std::clamp(p[i + 3], 0, 255);
                const int b = std::clamp(p[i + 4], 0, 255);
                m_fg = 0x80000000u | (static_cast<unsigned int>(r) << 16) | (static_cast<unsigned int>(g) << 8) |
                       static_cast<unsigned int>(b);
                i += 4;
            }
            break;
        }
        case 48: {
            if (i + 1 < p.size() && p[i + 1] == 5 && i + 2 < p.size()) {
                m_bg = p[i + 2] + 1;
                i += 2;
            } else if (i + 1 < p.size() && p[i + 1] == 2 && i + 4 < p.size()) {
                const int r = std::clamp(p[i + 2], 0, 255);
                const int g = std::clamp(p[i + 3], 0, 255);
                const int b = std::clamp(p[i + 4], 0, 255);
                m_bg = 0x80000000u | (static_cast<unsigned int>(r) << 16) | (static_cast<unsigned int>(g) << 8) |
                       static_cast<unsigned int>(b);
                i += 4;
            }
            break;
        }
        default:
            if (n >= 30 && n <= 37) {
                m_fg = n - 30 + 1;
            } else if (n >= 40 && n <= 47) {
                m_bg = n - 40 + 1;
            } else if (n >= 90 && n <= 97) {
                m_fg = n - 90 + 9;
            } else if (n >= 100 && n <= 107) {
                m_bg = n - 100 + 9;
            }
            break;
        }
    }
}

void Terminal::handleOsc(const QByteArray &body)
{
    if (body.startsWith("7;")) {
        const QUrl url = QUrl::fromEncoded(body.mid(2), QUrl::TolerantMode);
        const QString path = url.scheme() == QStringLiteral("file") ? url.path(QUrl::FullyDecoded) : QString{};
        if (path.startsWith(QLatin1Char('/')) && path != m_currentDirectory) {
            m_currentDirectory = path;
            m_currentDirectoryDirty = true;
        }
        return;
    }

    if (body.startsWith("133;") && body.size() > 4) {
        handleCommandMark(body.at(4));
        return;
    }

    // Form: <num>;<text>. We only honor 0 (icon+title) and 2 (title).
    const int idx = body.indexOf(';');
    if (idx <= 0) {
        return;
    }
    QByteArray const num = body.left(idx);
    const QString text = QString::fromUtf8(body.mid(idx + 1));
    if (num == "0" || num == "2") {
        if (m_title != text) {
            m_title = text;
            m_titleDirty = true;
        }
    }
    if ((num == "10" || num == "11") && body.mid(idx + 1) == "?") {
        m_colorQuery = static_cast<unsigned char>(num == "10" ? 10 : 11);
    }
    // "8;" (hyperlink) and others: ignored.
}

void Terminal::handleCommandMark(char mark)
{
    if (m_screen.inAltScreen()) {
        return;
    }

    const int line = m_screen.totalLines() - m_screen.rows() + m_screen.cursorRow();
    switch (mark) {
    case 'B':
        m_commandStartLine = line;
        break;
    case 'C':
        // C marks the start of execution. Use it as a fallback when a shell
        // emits execution/finish markers but omits the command-start marker.
        if (m_commandStartLine < 0) {
            m_commandStartLine = std::max(0, line - (m_screen.cursorCol() == 0 ? 1 : 0));
        }
        break;
    case 'D':
        if (m_commandStartLine >= 0) {
            const int endLine = std::max(m_commandStartLine, line - (m_screen.cursorCol() == 0 ? 1 : 0));
            m_commandBlocks.push_back({m_commandStartLine, endLine});
            m_commandStartLine = -1;
        }
        break;
    default:
        break;
    }
}

void Terminal::handleEsc(char c)
{
    switch (c) {
    case '7':
        m_screen.saveCursor();
        break;
    case '8':
        m_screen.restoreCursor();
        break;
    case 'M':
        m_screen.reverseIndex();
        break;
    case 'D':
        m_screen.lineFeed();
        break;
    case 'E':
        m_screen.carriageReturn();
        m_screen.lineFeed();
        break;
    case 'c':
        m_screen.fullReset();
        clearCommandBlocks();
        m_fg = 0;
        m_bg = 0;
        m_flags = CellFlag::None;
        break;
    default:
        break;
    }
}

void Terminal::cursorMove(int dRow, int dCol) // NOLINT: dRow/dCol are inherently swappable
{
    const int r = m_screen.cursorRow() + dRow;
    const int c = m_screen.cursorCol() + dCol;
    m_screen.moveCursor(r, c);
}

} // namespace konsoll
