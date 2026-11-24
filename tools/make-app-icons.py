#!/usr/bin/env python3
# Draws the layers of the app icons (assets/konsoll.icon and
# assets/konsoll-nvim.icon, compiled by cmake/AppIcons.cmake) after the
# original icon: a silver ring (the system's rounded square), a dark border
# and the terminal area with chamfered top-left and bottom-right corners,
# and the ">_" prompt or the Neovim "N" (assets/neovim-mark.svg).
#
#   python3 tools/make-app-icons.py
#
# icon.json stacks them: glyph, then area over border, then the silver ring.
import os, re
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The original icon (assets/konsoll.png, 1254 px), measured: its dark border
# spans x 85-1170, y 197-1013 with 160 px chamfers and 35 px corners; the
# terminal area is inset 42 px, with 120 px chamfers. Everything is scaled
# from it so that the border is WIDTH wide on the 1024 canvas, and HEIGHT
# stretches it vertically (1 keeps the original's proportions).
WIDTH = float(os.environ.get("ICON_WIDTH", 840))
HEIGHT = float(os.environ.get("ICON_HEIGHT", 1))
SCALE = WIDTH / 1085
ORIGIN_X = (1024 - WIDTH) / 2
ORIGIN_Y = (1024 - 816 * SCALE * HEIGHT) / 2

def point(x, y):
    """A point of the original icon on the 1024 canvas."""
    return ORIGIN_X + (x - 85) * SCALE, ORIGIN_Y + (y - 197) * SCALE * HEIGHT

def screen(left, top, right, bottom, chamfer, radius):
    """The original's screen shape: rounded top-right and bottom-left corners,
    chamfered top-left and bottom-right ones (in original pixels)."""
    x0, y0 = point(left, top)
    x1, y1 = point(right, bottom)
    c, r = chamfer * SCALE, radius * SCALE
    return (f"M{x0 + c:.1f} {y0:.1f} L{x1 - r:.1f} {y0:.1f} A{r:.1f} {r:.1f} 0 0 1 {x1:.1f} {y0 + r:.1f} "
            f"L{x1:.1f} {y1 - c:.1f} L{x1 - c:.1f} {y1:.1f} L{x0 + r:.1f} {y1:.1f} "
            f"A{r:.1f} {r:.1f} 0 0 1 {x0:.1f} {y1 - r:.1f} L{x0:.1f} {y0 + c:.1f} Z")

def polygon(points):
    return "M" + " L".join("{:.1f} {:.1f}".format(*point(x, y)) for x, y in points) + " Z"

def layer(body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" '
            f'viewBox="0 0 1024 1024">\n{body}\n</svg>\n')

# Silver ring: the whole canvas (the system masks it to its rounded square).
silver = layer('''  <defs><linearGradient id="s" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#DCE8EF"/><stop offset="1" stop-color="#9BA1A8"/>
  </linearGradient></defs>
  <rect width="1024" height="1024" fill="url(#s)"/>''')
border = layer(f'''  <path d="{screen(85, 197, 1170, 1013, 160, 35)}" fill="#141416"/>''')
area = layer(f'''  <defs><linearGradient id="a" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#2B2C30"/><stop offset="1" stop-color="#232427"/>
  </linearGradient></defs>
  <path d="{screen(127, 240, 1130, 975, 120, 30)}" fill="url(#a)"/>''')
# The original's prompt: ">" x 250-408, y 340-550; "_" x 422-594, y 515-552.
chevron = polygon([(250, 340), (408, 430), (408, 462), (250, 550), (250, 492), (352, 446), (250, 398)])
underscore = polygon([(422, 515), (594, 515), (594, 552), (422, 552)])
prompt = layer(f'''  <g fill="#FFFFFF">
    <path d="{chevron}"/>
    <path d="{underscore}"/>
  </g>''')
src = open(f"{root}/assets/neovim-mark.svg").read()
inner = re.search(r'(<defs>.*</g>)', src, re.S).group(1)
nx, ny = point(250, 340)
_, nbottom = point(250, 552)
s = (nbottom - ny) * 1.15 / 214
neovim = layer(f'''  <g transform="translate({nx:.1f} {ny - (nbottom - ny) * 0.075:.1f}) scale({s:.4f})">
  {inner}
  </g>''')

for name, glyph, glyph_svg in [("konsoll", "prompt", prompt), ("konsoll-nvim", "neovim", neovim)]:
    d = f"{root}/assets/{name}.icon/Assets"
    for f, text in [("silver.svg", silver), ("border.svg", border), ("area.svg", area), (f"{glyph}.svg", glyph_svg)]:
        open(f"{d}/{f}", "w").write(text)
