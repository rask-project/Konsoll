#include "Terminal.h"

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
}

QByteArray Terminal::daResponse()
{
    // Minimal "vt220-ish" primary device attributes reply.
    return "\x1b[?62c";
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
            m_screen.putChar(QChar::fromLatin1(static_cast<char>(c)), m_fg, m_bg, m_flags);
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
        } else {
            handleEsc(static_cast<char>(c));
            m_state = State::Ground;
        }
        break;

    case State::Csi:
        if (c >= 0x40 && c <= 0x7e) {
            handleCsi(static_cast<char>(c), m_csiPrivate);
            m_state = State::Ground;
            m_csiBody.clear();
            m_csiPrivate = false;
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

    if (codePoint <= 0xffff) {
        m_screen.putChar(QChar(static_cast<ushort>(codePoint)), m_fg, m_bg, m_flags);
        return;
    }

    const unsigned int supplementary = codePoint - 0x10000;
    m_screen.putChar(QChar(static_cast<ushort>(0xd800 + (supplementary >> 10))), m_fg, m_bg, m_flags);
    m_screen.putChar(QChar(static_cast<ushort>(0xdc00 + (supplementary & 0x3ff))), m_fg, m_bg, m_flags);
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
        } else if (c == ';') {
            out.push_back(has ? cur : 0);
            cur = 0;
            has = false;
        }
        // Intermediate bytes (e.g. '?') are ignored here.
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
    case 'h':  // NOLINT: similar no-op branches
    case 'l':
        break; // Non-private modes: not needed for the MVP.
    default:   // NOLINT: similar no-op branches
        break;
    }
}

void Terminal::handleDec(char final, const std::vector<int> &params)
{
    const int mode = paramAt(params, 0, 0);
    const bool set = (final == 'h');

    switch (mode) {
    case 25:
        m_screen.showCursor(set);
        break;
    case 1049:
        if (set) {
            m_screen.enterAltScreen();
        } else {
            m_screen.leaveAltScreen();
        }
        break;
    case 2004:
        break; // Bracketed paste: accepted, unused in the MVP.
    case 6:    // Origin mode: ignored.
        break;
    default:
        break;
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
    // "8;" (hyperlink) and others: ignored.
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
