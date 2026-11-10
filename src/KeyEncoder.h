#pragma once

#include <QByteArray>
#include <QString>

namespace konsoll
{

// Modifier bits as xterm encodes them (the CSI parameter is 1 + mods).
namespace KeyMod
{
inline constexpr unsigned int Shift = 1u << 0;
inline constexpr unsigned int Alt = 1u << 1; // Option
inline constexpr unsigned int Control = 1u << 2;
}

// Terminal input modes and key origin that change the encoding.
namespace KeyMode
{
inline constexpr unsigned int ApplicationCursor = 1u << 0; // DECCKM
inline constexpr unsigned int ApplicationKeypad = 1u << 1; // DECKPAM
inline constexpr unsigned int FromKeypad = 1u << 2;        // the key is on the numeric keypad
inline constexpr unsigned int NewlineMode = 1u << 3;       // LNM: Return sends CR LF
inline constexpr unsigned int Vt52 = 1u << 4;              // VT52 mode: ESC A, ESC P, ...
}

// Encodes a key press for the PTY, as xterm does: control bytes, cursor and
// function keys with modifier parameters (CSI 1;5A, CSI 15;2~), the
// application keypad, and the kitty keyboard protocol's disambiguated form
// (CSI code;mods u) when `kittyFlags` has its first bit set. `key` is a
// Qt::Key, `text` the text the key produced. Returns an empty array for a
// key with no terminal meaning.
QByteArray encodeKey(int key, const QString &text, unsigned int mods, unsigned int modes, unsigned int kittyFlags);

} // namespace konsoll
