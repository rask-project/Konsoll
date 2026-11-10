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

// Key event types (kitty keyboard protocol flag 2 reports repeat/release).
namespace KeyEventType
{
inline constexpr int Press = 1;
inline constexpr int Repeat = 2;
inline constexpr int Release = 3;
}

// Terminal input modes and key origin that change the encoding.
namespace KeyMode
{
inline constexpr unsigned int ApplicationCursor = 1u << 0; // DECCKM
inline constexpr unsigned int ApplicationKeypad = 1u << 1; // DECKPAM
inline constexpr unsigned int FromKeypad = 1u << 2;        // the key is on the numeric keypad
inline constexpr unsigned int NewlineMode = 1u << 3;       // LNM: Return sends CR LF
inline constexpr unsigned int Vt52 = 1u << 4;              // VT52 mode: ESC A, ESC P, ...
inline constexpr unsigned int OtherKeys1 = 1u << 5;        // xterm modifyOtherKeys 1
inline constexpr unsigned int OtherKeys2 = 1u << 6;        // xterm modifyOtherKeys 2
inline constexpr unsigned int OtherKeysCsiU = 1u << 7;     // formatOtherKeys 1: CSI k ; m u
inline constexpr unsigned int OptionMeta = 1u << 8;        // Option is Meta: ESC + key, no composition
}

// Encodes a key event for the PTY, as xterm does: control bytes, cursor and
// function keys with modifier parameters (CSI 1;5A, CSI 15;2~) and the
// application keypad; with `kittyFlags` set, the kitty keyboard protocol
// (disambiguate 1, event types 2, alternate keys 4, all keys as escape
// codes 8, associated text 16). `key` is a Qt::Key, `text` the text the key
// produced, `eventType` a KeyEventType. Returns an empty array for an event
// with no terminal meaning (releases outside kitty flag 2, ...).
QByteArray encodeKey(int key, const QString &text, unsigned int mods, unsigned int modes, unsigned int kittyFlags,
                     int eventType = KeyEventType::Press);

} // namespace konsoll
