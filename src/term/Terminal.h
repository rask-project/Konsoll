#pragma once

#include "Graphics.h"
#include "Screen.h"

#include <QByteArray>
#include <QHash>
#include <QString>

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace konsoll
{

// The Qt::CursorShape for an OSC 22 pointer name (CSS or X cursor font), or
// -1 when unknown.
int pointerCursorShape(const QByteArray &name);

// ANSI/VT100/xterm parser that drives a Screen. Consumes raw PTY bytes and
// maintains the SGR style state, OSC title and DA-reply bookkeeping.
// A request the terminal cannot answer itself: it needs the clipboard, the
// window or the user's permission. The session (TerminalBackend) decides.
struct TerminalRequest {
    enum Kind : std::uint8_t {
        ClipboardRead, // OSC 52 ; selection ; ?   (data = selection)
        TitleReport,   // XTWINOPS 21 (a = 0) / 20 icon label (a = 1)
        WindowOp,      // XTWINOPS 1-10: a = operation, b / c = its parameters
        Notification,  // OSC 9 / OSC 777 notify: data = title (may be empty), body = text
        ColumnMode     // DECCOLM with ?40: a = the new column count (80 / 132)
    };
    QByteArray data;
    QByteArray body;
    int kind{ClipboardRead};
    int a{0};
    int b{0};
    int c{0};
};

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
    // Images (kitty graphics protocol, sixel) and where they are placed.
    const GraphicsStore &graphics() const { return m_graphics; }
    // Kitty image animations: whether one plays, and advancing them.
    bool animating() const { return m_graphics.animating(); }
    bool advanceAnimations(int ms) { return m_graphics.advanceAnimations(ms); }

    // OSC window title.
    QString title() const { return m_title; }
    bool titleDirty() const { return (m_extraFlags & TitleDirty) != 0; }
    void clearTitleDirty() { setExtraFlag(TitleDirty, false); }
    QString currentDirectory() const { return m_currentDirectory; }
    QString remoteHost() const { return m_remoteHost; }
    // OSC 633 ; E command line (VS Code shell integration).
    QString commandLine() const { return m_commandLine; }
    // DECUDK: the string defined for Shift+F`number` (1-20), or empty.
    QByteArray userDefinedKey(int number) const;
    // OSC 22: the pointer shape the program asked for (empty = default).
    QByteArray pointerShape() const { return m_pointerShapes.isEmpty() ? QByteArray{} : m_pointerShapes.back(); }
    bool pointerShapeDirty() const { return (m_extraFlags & PointerShapeDirty) != 0; }
    void clearPointerShapeDirty() { setExtraFlag(PointerShapeDirty, false); }
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
    // Pending requests needing the session (see TerminalRequest).
    std::vector<TerminalRequest> takeRequests() { return std::exchange(m_requests, {}); }
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
    // iTerm2 SetUserVar=name=base64 values (decoded).
    QByteArray userVar(const QByteArray &name) const { return m_userVars.value(name); }

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
    bool newlineMode() const { return (m_extraFlags & NewlineMode) != 0; }
    bool vt52Mode() const { return (m_extraFlags & Vt52Mode) != 0; }
    // ?1036 metaSendsEscape / ?1039 altSendsEscape: Option prefixes ESC.
    bool metaSendsEscape() const { return (m_extraFlags & MetaSendsEscape) != 0; }
    // xterm modifyOtherKeys level (0-2) and whether keys use CSI code ; mods u.
    int modifyOtherKeys() const { return static_cast<int>((m_extraFlags & OtherKeysMask) >> OtherKeysShift); }
    bool otherKeysCsiU() const { return (m_extraFlags & OtherKeysCsiU) != 0; }
    QByteArray mouseReport(int button, int row, int col, int event, unsigned int modifiers);


private:
    // CSI parameters split on ';' into groups and on ':' into sub-parameters
    // (SGR 4:3, 38:2::r:g:b). An omitted value is -1.
    using ParamGroups = std::vector<std::vector<int>>;

    static constexpr std::uint64_t RemoteHostDirty = std::uint64_t{1} << 0;
    static constexpr std::uint64_t BracketedPasteEnabled = std::uint64_t{1} << 1;
    static constexpr std::uint64_t SynchronizedUpdates = std::uint64_t{1} << 2;
    static constexpr std::uint64_t CursorBlinkMode = std::uint64_t{1} << 3; // ?12
    static constexpr std::uint64_t SgrMouse = std::uint64_t{1} << 4;
    static constexpr std::uint64_t ApplicationCursorKeys = std::uint64_t{1} << 5;
    static constexpr std::uint64_t InBandResize = std::uint64_t{1} << 6; // ?2048
    static constexpr std::uint64_t NewlineMode = std::uint64_t{1} << 7;  // LNM: LF also returns
    static constexpr std::uint64_t ReverseVideo = std::uint64_t{1} << 8; // DECSCNM
    static constexpr std::uint64_t ApplicationKeypad = std::uint64_t{1} << 9; // DECKPAM
    static constexpr std::uint64_t TitleDirty = std::uint64_t{1} << 10;
    static constexpr std::uint64_t CurrentDirectoryDirty = std::uint64_t{1} << 11;
    static constexpr std::uint64_t ClipboardTextDirty = std::uint64_t{1} << 12;
    static constexpr std::uint64_t CommandFinishedPending = std::uint64_t{1} << 13;
    static constexpr std::uint64_t ClearPending = std::uint64_t{1} << 14;
    static constexpr std::uint64_t ColorSchemeReports = std::uint64_t{1} << 15; // ?2031
    static constexpr std::uint64_t Utf8Mouse = std::uint64_t{1} << 16;          // ?1005
    static constexpr std::uint64_t UrxvtMouse = std::uint64_t{1} << 17;         // ?1015
    static constexpr std::uint64_t SgrPixelMouse = std::uint64_t{1} << 18;      // ?1016
    static constexpr std::uint64_t DarkColorScheme = std::uint64_t{1} << 19;
    static constexpr std::uint64_t FocusReporting = std::uint64_t{1} << 20; // ?1004
    static constexpr std::uint64_t ColorsDirty = std::uint64_t{1} << 21;
    static constexpr std::uint64_t AlternateScroll = std::uint64_t{1} << 22; // ?1007, on by default
    static constexpr std::uint64_t NoClearOnColumnChange = std::uint64_t{1} << 23; // ?95
    static constexpr std::uint64_t NrcMode = std::uint64_t{1} << 24;               // ?42
    static constexpr std::uint64_t Vt52Mode = std::uint64_t{1} << 25;              // ?2 reset (DECANM)
    static constexpr std::uint64_t ErasureMode = std::uint64_t{1} << 26;           // ERM (6): erase protected cells too
    static constexpr std::uint64_t SixelDisplayMode = std::uint64_t{1} << 27;      // ?80 DECSDM: no sixel scrolling
    static constexpr std::uint64_t RectangleExtent = std::uint64_t{1} << 28;       // DECSACE 2: rectangle, else stream
    static constexpr unsigned int OtherKeysShift = 29u;             // XTMODKEYS modifyOtherKeys 0-2 (2 bits)
    static constexpr std::uint64_t OtherKeysMask = std::uint64_t{3} << OtherKeysShift;
    static constexpr std::uint64_t OtherKeysCsiU = std::uint64_t{1} << 31;         // XTFMTKEYS formatOtherKeys 1
    static constexpr std::uint64_t MetaSendsEscape = std::uint64_t{1} << 32;       // ?1036 / ?1039
    static constexpr std::uint64_t HighlightStarted = std::uint64_t{1} << 33;      // ?1001 highlight in progress
    static constexpr std::uint64_t PointerShapeDirty = std::uint64_t{1} << 34;     // OSC 22 changed the pointer
    static constexpr std::uint64_t Allow132Columns = std::uint64_t{1} << 35;       // ?40: DECCOLM may resize
    static constexpr std::uint64_t Columns132 = std::uint64_t{1} << 36;            // ?3 DECCOLM set
    static constexpr std::uint64_t EightBitControls = std::uint64_t{1} << 37;      // S8C1T: C1 replies
    static constexpr std::uint64_t UdkLocked = std::uint64_t{1} << 38;             // DECUDK locked

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
        StringEscape, // ESC inside a string: ST (ESC \) or a new sequence.
        Vt52Row,      // VT52 ESC Y: row byte next
        Vt52Column    // VT52 ESC Y row: column byte next
    };

    // Which string an ESC inside StringEscape belongs to.
    enum class StringKind : std::uint8_t {
        Osc,
        Dcs,
        Apc,
        Ignore
    };

    void processByte(unsigned char c);
    bool processUtf8Continuation(unsigned char c);
    void processGroundByte(unsigned char c);
    void executeControl(unsigned char c);
    void enterEscape();
    void enterString(State state, StringKind kind);
    void processEscapeByte(unsigned char c);
    void handleVt52Escape(unsigned char c);
    void processVt52AddressByte(unsigned char c);
    void processEscapeIntermediateByte(unsigned char c);
    void processCsiByte(unsigned char c);
    void processCsiIgnoreByte(unsigned char c);
    void processStringByte(unsigned char c);
    void processStringEscapeByte(unsigned char c);
    void finishString();
    void handleEscIntermediate(const QByteArray &intermediates, char final);
    void handlePrivateCsi(char final);
    void handlePrefixedCsi(char final);
    void handleCsiWithIntermediates(char final);
    void handleRectangle(char intermediate, char final);
    Screen::Rect rectangleParams(const std::vector<int> &params, int first) const;
    void changeRectangleAttributes(const std::vector<int> &params, bool reverse);
    void handleDcs(const QByteArray &body);
    void handleApc(const QByteArray &body);
    void handleSixel(const QByteArray &body);
    void handleGraphicsAttributes(const std::vector<int> &params);
    GraphicsContext graphicsContext() const;
    void eraseImages(int mode);
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
    void handleOscVsCode(const QByteArray &body);
    void handleOscPointerShape(const QByteArray &body);
    QByteArray pointerShapeQuery(const QByteArray &names) const;
    void handleOscTitle(const QByteArray &body);
    void handleCommandMark(char mark);
    void handleEsc(char c);
    void cursorMove(int dRow, int dCol);
    void setExtraFlag(std::uint64_t flag, bool on);
    void saveCursorState();
    void restoreCursorState(bool position);
    void setAltScreenMode(int mode, bool set);
    void softReset();
    void fullReset();
    void printCodePoint(unsigned int codePoint);
    bool extendPreviousCluster(unsigned int codePoint);
    bool followsImagePlaceholder() const;
    unsigned int mapCharset(unsigned char c);
    void repeatLastCharacter(int count);
    void handleAnsiMode(bool set, const std::vector<int> &params);
    unsigned int isoProtection() const;

    // Query replies (TerminalReports.cpp).
    void reply(const QByteArray &bytes);
    void setColumnMode(bool wide);
    void handleUserDefinedKeys(const QByteArray &body);
    void handleDeviceAttributes(char prefix, int param);
    void handleDeviceStatus(int query, bool privateQuery);
    void handleWindowOps(const std::vector<int> &params);
    void reportMode(int mode, bool privateMode);
    int decModeState(int mode) const;
    void handleDecrqss(const QByteArray &request);
    void handleXtgettcap(const QByteArray &names);
    void handleKittyKeyboard(char prefix, const std::vector<int> &params);
    void handleKeyModifierOptions(char final, const std::vector<int> &params);
    QByteArray sgrReport() const;
    bool handleOscColor(const QByteArray &body);
    void handleOscPalette(const QByteArray &body);
    void handleOscPaletteReset(const QByteArray &indices);
    void handleOscHyperlink(const QByteArray &body);
    void handleOscNotification(const QByteArray &body);
    void handleOscIterm(const QByteArray &command);
    void placeItermImage(const QByteArray &args, const QByteArray &base64);
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
    GraphicsStore m_graphics;
    std::vector<CommandBlock> m_commandBlocks;
    // CSI being parsed: parameter bytes (digits, ';', ':'), intermediate bytes
    // (0x20-0x2F; also an ESC sequence's) and the private prefix ('?', '>',
    // '<', '=', or 0).
    QByteArray m_csiParams;
    QByteArray m_csiIntermediates;
    // OSC / DCS string body.
    QByteArray m_stringBody;
    // Pending query replies (see takeResponses).
    QByteArray m_responses;
    // iTerm2 MultipartFile in progress (File= arguments and data so far)
    // and SetUserVar variables.
    QByteArray m_itermFileArgs;
    QByteArray m_itermFileData;
    QHash<QByteArray, QByteArray> m_userVars;
    // XTWINOPS 22/23 title stack.
    std::vector<QString> m_titleStack;
    // DECUDK strings for F1-F20 (index 0 = F1), sent on Shift+Fn.
    std::array<QByteArray, 20> m_userKeys;
    std::vector<TerminalRequest> m_requests;
    // OSC 8 targets; a cell's `link` is an index + 1. Keyed for reuse by
    // the OSC 8 id parameter and URI.
    std::vector<QString> m_links;
    QHash<QString, unsigned short> m_linkIds;
    std::uint64_t m_extraFlags = DarkColorScheme | AlternateScroll; // the default background is dark
    std::array<unsigned int, 256> m_paletteOverrides{};
    QString m_title;
    QString m_currentDirectory;
    QString m_remoteHost;
    // OSC 633 ; E: the command line of the running command.
    QString m_commandLine;
    // OSC 22 pointer shape stack (CSS / X cursor names); the top is shown.
    QList<QByteArray> m_pointerShapes;
    QString m_remoteHostToken;
    QString m_clipboardText;
    // SGR style (and OSC 8 link) applied to subsequent characters; `ch` unused.
    Cell m_pen;
    std::array<SavedTerminalState, 2> m_savedStates{}; // [0] main, [1] alternate
    unsigned int m_lastGraphic = 0; // last printed code point, for REP (0 = none)
    unsigned int m_fgOverride = 0;
    unsigned int m_bgOverride = 0;
    unsigned int m_cursorOverride = 0;
    unsigned int m_defaultFg = 0xffe0e0e0;
    unsigned int m_defaultBg = 0xff141414;

    unsigned int m_utf8CodePoint = 0;
    unsigned int m_utf8Minimum = 0;
    int m_commandStartLine = -1;
    int m_cellPixelWidth = 8;
    int m_cellPixelHeight = 16;
    State m_state = State::Ground;
    StringKind m_stringKind = StringKind::Ignore;

    unsigned char m_utf8Remaining = 0;
    char m_csiPrefix = 0;
    // G0..G3 designations ('B' ASCII, '0' DEC special graphics, 'A' UK), the
    // set invoked into GL (SO/SI, LS2/LS3) and a pending single shift.
    std::array<char, 4> m_charsets{'B', 'B', 'B', 'B'};
    unsigned char m_glSet = 0;
    unsigned char m_singleShift = 0;
    bool m_commandRunning = false;
    unsigned char m_mouseModes = 0;
    // Kitty keyboard flags stack: [depth] holds the current flags, the
    // entries below it the levels CSI > u pushed (at most 9).
    std::array<unsigned char, 10> m_kittyStack{};
    unsigned char m_kittyDepth = 0;
    unsigned char m_cursorStyle = 0;
};

} // namespace konsoll
