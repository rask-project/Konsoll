#include "CharWidth.h"

#include <algorithm>
#include <array>

namespace konsoll
{

namespace
{

struct Range {
    unsigned int first;
    unsigned int last;
};

// Sorted, non-overlapping. East_Asian_Width W/F plus Emoji_Presentation=Yes.
constexpr std::array kWideRanges{
    Range{.first = 0x1100, .last = 0x115F},   Range{.first = 0x231A, .last = 0x231B},
    Range{.first = 0x2329, .last = 0x232A},   Range{.first = 0x23E9, .last = 0x23EC},
    Range{.first = 0x23F0, .last = 0x23F0},   Range{.first = 0x23F3, .last = 0x23F3},
    Range{.first = 0x25FD, .last = 0x25FE},   Range{.first = 0x2614, .last = 0x2615},
    Range{.first = 0x2648, .last = 0x2653},   Range{.first = 0x267F, .last = 0x267F},
    Range{.first = 0x2693, .last = 0x2693},   Range{.first = 0x26A1, .last = 0x26A1},
    Range{.first = 0x26AA, .last = 0x26AB},   Range{.first = 0x26BD, .last = 0x26BE},
    Range{.first = 0x26C4, .last = 0x26C5},   Range{.first = 0x26CE, .last = 0x26CE},
    Range{.first = 0x26D4, .last = 0x26D4},   Range{.first = 0x26EA, .last = 0x26EA},
    Range{.first = 0x26F2, .last = 0x26F3},   Range{.first = 0x26F5, .last = 0x26F5},
    Range{.first = 0x26FA, .last = 0x26FA},   Range{.first = 0x26FD, .last = 0x26FD},
    Range{.first = 0x2705, .last = 0x2705},   Range{.first = 0x270A, .last = 0x270B},
    Range{.first = 0x2728, .last = 0x2728},   Range{.first = 0x274C, .last = 0x274C},
    Range{.first = 0x274E, .last = 0x274E},   Range{.first = 0x2753, .last = 0x2755},
    Range{.first = 0x2757, .last = 0x2757},   Range{.first = 0x2795, .last = 0x2797},
    Range{.first = 0x27B0, .last = 0x27B0},   Range{.first = 0x27BF, .last = 0x27BF},
    Range{.first = 0x2B1B, .last = 0x2B1C},   Range{.first = 0x2B50, .last = 0x2B50},
    Range{.first = 0x2B55, .last = 0x2B55},   Range{.first = 0x2E80, .last = 0x303E},
    Range{.first = 0x3041, .last = 0x33FF},   Range{.first = 0x3400, .last = 0x4DBF},
    Range{.first = 0x4E00, .last = 0x9FFF},   Range{.first = 0xA000, .last = 0xA4CF},
    Range{.first = 0xA960, .last = 0xA97F},   Range{.first = 0xAC00, .last = 0xD7A3},
    Range{.first = 0xF900, .last = 0xFAFF},   Range{.first = 0xFE10, .last = 0xFE19},
    Range{.first = 0xFE30, .last = 0xFE6F},   Range{.first = 0xFF00, .last = 0xFF60},
    Range{.first = 0xFFE0, .last = 0xFFE6},   Range{.first = 0x16FE0, .last = 0x16FE4},
    Range{.first = 0x16FF0, .last = 0x16FF1}, Range{.first = 0x17000, .last = 0x18CD5},
    Range{.first = 0x18D00, .last = 0x18D08}, Range{.first = 0x1AFF0, .last = 0x1B2FB},
    Range{.first = 0x1F004, .last = 0x1F004}, Range{.first = 0x1F0CF, .last = 0x1F0CF},
    Range{.first = 0x1F18E, .last = 0x1F18E}, Range{.first = 0x1F191, .last = 0x1F19A},
    Range{.first = 0x1F200, .last = 0x1F202}, Range{.first = 0x1F210, .last = 0x1F23B},
    Range{.first = 0x1F240, .last = 0x1F248}, Range{.first = 0x1F250, .last = 0x1F251},
    Range{.first = 0x1F260, .last = 0x1F265}, Range{.first = 0x1F300, .last = 0x1F320},
    Range{.first = 0x1F32D, .last = 0x1F335}, Range{.first = 0x1F337, .last = 0x1F37C},
    Range{.first = 0x1F37E, .last = 0x1F393}, Range{.first = 0x1F3A0, .last = 0x1F3CA},
    Range{.first = 0x1F3CF, .last = 0x1F3D3}, Range{.first = 0x1F3E0, .last = 0x1F3F0},
    Range{.first = 0x1F3F4, .last = 0x1F3F4}, Range{.first = 0x1F3F8, .last = 0x1F43E},
    Range{.first = 0x1F440, .last = 0x1F440}, Range{.first = 0x1F442, .last = 0x1F4FC},
    Range{.first = 0x1F4FF, .last = 0x1F53D}, Range{.first = 0x1F54B, .last = 0x1F54E},
    Range{.first = 0x1F550, .last = 0x1F567}, Range{.first = 0x1F57A, .last = 0x1F57A},
    Range{.first = 0x1F595, .last = 0x1F596}, Range{.first = 0x1F5A4, .last = 0x1F5A4},
    Range{.first = 0x1F5FB, .last = 0x1F64F}, Range{.first = 0x1F680, .last = 0x1F6C5},
    Range{.first = 0x1F6CC, .last = 0x1F6CC}, Range{.first = 0x1F6D0, .last = 0x1F6D2},
    Range{.first = 0x1F6D5, .last = 0x1F6D7}, Range{.first = 0x1F6DC, .last = 0x1F6DF},
    Range{.first = 0x1F6EB, .last = 0x1F6EC}, Range{.first = 0x1F6F4, .last = 0x1F6FC},
    Range{.first = 0x1F7E0, .last = 0x1F7EB}, Range{.first = 0x1F7F0, .last = 0x1F7F0},
    Range{.first = 0x1F90C, .last = 0x1F93A}, Range{.first = 0x1F93C, .last = 0x1F945},
    Range{.first = 0x1F947, .last = 0x1F9FF}, Range{.first = 0x1FA70, .last = 0x1FAFF},
    Range{.first = 0x20000, .last = 0x2FFFD}, Range{.first = 0x30000, .last = 0x3FFFD},
};

} // namespace

bool isWideChar(unsigned int codePoint)
{
    if (codePoint < kWideRanges.front().first) {
        return false;
    }
    // First range whose end is >= codePoint; wide if it also starts <= it.
    const auto range = std::ranges::lower_bound(kWideRanges, codePoint, {}, &Range::last);
    return range != kWideRanges.end() && range->first <= codePoint;
}

} // namespace konsoll
