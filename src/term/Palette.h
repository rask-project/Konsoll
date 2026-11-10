#pragma once

namespace konsoll
{

// The default 256-color palette (0xAARRGGBB, opaque): Konsoll's 16 ANSI
// colors, the 6x6x6 color cube and the 24-step gray ramp. `index` is 0..255.
unsigned int defaultPaletteColor(int index);

} // namespace konsoll
