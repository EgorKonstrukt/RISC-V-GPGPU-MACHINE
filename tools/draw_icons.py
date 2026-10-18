"""Draws the icons no set has a picture for, into linux/apps/icons (64 x 64, which
tools/make_icons.py brings down): glxgears.png, the demo's three gears in its own colours,
nxeyes.png, two eyes looking the same way, and halflife.png, an orange lambda in a ring.

    python tools/draw_icons.py
"""
import math, os
from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'linux', 'apps', 'icons')
SIZE, UP = 64, 8   # drawn eight times as large, then brought down


def gear(draw, cx, cy, outer, teeth, turn, colour):
    """A gear's outline of flat-topped teeth, its darker rim, and its hole."""
    inner, points = outer * 0.74, []
    for k in range(teeth):
        a = turn + 2 * math.pi * k / teeth
        step = 2 * math.pi / teeth
        for r, f in ((inner, 0.0), (outer, 0.14), (outer, 0.40), (inner, 0.54)):
            points.append((cx + r * math.cos(a + f * step), cy + r * math.sin(a + f * step)))
    dark = tuple(c * 2 // 5 for c in colour)
    light = tuple(min(255, c + 70) for c in colour)
    draw.polygon(points, fill=colour + (255,), outline=dark + (255,))
    draw.line(points + points[:1], fill=dark + (255,), width=UP * 3 // 2, joint='curve')
    ring = inner * 0.78
    draw.ellipse((cx - ring, cy - ring, cx + ring, cy + ring), fill=light + (255,), outline=dark + (255,), width=UP)
    hole = outer * 0.24
    draw.ellipse((cx - hole, cy - hole, cx + hole, cy + hole), fill=(0, 0, 0, 0), outline=dark + (255,), width=UP)


def gears():
    picture = Image.new('RGBA', (SIZE * UP, SIZE * UP), (0, 0, 0, 0))
    draw = ImageDraw.Draw(picture)
    u = UP
    gear(draw, 23 * u, 38 * u, 21 * u, 10, 0.0, (204, 26, 26))    # the demo's red, green and blue
    gear(draw, 49 * u, 16 * u, 13 * u, 7, 0.35, (20, 160, 46))
    gear(draw, 50 * u, 49 * u, 12 * u, 7, 0.10, (40, 60, 204))
    return picture


def eyes():
    picture = Image.new('RGBA', (SIZE * UP, SIZE * UP), (0, 0, 0, 0))
    draw = ImageDraw.Draw(picture)
    u = UP
    for cx in (17, 47):
        draw.ellipse(((cx - 14) * u, 8 * u, (cx + 14) * u, 56 * u), fill=(255, 255, 255, 255), outline=(0, 0, 0, 255), width=3 * u)
        px, py = cx + 4, 38   # both look down and to the right
        draw.ellipse(((px - 6) * u, (py - 8) * u, (px + 6) * u, (py + 8) * u), fill=(0, 0, 0, 255))
        draw.ellipse(((px - 4) * u, (py - 6) * u, (px - 1) * u, (py - 2) * u), fill=(255, 255, 255, 255))
    return picture


def lambda_ring():
    picture = Image.new('RGBA', (SIZE * UP, SIZE * UP), (0, 0, 0, 0))
    draw = ImageDraw.Draw(picture)
    u, orange = UP / 16, (244, 132, 28, 255)   # (in 1,024ths of the picture)
    draw.ellipse((24 * u, 24 * u, 1000 * u, 1000 * u), fill=orange)
    draw.ellipse((100 * u, 100 * u, 924 * u, 924 * u), fill=(34, 30, 28, 255))

    def stroke(a, b, width):
        draw.line(((a[0] * u, a[1] * u), (b[0] * u, b[1] * u)), fill=orange, width=round(width * u))
        for x, y in (a, b):
            draw.ellipse(((x - width / 2) * u, (y - width / 2) * u, (x + width / 2) * u, (y + width / 2) * u), fill=orange)
    stroke((300, 262), (420, 262), 96)    # the top's hook
    stroke((420, 262), (712, 790), 112)   # the long stroke
    stroke((545, 488), (330, 790), 104)   # the short leg
    draw.line(((712 * u, 790 * u), (790 * u, 790 * u)), fill=orange, width=round(96 * u))   # the foot
    return picture


for name, picture in (('glxgears', gears()), ('nxeyes', eyes()), ('halflife', lambda_ring())):
    picture.resize((SIZE, SIZE), Image.LANCZOS).save(os.path.join(OUT, name + '.png'))
    print('linux/apps/icons/%s.png' % name)
