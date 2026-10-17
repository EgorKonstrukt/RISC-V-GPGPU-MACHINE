// Frames a second, written into the caption of the window a program's frames are drawn in.
// The caption is rows of the same buffer as the window's own pixels, just above them, so this
// is a few hundred stores a second and asks the window system for nothing. Include after
// nano-X.h and call fps_caption() once a frame (docs/system/nanox.md, "Frames a second").
#ifndef FPS_CAPTION_H
#define FPS_CAPTION_H
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>

#define FPS_ABOVE 23            // rows from the caption's top to the window's first (nanowm.h: 22 and a line)
#define FPS_W 96              // the widest text, "999 fps 99.9 ms"
#define FPS_H 11
#define FPS_TOP 5               // its rows in the caption

static struct {
    int fd;                     // /dev/gpu, opened here
    uint32_t address, row;      // the window's buffer as last seen
    uint32_t* caption;          // the caption's first row above the window's first pixel, or 0
    int width;                  // of the program's window under that caption
    uint32_t since, frames;
    int left, used;             // where the text was drawn and how wide, 0: nothing drawn
    uint32_t under[FPS_H][FPS_W], drawn[FPS_H][FPS_W];   // the caption's pixels there, and ours
} fps_state = { -1 };

// 5 x 7 each: the digits, then f, p, s, m and the point
static const unsigned char fps_font[15][7] = {
    { 14, 17, 19, 21, 25, 17, 14 }, { 4, 12, 4, 4, 4, 4, 14 }, { 14, 17, 1, 2, 4, 8, 31 }, { 30, 1, 1, 14, 1, 1, 30 },
    { 2, 6, 10, 18, 31, 2, 2 }, { 31, 16, 30, 1, 1, 17, 14 }, { 6, 8, 16, 30, 17, 17, 14 }, { 31, 1, 2, 4, 8, 8, 8 },
    { 14, 17, 17, 14, 17, 17, 14 }, { 14, 17, 17, 15, 1, 2, 12 }, { 6, 9, 8, 28, 8, 8, 8 }, { 0, 0, 30, 17, 30, 16, 16 },
    { 0, 0, 15, 16, 14, 1, 30 }, { 0, 0, 26, 21, 21, 21, 21 }, { 0, 0, 0, 0, 0, 24, 24 },
};
#define FPS_GLYPH_F 10
#define FPS_GLYPH_M 13
#define FPS_GLYPH_POINT 14

static uint32_t fps_mix(uint32_t a, uint32_t b) { return 0xff000000u | (((a & 0xfefefe) + (b & 0xfefefe)) >> 1); }

// Finds the caption above a window's pixels; none for a window the window manager gave no frame.
static void fps_locate(const GR_WINDOW_INFO* info) {
    GR_WINDOW_INFO own = *info, above;
    uint32_t row = info->surface_row ? info->surface_row : (uint32_t)info->width, first, bytes;
    void* memory;
    int tries;

    fps_state.address = info->surface_address;
    fps_state.row = row;
    fps_state.caption = 0;
    if (!info->surface_address) return;
    // The window the frames go to is the program's own, in the window manager's frame, or a
    // part of that at its top left corner (a player's picture above its controls).
    for (tries = 0; ; tries++) {
        if (tries == 2 || own.parent == GR_ROOT_WINDOW_ID) return;
        GrGetWindowInfo(own.parent, &above);
        if (above.parent == GR_ROOT_WINDOW_ID) break;
        if (own.x != 0 || own.y != 0) return;
        own = above;
    }
    if (own.y < FPS_ABOVE || own.width < 200) return;
    fps_state.width = own.width;
    if (fps_state.fd < 0 && (fps_state.fd = open("/dev/gpu", O_RDWR)) < 0) return;
    first = info->surface_address - 0x86000000u - FPS_ABOVE * row * 4;
    bytes = (first & 4095) + FPS_ABOVE * row * 4;
    memory = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fps_state.fd, first & ~4095u);
    if (memory != MAP_FAILED) fps_state.caption = (uint32_t*)((char*)memory + (first & 4095));
}

// A number's glyphs at `to`: its digits, with a point before the last `decimals` of them.
static int fps_number(int* to, uint32_t v, int decimals) {
    int digits[6], n = 0, k, out = 0;
    do digits[n++] = (int)(v % 10), v /= 10; while (v || n <= decimals);
    for (k = n - 1; k >= 0; k--) {
        to[out++] = digits[k];
        if (k == decimals && decimals) to[out++] = FPS_GLYPH_POINT;
    }
    return out;
}

// A frame was shown. `now` is the machine's clock in milliseconds (the word at 0x87000034).
// Once a second the caption gets the rate and the mean frame's milliseconds, drawn on the
// caption's own pixels in its text's colour (dark on a light caption, light on a dark one).
static void fps_caption(const GR_WINDOW_INFO* info, uint32_t now) {
    uint32_t elapsed = now - fps_state.since, rate, tenths, *to, ink, shade, sum = 0;
    int x, y, k, n = 0, glyph[16], left, width, kept;

    fps_state.frames++;
    if (elapsed < 1000) return;
    rate = fps_state.frames * 1000 / elapsed;
    tenths = elapsed * 10 / fps_state.frames;
    fps_state.frames = 0;
    fps_state.since = now;
    if (info->surface_address != fps_state.address || (info->surface_row && info->surface_row != fps_state.row)) {
        fps_locate(info);
        fps_state.used = 0;
    }
    if (!fps_state.caption) return;
    n = fps_number(glyph, rate > 999 ? 999 : rate, 0);
    glyph[n++] = -1, glyph[n++] = FPS_GLYPH_F, glyph[n++] = FPS_GLYPH_F + 1, glyph[n++] = FPS_GLYPH_F + 2, glyph[n++] = -1;
    n += tenths < 1000 ? fps_number(glyph + n, tenths, 1) : fps_number(glyph + n, tenths > 9999 ? 999 : tenths / 10, 0);
    glyph[n++] = -1, glyph[n++] = FPS_GLYPH_M, glyph[n++] = FPS_GLYPH_F + 2;
    width = 0;
    for (k = 0; k < n; k++) width += glyph[k] == FPS_GLYPH_POINT ? 3 : 6;
    // right-aligned left of the caption's three boxes (18 wide each, nanowm.h)
    left = fps_state.width - 3 * 18 - 10 - width;
    if (left < fps_state.width / 2) return;
    // the caption as the window manager painted it: what is there, unless it is our last text
    kept = fps_state.used > 0;
    for (y = 0; y < FPS_H && kept; y++)
        for (x = 0, to = fps_state.caption + (FPS_TOP + y) * fps_state.row + fps_state.left; x < fps_state.used; x++)
            if (to[x] != fps_state.drawn[y][x]) { kept = 0; break; }
    for (y = 0; y < FPS_H; y++) {
        to = fps_state.caption + (FPS_TOP + y) * fps_state.row;
        if (kept)
            for (x = 0; x < fps_state.used; x++) to[fps_state.left + x] = fps_state.under[y][x];
        for (x = 0; x < width + 1; x++) fps_state.under[y][x] = to[left + x];
    }
    for (y = 0; y < FPS_H; y++)
        for (x = 0; x < width; x += 4) {
            uint32_t c = fps_state.under[y][x];
            sum += ((c >> 16 & 255) * 3 + (c >> 8 & 255) * 6 + (c & 255)) / 10;
        }
    ink = sum / (FPS_H * ((width + 3) / 4)) > 140 ? 0xff101828u : 0xfff4f8ffu;
    shade = ink == 0xff101828u ? 0xffffffffu : 0xff000000u;
    for (y = 0; y < FPS_H; y++) {
        to = fps_state.caption + (FPS_TOP + y) * fps_state.row + left;
        for (x = 0; x < width + 1; x++) to[x] = fps_state.under[y][x];
    }
    // the shadow one pixel down and right, half blended, then the letters
    for (k = 0, x = 0; k < n; x += glyph[k] == FPS_GLYPH_POINT ? 3 : 6, k++)
        for (y = 0; y < 7 && glyph[k] >= 0; y++)
            for (int i = 0; i < 5; i++)
                if (fps_font[glyph[k]][y] & (16 >> i)) {
                    to = fps_state.caption + (FPS_TOP + 3 + y) * fps_state.row + left + x + i;
                    to[fps_state.row + 1] = fps_mix(to[fps_state.row + 1], shade);
                }
    for (k = 0, x = 0; k < n; x += glyph[k] == FPS_GLYPH_POINT ? 3 : 6, k++)
        for (y = 0; y < 7 && glyph[k] >= 0; y++)
            for (int i = 0; i < 5; i++)
                if (fps_font[glyph[k]][y] & (16 >> i))
                    fps_state.caption[(FPS_TOP + 3 + y) * fps_state.row + left + x + i] = ink;
    for (y = 0; y < FPS_H; y++)
        for (x = 0, to = fps_state.caption + (FPS_TOP + y) * fps_state.row + left; x < width + 1; x++)
            fps_state.drawn[y][x] = to[x];
    fps_state.left = left, fps_state.used = width + 1;
}
#endif
