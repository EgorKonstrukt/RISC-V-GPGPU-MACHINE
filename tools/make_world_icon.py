"""The VRChat world's picture: a capture of the room with its name and what it has lettered on.

    python tools/make_world_icon.py CAPTURE.png [OUT.png]

CAPTURE is 4:3 (a camera rendered in play mode with the machine running: docs/world/world.md, "The
world's picture"). The lettering is the room's own typeface, from the Unity project's fonts.
"""
import os, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONTS = 'C:/Development/VRChat/ShaderEmu/Assets/ShaderEmu/Fonts'
TOP = 'ShaderEmu: RISC-V computer'
UNDER = 'Emulated on your GPU in shaders'
BOTTOM = ['Linux', 'Old games', 'NES', 'Browser', 'Multimedia', 'IDE']
W, H = 1200, 900
AMBER = (250, 192, 51)


def band(picture, y0, y1, upwards):
    """Darkens a strip towards the picture's edge, so that letters read on any room."""
    shade = Image.new('L', (1, y1 - y0))
    for y in range(y1 - y0):
        t = y / (y1 - y0 - 1)
        shade.putpixel((0, y), int(215 * ((1 - t) if upwards else t) ** 0.8))
    black = Image.new('RGB', (W, y1 - y0), (6, 8, 14))
    picture.paste(black, (0, y0), shade.resize((W, y1 - y0)))


def lettered(picture, text, font, centre_y, colour, parts=None):
    draw = ImageDraw.Draw(picture)
    width = draw.textlength(text, font=font)
    x = (W - width) / 2
    # a soft shadow under the letters, then the letters
    shadow = Image.new('RGBA', picture.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).text((x + 3, centre_y + 4), text, font=font, fill=(0, 0, 0, 230), anchor='lm')
    picture.paste(shadow.filter(ImageFilter.GaussianBlur(5)), (0, 0), shadow.filter(ImageFilter.GaussianBlur(5)))
    draw = ImageDraw.Draw(picture)
    if not parts:
        draw.text((x, centre_y), text, font=font, fill=colour, anchor='lm')
        return
    for part, part_colour in parts:
        draw.text((x, centre_y), part, font=font, fill=part_colour, anchor='lm')
        x += draw.textlength(part, font=font)


def fitted(path, text, size):
    """The typeface at the largest size, up to `size`, at which the text leaves a margin each side."""
    while ImageFont.truetype(path, size).getlength(text) > W - 110:
        size -= 2
    return ImageFont.truetype(path, size)


def main():
    source = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(source), 'world_icon.png')
    picture = Image.open(source).convert('RGB').resize((W, H), Image.LANCZOS)
    band(picture, 0, 290, True)
    band(picture, H - 190, H, False)
    bold = os.path.join(FONTS, 'IBMPlexSansCondensed-Bold.ttf')
    title = fitted(bold, TOP, 104)
    lettered(picture, TOP, title, 84, 'white', [('ShaderEmu', AMBER), (': RISC-V computer', (255, 255, 255))])
    lettered(picture, UNDER, ImageFont.truetype(os.path.join(FONTS, 'IBMPlexSansCondensed-Medium.ttf'), 50), 172, (232, 236, 244))
    parts = []
    for i, word in enumerate(BOTTOM):
        if i:
            parts.append(('  \u2022  ', AMBER))
        parts.append((word, (255, 255, 255)))
    lettered(picture, ''.join(p for p, _ in parts), fitted(bold, ''.join(p for p, _ in parts), 52), H - 70, 'white', parts)
    picture.save(out)
    print(out)


if __name__ == '__main__':
    main()
