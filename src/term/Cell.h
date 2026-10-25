#pragma once

#include <QChar>

namespace konsoll
{

// Bitmask of per-cell style flags. Plain unsigned constants (not an enum):
// a 1-byte enum promotes its values to signed int in |, &, ~ and trips
// bugprone-signed-bitwise, while a wider enum base trips performance-enum-size.
namespace CellFlag
{
inline constexpr unsigned int None = 0;
inline constexpr unsigned int Bold = 1u << 0;
inline constexpr unsigned int Dim = 1u << 1;
inline constexpr unsigned int Underline = 1u << 2;
inline constexpr unsigned int Strike = 1u << 3;
inline constexpr unsigned int Reverse = 1u << 4;
inline constexpr unsigned int Invisible = 1u << 5;
}

// Encoded color value stored per cell:
//   0                       -> default (resolved at render time)
//   1..256                  -> 256-color palette index
//   0x80000000 | (r<<16|g<<8|b) -> truecolor (24-bit)
// Members ordered heaviest-first (4-byte colors, then the 2-byte glyph and
// 1-byte flags) so the layout is padding-free under -Wpadded.
struct Cell {
    unsigned int fg = 0;
    unsigned int bg = 0;
    QChar ch = u' ';
    unsigned char flags = CellFlag::None;
};

} // namespace konsoll
