#include "Terminal.h"

#include "CharWidth.h"
#include "Charsets.h"
#include "Grapheme.h"

#include <QChar>
#include <QList>
#include <QUrl>

#include <algorithm>
#include <iterator>
#include <array>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

namespace konsoll
{

Terminal::Terminal(int rows, int cols, const QString &remoteHostToken)
    : m_screen(rows, cols)
    , m_remoteHostToken(remoteHostToken)
{}

void Terminal::write(const QByteArray &data)
{
    for (const char b : data) {
        processByte(static_cast<unsigned char>(b));
    }
}

void Terminal::resize(int rows, int cols)
{
    m_screen.resize(rows, cols);
    // Images follow their text when the main screen re-wraps.
    const Screen::ReflowMap reflow = m_screen.takeReflowMap();
    m_graphics.remapAnchors(reflow.base, reflow.anchors);
    clearCommandBlocks();
    if ((m_extraFlags & InBandResize) != 0) {
        reportInBandSize();
    }
}

void Terminal::clearCommandBlocks()
{
    m_commandBlocks.clear();
    m_commandStartLine = -1;
}

namespace
{
// Upper bound for an OSC/DCS string. OSC 52 clipboard payloads are the largest
// legitimate strings (see handleOscClipboard); anything longer is dropped.
constexpr qsizetype kMaxStringLength = 1'500'000;
// CSI parameter and intermediate bytes kept per sequence; longer runs are
// malformed and only need to be consumed.
constexpr qsizetype kMaxCsiParams = 256;
constexpr qsizetype kMaxCsiIntermediates = 2;

constexpr unsigned char kEsc = 0x1b;
constexpr unsigned char kCan = 0x18;
constexpr unsigned char kSub = 0x1a;
constexpr unsigned char kBel = 0x07;

bool isFinalByte(unsigned char c)
{
    return c >= 0x40 && c <= 0x7e;
}

bool isIntermediateByte(unsigned char c)
{
    return c >= 0x20 && c <= 0x2f;
}

bool isParameterByte(unsigned char c)
{
    return c >= 0x30 && c <= 0x3f;
}
} // namespace

void Terminal::processByte(unsigned char c)
{
    switch (m_state) {
    case State::Ground:
        if (m_utf8Remaining != 0 && processUtf8Continuation(c)) {
            return;
        }
        processGroundByte(c);
        return;
    case State::Escape:
        processEscapeByte(c);
        return;
    case State::EscapeIntermediate:
        processEscapeIntermediateByte(c);
        return;
    case State::Csi:
        processCsiByte(c);
        return;
    case State::CsiIgnore:
        processCsiIgnoreByte(c);
        return;
    case State::Osc:
    case State::Dcs:
    case State::IgnoreString:
        processStringByte(c);
        return;
    case State::StringEscape:
        processStringEscapeByte(c);
        return;
    case State::Vt52Row:
    case State::Vt52Column:
        processVt52AddressByte(c);
        return;
    }
}

bool Terminal::processUtf8Continuation(unsigned char c)
{
    if (c >= 0x80 && c <= 0xbf) {
        m_utf8CodePoint = (m_utf8CodePoint << 6) | (c & 0x3fu);
        --m_utf8Remaining;
        if (m_utf8Remaining == 0) {
            putCodePoint(m_utf8CodePoint < m_utf8Minimum ? 0xfffd : m_utf8CodePoint);
            m_utf8Minimum = 0;
        }
        return true;
    }
    putCodePoint(0xfffd);
    m_utf8Remaining = 0;
    m_utf8Minimum = 0;
    return false;
}

void Terminal::processGroundByte(unsigned char c)
{
    if (c < 0x20 || c == 0x7f) {
        executeControl(c);
    } else if (c < 0x80) {
        printCodePoint(mapCharset(c));
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
}

// C0 controls run in every state except strings, so a CR or BS embedded in a
// CSI sequence still takes effect (as on a real VT).
void Terminal::executeControl(unsigned char c)
{
    switch (c) {
    case kEsc:
        enterEscape();
        break;
    case '\r':
        m_screen.carriageReturn();
        break;
    case '\n':
    case 0x0b:
    case 0x0c:
        // VT and FF act as LF, as in xterm. In LNM, LF also returns.
        if ((m_extraFlags & NewlineMode) != 0) {
            m_screen.carriageReturn();
        }
        m_screen.lineFeed();
        break;
    case 0x0e: // SO: invoke G1 into GL
        m_glSet = 1;
        break;
    case 0x0f: // SI: invoke G0 into GL
        m_glSet = 0;
        break;
    case '\b':
        m_screen.backspace();
        break;
    case '\t':
        m_screen.tab();
        break;
    case kCan:
    case kSub:
        // CAN/SUB abort any sequence in progress.
        m_state = State::Ground;
        break;
    case kBel:
        setExtraFlag(BellPending, true);
        break;
    default:
        // Every other C0 control (NUL, BEL, ...) and DEL has no glyph:
        // drop it instead of storing it in a cell, where it renders as a
        // missing-glyph box over the text.
        break;
    }
}

void Terminal::enterEscape()
{
    m_state = State::Escape;
    m_csiIntermediates.clear(); // ESC intermediates share the CSI buffer
}

void Terminal::enterString(State state, StringKind kind)
{
    m_state = state;
    m_stringKind = kind;
    m_stringBody.clear();
}

void Terminal::processEscapeByte(unsigned char c)
{
    if (c < 0x20) {
        executeControl(c);
        return;
    }
    if ((m_extraFlags & Vt52Mode) != 0) {
        handleVt52Escape(c);
        return;
    }
    switch (c) {
    case '[':
        m_state = State::Csi;
        m_csiParams.clear();
        m_csiIntermediates.clear();
        m_csiPrefix = 0;
        return;
    case ']':
        enterString(State::Osc, StringKind::Osc);
        return;
    case 'P':
        enterString(State::Dcs, StringKind::Dcs);
        return;
    case 'X': // SOS
    case '^': // PM
        enterString(State::IgnoreString, StringKind::Ignore);
        return;
    case '_': // APC: kitty graphics (APC G ...)
        enterString(State::Dcs, StringKind::Apc);
        return;
    default:
        break;
    }
    if (isIntermediateByte(c)) {
        // ESC sequences with an intermediate byte (for example ESC ( B
        // to select the ASCII character set) end with a separate final
        // byte. Consume both; otherwise that final byte leaks as text.
        m_csiIntermediates.append(static_cast<char>(c));
        m_state = State::EscapeIntermediate;
        return;
    }
    m_state = State::Ground;
    if (c != 0x7f) {
        handleEsc(static_cast<char>(c));
    }
}

// VT52 mode (DECANM reset): the VT52's own escape sequences; ESC < returns
// to ANSI mode.
void Terminal::handleVt52Escape(unsigned char c)
{
    m_state = State::Ground;
    switch (c) {
    case 'A':
        m_screen.cursorUp(1);
        break;
    case 'B':
        m_screen.cursorDown(1);
        break;
    case 'C':
        cursorMove(0, 1);
        break;
    case 'D':
        cursorMove(0, -1);
        break;
    case 'F': // graphics character set
        m_charsets[0] = Charset::DecSpecialGraphics;
        break;
    case 'G':
        m_charsets[0] = Charset::Ascii;
        break;
    case 'H':
        m_screen.moveCursor(0, 0);
        break;
    case 'I':
        m_screen.reverseIndex();
        break;
    case 'J':
        m_screen.eraseInDisplay(0);
        break;
    case 'K':
        m_screen.eraseInLine(0);
        break;
    case 'Y': // direct cursor address: ESC Y row+32 col+32
        m_state = State::Vt52Row;
        break;
    case 'Z': // identify
        reply("\x1b/Z");
        break;
    case '=':
        setExtraFlag(ApplicationKeypad, true);
        break;
    case '>':
        setExtraFlag(ApplicationKeypad, false);
        break;
    case '<':
        setExtraFlag(Vt52Mode, false);
        break;
    default:
        break;
    }
}

void Terminal::processVt52AddressByte(unsigned char c)
{
    if (c < 0x20) {
        executeControl(c);
        return;
    }
    if (m_state == State::Vt52Row) {
        m_csiParams = QByteArray(1, static_cast<char>(c)); // keep the row byte
        m_state = State::Vt52Column;
        return;
    }
    m_state = State::Ground;
    // An out-of-range line or column leaves that coordinate unchanged, as
    // on the VT52.
    const int row = static_cast<unsigned char>(m_csiParams.front()) - 32;
    const int col = static_cast<int>(c) - 32;
    m_screen.moveCursor(row < m_screen.rows() ? row : m_screen.cursorRow(),
                        col < m_screen.cols() ? col : m_screen.cursorCol());
}

void Terminal::processEscapeIntermediateByte(unsigned char c)
{
    if (c < 0x20) {
        executeControl(c);
    } else if (c >= 0x30 && c <= 0x7e) {
        m_state = State::Ground;
        handleEscIntermediate(m_csiIntermediates, static_cast<char>(c));
    } else if (isIntermediateByte(c) && m_csiIntermediates.size() < kMaxCsiIntermediates) {
        m_csiIntermediates.append(static_cast<char>(c)); // ESC ( % 6
    }
    // Further intermediates (and DEL) are consumed.
}

void Terminal::processCsiByte(unsigned char c)
{
    if (c < 0x20) {
        executeControl(c);
    } else if (isFinalByte(c)) {
        m_state = State::Ground;
        handleCsi(static_cast<char>(c));
    } else if (isIntermediateByte(c)) {
        if (m_csiIntermediates.size() < kMaxCsiIntermediates) {
            m_csiIntermediates.append(static_cast<char>(c));
        } else {
            m_state = State::CsiIgnore;
        }
    } else if (isParameterByte(c)) {
        const bool prefixByte = c >= 0x3c; // '<' '=' '>' '?'
        // Parameters after an intermediate byte are malformed.
        const bool afterIntermediate = !m_csiIntermediates.isEmpty();
        if (prefixByte && !afterIntermediate && m_csiParams.isEmpty() && m_csiPrefix == 0) {
            // A private prefix only at the start: CSI ? 1049 h, CSI > c.
            m_csiPrefix = static_cast<char>(c);
        } else if (afterIntermediate || prefixByte || m_csiParams.size() >= kMaxCsiParams) {
            m_state = State::CsiIgnore;
        } else {
            m_csiParams.append(static_cast<char>(c));
        }
    }
    // DEL and 8-bit bytes inside a CSI are ignored.
}

void Terminal::processCsiIgnoreByte(unsigned char c)
{
    if (c < 0x20) {
        executeControl(c);
    } else if (isFinalByte(c)) {
        m_state = State::Ground;
    }
}

void Terminal::processStringByte(unsigned char c)
{
    if (c == kEsc) {
        m_state = State::StringEscape;
        return;
    }
    if (c == kCan || c == kSub) {
        m_state = State::Ground;
        m_stringBody.clear();
        return;
    }
    if (c == kBel && m_stringKind == StringKind::Osc) {
        finishString();
        return;
    }
    // Other C0 controls are ignored inside strings. 8-bit bytes are UTF-8
    // text (a title such as "œ" contains 0x9c); only the UTF-8 encoded C1
    // ST (U+009C, C2 9C) ends the string.
    if (c == 0x9c && m_stringBody.endsWith('\xc2')) {
        m_stringBody.chop(1);
        finishString();
        return;
    }
    if (c >= 0x20 && m_stringKind != StringKind::Ignore && m_stringBody.size() < kMaxStringLength) {
        m_stringBody.append(static_cast<char>(c));
    }
}

void Terminal::processStringEscapeByte(unsigned char c)
{
    // ESC \ is ST. Any other byte also ends the string (as in xterm) and
    // starts a new escape sequence.
    finishString();
    if (c != '\\') {
        enterEscape();
        processEscapeByte(c);
    }
}

void Terminal::finishString()
{
    m_state = State::Ground;
    const QByteArray body = std::exchange(m_stringBody, QByteArray{});
    if (m_stringKind == StringKind::Osc) {
        handleOsc(body);
    } else if (m_stringKind == StringKind::Dcs) {
        handleDcs(body);
    } else if (m_stringKind == StringKind::Apc) {
        handleApc(body);
    }
}

void Terminal::putCodePoint(unsigned int codePoint)
{
    if (codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff) || codePoint < 0x80) {
        codePoint = 0xfffd;
    }

    // Zero-width code points (variation selectors, combining marks, ZWJ...)
    // take no cell — applications lay lines out with wcwidth semantics, so a
    // cell per mark pushes the rest of the row right (and the last column onto
    // the next row). Fold the mark into the previous character when NFC has a
    // precomposed form (e + U+0301 -> é, as in macOS NFD file names);
    // otherwise drop it.
    // C1 controls (U+0080..U+009F, UTF-8 encoded as xterm does in UTF-8
    // mode) act as ESC + (code - 0x40): CSI, OSC, DCS, IND, NEL, RI, ...
    if (codePoint >= 0x80 && codePoint <= 0x9f) {
        enterEscape();
        processEscapeByte(static_cast<unsigned char>(codePoint - 0x40));
        return;
    }

    // Mode 2027: the code point may continue the previous cell's cluster.
    // Kitty image placeholders always keep their row / column diacritics.
    if ((m_screen.graphemeClustering() || followsImagePlaceholder()) && extendPreviousCluster(codePoint)) {
        return;
    }

    if (isZeroWidth(codePoint)) {
        combineWithPrevious(codePoint);
        return;
    }

    printCodePoint(codePoint);
}

// Writes one graphic character. Supplementary-plane characters (e.g. Nerd
// Font icons, U+F0xxx) occupy one cell holding the full code point; the
// renderer encodes them as a surrogate-pair string. East Asian wide
// characters and emoji take two.
bool Terminal::followsImagePlaceholder() const
{
    const std::optional<unsigned int> previous = m_screen.previousChar();
    return previous && cellCodePoints(*previous).front() == kImagePlaceholder;
}

// S8C1T: replies use C1 controls (UTF-8 encoded) instead of ESC Fe.
void Terminal::reply(const QByteArray &bytes)
{
    if ((m_extraFlags & EightBitControls) == 0) {
        m_responses += bytes;
        return;
    }
    qsizetype i = 0;
    while (i < bytes.size()) {
        const char next = i + 1 < bytes.size() ? bytes.at(i + 1) : '\0';
        if (bytes.at(i) == '\x1b' && next >= 0x40 && next <= 0x5f) {
            m_responses += QString(QChar(static_cast<char16_t>(next + 0x40))).toUtf8();
            i += 2;
        } else {
            m_responses += bytes.at(i);
            ++i;
        }
    }
}

// DECCOLM: with ?40 the screen becomes 80 / 132 columns (and the window is
// asked to fit); without it the width stays. Either way the switch clears
// the screen, resets the margins and homes the cursor, as xterm does; ?95
// (DECNCSM) keeps the content.
void Terminal::setColumnMode(bool wide)
{
    if ((m_extraFlags & Allow132Columns) != 0) {
        setExtraFlag(Columns132, wide);
        const int columns = wide ? 132 : 80;
        if (columns != m_screen.cols()) {
            resize(m_screen.rows(), columns);
        }
        m_requests.push_back(TerminalRequest{
            .data = {}, .body = {}, .kind = TerminalRequest::ColumnMode, .a = columns, .b = 0, .c = 0});
    }
    if ((m_extraFlags & NoClearOnColumnChange) == 0) {
        m_screen.eraseInDisplay(2);
    }
    m_screen.resetScrollRegion();
    m_screen.moveCursor(0, 0);
}

namespace
{
// DECUDK key selectors (VT220 numbering) for F1-F20.
constexpr std::array<int, 20> kUdkSelectors{11, 12, 13, 14, 15, 17, 18, 19, 20, 21,
                                            23, 24, 25, 26, 28, 29, 31, 32, 33, 34};
} // namespace

QByteArray Terminal::userDefinedKey(int number) const
{
    return number >= 1 && number <= 20 ? m_userKeys[static_cast<std::size_t>(number - 1)] : QByteArray{};
}

// DECUDK: DCS Pc ; Pl | key/hex ; key/hex ... ST. Pc 0 clears every key
// first; Pl 0 (default) locks the keys against later definitions.
void Terminal::handleUserDefinedKeys(const QByteArray &body)
{
    if ((m_extraFlags & UdkLocked) != 0) {
        return;
    }
    const qsizetype bar = body.indexOf('|');
    const QList<QByteArray> params = body.left(bar).split(';');
    if (params.value(0).toInt() == 0 && !params.value(0).isEmpty()) {
        m_userKeys = {};
    }
    std::ranges::for_each(body.mid(bar + 1).split(';'), [this](const QByteArray &definition) {
        const qsizetype slash = definition.indexOf('/');
        const auto *const found = std::ranges::find(kUdkSelectors, definition.left(slash).toInt());
        if (slash > 0 && found != kUdkSelectors.end()) {
            m_userKeys[static_cast<std::size_t>(found - kUdkSelectors.begin())] =
                QByteArray::fromHex(definition.mid(slash + 1)).left(256);
        }
    });
    if (params.value(1).toInt() == 0) {
        setExtraFlag(UdkLocked, true);
    }
}

// Appends `codePoint` to the cluster in the previous cell (mode 2027),
// widening the cell when the cluster gains emoji presentation.
bool Terminal::extendPreviousCluster(unsigned int codePoint)
{
    const std::optional<unsigned int> previous = m_screen.previousChar();
    if (!previous) {
        return false;
    }
    std::u32string cluster = cellCodePoints(*previous);
    if (!extendsCluster(cluster, static_cast<char32_t>(codePoint))) {
        return false;
    }
    cluster.push_back(static_cast<char32_t>(codePoint));
    // A letter with a mark that has a precomposed form stays one code point.
    const QList<uint> composed =
        QString::fromUcs4(cluster.data(), static_cast<qsizetype>(cluster.size())).normalized(QString::NormalizationForm_C).toUcs4();
    const int width = graphemeWidth(cluster);
    const bool wasWide = m_screen.previousCharIsWide();
    const unsigned int cell = composed.size() == 1 ? composed.front() : internCluster(cluster, width);
    m_screen.replacePreviousChar(cell);
    if (width == 2 && !wasWide) {
        m_screen.widenPreviousChar();
    }
    m_lastGraphic = cell;
    return true;
}

void Terminal::printCodePoint(unsigned int codePoint)
{
    if (clusterWidth(codePoint) == 2 || (codePoint < kClusterBase && isWideChar(codePoint))) {
        m_screen.putWideChar(codePoint, m_pen);
    } else {
        m_screen.putChar(codePoint, m_pen);
    }
    m_lastGraphic = codePoint;
}

// Maps a printable ASCII byte through the character set in GL (or the one a
// single shift selected for this character).
unsigned int Terminal::mapCharset(unsigned char c)
{
    const unsigned char set = m_singleShift != 0 ? m_singleShift : m_glSet;
    m_singleShift = 0;
    return mapCharsetByte(m_charsets[set], c, (m_extraFlags & NrcMode) != 0);
}

// REP: repeat the preceding graphic character; bounded by the screen size.
void Terminal::repeatLastCharacter(int count)
{
    if (m_lastGraphic == 0) {
        return;
    }
    const int times = std::min(std::max(count, 1), m_screen.rows() * m_screen.cols());
    // printCodePoint keeps m_lastGraphic unchanged while repeating it.
    std::ranges::for_each(std::views::iota(0, times), [this](int) { printCodePoint(m_lastGraphic); });
    m_lastGraphic = 0; // a second REP does not repeat again
}

bool Terminal::isZeroWidth(unsigned int codePoint)
{
    // Markus Kuhn's wcwidth rules: nonspacing/enclosing marks and format
    // characters (except SOFT HYPHEN), plus Hangul medial vowels/final
    // consonants.
    if (codePoint == 0x00adu) {
        return false;
    }
    if (codePoint >= 0x1160u && codePoint <= 0x11ffu) {
        return true;
    }
    const QChar::Category category = QChar::category(static_cast<char32_t>(codePoint));
    return category == QChar::Mark_NonSpacing || category == QChar::Mark_Enclosing || category == QChar::Other_Format;
}

void Terminal::combineWithPrevious(unsigned int mark)
{
    // VS16 requests emoji presentation, which TUIs lay out two cells wide.
    if (mark == 0xfe0fu) {
        m_screen.widenPreviousChar();
        return;
    }
    const std::optional<unsigned int> base = m_screen.previousChar();
    if (!base || *base >= kClusterBase) {
        return;
    }
    const std::array<char32_t, 2> pair{static_cast<char32_t>(*base), static_cast<char32_t>(mark)};
    const QList<uint> composed = QString::fromUcs4(pair.data(), pair.size()).normalized(QString::NormalizationForm_C).toUcs4();
    if (composed.size() == 1) {
        m_screen.replacePreviousChar(composed.front());
    }
}

Terminal::ParamGroups Terminal::parseParamGroups(const QByteArray &body)
{
    // Values saturate instead of overflowing on absurd digit runs.
    constexpr int kMaxParam = 99999;
    ParamGroups groups(1);
    int value = -1;
    std::ranges::for_each(body, [&](char c) {
        if (c >= '0' && c <= '9') {
            value = std::min((std::max(value, 0) * 10) + (c - '0'), kMaxParam);
            return;
        }
        groups.back().push_back(value);
        value = -1;
        if (c == ';') {
            groups.emplace_back();
        }
        // ':' starts the next sub-parameter of the same group.
    });
    groups.back().push_back(value);
    return groups;
}

std::vector<int> Terminal::parseParams(const QByteArray &body)
{
    const ParamGroups groups = parseParamGroups(body);
    std::vector<int> out(groups.size());
    std::ranges::transform(groups, out.begin(), [](const std::vector<int> &group) { return std::max(group.front(), 0); });
    return out;
}

int Terminal::paramAt(const std::vector<int> &p, int i, int def) // NOLINT: i/def are inherently swappable
{
    if (i >= static_cast<int>(p.size()) || p[i] == 0) {
        return def;
    }
    return p[i];
}

void Terminal::handleCsi(char final)
{
    // Dispatch on prefix + intermediates + final: CSI ? 1049 s (XTSAVE),
    // CSI ? 2026 $ p (DECRQM) and CSI > 4 ; 2 m (modifyOtherKeys) only share
    // their final byte with CSI s, CSI p and SGR.
    if (!m_csiIntermediates.isEmpty()) {
        handleCsiWithIntermediates(final);
        return;
    }
    if (m_csiPrefix == '?') {
        handlePrivateCsi(final);
        return;
    }
    if (m_csiPrefix != 0) {
        handlePrefixedCsi(final);
        return;
    }

    const std::vector<int> p = parseParams(m_csiParams);

    switch (final) {
    case 'A': // CUU
        m_screen.cursorUp(paramAt(p, 0, 1));
        break;
    case 'B': // CUD
        m_screen.cursorDown(paramAt(p, 0, 1));
        break;
    case 'E': // CNL
        m_screen.cursorDown(paramAt(p, 0, 1));
        m_screen.carriageReturn();
        break;
    case 'F': // CPL
        m_screen.cursorUp(paramAt(p, 0, 1));
        m_screen.carriageReturn();
        break;
    case 'C': // CUF
        m_screen.cursorForward(paramAt(p, 0, 1));
        break;
    case 'a': // HPR
        cursorMove(0, paramAt(p, 0, 1));
        break;
    case 'D': // CUB
        m_screen.cursorBack(paramAt(p, 0, 1));
        break;
    case 'G': // CHA
    case '`': // HPA
        m_screen.moveCursor(m_screen.cursorRow(), paramAt(p, 0, 1) - 1);
        break;
    case 'd': // VPA
        m_screen.moveCursorAddressed(paramAt(p, 0, 1) - 1, m_screen.cursorCol());
        break;
    case 'e': // VPR
        cursorMove(paramAt(p, 0, 1), 0);
        break;
    case 'H': // CUP
    case 'f': // HVP
        m_screen.moveCursorAddressed(paramAt(p, 0, 1) - 1, paramAt(p, 1, 1) - 1);
        break;
    case 'I': // CHT
        m_screen.tab(paramAt(p, 0, 1));
        break;
    case 'Z': // CBT
        m_screen.backTab(paramAt(p, 0, 1));
        break;
    case 'b': // REP
        repeatLastCharacter(paramAt(p, 0, 1));
        break;
    case 'g': // TBC
        m_screen.clearTabStop(paramAt(p, 0, 0));
        break;
    case 's':
        // DECSLRM under DECLRMM (?69), otherwise SCOSC.
        if (m_screen.leftRightMarginMode()) {
            m_screen.setLeftRightMargins(paramAt(p, 0, 1), paramAt(p, 1, m_screen.cols()));
        } else {
            saveCursorState();
        }
        break;
    case 'u': // SCORC
        restoreCursorState(true);
        break;
    case 'h': // SM
    case 'l': // RM
        handleAnsiMode(final == 'h', p);
        break;
    case 'J':
        eraseImages(paramAt(p, 0, 0));
        m_screen.eraseInDisplay(paramAt(p, 0, 0), isoProtection());
        if (paramAt(p, 0, 0) == 2) {
            setExtraFlag(ClearPending, true); // full-screen erase: selection now sits over blanks.
        }
        break;
    case 'K':
        m_screen.eraseInLine(paramAt(p, 0, 0), isoProtection());
        break;
    case 'm':
        handleSgr(parseParamGroups(m_csiParams));
        m_screen.setEraseBackground(m_pen.bg);
        break;
    case 'X':
        m_screen.eraseChars(paramAt(p, 0, 1), isoProtection());
        break;
    case 'P':
        m_screen.deleteColumns(paramAt(p, 0, 1));
        break;
    case '@':
        m_screen.insertColumns(paramAt(p, 0, 1));
        break;
    case 'L':
        m_screen.insertLines(paramAt(p, 0, 1));
        break;
    case 'M':
        m_screen.deleteLines(paramAt(p, 0, 1));
        break;
    case 'S':
        m_screen.scrollUp(paramAt(p, 0, 1));
        break;
    case 'T':
        if (p.size() >= 5) {
            // Highlight tracking (?1001): the application starts (func != 0)
            // or aborts a highlight; the release then reports its end.
            setExtraFlag(HighlightStarted, (m_mouseModes & 0x08u) != 0 && p[0] != 0);
        } else {
            m_screen.scrollDown(paramAt(p, 0, 1)); // SD
        }
        break;
    case 'r':
        // DECSTBM always sets the scroll region; omitted parameters restore
        // the full-screen region. CSI r without parameters is not SU.
        m_screen.setScrollRegion(paramAt(p, 0, 1), paramAt(p, 1, m_screen.rows()));
        break;
    case 'c': // DA1
        handleDeviceAttributes(0, paramAt(p, 0, 0));
        break;
    case 'n': // DSR
        handleDeviceStatus(paramAt(p, 0, 0), false);
        break;
    case 't': // XTWINOPS
        handleWindowOps(p);
        break;
    case 'x': // DECREQTPARM: solicited (0) / unsolicited (1) VT100 parameters
        if (paramAt(p, 0, 0) <= 1) {
            // No parity, 8 bits, 38400 baud transmit/receive, clock 1, flags 0.
            reply("\x1b[" + QByteArray::number(paramAt(p, 0, 0) + 2) + ";1;1;128;128;1;0x");
        }
        break;
    default:
        break;
    }
}

void Terminal::handlePrivateCsi(char final)
{
    const std::vector<int> params = parseParams(m_csiParams);
    switch (final) {
    case 'h':
    case 'l':
        handleDec(final == 'h', params);
        break;
    case 'u':
        // Kitty keyboard protocol query: CSI ? u.
        handleKittyKeyboard('?', params);
        break;
    case 'n': // DECDSR
        handleDeviceStatus(paramAt(params, 0, 0), true);
        break;
    case 'S': // XTSMGRAPHICS
        handleGraphicsAttributes(params);
        break;
    case 'm': // XTQMODKEYS: CSI ? 4 m
        if (paramAt(params, 0, 0) == 4) {
            reply("\x1b[>4;" + QByteArray::number(modifyOtherKeys()) + "m");
        }
        break;
    case 'J': // DECSED: selective erase keeps protected cells
        m_screen.eraseInDisplay(paramAt(params, 0, 0), CellAttr::DecProtected | CellAttr::IsoProtected);
        break;
    case 'K': // DECSEL
        m_screen.eraseInLine(paramAt(params, 0, 0), CellAttr::DecProtected | CellAttr::IsoProtected);
        break;
    default:
        break;
    }
}

void Terminal::handlePrefixedCsi(char final)
{
    const std::vector<int> params = parseParams(m_csiParams);
    if (final == 'u') {
        // Kitty keyboard protocol: CSI > flags u (push), CSI < u (pop),
        // CSI = flags ; mode u (set).
        handleKittyKeyboard(m_csiPrefix, params);
    } else if (final == 'c') {
        handleDeviceAttributes(m_csiPrefix, paramAt(params, 0, 0)); // DA2 (>) / DA3 (=)
    } else if (final == 'q' && m_csiPrefix == '>') {
        // XTVERSION: DCS > | name version ST.
        reply(QByteArray("\x1bP>|Konsoll ") + KONSOLL_VERSION + "\x1b\\");
    } else if (m_csiPrefix == '>' && (final == 'm' || final == 'n' || final == 'f')) {
        handleKeyModifierOptions(final, params); // XTMODKEYS / XTFMTKEYS
    }
    // Other prefixed sequences are consumed without effect; they are not SGR.
}

// XTMODKEYS (CSI > 4 ; Pv m sets modifyOtherKeys, CSI > 4 n resets it) and
// XTFMTKEYS (CSI > 4 ; Pv f picks CSI 27 ; m ; k ~ or CSI k ; m u). The other
// resources (modifyCursorKeys, ...) keep xterm's defaults.
void Terminal::handleKeyModifierOptions(char final, const std::vector<int> &params)
{
    if (params.empty() || params.front() != 4) {
        return;
    }
    const int value = params.size() > 1 ? std::clamp(params[1], 0, 2) : 0;
    if (final == 'f') {
        setExtraFlag(OtherKeysCsiU, value == 1);
        return;
    }
    const std::uint64_t level = final == 'n' ? 0u : static_cast<std::uint64_t>(value);
    m_extraFlags = (m_extraFlags & ~OtherKeysMask) | (level << OtherKeysShift);
}

void Terminal::handleCsiWithIntermediates(char final)
{
    if (m_csiIntermediates == "!" && final == 'p') {
        softReset(); // DECSTR
    } else if (m_csiIntermediates == " " && (final == '@' || final == 'A')) {
        // SL / SR: scroll the region left / right.
        const std::vector<int> params = parseParams(m_csiParams);
        m_screen.scrollHorizontally(final == '@' ? paramAt(params, 0, 1) : -paramAt(params, 0, 1));
    } else if (m_csiIntermediates == "\"" && final == 'q') {
        // DECSCA: 1 protects the following characters, 0 / 2 do not.
        const std::vector<int> params = parseParams(m_csiParams);
        const bool protect = paramAt(params, 0, 0) == 1;
        m_pen.attrs = static_cast<unsigned char>(protect ? (m_pen.attrs | CellAttr::DecProtected)
                                                         : (m_pen.attrs & ~CellAttr::DecProtected));
    } else if (m_csiIntermediates == " " && final == 'q') {
        // DECSCUSR: cursor shape and blinking.
        const std::vector<int> params = parseParams(m_csiParams);
        m_cursorStyle = static_cast<unsigned char>(std::clamp(paramAt(params, 0, 0), 0, 6));
    } else if (m_csiIntermediates == "$" && final == 'p') {
        // DECRQM: CSI ? Ps $ p (DEC mode) / CSI Ps $ p (ANSI mode).
        const std::vector<int> params = parseParams(m_csiParams);
        reportMode(paramAt(params, 0, 0), m_csiPrefix == '?');
    } else if (m_csiIntermediates == "$" || m_csiIntermediates == "*") {
        handleRectangle(m_csiIntermediates.front(), final);
    } else if (m_csiIntermediates == "'" && (final == '}' || final == '~')) {
        // DECIC / DECDC: insert / delete columns.
        const std::vector<int> params = parseParams(m_csiParams);
        if (final == '}') {
            m_screen.insertColumnsInRegion(paramAt(params, 0, 1));
        } else {
            m_screen.deleteColumnsInRegion(paramAt(params, 0, 1));
        }
    }
}

namespace
{
bool applyBasicColor(int code, Cell &pen); // SGR 30-37 / 40-47 / 90-97 / 100-107, defined with the SGR code
} // namespace

// A rectangle from CSI Pt ; Pl ; Pb ; Pr starting at params[first]; in
// origin mode relative to the scroll region and the left/right margins.
Screen::Rect Terminal::rectangleParams(const std::vector<int> &params, int first) const
{
    const bool origin = m_screen.originMode();
    const int top = origin ? m_screen.scrollTop() : 0;
    const int left = origin ? m_screen.leftMargin() : 0;
    const int bottomLimit = origin ? m_screen.scrollBottom() : m_screen.rows() - 1;
    const int rightLimit = origin ? m_screen.rightMargin() : m_screen.cols() - 1;
    return Screen::Rect{
        .top = std::min(top + paramAt(params, first, 1) - 1, bottomLimit),
        .left = std::min(left + paramAt(params, first + 1, 1) - 1, rightLimit),
        .bottom = std::min(top + paramAt(params, first + 2, bottomLimit - top + 1) - 1, bottomLimit),
        .right = std::min(left + paramAt(params, first + 3, rightLimit - left + 1) - 1, rightLimit),
    };
}

// VT420 rectangular area operations: CSI ... $ x/z/{/v/r/t and * x / * y.
void Terminal::handleRectangle(char intermediate, char final)
{
    const std::vector<int> p = parseParams(m_csiParams);
    if (intermediate == '*') {
        if (final == 'x') {
            setExtraFlag(RectangleExtent, paramAt(p, 0, 0) == 2); // DECSACE
        } else if (final == 'y') {
            // DECRQCRA: DCS Pid ! ~ checksum ST.
            const int checksum = m_screen.checksumRect(rectangleParams(p, 2));
            reply("\x1bP" + QByteArray::number(paramAt(p, 0, 0)) + "!~" +
                  QByteArray::number(checksum, 16).toUpper().rightJustified(4, '0') + "\x1b\\");
        }
        return;
    }
    switch (final) {
    case 'x': { // DECFRA: fill with a character in the current rendition
        const int ch = paramAt(p, 0, 0);
        if ((ch >= 32 && ch <= 126) || (ch >= 160 && ch <= 255)) {
            Cell cell = m_pen;
            cell.ch = static_cast<unsigned int>(ch);
            m_screen.fillRect(rectangleParams(p, 1), cell);
        }
        break;
    }
    case 'z': // DECERA
        m_screen.eraseRect(rectangleParams(p, 0), 0);
        break;
    case '{': // DECSERA: selective
        m_screen.eraseRect(rectangleParams(p, 0), CellAttr::DecProtected | CellAttr::IsoProtected);
        break;
    case 'v': { // DECCRA: copy (single page) to Ptd ; Pld
        const Screen::Rect target = rectangleParams(p, 5);
        m_screen.copyRect(rectangleParams(p, 0), target.top, target.left);
        break;
    }
    case 'r': // DECCARA
    case 't': // DECRARA
        changeRectangleAttributes(p, final == 't');
        break;
    default:
        break;
    }
}

// DECCARA sets / DECRARA toggles bold (1), underline (4), blink (5),
// reverse (7) and hidden (8); DECCARA also clears them (0, 22-28) and sets
// colors.
void Terminal::changeRectangleAttributes(const std::vector<int> &params, bool reverse)
{
    struct Attribute {
        int code;
        unsigned int flag;
        unsigned int attr;
    };
    static constexpr std::array kAttributes{
        Attribute{.code = 1, .flag = CellFlag::Bold, .attr = 0},
        Attribute{.code = 4, .flag = CellFlag::Underline, .attr = 0},
        Attribute{.code = 5, .flag = 0, .attr = CellAttr::Blink},
        Attribute{.code = 7, .flag = CellFlag::Reverse, .attr = 0},
        Attribute{.code = 8, .flag = CellFlag::Invisible, .attr = 0},
    };
    std::vector<int> codes(params.begin() + std::min<std::ptrdiff_t>(4, std::ssize(params)), params.end());
    if (codes.empty()) {
        codes.push_back(0);
    }
    const bool stream = (m_extraFlags & RectangleExtent) == 0;
    m_screen.changeRect(rectangleParams(params, 0), stream, [&codes, &reverse](Cell &cell) {
        std::ranges::for_each(codes, [&cell, &reverse](int code) {
            std::ranges::for_each(kAttributes, [&](const Attribute &a) {
                const bool selected = code == a.code || code == 0 || code == a.code + 20;
                if (!selected) {
                    return;
                }
                const bool on = reverse ? ((cell.flags & a.flag) == 0u && (cell.attrs & a.attr) == 0u)
                                        : (code == a.code);
                cell.flags = static_cast<unsigned char>(on ? (cell.flags | a.flag) : (cell.flags & ~a.flag));
                cell.attrs = static_cast<unsigned char>(on ? (cell.attrs | a.attr) : (cell.attrs & ~a.attr));
            });
            if (!reverse) {
                applyBasicColor(code, cell);
            }
        });
    });
}

void Terminal::handleEscIntermediate(const QByteArray &intermediates, char final)
{
    if (intermediates == "#") {
        switch (final) {
        case '3': // DECDHL top half
            m_screen.setLineSize(Screen::DoubleHeightTop);
            break;
        case '4': // DECDHL bottom half
            m_screen.setLineSize(Screen::DoubleHeightBottom);
            break;
        case '5': // DECSWL
            m_screen.setLineSize(Screen::SingleLine);
            break;
        case '6': // DECDWL
            m_screen.setLineSize(Screen::DoubleWidthLine);
            break;
        case '8': // DECALN
            m_screen.alignmentTest();
            break;
        default:
            break;
        }
        return;
    }
    // SCS: designate a character set into G0..G3.
    int slot = -1;
    const char charset = designatedCharset(intermediates, final, slot);
    if (slot >= 0) {
        m_charsets[static_cast<std::size_t>(slot)] = charset;
    }
    // S7C1T / S8C1T: replies in 7-bit (ESC Fe) or 8-bit (C1) controls.
    if (intermediates == " " && (final == 'F' || final == 'G')) {
        setExtraFlag(EightBitControls, final == 'G');
    }
    // ESC % G (UTF-8), ...: nothing to do.
}

// ED/EL/ECH skip ISO-protected cells unless ERM (erasure mode) is set.
unsigned int Terminal::isoProtection() const
{
    return (m_extraFlags & ErasureMode) != 0 ? 0u : CellAttr::IsoProtected;
}

void Terminal::handleAnsiMode(bool set, const std::vector<int> &params)
{
    std::ranges::for_each(params, [this, &set](int mode) {
        if (mode == 6) {
            setExtraFlag(ErasureMode, set); // ERM
        } else if (mode == 4) {
            m_screen.setInsertMode(set); // IRM
        } else if (mode == 20) {
            setExtraFlag(NewlineMode, set); // LNM
        }
    });
}

// DECSC: Screen saves the position; the pen and character sets live here.
void Terminal::saveCursorState()
{
    m_screen.saveCursor();
    m_savedStates[m_screen.inAltScreen() ? 1 : 0] = SavedTerminalState{
        .pen = m_pen,
        .charsets = m_charsets,
        .glSet = m_glSet,
    };
}

void Terminal::restoreCursorState(bool position)
{
    if (position) {
        m_screen.restoreCursor();
    }
    const SavedTerminalState &saved = m_savedStates[m_screen.inAltScreen() ? 1 : 0];
    m_pen = saved.pen;
    m_charsets = saved.charsets;
    m_glSet = static_cast<unsigned char>(saved.glSet);
    m_screen.setEraseBackground(m_pen.bg);
}

// 47 / 1047 / 1049 switch buffers; 1048 only saves/restores the cursor.
void Terminal::setAltScreenMode(int mode, bool set)
{
    if (mode == 1048) {
        if (set) {
            saveCursorState();
        } else {
            restoreCursorState(true);
        }
        return;
    }
    clearCommandBlocks();
    if (mode == 1049 && set) {
        saveCursorState();
    }
    if (set) {
        m_screen.enterAltScreen();
        return;
    }
    // Leaving 1049 restores the main screen's DECSC cursor before the main
    // screen re-wraps; the pen and charsets follow. Alternate-screen images
    // go with that screen.
    m_graphics.clearAlternate();
    m_screen.leaveAltScreen(mode == 1049);
    const Screen::ReflowMap reflow = m_screen.takeReflowMap();
    m_graphics.remapAnchors(reflow.base, reflow.anchors);
    if (mode == 1049) {
        restoreCursorState(false);
    }
}

// DECSTR: modes, margins, pen, character sets and saved cursor back to their
// power-on values; screen content stays.
void Terminal::softReset()
{
    m_screen.softReset();
    m_pen = Cell{};
    m_screen.setEraseBackground(0);
    m_charsets = {'B', 'B', 'B', 'B'};
    m_glSet = 0;
    m_singleShift = 0;
    m_savedStates[m_screen.inAltScreen() ? 1 : 0] = {};
    setExtraFlag(ApplicationCursorKeys | ApplicationKeypad | NewlineMode, false);
}

// RIS (ESC c): everything, including the alternate screen and input modes.
void Terminal::fullReset()
{
    m_screen.fullReset();
    m_graphics.clear();
    clearCommandBlocks();
    m_pen = Cell{};
    m_charsets = {'B', 'B', 'B', 'B'};
    m_glSet = 0;
    m_singleShift = 0;
    m_savedStates = {};
    m_lastGraphic = 0;
    m_mouseModes = 0;
    m_kittyStack = {};
    m_kittyDepth = 0;
    m_cursorStyle = 0;
    m_titleStack.clear();
    if (!m_pointerShapes.isEmpty()) {
        m_pointerShapes.clear();
        m_extraFlags |= PointerShapeDirty;
    }
    // Keep only notifications the backend has not consumed yet.
    m_extraFlags &= RemoteHostDirty | TitleDirty | CurrentDirectoryDirty | ClipboardTextDirty | CommandFinishedPending |
                    DarkColorScheme | PointerShapeDirty | BellPending | ProgressDirty;
    m_extraFlags |= AlternateScroll | ColorsDirty;
    m_paletteOverrides = {};
    m_fgOverride = 0;
    m_bgOverride = 0;
    m_cursorOverride = 0;
}

void Terminal::handleDcs(const QByteArray &body)
{
    if (body.startsWith("+q")) {
        handleXtgettcap(body.mid(2)); // XTGETTCAP
        return;
    }
    if (body.startsWith("$q")) {
        handleDecrqss(body.mid(2)); // DECRQSS
        return;
    }
    // Sixel: DCS P1 ; P2 ; P3 q data ST. DECUDK: DCS Pc ; Pl | ... ST.
    const auto final = std::ranges::find_if(body, [](char c) { return (c < '0' || c > '9') && c != ';'; });
    if (final != body.end() && *final == 'q') {
        handleSixel(body);
    } else if (final != body.end() && *final == '|') {
        handleUserDefinedKeys(body);
    }
    // tmux passthrough and other DCS strings are consumed unused.
}

void Terminal::handleApc(const QByteArray &body)
{
    if (!body.startsWith('G')) {
        return;
    }
    const KittyResult result = m_graphics.handleKitty(body.mid(1), graphicsContext());
    reply(result.reply);
    if (result.placed != 0 && result.moveCursor != 0) {
        // The cursor moves to the cell after the image's last column, on its
        // last row (scrolling if needed), as in kitty.
        const int column = m_screen.cursorCol();
        std::ranges::for_each(std::views::iota(1, std::max(result.cursorRows, 1)), [this](int) { m_screen.lineFeed(); });
        m_screen.moveCursor(m_screen.cursorRow(), std::min(column + result.cursorColumns, m_screen.cols() - 1));
    }
}

void Terminal::handleSixel(const QByteArray &body)
{
    const qsizetype q = body.indexOf('q');
    const std::vector<int> params = parseParams(body.left(q));
    const QImage image = decodeSixel(body.mid(q + 1), paramAt(params, 1, 0) == 1);
    if (image.isNull()) {
        return;
    }
    const bool displayMode = (m_extraFlags & SixelDisplayMode) != 0;
    GraphicsContext context = graphicsContext();
    if (displayMode) {
        // DECSDM: the image goes to the top-left corner, nothing scrolls.
        context.anchor = m_screen.rowAnchor(0);
        context.column = 0;
    }
    const int rows = m_graphics.addSixel(image, context);
    if (!displayMode) {
        // Sixel scrolling: the cursor goes below the image, in its column.
        const int column = m_screen.cursorCol();
        std::ranges::for_each(std::views::iota(0, rows), [this](int) { m_screen.lineFeed(); });
        m_screen.moveCursor(m_screen.cursorRow(), column);
    }
}

GraphicsContext Terminal::graphicsContext() const
{
    return GraphicsContext{
        .anchor = m_screen.rowAnchor(m_screen.cursorRow()),
        .column = m_screen.cursorCol(),
        .cellWidth = m_cellPixelWidth,
        .cellHeight = m_cellPixelHeight,
        .alternate = m_screen.inAltScreen() ? 1 : 0,
        .top = m_screen.rowAnchor(0),
    };
}

// ED 2 removes the images on the screen, ED 3 those in the history.
void Terminal::eraseImages(int mode)
{
    const int alternate = m_screen.inAltScreen() ? 1 : 0;
    if (mode == 2) {
        m_graphics.erasePlacements(alternate, m_screen.rowAnchor(0), m_screen.rowAnchor(m_screen.rows() - 1));
    } else if (mode == 3 && alternate == 0) {
        m_graphics.erasePlacements(0, m_screen.lineAnchor(0) - (1LL << 40), m_screen.rowAnchor(0) - 1);
    }
}

// XTSMGRAPHICS: CSI ? Pi ; Pa ; Pv S. Item 1 = color registers, 2 = sixel
// geometry; any action reads back the (fixed) values.
void Terminal::handleGraphicsAttributes(const std::vector<int> &params)
{
    const int item = paramAt(params, 0, 0);
    if (item == 1) {
        reply("\x1b[?1;0;256S");
    } else if (item == 2) {
        reply("\x1b[?2;0;" + QByteArray::number(m_screen.cols() * m_cellPixelWidth) + ";" +
              QByteArray::number(m_screen.rows() * m_cellPixelHeight) + "S");
    } else {
        reply("\x1b[?" + QByteArray::number(item) + ";1;0S"); // not supported (ReGIS)
    }
}

void Terminal::handleDec(bool set, const std::vector<int> &params)
{
    for (const int mode : params) {
        switch (mode) {
        case 1:
            setExtraFlag(ApplicationCursorKeys, set);
            break;
        case 2:
            setExtraFlag(Vt52Mode, !set); // DECANM reset: VT52 mode
            break;
        case 3:
            setColumnMode(set);
            break;
        case 40:
            setExtraFlag(Allow132Columns, set);
            break;
        case 5:
            setExtraFlag(ReverseVideo, set); // DECSCNM
            break;
        case 42:
            setExtraFlag(NrcMode, set); // DECNRCM
            break;
        case 80:
            setExtraFlag(SixelDisplayMode, set); // DECSDM
            break;
        case 45:
            m_screen.setReverseWrap(set);
            break;
        case 69:
            m_screen.setLeftRightMarginMode(set); // DECLRMM
            break;
        case 95:
            setExtraFlag(NoClearOnColumnChange, set); // DECNCSM
            break;
        case 6:
            m_screen.setOriginMode(set); // DECOM
            m_screen.moveCursorAddressed(0, 0);
            break;
        case 7:
            m_screen.setAutoWrap(set); // DECAWM
            break;
        case 12:
            setExtraFlag(CursorBlinkMode, set); // AT&T 610 cursor blinking
            break;
        case 25:
            m_screen.showCursor(set);
            break;
        case 1004:
            setExtraFlag(FocusReporting, set);
            break;
        case 1007:
            setExtraFlag(AlternateScroll, set);
            break;
        case 47:
        case 1047:
        case 1048:
        case 1049:
            setAltScreenMode(mode, set);
            break;
        case 1000:
            m_mouseModes = set ? static_cast<unsigned char>(m_mouseModes | 0x01u)
                               : static_cast<unsigned char>(m_mouseModes & ~0x01u);
            break;
        case 1001: // highlight tracking: reports presses like 1000
            m_mouseModes = set ? static_cast<unsigned char>(m_mouseModes | 0x08u)
                               : static_cast<unsigned char>(m_mouseModes & ~0x08u);
            break;
        case 1002:
            m_mouseModes = set ? static_cast<unsigned char>(m_mouseModes | 0x02u)
                               : static_cast<unsigned char>(m_mouseModes & ~0x02u);
            break;
        case 1003:
            m_mouseModes = set ? static_cast<unsigned char>(m_mouseModes | 0x04u)
                               : static_cast<unsigned char>(m_mouseModes & ~0x04u);
            break;
        case 1005:
            setExtraFlag(Utf8Mouse, set);
            break;
        case 1006:
            setExtraFlag(SgrMouse, set);
            break;
        case 1015:
            setExtraFlag(UrxvtMouse, set);
            break;
        case 1016:
            setExtraFlag(SgrPixelMouse, set);
            break;
        case 1045:
            m_screen.setExtendedReverseWrap(set);
            break;
        case 1036: // metaSendsEscape
        case 1039: // altSendsEscape
            setExtraFlag(MetaSendsEscape, set);
            break;
        case 2027:
            m_screen.setGraphemeClustering(set);
            break;
        case 2031:
            setExtraFlag(ColorSchemeReports, set);
            break;
        case 2048:
            // In-band resize: report the size now, then on every resize.
            setExtraFlag(InBandResize, set);
            if (set) {
                reportInBandSize();
            }
            break;
        case 2004:
            setExtraFlag(BracketedPasteEnabled, set);
            break;
        case 2026:
            setExtraFlag(SynchronizedUpdates, set);
            break;
        default:
            break;
        }
    }
}

namespace
{
// SGR codes that only turn CellFlag / CellAttr bits on or off.
struct SgrToggle {
    int code;
    unsigned int flagsOn;
    unsigned int flagsOff;
    unsigned int attrsOn;
    unsigned int attrsOff;
};

constexpr std::array kSgrToggles{
    SgrToggle{.code = 1, .flagsOn = CellFlag::Bold, .flagsOff = 0, .attrsOn = 0, .attrsOff = 0},
    SgrToggle{.code = 2, .flagsOn = CellFlag::Dim, .flagsOff = 0, .attrsOn = 0, .attrsOff = 0},
    SgrToggle{.code = 3, .flagsOn = 0, .flagsOff = 0, .attrsOn = CellAttr::Italic, .attrsOff = 0},
    SgrToggle{.code = 5, .flagsOn = 0, .flagsOff = 0, .attrsOn = CellAttr::Blink, .attrsOff = 0},
    SgrToggle{.code = 6, .flagsOn = 0, .flagsOff = 0, .attrsOn = CellAttr::Blink, .attrsOff = 0},
    SgrToggle{.code = 7, .flagsOn = CellFlag::Reverse, .flagsOff = 0, .attrsOn = 0, .attrsOff = 0},
    SgrToggle{.code = 8, .flagsOn = CellFlag::Invisible, .flagsOff = 0, .attrsOn = 0, .attrsOff = 0},
    SgrToggle{.code = 9, .flagsOn = CellFlag::Strike, .flagsOff = 0, .attrsOn = 0, .attrsOff = 0},
    SgrToggle{.code = 22, .flagsOn = 0, .flagsOff = CellFlag::Bold | CellFlag::Dim, .attrsOn = 0, .attrsOff = 0},
    SgrToggle{.code = 23, .flagsOn = 0, .flagsOff = 0, .attrsOn = 0, .attrsOff = CellAttr::Italic},
    SgrToggle{.code = 25, .flagsOn = 0, .flagsOff = 0, .attrsOn = 0, .attrsOff = CellAttr::Blink},
    SgrToggle{.code = 27, .flagsOn = 0, .flagsOff = CellFlag::Reverse, .attrsOn = 0, .attrsOff = 0},
    SgrToggle{.code = 28, .flagsOn = 0, .flagsOff = CellFlag::Invisible, .attrsOn = 0, .attrsOff = 0},
    SgrToggle{.code = 29, .flagsOn = 0, .flagsOff = CellFlag::Strike, .attrsOn = 0, .attrsOff = 0},
    SgrToggle{.code = 53, .flagsOn = 0, .flagsOff = 0, .attrsOn = CellAttr::Overline, .attrsOff = 0},
    SgrToggle{.code = 55, .flagsOn = 0, .flagsOff = 0, .attrsOn = 0, .attrsOff = CellAttr::Overline},
};

unsigned int indexedColor(int index)
{
    return static_cast<unsigned int>(std::clamp(index, 0, 255) + 1); // 256 palette -> 1..256
}

unsigned int rgbColor(int r, int g, int b)
{
    const auto channel = [](int v) { return static_cast<unsigned int>(std::clamp(v, 0, 255)); };
    return 0x80000000u | (channel(r) << 16) | (channel(g) << 8) | channel(b);
}

// The basic 8/16 colors: 30-37/90-97 foreground, 40-47/100-107 background.
// Returns false for any other code.
bool applyBasicColor(int code, Cell &pen)
{
    if (code >= 30 && code <= 37) {
        pen.fg = static_cast<unsigned int>(code - 30 + 1);
    } else if (code >= 40 && code <= 47) {
        pen.bg = static_cast<unsigned int>(code - 40 + 1);
    } else if (code >= 90 && code <= 97) {
        pen.fg = static_cast<unsigned int>(code - 90 + 9);
    } else if (code >= 100 && code <= 107) {
        pen.bg = static_cast<unsigned int>(code - 100 + 9);
    } else {
        return false;
    }
    return true;
}
} // namespace

void Terminal::handleSgr(const ParamGroups &groups)
{
    std::size_t index = 0;
    while (index < groups.size()) {
        index += applySgr(groups, index);
    }
}

// Applies the SGR group at `index` and returns how many groups it consumed
// (the semicolon forms 38;5;n and 38;2;r;g;b span several).
std::size_t Terminal::applySgr(const ParamGroups &groups, std::size_t index)
{
    const std::vector<int> &group = groups[index];
    const int code = std::max(group.front(), 0);
    switch (code) {
    case 0:
        resetPen();
        break;
    case 4:
        // 4 alone is a single underline; 4:0 turns it off, 4:1..4:5 pick a style.
        setUnderline(group.size() > 1 ? std::max(group[1], 0) : 1);
        break;
    case 21:
        setUnderline(2); // double underline (ECMA-48), as in kitty and VTE.
        break;
    case 24:
        setUnderline(0);
        break;
    case 38:
        return applyExtendedColor(groups, index, m_pen.fg);
    case 48:
        return applyExtendedColor(groups, index, m_pen.bg);
    case 58:
        return applyExtendedColor(groups, index, m_pen.ul);
    case 39:
        m_pen.fg = 0;
        break;
    case 49:
        m_pen.bg = 0;
        break;
    case 59:
        m_pen.ul = 0;
        break;
    default:
        applyBasicSgr(code);
        break;
    }
    return 1;
}

void Terminal::applyBasicSgr(int code)
{
    if (applyBasicColor(code, m_pen)) {
        return;
    }
    const auto *const toggle = std::ranges::find(kSgrToggles, code, &SgrToggle::code);
    if (toggle == kSgrToggles.end()) {
        return; // fonts, framed/encircled, ideograms: not rendered.
    }
    m_pen.flags = static_cast<unsigned char>((m_pen.flags | toggle->flagsOn) & ~toggle->flagsOff);
    m_pen.attrs = static_cast<unsigned char>((m_pen.attrs | toggle->attrsOn) & ~toggle->attrsOff);
}

// `style` is the SGR 4:n sub-parameter: 0 none, 1 single, 2 double, 3 curly,
// 4 dotted, 5 dashed.
void Terminal::setUnderline(int style)
{
    m_pen.attrs = static_cast<unsigned char>(m_pen.attrs & ~CellAttr::UnderlineMask);
    if (style <= 0 || style > 5) {
        m_pen.flags = static_cast<unsigned char>(m_pen.flags & ~CellFlag::Underline);
        return;
    }
    m_pen.flags = static_cast<unsigned char>(m_pen.flags | CellFlag::Underline);
    const auto bits = static_cast<unsigned int>(style - 1) << CellAttr::UnderlineShift;
    m_pen.attrs = static_cast<unsigned char>(m_pen.attrs | bits);
}

void Terminal::resetPen()
{
    // SGR 0 resets the style; the OSC 8 hyperlink is not part of it.
    const unsigned short link = m_pen.link;
    m_pen = Cell{};
    m_pen.link = link;
}

// 38/48/58 extended colors, in the colon form (38:5:n, 38:2:r:g:b,
// 38:2:colorspace:r:g:b) or the semicolon form (38;5;n, 38;2;r;g;b).
// Returns the number of groups consumed.
std::size_t Terminal::applyExtendedColor(const ParamGroups &groups, std::size_t index, unsigned int &color)
{
    const std::vector<int> &group = groups[index];
    if (group.size() > 1) {
        if (group[1] == 5 && group.size() > 2) {
            color = indexedColor(group[2]);
        } else if (group[1] == 2 && group.size() >= 5) {
            const std::size_t first = group.size() >= 6 ? 3 : 2; // skip the colorspace id
            color = rgbColor(group[first], group[first + 1], group[first + 2]);
        }
        return 1;
    }
    const auto at = [&groups, index](std::size_t offset) {
        return index + offset < groups.size() ? groups[index + offset].front() : -1;
    };
    if (at(1) == 5) {
        if (at(2) >= 0) {
            color = indexedColor(at(2));
        }
        return 3;
    }
    if (at(1) == 2) {
        color = rgbColor(at(2), at(3), at(4));
        return 5;
    }
    return 1;
}

void Terminal::handleOsc(const QByteArray &body)
{
    if (body.startsWith("1337;RemoteHost=")) {
        handleOscRemoteHost(body);
        return;
    }
    if (body.startsWith("52;")) {
        handleOscClipboard(body);
        return;
    }
    if (body.startsWith("7;")) {
        handleOscCurrentDirectory(body);
        return;
    }
    if (body.startsWith("133;")) {
        handleOscCommandMark(body);
        return;
    }
    if (body.startsWith("633;")) {
        handleOscVsCode(body.mid(4));
        return;
    }
    if (body.startsWith("22;")) {
        handleOscPointerShape(body.mid(3));
        return;
    }
    if (body.startsWith("8;")) {
        handleOscHyperlink(body);
        return;
    }
    if (body == "9;4" || body.startsWith("9;4;")) {
        handleOscProgress(body.mid(4));
        return;
    }
    if (body.startsWith("9;") || body.startsWith("777;")) {
        handleOscNotification(body);
        return;
    }
    if (body.startsWith("1337;")) {
        handleOscIterm(body.mid(5));
        return;
    }
    if (body.startsWith("4;")) {
        handleOscPalette(body.mid(2));
        return;
    }
    if (body == "104" || body.startsWith("104;")) {
        handleOscPaletteReset(body.mid(4));
        return;
    }
    if (handleOscColor(body)) {
        return;
    }
    handleOscTitle(body);
    // Other OSCs (icon name, notifications, ...) are ignored.
}

// iTerm2 OSC 1337 commands: inline images (File=, MultipartFile /
// FilePart / FileEnd), CurrentDir, SetUserVar, ReportCellSize and
// ClearScrollback. RemoteHost is handled before (it carries a token).
void Terminal::handleOscIterm(const QByteArray &command)
{
    constexpr qsizetype kMaxFile = 400LL * 1024 * 1024;
    if (command.startsWith("File=")) {
        const qsizetype colon = command.indexOf(':');
        if (colon > 0) {
            placeItermImage(command.mid(5, colon - 5), command.mid(colon + 1));
        }
    } else if (command.startsWith("MultipartFile=")) {
        m_itermFileArgs = command.mid(14);
        m_itermFileData.clear();
    } else if (command.startsWith("FilePart=")) {
        if (m_itermFileData.size() + command.size() <= kMaxFile) {
            m_itermFileData += command.mid(9);
        }
    } else if (command == "FileEnd") {
        placeItermImage(std::exchange(m_itermFileArgs, {}), std::exchange(m_itermFileData, {}));
    } else if (command.startsWith("CurrentDir=")) {
        const QString path = QString::fromUtf8(command.mid(11));
        if (path.startsWith(QLatin1Char('/')) && path != m_currentDirectory) {
            m_currentDirectory = path;
            setExtraFlag(CurrentDirectoryDirty, true);
        }
    } else if (command.startsWith("SetUserVar=")) {
        const QByteArray assignment = command.mid(11);
        const qsizetype equals = assignment.indexOf('=');
        if (equals > 0 && m_userVars.size() < 256) {
            m_userVars.insert(assignment.left(equals), QByteArray::fromBase64(assignment.mid(equals + 1)).left(4096));
        }
    } else if (command == "ReportCellSize") {
        // Height ; width in points; the cell size is kept in device pixels.
        reply("\x1b]1337;ReportCellSize=" + QByteArray::number(m_cellPixelHeight) + ";" +
              QByteArray::number(m_cellPixelWidth) + "\x1b\\");
    } else if (command == "ClearScrollback") {
        m_screen.clearScrollback();
    }
}

void Terminal::placeItermImage(const QByteArray &args, const QByteArray &base64)
{
    const ItermResult placed =
        m_graphics.placeItermFile(args, base64, graphicsContext(), m_screen.cols(), m_screen.rows());
    if (placed.rows == 0) {
        return;
    }
    // Like a large character: the cursor ends after the image, on its last row.
    const int column = m_screen.cursorCol();
    std::ranges::for_each(std::views::iota(1, placed.rows), [this](int) { m_screen.lineFeed(); });
    m_screen.moveCursor(m_screen.cursorRow(), std::min(column + placed.columns, m_screen.cols() - 1));
}

// OSC 9 ; text (iTerm2) and OSC 777 ; notify ; title ; body (rxvt) ask for a
// desktop notification. OSC 9 ; N ; ... with a number is ConEmu's family
// (progress, sleep, ...), not a notification.
void Terminal::handleOscNotification(const QByteArray &body)
{
    constexpr qsizetype kMaxText = 4096;
    QByteArray title;
    QByteArray text;
    if (body.startsWith("777;")) {
        const QList<QByteArray> fields = body.split(';');
        if (fields.size() < 3 || fields[1] != "notify") {
            return;
        }
        title = fields[2];
        text = fields.size() > 3 ? body.mid(body.indexOf(';', body.indexOf(';', 4) + 1) + 1) : QByteArray{};
    } else {
        text = body.mid(2);
        const qsizetype separator = text.indexOf(';');
        bool numeric = false;
        text.left(separator).toInt(&numeric);
        if (separator > 0 && numeric) {
            return; // ConEmu OSC 9 ; N ; ...
        }
    }
    if (text.isEmpty() && title.isEmpty()) {
        return;
    }
    m_requests.push_back(TerminalRequest{.data = title.left(kMaxText),
                                         .body = text.left(kMaxText),
                                         .kind = TerminalRequest::Notification,
                                         .a = 0,
                                         .b = 0,
                                         .c = 0});
}

// OSC 9 ; 4 ; state ; percent: 0 removes the progress, 1 sets it, 2 is an
// error, 3 indeterminate and 4 paused. A missing percent keeps the last one
// for 2 and 4 (ConEmu); `body` is what follows "9;4;".
void Terminal::handleOscProgress(const QByteArray &body)
{
    const QList<QByteArray> fields = body.split(';');
    bool stateOk = false;
    const int state = fields.value(0).toInt(&stateOk);
    if (!stateOk || state < ProgressNone || state > ProgressPaused) {
        return;
    }
    bool valueOk = false;
    const int value = std::clamp(fields.value(1).toInt(&valueOk), 0, 100);
    int newValue = valueOk ? value : 0;
    if (state == ProgressNone || state == ProgressIndeterminate) {
        newValue = 0;
    } else if (!valueOk && state != ProgressNormal) {
        newValue = m_progressValue;
    }
    if (state != m_progressState || newValue != m_progressValue) {
        m_progressState = static_cast<std::uint16_t>(state);
        m_progressValue = static_cast<std::uint16_t>(newValue);
        setExtraFlag(ProgressDirty, true);
    }
}

void Terminal::handleOscRemoteHost(const QByteArray &body)
{
    const QByteArray payload = body.mid(sizeof("1337;RemoteHost=") - 1);
    const qsizetype separator = payload.indexOf(';');
    if (separator < 0 || payload.first(separator) != m_remoteHostToken.toUtf8()) {
        return;
    }
    const QString host = QString::fromUtf8(payload.mid(separator + 1));
    if (host != m_remoteHost) {
        m_remoteHost = host;
        m_extraFlags |= RemoteHostDirty;
    }
}

// OSC 52;selection;base64 stores text copied by a remote terminal app.
// Bound the encoded payload to avoid untrusted PTY output allocating an
// arbitrarily large clipboard value.
void Terminal::handleOscClipboard(const QByteArray &body)
{
    const int separator = static_cast<int>(body.indexOf(';', 3));
    if (separator < 0 || body.size() - separator - 1 > 1'400'000) {
        return;
    }
    const QByteArray selection = body.mid(3, separator - 3);
    if (body.mid(separator + 1) == "?") {
        // Reading the clipboard is up to the user (see TerminalBackend).
        m_requests.push_back(TerminalRequest{.data = selection.isEmpty() ? QByteArray("c") : selection,
                                             .body = {},
                                             .kind = TerminalRequest::ClipboardRead,
                                             .a = 0,
                                             .b = 0,
                                             .c = 0});
        return;
    }
    if (selection.isEmpty() || selection.contains('c') || selection.contains('p')) {
        const QByteArray encoded = body.mid(separator + 1);
        const QByteArray decoded = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (!encoded.isEmpty() && decoded.size() <= 1'000'000) {
            m_clipboardText = QString::fromUtf8(decoded);
            setExtraFlag(ClipboardTextDirty, true);
        }
    }
}

// OSC 633 (VS Code shell integration): A-D are the OSC 133 marks, E ; line
// the command line (\xAB escapes, \\ for '\\'), P ; Cwd=path the directory.
void Terminal::handleOscVsCode(const QByteArray &body)
{
    if (body.isEmpty()) {
        return;
    }
    const char kind = body.at(0);
    if (kind >= 'A' && kind <= 'D') {
        handleOscCommandMark("133;" + body);
    } else if (kind == 'E' && body.startsWith("E;")) {
        QByteArray line = body.mid(2);
        line = line.left(line.indexOf(';')); // drop the nonce
        QByteArray decoded;
        qsizetype i = 0;
        while (i < line.size()) {
            if (line.at(i) == '\\' && i + 3 < line.size() && line.at(i + 1) == 'x') {
                decoded += QByteArray::fromHex(line.mid(i + 2, 2));
                i += 4;
            } else if (line.at(i) == '\\' && i + 1 < line.size() && line.at(i + 1) == '\\') {
                decoded += '\\';
                i += 2;
            } else {
                decoded += line.at(i);
                ++i;
            }
        }
        m_commandLine = QString::fromUtf8(decoded).left(4096);
    } else if (body.startsWith("P;Cwd=")) {
        const QString path = QString::fromUtf8(body.mid(6));
        if (path.startsWith(QLatin1Char('/')) && path != m_currentDirectory) {
            m_currentDirectory = path;
            setExtraFlag(CurrentDirectoryDirty, true);
        }
    }
}

int pointerCursorShape(const QByteArray &name)
{
    using Entry = std::pair<std::string_view, Qt::CursorShape>;
    // CSS names (kitty's OSC 22) and their X cursor-font aliases.
    static constexpr std::array kShapes{
        Entry{"default", Qt::ArrowCursor},       Entry{"left_ptr", Qt::ArrowCursor},
        Entry{"text", Qt::IBeamCursor},          Entry{"xterm", Qt::IBeamCursor},
        Entry{"vertical-text", Qt::IBeamCursor}, Entry{"pointer", Qt::PointingHandCursor},
        Entry{"hand2", Qt::PointingHandCursor},  Entry{"hand1", Qt::PointingHandCursor},
        Entry{"help", Qt::WhatsThisCursor},      Entry{"question_arrow", Qt::WhatsThisCursor},
        Entry{"wait", Qt::WaitCursor},           Entry{"watch", Qt::WaitCursor},
        Entry{"progress", Qt::BusyCursor},       Entry{"crosshair", Qt::CrossCursor},
        Entry{"cell", Qt::CrossCursor},          Entry{"tcross", Qt::CrossCursor},
        Entry{"move", Qt::SizeAllCursor},        Entry{"fleur", Qt::SizeAllCursor},
        Entry{"all-scroll", Qt::SizeAllCursor},  Entry{"not-allowed", Qt::ForbiddenCursor},
        Entry{"no-drop", Qt::ForbiddenCursor},   Entry{"grab", Qt::OpenHandCursor},
        Entry{"grabbing", Qt::ClosedHandCursor}, Entry{"copy", Qt::DragCopyCursor},
        Entry{"alias", Qt::DragLinkCursor},      Entry{"ew-resize", Qt::SizeHorCursor},
        Entry{"col-resize", Qt::SplitHCursor},   Entry{"e-resize", Qt::SizeHorCursor},
        Entry{"w-resize", Qt::SizeHorCursor},    Entry{"sb_h_double_arrow", Qt::SizeHorCursor},
        Entry{"ns-resize", Qt::SizeVerCursor},   Entry{"row-resize", Qt::SplitVCursor},
        Entry{"n-resize", Qt::SizeVerCursor},    Entry{"s-resize", Qt::SizeVerCursor},
        Entry{"sb_v_double_arrow", Qt::SizeVerCursor}, Entry{"nesw-resize", Qt::SizeBDiagCursor},
        Entry{"ne-resize", Qt::SizeBDiagCursor}, Entry{"sw-resize", Qt::SizeBDiagCursor},
        Entry{"nwse-resize", Qt::SizeFDiagCursor}, Entry{"nw-resize", Qt::SizeFDiagCursor},
        Entry{"se-resize", Qt::SizeFDiagCursor},
    };
    const std::string_view wanted(name.constData(), static_cast<std::size_t>(name.size()));
    const auto *const found = std::ranges::find(kShapes, wanted, &Entry::first);
    return found == kShapes.end() ? -1 : static_cast<int>(found->second);
}

// The OSC 22 reply to ?name,...: 1 / 0 per name, __current__ = the shown one.
QByteArray Terminal::pointerShapeQuery(const QByteArray &names) const
{
    QList<QByteArray> answers;
    std::ranges::transform(names.split(','), std::back_inserter(answers), [this](const QByteArray &name) {
        if (name == "__current__") {
            return pointerShape().isEmpty() ? QByteArray("0") : pointerShape();
        }
        if (name == "__default__" || name == "__grabbed__") {
            return QByteArray("default");
        }
        return QByteArray(pointerCursorShape(name) >= 0 ? "1" : "0");
    });
    return "\x1b]22;" + answers.join(',') + "\x1b\\";
}

// OSC 22 (xterm / kitty): name sets the pointer (empty / default resets),
// >name,... pushes, < pops, ?name,... asks which are supported.
void Terminal::handleOscPointerShape(const QByteArray &body)
{
    constexpr qsizetype kMaxDepth = 16;
    if (body.startsWith('?')) {
        reply(pointerShapeQuery(body.mid(1)));
        return;
    }
    if (body.startsWith('>')) {
        std::ranges::for_each(body.mid(1).split(','), [this](const QByteArray &name) {
            if (!name.isEmpty() && m_pointerShapes.size() < kMaxDepth) {
                m_pointerShapes.push_back(name);
            }
        });
    } else if (body.startsWith('<')) {
        if (m_pointerShapes.isEmpty()) {
            return;
        }
        m_pointerShapes.pop_back();
    } else if (body.isEmpty() || body == "default") {
        m_pointerShapes.clear();
    } else if (m_pointerShapes.isEmpty()) {
        m_pointerShapes.push_back(body);
    } else {
        m_pointerShapes.back() = body;
    }
    setExtraFlag(PointerShapeDirty, true);
}

void Terminal::handleOscCurrentDirectory(const QByteArray &body)
{
    const QUrl url = QUrl::fromEncoded(body.mid(2), QUrl::TolerantMode);
    const QString path = url.scheme() == QStringLiteral("file") ? url.path(QUrl::FullyDecoded) : QString{};
    if (path.startsWith(QLatin1Char('/')) && path != m_currentDirectory) {
        m_currentDirectory = path;
        setExtraFlag(CurrentDirectoryDirty, true);
    }
}

void Terminal::handleOscCommandMark(const QByteArray &body)
{
    if (body.size() <= 4) {
        return;
    }
    handleCommandMark(body.at(4));
    if (body.at(4) == 'D') {
        // D ; status ; ... -- later fields (aid=, err=) are ignored.
        bool ok = false;
        const int code = body.size() > 6 && body.at(5) == ';' ? body.mid(6).split(';').value(0).toInt(&ok) : 0;
        m_lastExitCode = ok ? code : -1;
        setExtraFlag(CommandFinishedPending, true);
    }
}

// Form: <num>;<text>. We only honor 0 (icon+title) and 2 (title).
void Terminal::handleOscTitle(const QByteArray &body)
{
    const int idx = static_cast<int>(body.indexOf(';'));
    if (idx <= 0) {
        return;
    }
    const QByteArray num = body.left(idx);
    const QString text = QString::fromUtf8(body.mid(idx + 1));
    if (num == "0" || num == "2") {
        if (m_title != text) {
            m_title = text;
            setExtraFlag(TitleDirty, true);
        }
    }
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
        m_commandRunning = true;
        if (m_commandStartLine < 0) {
            m_commandStartLine = std::max(0, line - (m_screen.cursorCol() == 0 ? 1 : 0));
        }
        break;
    case 'D':
        m_commandRunning = false;
        if (m_commandStartLine >= 0) {
            const int endLine = std::max(m_commandStartLine, line - (m_screen.cursorCol() == 0 ? 1 : 0));
            m_commandBlocks.push_back({.startLine = m_commandStartLine, .endLine = endLine});
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
    case '7': // DECSC
        saveCursorState();
        break;
    case '8': // DECRC
        restoreCursorState(true);
        break;
    case 'H': // HTS
        m_screen.setTabStop();
        break;
    case '6': // DECBI
        m_screen.backIndex();
        break;
    case '9': // DECFI
        m_screen.forwardIndex();
        break;
    case 'V': // SPA: start of guarded (ISO protected) area
        m_pen.attrs = static_cast<unsigned char>(m_pen.attrs | CellAttr::IsoProtected);
        break;
    case 'W': // EPA
        m_pen.attrs = static_cast<unsigned char>(m_pen.attrs & ~CellAttr::IsoProtected);
        break;
    case 'n': // LS2
        m_glSet = 2;
        break;
    case 'o': // LS3
        m_glSet = 3;
        break;
    case 'N': // SS2
        m_singleShift = 2;
        break;
    case 'O': // SS3
        m_singleShift = 3;
        break;
    case '=': // DECKPAM
        setExtraFlag(ApplicationKeypad, true);
        break;
    case '>': // DECKPNM
        setExtraFlag(ApplicationKeypad, false);
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
    case 'c': // RIS
        fullReset();
        break;
    default:
        break;
    }
}

void Terminal::setExtraFlag(std::uint64_t flag, bool on)
{
    m_extraFlags = on ? (m_extraFlags | flag) : (m_extraFlags & ~flag);
}

void Terminal::cursorMove(int dRow, int dCol) // NOLINT: dRow/dCol are inherently swappable
{
    const int r = m_screen.cursorRow() + dRow;
    const int c = m_screen.cursorCol() + dCol;
    m_screen.moveCursor(r, c);
}

} // namespace konsoll
