// Frames a second, written into the caption of the window a program's frames are drawn in.
// The caption is rows of the same buffer as the window's own pixels, just above them, so this
// is a few hundred stores a second and asks the window system for nothing. Include after
// nano-X.h and call fps_caption() once a frame (docs/nanox.md, "Frames a second").
#ifndef FPS_CAPTION_H
#define FPS_CAPTION_H
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>

#define FPS_ABOVE 23            // rows from the caption's top to the window's first (nanowm.h: 22 and a line)
#define FPS_W 46
#define FPS_H 11

static struct {
    int fd;                     // /dev/gpu, opened here
    uint32_t address, row;      // the window's buffer as last seen
    uint32_t* caption;          // the caption's first row above the window's first pixel, or 0
    int width;                  // of the program's window under that caption
    uint32_t since, frames;
} fps_state = { -1, 0, 0, 0, 0, 0, 0 };

// 5 x 7 each: the digits, then f, p and s
static const unsigned char fps_font[13][7] = {
    { 14, 17, 19, 21, 25, 17, 14 }, { 4, 12, 4, 4, 4, 4, 14 }, { 14, 17, 1, 2, 4, 8, 31 }, { 30, 1, 1, 14, 1, 1, 30 },
    { 2, 6, 10, 18, 31, 2, 2 }, { 31, 16, 30, 1, 1, 17, 14 }, { 6, 8, 16, 30, 17, 17, 14 }, { 31, 1, 2, 4, 8, 8, 8 },
    { 14, 17, 17, 14, 17, 17, 14 }, { 14, 17, 17, 15, 1, 2, 12 }, { 6, 9, 8, 28, 8, 8, 8 }, { 0, 0, 30, 17, 30, 16, 16 },
    { 0, 0, 15, 16, 14, 1, 30 },
};

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

// A frame was shown. `now` is the machine's clock in milliseconds (the word at 0x87000034).
static void fps_caption(const GR_WINDOW_INFO* info, uint32_t now) {
    uint32_t elapsed = now - fps_state.since, rate, digits[3], *to;
    int x, y, k, n = 0, glyph[7], left;

    fps_state.frames++;
    if (elapsed < 1000) return;
    rate = fps_state.frames * 1000 / elapsed;
    fps_state.frames = 0;
    fps_state.since = now;
    if (info->surface_address != fps_state.address || (info->surface_row && info->surface_row != fps_state.row)) fps_locate(info);
    if (!fps_state.caption) return;
    if (rate > 999) rate = 999;
    digits[0] = rate / 100, digits[1] = rate / 10 % 10, digits[2] = rate % 10;
    for (k = 0; k < 3; k++) glyph[n++] = k < 2 && rate < (k ? 10u : 100u) ? -1 : (int)digits[k];
    glyph[n++] = -1, glyph[n++] = 10, glyph[n++] = 11, glyph[n++] = 12;
    // left of the caption's three boxes (18 wide each, nanowm.h)
    left = fps_state.width - 3 * 18 - 10 - FPS_W;
    for (y = 0; y < FPS_H; y++) {
        to = fps_state.caption + (5 + y) * fps_state.row + left;
        for (x = 0; x < FPS_W; x++) to[x] = 0xff08205a;
        if (y < 2 || y >= 9) continue;
        for (k = 0; k < 7; k++)
            for (x = 0; x < 5 && glyph[k] >= 0; x++)
                if (fps_font[glyph[k]][y - 2] & (16 >> x)) to[2 + 6 * k + x] = 0xffffe060;
    }
}
#endif
