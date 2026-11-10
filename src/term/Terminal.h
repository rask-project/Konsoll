#pragma once

#include "Screen.h"

#include <QByteArray>
#include <QString>

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
    bool titleDirty() const { return m_titleDirty; }
    void clearTitleDirty() { m_titleDirty = false; }
    QString currentDirectory() const { return m_currentDirectory; }
    QString remoteHost() const { return m_remoteHost; }
    bool remoteHostDirty() const { return (m_extraFlags & RemoteHostDirty) != 0; }
    void clearRemoteHostDirty() { m_extraFlags &= static_cast<unsigned char>(~RemoteHostDirty); }
    bool currentDirectoryDirty() const { return m_currentDirectoryDirty; }
    void clearCurrentDirectoryDirty() { m_currentDirectoryDirty = false; }
    QString clipboardText() const { return m_clipboardText; }
    bool clipboardTextDirty() const { return m_clipboardTextDirty; }
    void clearClipboardTextDirty() { m_clipboardTextDirty = false; }

    // Device-attributes reply (so apps like vim/less don't hang waiting).
    bool daPending() const { return m_daPending; }
    void clearDaPending() { m_daPending = false; }
    static QByteArray daResponse();
    int dsrQuery() const { return m_dsrQuery; }
    void clearDsrQuery() { m_dsrQuery = 0; }
    QByteArray dsrResponse() const;

    const std::vector<CommandBlock> &commandBlocks() const { return m_commandBlocks; }
    int activeCommandStartLine() const { return m_commandStartLine; }
    void clearCommandBlocks();
    bool commandFinished() const { return m_commandFinished; }
    void clearCommandFinished() { m_commandFinished = false; }
    // True while a command is executing (OSC 133 C -> D window).
    bool commandRunning() const { return m_commandRunning; }

    // Set when a full-screen erase (ESC[2J) is processed, so the backend can
    // drop any selection that now sits over blank lines.
    bool clearPending() const { return m_clearPending; }
    void clearClearPending() { m_clearPending = false; }

    bool mouseTracking() const { return m_mouseModes != 0; }
    bool mouseMotionTracking() const { return (m_mouseModes & 0x06u) != 0; }
    bool sgrMouse() const { return (m_extraFlags & SgrMouse) != 0; }
    bool kittyKeyboardEnabled() const { return (m_extraFlags & KittyKeyboardEnabled) != 0; }
    bool bracketedPasteEnabled() const { return (m_extraFlags & BracketedPasteEnabled) != 0; }
    bool applicationCursorKeys() const { return (m_extraFlags & ApplicationCursorKeys) != 0; }
    bool keyboardProtocolQueryPending() const { return (m_extraFlags & KeyboardProtocolQueryPending) != 0; }
    void clearKeyboardProtocolQueryPending() { setExtraFlag(KeyboardProtocolQueryPending, false); }
    bool synchronizedUpdates() const { return (m_extraFlags & SynchronizedUpdates) != 0; }
    QByteArray mouseReport(int button, int row, int col, int event, unsigned int modifiers) const;

    // OSC 10/11 foreground/background color queries from full-screen apps.
    unsigned char colorQuery() const { return m_colorQuery; }
    void clearColorQuery() { m_colorQuery = 0; }
    static QByteArray colorQueryResponse(unsigned char query);

private:
    // CSI parameters split on ';' into groups and on ':' into sub-parameters
    // (SGR 4:3, 38:2::r:g:b). An omitted value is -1.
    using ParamGroups = std::vector<std::vector<int>>;

    static constexpr unsigned char RemoteHostDirty = 0x01;
    static constexpr unsigned char BracketedPasteEnabled = 0x02;
    static constexpr unsigned char SynchronizedUpdates = 0x04;
    static constexpr unsigned char KittyKeyboardEnabled = 0x08;
    static constexpr unsigned char SgrMouse = 0x10;
    static constexpr unsigned char ApplicationCursorKeys = 0x20;
    static constexpr unsigned char KeyboardProtocolQueryPending = 0x40;

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
    void handleOscColorQuery(const QByteArray &body);
    void handleCommandMark(char mark);
    void handleEsc(char c);
    void cursorMove(int dRow, int dCol);
    void setExtraFlag(unsigned char flag, bool on);

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
    QString m_title;
    QString m_currentDirectory;
    QString m_remoteHost;
    QString m_remoteHostToken;
    QString m_clipboardText;
    // SGR style (and OSC 8 link) applied to subsequent characters; `ch` unused.
    Cell m_pen;
    unsigned int m_utf8CodePoint = 0;
    unsigned int m_utf8Minimum = 0;
    int m_commandStartLine = -1;
    State m_state = State::Ground;
    StringKind m_stringKind = StringKind::Ignore;

    unsigned char m_utf8Remaining = 0;
    char m_csiPrefix = 0;
    char m_escIntermediate = 0;
    bool m_titleDirty = false;
    bool m_currentDirectoryDirty = false;
    bool m_clipboardTextDirty = false;
    bool m_daPending = false;
    bool m_clearPending = false;
    bool m_commandFinished = false;
    bool m_commandRunning = false;
    unsigned char m_dsrQuery = 0;
    unsigned char m_colorQuery = 0;
    unsigned char m_extraFlags = 0;
    unsigned char m_mouseModes = 0;
};

} // namespace konsoll
