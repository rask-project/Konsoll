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
    explicit Terminal(int rows, int cols);

    void write(const QByteArray &data);
    void resize(int rows, int cols);

    Screen &screen() { return m_screen; }
    const Screen &screen() const { return m_screen; }

    // OSC window title.
    QString title() const { return m_title; }
    bool titleDirty() const { return m_titleDirty; }
    void clearTitleDirty() { m_titleDirty = false; }

    // Device-attributes reply (so apps like vim/less don't hang waiting).
    bool daPending() const { return m_daPending; }
    void clearDaPending() { m_daPending = false; }
    static QByteArray daResponse();

private:
    enum class State : std::uint8_t {
        Ground,
        Escape,
        Csi,
        Osc
    };

    void processByte(unsigned char c);
    void putCodePoint(unsigned int codePoint);
    void handleCsi(char final, bool privateMode);
    void handleDec(char final, const std::vector<int> &params);
    void handleSgr(const std::vector<int> &params);
    void handleOsc(const QByteArray &body);
    void handleEsc(char c);
    void cursorMove(int dRow, int dCol);

    static std::vector<int> parseParams(const QByteArray &body);
    static int paramAt(const std::vector<int> &p, int i, int def);

    Screen m_screen;
    QByteArray m_csiBody;
    QByteArray m_oscBody;
    QString m_title;
    unsigned int m_fg = 0;
    unsigned int m_bg = 0;
    unsigned int m_utf8CodePoint = 0;
    unsigned int m_utf8Minimum = 0;
    State m_state = State::Ground;

    // SGR style state applied to subsequent characters.
    unsigned char m_flags = CellFlag::None;
    unsigned char m_utf8Remaining = 0;
    bool m_csiPrivate = false;
    bool m_titleDirty = false;
    bool m_daPending = false;
};

} // namespace konsoll
