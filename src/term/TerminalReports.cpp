// Replies to terminal queries and unsolicited reports: device attributes and
// status, mode and setting requests, termcap queries, window reports, color
// queries and mouse reports. Split from Terminal.cpp, which parses and
// dispatches the sequences.

#include "Terminal.h"

#include "Palette.h"

#include <QColor>
#include <QString>

#include <algorithm>
#include <array>
#include <ranges>
#include <string_view>

namespace konsoll
{

namespace
{

QByteArray number(int value)
{
    return QByteArray::number(value);
}

// KONSOLL_VERSION "3.0.1" as the DA2 firmware version 30001.
int versionNumber()
{
    const QList<QByteArray> parts = QByteArray(KONSOLL_VERSION).split('.');
    const auto part = [&parts](qsizetype index) { return index < parts.size() ? parts[index].toInt() : 0; };
    return (part(0) * 10000) + (part(1) * 100) + part(2);
}

// An OSC 10/11/12 / OSC 4 color reply value: rgb:rrrr/gggg/bbbb.
QByteArray rgbSpec(unsigned int argb)
{
    const QColor color = QColor::fromRgba(argb);
    const auto channel = [](int value) { return QByteArray::number(value * 257, 16).rightJustified(4, '0'); };
    return "rgb:" + channel(color.red()) + "/" + channel(color.green()) + "/" + channel(color.blue());
}

bool isDarkBackground(unsigned int argb)
{
    const QColor color = QColor::fromRgba(argb);
    return ((color.red() * 299) + (color.green() * 587) + (color.blue() * 114)) / 1000 < 128;
}

// The SGR parameters selecting an encoded cell color (see Cell.h), with the
// base code for basic colors (30 fg / 40 bg) and extended selector (38/48/58).
QByteArray colorSgr(unsigned int encoded, int basicBase, int extended)
{
    if (encoded == 0) {
        return {};
    }
    if (encoded <= 8 && basicBase != 0) {
        return ";" + number(basicBase + static_cast<int>(encoded) - 1);
    }
    if (encoded <= 16 && basicBase != 0) {
        return ";" + number(basicBase + 60 + static_cast<int>(encoded) - 9);
    }
    if (encoded <= 256) {
        return ";" + number(extended) + ":5:" + number(static_cast<int>(encoded) - 1);
    }
    const auto component = [encoded](unsigned int shift) { return number(static_cast<int>((encoded >> shift) & 0xffu)); };
    return ";" + number(extended) + ":2::" + component(16) + ":" + component(8) + ":" + component(0);
}

// XTGETTCAP: the terminfo capabilities an application may ask for. A
// capability with an empty value is boolean.
struct TermCap {
    std::string_view name;
    std::string_view value;
};

constexpr std::array kTermCaps{
    TermCap{.name = "TN", .value = "xterm-256color"},
    TermCap{.name = "name", .value = "xterm-256color"},
    TermCap{.name = "Co", .value = "256"},
    TermCap{.name = "colors", .value = "256"},
    TermCap{.name = "RGB", .value = ""},
    TermCap{.name = "Tc", .value = ""},
    TermCap{.name = "bce", .value = ""},
    TermCap{.name = "setrgbf", .value = "\x1b[38:2::%p1%d:%p2%d:%p3%dm"},
    TermCap{.name = "setrgbb", .value = "\x1b[48:2::%p1%d:%p2%d:%p3%dm"},
    TermCap{.name = "Smulx", .value = "\x1b[4:%p1%dm"},
    TermCap{.name = "Setulc",
            .value = "\x1b[58:2::%p1%{65536}%/%d:%p1%{256}%/%{255}%&%d:%p1%{255}%&%dm"},
    TermCap{.name = "Ss", .value = "\x1b[%p1%d q"},
    TermCap{.name = "Se", .value = "\x1b[2 q"},
    TermCap{.name = "Ms", .value = "\x1b]52;%p1%s;%p2%s\x07"},
    TermCap{.name = "Sync", .value = "\x1b[?2026%?%p1%{1}%-%tl%eh%;"},
    TermCap{.name = "BE", .value = "\x1b[?2004h"},
    TermCap{.name = "BD", .value = "\x1b[?2004l"},
    TermCap{.name = "PS", .value = "\x1b[200~"},
    TermCap{.name = "PE", .value = "\x1b[201~"},
    TermCap{.name = "smcup", .value = "\x1b[?1049h"},
    TermCap{.name = "rmcup", .value = "\x1b[?1049l"},
    TermCap{.name = "kbs", .value = "\x7f"},
};

} // namespace

void Terminal::setDefaultColors(unsigned int foreground, unsigned int background)
{
    m_defaultFg = foreground;
    m_defaultBg = background;
    const bool dark = isDarkBackground(background);
    if (dark == ((m_extraFlags & DarkColorScheme) != 0)) {
        return;
    }
    setExtraFlag(DarkColorScheme, dark);
    if ((m_extraFlags & ColorSchemeReports) != 0) {
        reply(dark ? "\x1b[?997;1n" : "\x1b[?997;2n"); // mode 2031 notification
    }
}

void Terminal::setCellPixelSize(int width, int height)
{
    m_cellPixelWidth = static_cast<unsigned short>(std::clamp(width, 1, 0xffff));
    m_cellPixelHeight = static_cast<unsigned short>(std::clamp(height, 1, 0xffff));
}

void Terminal::handleDeviceAttributes(char prefix, int param)
{
    if (param != 0) {
        return;
    }
    if (prefix == 0) {
        reply("\x1b[?62;22c"); // DA1: VT220 with ANSI color
    } else if (prefix == '>') {
        reply("\x1b[>1;" + number(versionNumber()) + ";0c"); // DA2: VT220, firmware version
    } else if (prefix == '=') {
        reply("\x1bP!|00000000\x1b\\"); // DA3: unit id
    }
}

void Terminal::handleDeviceStatus(int query, bool privateQuery)
{
    const int row = m_screen.cursorRow() - (m_screen.originMode() ? m_screen.scrollTop() : 0) + 1;
    const int col = m_screen.cursorCol() + 1;
    if (!privateQuery) {
        if (query == 5) {
            reply("\x1b[0n"); // operating status: OK
        } else if (query == 6) {
            reply("\x1b[" + number(row) + ";" + number(col) + "R"); // CPR
        }
        return;
    }
    switch (query) {
    case 6: // DECXCPR
        reply("\x1b[?" + number(row) + ";" + number(col) + "R");
        break;
    case 15: // printer: none
        reply("\x1b[?13n");
        break;
    case 25: // user-defined keys: locked
        reply("\x1b[?21n");
        break;
    case 26: // keyboard: North American
        reply("\x1b[?27;1;0;0n");
        break;
    case 996: // color scheme: 1 dark, 2 light
        reply((m_extraFlags & DarkColorScheme) != 0 ? "\x1b[?997;1n" : "\x1b[?997;2n");
        break;
    default:
        break;
    }
}

void Terminal::handleWindowOps(const std::vector<int> &params)
{
    const int rows = m_screen.rows();
    const int cols = m_screen.cols();
    switch (paramAt(params, 0, 0)) {
    case 11: // window state: not iconified
        reply("\x1b[1t");
        break;
    case 13: // window position
        reply("\x1b[3;0;0t");
        break;
    case 14: // text area size in pixels
    case 15: // screen size in pixels
        reply("\x1b[4;" + number(rows * m_cellPixelHeight) + ";" + number(cols * m_cellPixelWidth) + "t");
        break;
    case 16: // cell size in pixels
        reply("\x1b[6;" + number(m_cellPixelHeight) + ";" + number(m_cellPixelWidth) + "t");
        break;
    case 18: // text area size in characters
        reply("\x1b[8;" + number(rows) + ";" + number(cols) + "t");
        break;
    case 19: // screen size in characters
        reply("\x1b[9;" + number(rows) + ";" + number(cols) + "t");
        break;
    case 22: // push the title
        m_titleStack.push_back(m_title);
        break;
    case 23: // pop the title
        if (!m_titleStack.empty()) {
            m_title = m_titleStack.back();
            m_titleStack.pop_back();
            setExtraFlag(TitleDirty, true);
        }
        break;
    default:
        // Window manipulation (resize, move, raise) and title reports are
        // not exposed to applications.
        break;
    }
}

void Terminal::reportInBandSize()
{
    const int rows = m_screen.rows();
    const int cols = m_screen.cols();
    reply("\x1b[48;" + number(rows) + ";" + number(cols) + ";" + number(rows * m_cellPixelHeight) + ";" +
          number(cols * m_cellPixelWidth) + "t");
}

// DECRQM state: 0 not recognized, 1 set, 2 reset, 4 permanently reset.
int Terminal::decModeState(int mode) const
{
    const auto state = [](bool on) { return on ? 1 : 2; };
    const auto flag = [this, &state](unsigned int bit) { return state((m_extraFlags & bit) != 0); };
    switch (mode) {
    case 1:
        return flag(ApplicationCursorKeys);
    case 2: // DECANM: ANSI
        return flag(Vt52Mode) == 1 ? 2 : 1;
    case 3: // DECCOLM: 80 columns
        return 2;
    case 5:
        return flag(ReverseVideo);
    case 42:
        return flag(NrcMode);
    case 95:
        return flag(NoClearOnColumnChange);
    case 6:
        return state(m_screen.originMode());
    case 7:
        return state(m_screen.autoWrap());
    case 12:
        return flag(CursorBlinkMode);
    case 25:
        return state(m_screen.cursorVisible());
    case 1004:
        return flag(FocusReporting);
    case 1007:
        return flag(AlternateScroll);
    case 47:
    case 1047:
    case 1049:
        return state(m_screen.inAltScreen());
    case 1048:
        return 2;
    case 1000:
        return state((m_mouseModes & 0x01u) != 0);
    case 1002:
        return state((m_mouseModes & 0x02u) != 0);
    case 1003:
        return state((m_mouseModes & 0x04u) != 0);
    case 1005:
        return flag(Utf8Mouse);
    case 1006:
        return flag(SgrMouse);
    case 1015:
        return flag(UrxvtMouse);
    case 1016:
        return flag(SgrPixelMouse);
    case 2004:
        return flag(BracketedPasteEnabled);
    case 2026:
        return flag(SynchronizedUpdates);
    case 2031:
        return flag(ColorSchemeReports);
    case 2048:
        return flag(InBandResize);
    case 69: // DECLRMM: left/right margins are not supported
        return 4;
    default:
        return 0;
    }
}

void Terminal::reportMode(int mode, bool privateMode)
{
    if (privateMode) {
        reply("\x1b[?" + number(mode) + ";" + number(decModeState(mode)) + "$y");
        return;
    }
    int state = 0;
    if (mode == 4) {
        state = m_screen.insertMode() ? 1 : 2; // IRM
    } else if (mode == 20) {
        state = (m_extraFlags & NewlineMode) != 0 ? 1 : 2; // LNM
    } else if (mode == 6) {
        state = (m_extraFlags & ErasureMode) != 0 ? 1 : 2; // ERM
    }
    reply("\x1b[" + number(mode) + ";" + number(state) + "$y");
}

// The current pen as SGR parameters (DECRQSS "m").
QByteArray Terminal::sgrReport() const
{
    QByteArray sgr("0");
    const auto add = [&sgr](bool on, const char *code) {
        if (on) {
            sgr += ";";
            sgr += code;
        }
    };
    add((m_pen.flags & CellFlag::Bold) != 0, "1");
    add((m_pen.flags & CellFlag::Dim) != 0, "2");
    add((m_pen.attrs & CellAttr::Italic) != 0, "3");
    if ((m_pen.flags & CellFlag::Underline) != 0) {
        sgr += underlineStyle(m_pen) == UnderlineStyle::Single ? ";4" : ";4:" + number(static_cast<int>(underlineStyle(m_pen)) + 1);
    }
    add((m_pen.attrs & CellAttr::Blink) != 0, "5");
    add((m_pen.flags & CellFlag::Reverse) != 0, "7");
    add((m_pen.flags & CellFlag::Invisible) != 0, "8");
    add((m_pen.flags & CellFlag::Strike) != 0, "9");
    add((m_pen.attrs & CellAttr::Overline) != 0, "53");
    sgr += colorSgr(m_pen.fg, 30, 38);
    sgr += colorSgr(m_pen.bg, 40, 48);
    sgr += colorSgr(m_pen.ul, 0, 58);
    return sgr;
}

void Terminal::handleDecrqss(const QByteArray &request)
{
    QByteArray setting;
    if (request == "m") {
        setting = sgrReport() + "m";
    } else if (request == "r") {
        setting = number(m_screen.scrollTop() + 1) + ";" + number(m_screen.scrollBottom() + 1) + "r";
    } else if (request == " q") {
        setting = number(m_cursorStyle) + " q"; // DECSCUSR
    } else if (request == "\"p") {
        setting = "62;1\"p"; // DECSCL: VT200, 7-bit controls
    } else if (request == "\"q") {
        setting = (m_pen.attrs & CellAttr::DecProtected) != 0 ? "1\"q" : "0\"q"; // DECSCA
    }
    if (setting.isEmpty()) {
        reply("\x1bP0$r\x1b\\");
        return;
    }
    reply("\x1bP1$r" + setting + "\x1b\\");
}

void Terminal::handleXtgettcap(const QByteArray &names)
{
    std::ranges::for_each(names.split(';'), [this](const QByteArray &hexName) {
        const QByteArray name = QByteArray::fromHex(hexName);
        const auto *const cap = std::ranges::find(kTermCaps, std::string_view(name.constData(), name.size()),
                                                  &TermCap::name);
        if (name.isEmpty() || cap == kTermCaps.end()) {
            reply("\x1bP0+r" + hexName + "\x1b\\");
            return;
        }
        QByteArray entry = name.toHex();
        if (!cap->value.empty()) {
            entry += "=" + QByteArray(cap->value.data(), static_cast<qsizetype>(cap->value.size())).toHex();
        }
        reply("\x1bP1+r" + entry + "\x1b\\");
    });
}

// Kitty keyboard protocol: CSI ? u (query), CSI > flags u (push), CSI < n u
// (pop n levels), CSI = flags ; mode u (set / or / and-not the current level).
void Terminal::handleKittyKeyboard(char prefix, const std::vector<int> &params)
{
    const auto flags = static_cast<unsigned char>(std::clamp(paramAt(params, 0, 0), 0, 31));
    unsigned char &current = m_kittyStack[m_kittyDepth];
    switch (prefix) {
    case '?':
        reply("\x1b[?" + number(current) + "u");
        break;
    case '>':
        // A full stack drops its oldest level, as the spec allows.
        if (m_kittyDepth + 1U < m_kittyStack.size()) {
            ++m_kittyDepth;
        } else {
            std::ranges::rotate(m_kittyStack, m_kittyStack.begin() + 1);
        }
        m_kittyStack[m_kittyDepth] = flags;
        break;
    case '<': {
        const int levels = std::min(paramAt(params, 0, 1), static_cast<int>(m_kittyDepth));
        m_kittyDepth = static_cast<unsigned char>(m_kittyDepth - levels);
        if (levels == 0) {
            m_kittyStack[0] = 0; // popping past the bottom resets the flags
        }
        break;
    }
    case '=': {
        const int mode = paramAt(params, 1, 1);
        if (mode == 1) {
            current = flags;
        } else if (mode == 2) {
            current = static_cast<unsigned char>(current | flags);
        } else if (mode == 3) {
            current = static_cast<unsigned char>(current & ~flags);
        }
        break;
    }
    default:
        break;
    }
}

namespace
{
// A color spec from OSC 4/10/11/12: rgb:r/g/b with 1-4 hex digits per
// channel, #rgb / #rrggbb / #rrrrggggbbbb, or an X11/CSS color name.
// Returns 0 when it is not a color.
unsigned int parseColorSpec(const QByteArray &spec)
{
    if (spec.startsWith("rgb:")) {
        const QList<QByteArray> parts = spec.mid(4).split('/');
        if (parts.size() != 3) {
            return 0;
        }
        std::array<int, 3> channels{};
        bool valid = true;
        std::ranges::transform(parts, channels.begin(), [&valid](const QByteArray &part) {
            bool ok = false;
            const int value = part.toInt(&ok, 16);
            valid = valid && ok && !part.isEmpty() && part.size() <= 4;
            // Scale an n-digit hex value to 8 bits.
            const int max = (1 << (4 * std::clamp(static_cast<int>(part.size()), 1, 4))) - 1;
            return ok ? (value * 255) / max : 0;
        });
        return valid ? qRgb(channels[0], channels[1], channels[2]) : 0u;
    }
    const QColor color = QColor::fromString(QString::fromLatin1(spec));
    return color.isValid() ? color.rgba() | 0xff000000u : 0u;
}
} // namespace

unsigned int Terminal::paletteColor(int index) const
{
    const auto slot = static_cast<std::size_t>(std::clamp(index, 0, 255));
    return m_paletteOverrides[slot] != 0 ? m_paletteOverrides[slot] : defaultPaletteColor(index);
}

// OSC 10/11/12 current colors: foreground, background, cursor.
unsigned int Terminal::dynamicColor(int code) const
{
    if (code == 11) {
        return m_bgOverride != 0 ? m_bgOverride : m_defaultBg;
    }
    if (code == 12 && m_cursorOverride != 0) {
        return m_cursorOverride;
    }
    return m_fgOverride != 0 ? m_fgOverride : m_defaultFg;
}

void Terminal::setDynamicColor(int code, unsigned int color)
{
    if (code == 10) {
        m_fgOverride = color;
    } else if (code == 11) {
        m_bgOverride = color;
    } else if (code == 12) {
        m_cursorOverride = color;
    }
    setExtraFlag(ColorsDirty, true);
}

// OSC 10/11/12 (query "?" or set a color spec) and 110/111/112 (reset).
// "10;fg;bg" sets consecutive colors, as in xterm. Returns whether the OSC
// was one of them.
bool Terminal::handleOscColor(const QByteArray &body)
{
    if (body == "110" || body == "111" || body == "112") {
        setDynamicColor(body.toInt() - 100, 0);
        return true;
    }
    const QList<QByteArray> fields = body.split(';');
    const int first = fields.front().toInt();
    if (fields.size() < 2 || first < 10 || first > 12) {
        return false;
    }
    int code = first;
    std::ranges::for_each(fields | std::views::drop(1), [this, &code](const QByteArray &value) {
        if (code > 12) {
            return;
        }
        if (value == "?") {
            reply("\x1b]" + number(code) + ";" + rgbSpec(dynamicColor(code)) + "\x1b\\");
        } else if (const unsigned int color = parseColorSpec(value); color != 0) {
            setDynamicColor(code, color);
        }
        ++code;
    });
    return true;
}

// OSC 4 ; index ; spec-or-? [; index ; spec-or-? ...]
void Terminal::handleOscPalette(const QByteArray &body)
{
    const QList<QByteArray> fields = body.split(';');
    std::ranges::for_each(std::views::iota(qsizetype{0}, fields.size() / 2), [this, &fields](qsizetype pair) {
        bool ok = false;
        const int index = fields[pair * 2].toInt(&ok);
        const QByteArray &value = fields[(pair * 2) + 1];
        if (!ok || index < 0 || index > 255) {
            return;
        }
        if (value == "?") {
            reply("\x1b]4;" + number(index) + ";" + rgbSpec(paletteColor(index)) + "\x1b\\");
        } else if (const unsigned int color = parseColorSpec(value); color != 0) {
            m_paletteOverrides[static_cast<std::size_t>(index)] = color;
            setExtraFlag(ColorsDirty, true);
        }
    });
}

// OSC 104 resets the whole palette, OSC 104 ; i ; j ... the listed entries
// (`indices` is what follows "104;").
void Terminal::handleOscPaletteReset(const QByteArray &indices)
{
    if (indices.isEmpty()) {
        m_paletteOverrides = {};
    } else {
        std::ranges::for_each(indices.split(';'), [this](const QByteArray &field) {
            bool ok = false;
            const int index = field.toInt(&ok);
            if (ok && index >= 0 && index <= 255) {
                m_paletteOverrides[static_cast<std::size_t>(index)] = 0;
            }
        });
    }
    setExtraFlag(ColorsDirty, true);
}

// OSC 8 ; params ; URI starts a hyperlink (an empty URI ends it). Cells
// printed meanwhile carry its id; the same id= parameter and URI reuse one
// entry, so a link split across lines stays one link.
void Terminal::handleOscHyperlink(const QByteArray &body)
{
    constexpr qsizetype kMaxUriLength = 8192;
    const qsizetype separator = body.indexOf(';', 2);
    if (separator < 0) {
        return;
    }
    const QByteArray params = body.mid(2, separator - 2);
    const QByteArray uri = body.mid(separator + 1);
    if (uri.isEmpty() || uri.size() > kMaxUriLength) {
        m_pen.link = 0;
        return;
    }
    QByteArray id;
    std::ranges::for_each(params.split(':'), [&id](const QByteArray &param) {
        if (param.startsWith("id=")) {
            id = param.mid(3);
        }
    });
    const QString target = QString::fromUtf8(uri);
    const QString key = id.isEmpty() ? target : QString::fromUtf8(id) + QLatin1Char('\n') + target;
    if (const auto existing = m_linkIds.constFind(key); existing != m_linkIds.cend()) {
        m_pen.link = existing.value();
        return;
    }
    if (m_links.size() >= 0xffffu) {
        m_pen.link = 0; // table full: print the text unlinked
        return;
    }
    m_links.push_back(target);
    m_pen.link = static_cast<unsigned short>(m_links.size());
    m_linkIds.insert(key, m_pen.link);
}

QString Terminal::linkTarget(unsigned short id) const
{
    return id == 0 || id > m_links.size() ? QString{} : m_links[id - 1U];
}

namespace
{
// xterm mouse button code: button (or 3 = none / release in legacy
// encodings, 64/65 = wheel), +32 for motion, +4/8/16 for Shift/Meta/Control.
int mouseButtonCode(int button, int event, unsigned int modifiers)
{
    const bool wheel = event == 3 || event == 4;
    int code = wheel ? (event == 3 ? 64 : 65) : ((button < 0) ? 3 : button);
    code |= event == 2 ? 32 : 0;
    code |= (modifiers & 1u) != 0 ? 4 : 0;
    code |= (modifiers & 2u) != 0 ? 8 : 0;
    code |= (modifiers & 4u) != 0 ? 16 : 0;
    return code;
}

// Legacy X10 encoding: CSI M Cb Cx Cy, bytes offset by 32 (coordinates
// up to 223), or the 1005 UTF-8 extension (up to 2015).
QByteArray legacyMouseReport(int code, int x, int y, bool utf8)
{
    const int limit = utf8 ? 2015 : 223;
    if (x > limit || y > limit || code > 255) {
        return {};
    }
    const auto encode = [utf8](int value) {
        return utf8 ? QString(QChar(value)).toUtf8() : QByteArray(1, static_cast<char>(value));
    };
    return "\x1b[M" + encode(code) + encode(x + 32) + encode(y + 32);
}
} // namespace

QByteArray Terminal::mouseReport(int button, int row, int col, int event, unsigned int modifiers) const
{
    const bool anyMotion = (m_mouseModes & 0x04u) != 0;
    const bool buttonMotion = (m_mouseModes & 0x02u) != 0;
    const bool motion = event == 2;
    const bool release = event == 1;
    if (!mouseTracking() || (motion && !anyMotion && (!buttonMotion || button < 0))) {
        return {};
    }
    const int code = mouseButtonCode(button, event, modifiers);

    if ((m_extraFlags & (SgrMouse | SgrPixelMouse)) != 0) {
        // SGR (1006): 1-based cells; SGR-Pixels (1016): 1-based pixels.
        const bool pixels = (m_extraFlags & SgrPixelMouse) != 0;
        const int x = pixels ? (std::max(col, 0) * m_cellPixelWidth) + 1 : std::clamp(col + 1, 1, m_screen.cols());
        const int y = pixels ? (std::max(row, 0) * m_cellPixelHeight) + 1 : std::clamp(row + 1, 1, m_screen.rows());
        return "\x1b[<" + number(code) + ";" + number(x) + ";" + number(y) + (release ? "m" : "M");
    }

    const int x = std::clamp(col + 1, 1, m_screen.cols());
    const int y = std::clamp(row + 1, 1, m_screen.rows());
    const int legacyCode = (release ? 3 | (code & 28) : code) + 32;
    if ((m_extraFlags & UrxvtMouse) != 0) {
        return "\x1b[" + number(legacyCode) + ";" + number(x) + ";" + number(y) + "M";
    }
    return legacyMouseReport(legacyCode, x, y, (m_extraFlags & Utf8Mouse) != 0);
}

} // namespace konsoll
