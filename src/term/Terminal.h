#pragma once

#include "Screen.h"

#include <QByteArray>
#include <QHash>
#include <QString>

#include <array>
#include <utility>
#include <vector>

namespace konsoll
{

// ANSI/VT100/xterm parser that drives a Screen. Consumes raw PTY bytes and
// maintains the SGR style state, OSC title and DA-reply bookkeeping.
class Terminal
{
public:
    struct CommandBlock {
        int startLine{0};
        int endLine{0};
    };

    explicit Terminal(int rows, int cols, const QString &remoteHostToken);

    void write(const QByteArray &data);
    void resize(int rows, int cols);

    Screen &screen() { return m_screen; }
    const Screen &screen() const { return m_screen; }

    // OSC window title.
    QString title() const { return m_title; }
    bool titleDirty() const { return (m_extraFlags & TitleDirty) != 0; }
    void clearTitleDirty() { setExtraFlag(TitleDirty, false); }
    QString currentDirectory() const { return m_currentDirectory; }
    QString remoteHost() const { return m_remoteHost; }
    bool remoteHostDirty() const { return (m_extraFlags & RemoteHostDirty) != 0; }
    void clearRemoteHostDirty() { setExtraFlag(RemoteHostDirty, false); }
    bool currentDirectoryDirty() const { return (m_extraFlags & CurrentDirectoryDirty) != 0; }
    void clearCurrentDirectoryDirty() { setExtraFlag(CurrentDirectoryDirty, false); }
    QString clipboardText() const { return m_clipboardText; }
    bool clipboardTextDirty() const { return (m_extraFlags & ClipboardTextDirty) != 0; }
    void clearClipboardTextDirty() { setExtraFlag(ClipboardTextDirty, false); }

    // Replies to queries (DA, DSR, DECRQM, DECRQSS, XTGETTCAP, XTVERSION,
    // XTWINOPS, OSC color queries, kitty keyboard), in the order the queries
    // arrived, plus unsolicited reports (in-band resize, color scheme). The
    // backend writes them to the PTY.
    QByteArray takeResponses() { return std::exchange(m_responses, QByteArray{}); }
    // The colors the session renders by default (0xAARRGGBB), for OSC
    // 10/11/12 queries and the dark/light color-scheme report.
    void setDefaultColors(unsigned int foreground, unsigned int background);
    // Cell size in pixels, for XTWINOPS size reports and in-band resize.
    void setCellPixelSize(int width, int height);

    // Colors applications set with OSC 4 / 10 / 11 / 12 (0xAARRGGBB; 0 =
    // not overridden). paletteColor() falls back to the default palette.
    unsigned int paletteColor(int index) const;
    unsigned int foregroundOverride() const { return m_fgOverride; }
    unsigned int backgroundOverride() const { return m_bgOverride; }
    unsigned int cursorColorOverride() const { return m_cursorOverride; }
    bool colorsDirty() const { return (m_extraFlags & ColorsDirty) != 0; }
    void clearColorsDirty() { setExtraFlag(ColorsDirty, false); }

    // OSC 8 hyperlink target of a cell's `link` id (empty for 0 / unknown).
    QString linkTarget(unsigned short id) const;

    // ?1007: in the alternate screen, the wheel sends cursor keys.
    bool alternateScroll() const { return (m_extraFlags & AlternateScroll) != 0; }

    const std::vector<CommandBlock> &commandBlocks() const { return m_commandBlocks; }
    int activeCommandStartLine() const { return m_commandStartLine; }
    void clearCommandBlocks();
    bool commandFinished() const { return (m_extraFlags & CommandFinishedPending) != 0; }
    void clearCommandFinished() { setExtraFlag(CommandFinishedPending, false); }
    // True while a command is executing (OSC 133 C -> D window).
    bool commandRunning() const { return m_commandRunning; }

    // Set when a full-screen erase (ESC[2J) is processed, so the backend can
    // drop any selection that now sits over blank lines.
    bool clearPending() const { return (m_extraFlags & ClearPending) != 0; }
    void clearClearPending() { setExtraFlag(ClearPending, false); }

    bool mouseTracking() const { return m_mouseModes != 0; }
    bool mouseMotionTracking() const { return (m_mouseModes & 0x06u) != 0; }
    bool sgrMouse() const { return (m_extraFlags & SgrMouse) != 0; }
    bool kittyKeyboardEnabled() const { return kittyKeyboardFlags() != 0; }
    bool bracketedPasteEnabled() const { return (m_extraFlags & BracketedPasteEnabled) != 0; }
    bool applicationCursorKeys() const { return (m_extraFlags & ApplicationCursorKeys) != 0; }
    // Kitty keyboard protocol progressive-enhancement flags (CSI > flags u).
    unsigned int kittyKeyboardFlags() const { return m_kittyStack[m_kittyDepth]; }
    // DECSCUSR cursor style: 0/1 blinking block, 2 block, 3/4 underline,
    // 5/6 bar (odd = blinking).
    int cursorStyle() const { return m_cursorStyle; }
    // 0 block, 1 underline, 2 bar.
    int cursorShape() const { return m_cursorStyle >= 5 ? 2 : (m_cursorStyle >= 3 ? 1 : 0); }
    // Blinking from DECSCUSR 1/3/5, or ?12 for the default style.
    bool cursorBlinking() const
    {
        return (m_cursorStyle != 0 && (m_cursorStyle % 2) == 1) ||
               (m_cursorStyle == 0 && (m_extraFlags & CursorBlinkMode) != 0);
    }
    // ?1004: the application wants CSI I / CSI O on focus changes.
    bool focusReporting() const { return (m_extraFlags & FocusReporting) != 0; }
    bool synchronizedUpdates() const { return (m_extraFlags & SynchronizedUpdates) != 0; }
    bool reverseVideo() const { return (m_extraFlags & ReverseVideo) != 0; }
    bool applicationKeypad() const { return (m_extraFlags & ApplicationKeypad) != 0; }
    QByteArray mouseReport(int button, int row, int col, int event, unsigned int modifiers) const;


private:
    // CSI parameters split on ';' into groups and on ':' into sub-parameters
    // (SGR 4:3, 38:2::r:g:b). An omitted value is -1.
    using ParamGroups = std::vector<std::vector<int>>;

    static constexpr unsigned int RemoteHostDirty = 1u << 0;
    static constexpr unsigned int BracketedPasteEnabled = 1u << 1;
    static constexpr unsigned int SynchronizedUpdates = 1u << 2;
    static constexpr unsigned int CursorBlinkMode = 1u << 3; // ?12
    static constexpr unsigned int SgrMouse = 1u << 4;
    static constexpr unsigned int ApplicationCursorKeys = 1u << 5;
    static constexpr unsigned int InBandResize = 1u << 6; // ?2048
    static constexpr unsigned int NewlineMode = 1u << 7;  // LNM: LF also returns
    static constexpr unsigned int ReverseVideo = 1u << 8; // DECSCNM
    static constexpr unsigned int ApplicationKeypad = 1u << 9; // DECKPAM
    static constexpr unsigned int TitleDirty = 1u << 10;
    static constexpr unsigned int CurrentDirectoryDirty = 1u << 11;
    static constexpr unsigned int ClipboardTextDirty = 1u << 12;
    static constexpr unsigned int CommandFinishedPending = 1u << 13;
    static constexpr unsigned int ClearPending = 1u << 14;
    static constexpr unsigned int ColorSchemeReports = 1u << 15; // ?2031
    static constexpr unsigned int Utf8Mouse = 1u << 16;          // ?1005
    static constexpr unsigned int UrxvtMouse = 1u << 17;         // ?1015
    static constexpr unsigned int SgrPixelMouse = 1u << 18;      // ?1016
    static constexpr unsigned int DarkColorScheme = 1u << 19;
    static constexpr unsigned int FocusReporting = 1u << 20; // ?1004
    static constexpr unsigned int ColorsDirty = 1u << 21;
    static constexpr unsigned int AlternateScroll = 1u << 22; // ?1007, on by default

    // DECSC state beyond the cursor position (which Screen saves): the pen
    // and the character set designations, one slot per screen.
    struct SavedTerminalState {
        Cell pen;
        std::array<char, 4> charsets{'B', 'B', 'B', 'B'};
        int glSet{0};
    };

    // DEC/ECMA-48 parser states (after Paul Williams' VT500 state machine).
    // Every sequence is consumed whole, so an unsupported one never leaks its
    // bytes as text.
    enum class State : std::uint8_t {
        Ground,
        Escape,
        EscapeIntermediate,
        Csi,
        CsiIgnore,
        Osc,   // OSC string: collected, dispatched on BEL or ST.
        Dcs,   // DCS string: collected, dispatched on ST.
        IgnoreString, // SOS / PM / APC: consumed and dropped.
        StringEscape // ESC inside a string: ST (ESC \) or a new sequence.
    };

    // Which string an ESC inside StringEscape belongs to.
    enum class StringKind : std::uint8_t {
        Osc,
        Dcs,
        Ignore
    };

    void processByte(unsigned char c);
    bool processUtf8Continuation(unsigned char c);
    void processGroundByte(unsigned char c);
    void executeControl(unsigned char c);
    void enterEscape();
    void enterString(State state, StringKind kind);
    void processEscapeByte(unsigned char c);
    void processEscapeIntermediateByte(unsigned char c);
    void processCsiByte(unsigned char c);
    void processCsiIgnoreByte(unsigned char c);
    void processStringByte(unsigned char c);
    void processStringEscapeByte(unsigned char c);
    void finishString();
    void handleEscIntermediate(char intermediate, char final);
    void handlePrivateCsi(char final);
    void handlePrefixedCsi(char final);
    void handleCsiWithIntermediates(char final);
    void handleDcs(const QByteArray &body);
    void putCodePoint(unsigned int codePoint);
    void combineWithPrevious(unsigned int mark);
    static bool isZeroWidth(unsigned int codePoint);
    void handleCsi(char final);
    void handleDec(bool set, const std::vector<int> &params);
    void handleSgr(const ParamGroups &groups);
    void handleOsc(const QByteArray &body);
    void handleOscRemoteHost(const QByteArray &body);
    void handleOscClipboard(const QByteArray &body);
    void handleOscCurrentDirectory(const QByteArray &body);
    void handleOscCommandMark(const QByteArray &body);
    void handleOscTitle(const QByteArray &body);
    void handleCommandMark(char mark);
    void handleEsc(char c);
    void cursorMove(int dRow, int dCol);
    void setExtraFlag(unsigned int flag, bool on);
    void saveCursorState();
    void restoreCursorState(bool position);
    void setAltScreenMode(int mode, bool set);
    void softReset();
    void fullReset();
    void printCodePoint(unsigned int codePoint);
    unsigned int mapCharset(unsigned char c);
    void repeatLastCharacter(int count);
    void handleAnsiMode(bool set, const std::vector<int> &params);

    // Query replies (TerminalReports.cpp).
    void reply(const QByteArray &bytes) { m_responses += bytes; }
    void handleDeviceAttributes(char prefix, int param);
    void handleDeviceStatus(int query, bool privateQuery);
    void handleWindowOps(const std::vector<int> &params);
    void reportMode(int mode, bool privateMode);
    int decModeState(int mode) const;
    void handleDecrqss(const QByteArray &request);
    void handleXtgettcap(const QByteArray &names);
    void handleKittyKeyboard(char prefix, const std::vector<int> &params);
    QByteArray sgrReport() const;
    bool handleOscColor(const QByteArray &body);
    void handleOscPalette(const QByteArray &body);
    void handleOscPaletteReset(const QByteArray &indices);
    void handleOscHyperlink(const QByteArray &body);
    unsigned int dynamicColor(int code) const;
    void setDynamicColor(int code, unsigned int color);
    void reportInBandSize();

    static ParamGroups parseParamGroups(const QByteArray &body);
    // The first value of each group, omitted -> 0 (the "default" marker).
    static std::vector<int> parseParams(const QByteArray &body);
    std::size_t applySgr(const ParamGroups &groups, std::size_t index);
    void applyBasicSgr(int code);
    void setUnderline(int style);
    void resetPen();
    static std::size_t applyExtendedColor(const ParamGroups &groups, std::size_t index, unsigned int &color);
    static int paramAt(const std::vector<int> &p, int i, int def);

    Screen m_screen;
    std::vector<CommandBlock> m_commandBlocks;
    // CSI being parsed: parameter bytes (digits, ';', ':'), intermediate bytes
    // (0x20-0x2F) and the private prefix ('?', '>', '<', '=', or 0).
    QByteArray m_csiParams;
    QByteArray m_csiIntermediates;
    // OSC / DCS string body.
    QByteArray m_stringBody;
    // Pending query replies (see takeResponses).
    QByteArray m_responses;
    // XTWINOPS 22/23 title stack.
    std::vector<QString> m_titleStack;
    // OSC 8 targets; a cell's `link` is an index + 1. Keyed for reuse by
    // the OSC 8 id parameter and URI.
    std::vector<QString> m_links;
    QHash<QString, unsigned short> m_linkIds;
    std::array<unsigned int, 256> m_paletteOverrides{};
    QString m_title;
    QString m_currentDirectory;
    QString m_remoteHost;
    QString m_remoteHostToken;
    QString m_clipboardText;
    // SGR style (and OSC 8 link) applied to subsequent characters; `ch` unused.
    Cell m_pen;
    std::array<SavedTerminalState, 2> m_savedStates{}; // [0] main, [1] alternate
    unsigned int m_lastGraphic = 0; // last printed code point, for REP (0 = none)
    unsigned int m_extraFlags = DarkColorScheme | AlternateScroll; // the default background is dark
    unsigned int m_fgOverride = 0;
    unsigned int m_bgOverride = 0;
    unsigned int m_cursorOverride = 0;
    unsigned int m_defaultFg = 0xffe0e0e0;
    unsigned int m_defaultBg = 0xff141414;

    unsigned int m_utf8CodePoint = 0;
    unsigned int m_utf8Minimum = 0;
    int m_commandStartLine = -1;
    State m_state = State::Ground;
    StringKind m_stringKind = StringKind::Ignore;

    unsigned char m_utf8Remaining = 0;
    char m_csiPrefix = 0;
    char m_escIntermediate = 0;
    // G0..G3 designations ('B' ASCII, '0' DEC special graphics, 'A' UK), the
    // set invoked into GL (SO/SI, LS2/LS3) and a pending single shift.
    std::array<char, 4> m_charsets{'B', 'B', 'B', 'B'};
    unsigned char m_glSet = 0;
    unsigned char m_singleShift = 0;
    bool m_commandRunning = false;
    unsigned short m_cellPixelWidth = 8;
    unsigned short m_cellPixelHeight = 16;
    unsigned char m_mouseModes = 0;
    // Kitty keyboard flags stack: [depth] holds the current flags, the
    // entries below it the levels CSI > u pushed (at most 8).
    std::array<unsigned char, 9> m_kittyStack{};
    unsigned char m_kittyDepth = 0;
    unsigned char m_cursorStyle = 0;
};

} // namespace konsoll
