#pragma once

#include <QByteArray>

namespace konsoll
{

// Character sets a G0..G3 slot can hold. ISO 2022 designation finals where
// they are unambiguous, internal ids otherwise.
namespace Charset
{
inline constexpr char Ascii = 'B';
inline constexpr char DecSpecialGraphics = '0';
inline constexpr char British = 'A';
inline constexpr char Latin1 = 'L';          // 96-character ESC - A / . A / / A
inline constexpr char DecSupplemental = 'S'; // ESC ( % 5, ESC ( <
inline constexpr char Portuguese = 'P';      // ESC ( % 6
} // namespace Charset

// The set an SCS designation selects: `intermediates` are the ESC
// intermediates ("(" G0 ... "+" G3 for 94-character sets, "-" G1 ... "/" G3
// for 96-character ones, plus "%" for two-byte finals). Unknown sets map to
// ASCII. `slot` receives the G0..G3 index, or -1 when the sequence is not a
// designation.
char designatedCharset(const QByteArray &intermediates, char final, int &slot);

// Maps a printable 7-bit byte through `charset`. National replacement sets
// apply only when `nrcMode` (DECNRCM) is on, as on a VT220.
unsigned int mapCharsetByte(char charset, unsigned char c, bool nrcMode);

} // namespace konsoll
