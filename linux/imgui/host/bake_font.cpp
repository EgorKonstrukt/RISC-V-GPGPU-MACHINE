// Runs on the host, at build time: Dear ImGui builds its default font's atlas here, and this
// writes it down as font_baked.h for imgui_impl_shaderemu.cpp, so that no program rasterises
// the font when it starts (docs/apps/imgui.md). Floats are written as their bits.
#include <stdio.h>
#include <string.h>
#include "imgui.h"

static unsigned bits(float f)
{
    unsigned u;
    memcpy(&u, &f, 4);
    return u;
}

int main()
{
    ImFontAtlas atlas;
    unsigned char* pixels;
    int width, height;
    atlas.GetTexDataAsAlpha8(&pixels, &width, &height);
    const ImFont* font = atlas.Fonts[0];
    if (atlas.Fonts.Size != 1 || atlas.CustomRects.Size != 2 || sizeof(ImFontGlyph) != 40 || (width * height) % 4) return 1;

    printf("// Dear ImGui %s's default font as its atlas builds it: made by host/bake_font.cpp, not kept.\n", IMGUI_VERSION);
    printf("#define BAKED_VERSION \"%s\"\n", IMGUI_VERSION);
    printf("enum { BAKED_W = %d, BAKED_H = %d, BAKED_GLYPHS = %d, BAKED_SURFACE = %d, BAKED_CURSORS = %d, BAKED_LINES = %d };\n", width,
           height, font->Glyphs.Size, font->MetricsTotalSurface, atlas.PackIdMouseCursors, atlas.PackIdLines);
    // the two rectangles the atlas keeps for itself: width, height, x, y
    printf("static const unsigned short baked_rects[2][4] = {");
    for (const ImFontAtlasCustomRect& r : atlas.CustomRects) printf("{%d, %d, %d, %d}, ", r.Width, r.Height, r.X, r.Y);
    printf("};\n");
    // ascent, descent, the texture's scale and its white texel, then the lines' texture coordinates
    printf("static const unsigned baked_floats[6 + %d] = {\n", (int)(sizeof atlas.TexUvLines / 4));
    printf("0x%08x, 0x%08x, 0x%08x, 0x%08x, 0x%08x, 0x%08x,", bits(font->Ascent), bits(font->Descent), bits(atlas.TexUvScale.x),
           bits(atlas.TexUvScale.y), bits(atlas.TexUvWhitePixel.x), bits(atlas.TexUvWhitePixel.y));
    for (unsigned i = 0; i < sizeof atlas.TexUvLines / 4; i++) printf("%s0x%08x,", i % 8 ? " " : "\n", bits((&atlas.TexUvLines[0].x)[i]));
    printf("\n};\n");
    // the glyphs as the font has them, ten words each (ImFontGlyph)
    printf("static const unsigned baked_glyphs[BAKED_GLYPHS * 10] = {");
    for (int i = 0; i < font->Glyphs.Size * 10; i++) printf("%s0x%08x,", i % 10 ? " " : "\n", ((const unsigned*)font->Glyphs.Data)[i]);
    printf("\n};\n");
    // the picture, a byte of opacity a texel, four to a word
    printf("static const unsigned baked_alpha[BAKED_W * BAKED_H / 4] = {");
    for (int i = 0; i < width * height / 4; i++) {
        unsigned word;
        memcpy(&word, pixels + 4 * i, 4);
        if (word) printf("%s0x%08x,", i % 12 ? " " : "\n", word);
        else printf("%s0,", i % 12 ? " " : "\n");
    }
    printf("\n};\n");
    return 0;
}
