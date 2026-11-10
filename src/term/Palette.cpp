#include "Palette.h"

#include <QColor>

#include <algorithm>
#include <array>

namespace konsoll
{

namespace
{
std::array<unsigned int, 256> buildPalette()
{
    std::array<unsigned int, 256> p{};
    p[0] = qRgb(0, 0, 0);
    p[1] = qRgb(194, 54, 33);
    p[2] = qRgb(151, 185, 15);
    p[3] = qRgb(181, 138, 4);
    // Keep ANSI blue vivid enough to remain legible over the translucent
    // window when shells use the normal (non-bold) blue for their prompt.
    p[4] = qRgb(84, 110, 255);
    p[5] = qRgb(140, 83, 194);
    p[6] = qRgb(48, 190, 225);
    p[7] = qRgb(203, 204, 205);
    p[8] = qRgb(42, 42, 42);
    p[9] = qRgb(234, 92, 53);
    p[10] = qRgb(181, 192, 55);
    p[11] = qRgb(253, 177, 44);
    p[12] = qRgb(72, 118, 255);
    p[13] = qRgb(214, 108, 255);
    p[14] = qRgb(52, 189, 244);
    p[15] = qRgb(242, 242, 242);
    int i = 0;

    for (int r = 0; r < 6; ++r) {
        for (int g = 0; g < 6; ++g) {
            for (int b = 0; b < 6; ++b) {
                p[16 + i] =
                    qRgb((r != 0) ? (r * 40) + 55 : 0, (g != 0) ? (g * 40) + 55 : 0, (b != 0) ? (b * 40) + 55 : 0);
                ++i;
            }
        }
    }

    for (int i2 = 0; i2 < 24; ++i2) {
        const int v = (i2 * 10) + 8;
        p[232 + i2] = qRgb(v, v, v);
    }

    return p;
}
} // namespace

unsigned int defaultPaletteColor(int index)
{
    static const std::array<unsigned int, 256> palette = buildPalette();
    return palette[static_cast<std::size_t>(std::clamp(index, 0, 255))];
}

} // namespace konsoll
