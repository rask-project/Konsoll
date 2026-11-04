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
    bool sgrMouse() const { return m_sgrMouse; }
    bool kittyKeyboardEnabled() const { return m_kittyKeyboardEnabled; }
    bool bracketedPasteEnabled() const { return (m_extraFlags & BracketedPasteEnabled) != 0; }
    bool applicationCursorKeys() const { return m_applicationCursorKeys; }
    bool keyboardProtocolQueryPending() const { return m_keyboardProtocolQueryPending; }
    void clearKeyboardProtocolQueryPending() { m_keyboardProtocolQueryPending = false; }
    QByteArray mouseReport(int button, int row, int col, int event, unsigned int modifiers) const;

    // OSC 10/11 foreground/background color queries from full-screen apps.
    unsigned char colorQuery() const { return m_colorQuery; }
    void clearColorQuery() { m_colorQuery = 0; }
    static QByteArray colorQueryResponse(unsigned char query);

private:
    static constexpr unsigned char RemoteHostDirty = 0x01;
    static constexpr unsigned char BracketedPasteEnabled = 0x02;

    enum class State : std::uint8_t {
        Ground,
        Escape,
        EscapeIntermediate,
        Csi,
        Osc
    };

    void processByte(unsigned char c);
    void putCodePoint(unsigned int codePoint);
    void handleCsi(char final, bool privateMode);
    void handleDec(char final, const std::vector<int> &params);
    void handleSgr(const std::vector<int> &params);
    void handleOsc(const QByteArray &body);
    void handleCommandMark(char mark);
    void handleEsc(char c);
    void cursorMove(int dRow, int dCol);

    static std::vector<int> parseParams(const QByteArray &body);
    static int paramAt(const std::vector<int> &p, int i, int def);

    Screen m_screen;
    std::vector<CommandBlock> m_commandBlocks;
    QByteArray m_csiBody;
    QByteArray m_oscBody;
    QString m_title;
    QString m_currentDirectory;
    QString m_remoteHost;
    QString m_remoteHostToken;
    QString m_clipboardText;
    unsigned int m_fg = 0;
    unsigned int m_bg = 0;
    unsigned int m_utf8CodePoint = 0;
    unsigned int m_utf8Minimum = 0;
    int m_commandStartLine = -1;
    State m_state = State::Ground;

    // SGR style state applied to subsequent characters.
    unsigned char m_flags = CellFlag::None;
    unsigned char m_utf8Remaining = 0;
    bool m_csiPrivate = false;
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
    bool m_sgrMouse = false;
    bool m_applicationCursorKeys = false;
    bool m_kittyKeyboardEnabled = false;
    bool m_keyboardProtocolQueryPending = false;
};

} // namespace konsoll
