#pragma once

#include <QChar>

namespace konsoll
{

// Bitmask of per-cell style flags.
enum CellFlag : unsigned char {
    None = 0,
    Bold = 1u << 0,
    Dim = 1u << 1,
    Underline = 1u << 2,
    Strike = 1u << 3,
    Reverse = 1u << 4,
    Invisible = 1u << 5,
};

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
    unsigned char flags = None;
};

} // namespace konsoll
