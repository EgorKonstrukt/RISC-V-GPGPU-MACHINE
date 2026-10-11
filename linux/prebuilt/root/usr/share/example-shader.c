/*
 * A "shader" on the processors: shade() is called for every pixel of the picture, as a
 * fragment shader is, and the machine's 255 worker cores share the picture between them.
 * Change shade() and Run again. Keys: q leaves.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#define MWINCLUDECOLORS
#include <nano-X.h>
#include <GLES/segl.h>
#include <mcw.h>

#define WIDTH 160
#define HEIGHT 120
#define SCALE 3
#define PIECE 80	/* pixels a piece: 320 bytes, whole 16s, so no two cores write one piece of memory */
#define PIECES (WIDTH * HEIGHT / PIECE)
#define ONE 65536

static uint32_t *pixels;	/* the picture, in memory the GPU draws from */
static float now;		/* seconds: the first core sets it between two pictures */
static int workers;

/* A wave from -1 to 1 that comes round once for every 1 of v. Not sinf(): the C library works
 * a sine out in doubles, which this machine has no instructions for (a double is a call of
 * hundreds of instructions each time), and a picture took seconds. This is a few floats. */
static float wave(float v)
{
    float p = 2.0f * (v - (float)(int)v) - 1.0f;

    if (p < -1.0f)
        p += 2.0f;
    return 4.0f * p * (1.0f - (p < 0.0f ? -p : p));
}

/* A pixel's colour: x and y from -1 to 1 (y up), t in seconds. Red, green and blue from 0 to 1. */
static void shade(float x, float y, float t, float *r, float *g, float *b)
{
    float far = sqrtf(x * x + y * y);
    float rings = wave(1.6f * far - 0.5f * t), bend = wave(0.7f * x + 0.2f * t) * wave(0.9f * y - 0.15f * t);
    float glow = 0.5f + 0.5f * rings * wave(far + 0.6f * bend + 0.1f * t);

    *r = glow * (0.6f + 0.4f * wave(0.16f * t + far * 0.5f));
    *g = glow * (0.5f + 0.5f * bend);
    *b = 0.25f + 0.75f * (1.0f - far * 0.6f) * glow;
}

static unsigned milliseconds(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned)t.tv_sec * 1000u + (unsigned)t.tv_nsec / 1000000u;
}

static uint32_t channel(float v)
{
    return v <= 0.0f ? 0 : v >= 1.0f ? 255 : (uint32_t)(v * 255.0f);
}

/* A worker's job: every `count`th piece of the picture from piece `first` on (half a row
 * each, 240 of them: about one a worker). A job makes no system call. */
static uint32_t pieces(uint32_t first, uint32_t count)
{
    uint32_t piece, at, row, column;
    float r, g, b;

    for (piece = first; piece < PIECES; piece += count)
        for (at = piece * PIECE; at < (piece + 1) * PIECE; at++) {
            row = at / WIDTH, column = at % WIDTH;
            shade((float)column * (2.0f / WIDTH) - 1.0f, 1.0f - (float)row * (2.0f / HEIGHT), now, &r, &g, &b);
            pixels[at] = channel(r) | channel(g) << 8 | channel(b) << 16 | 0xff000000u;
        }
    return 0;
}

int main(void)
{
    static const GLfixed whole[16] = { 2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE };
    static const GLfixed corners[12] = { 0, 0, 0, ONE, 0, 0, ONE, ONE, 0, 0, ONE, 0 }, uv[8] = { 0, 0, ONE, 0, ONE, ONE, 0, ONE };
    GR_WINDOW_ID window;
    GR_EVENT event;
    GLuint texture;
    int width, height, k, pictures = 0;
    unsigned since;

    if (GrOpen() < 0)
        return 1;
    window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Shader on the worker cores", GR_ROOT_WINDOW_ID, -1, -1, WIDTH * SCALE, HEIGHT * SCALE, BLACK);
    GrSelectEvents(window, GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_EXPOSURE);
    GrMapWindow(window);
    do
        GrGetNextEvent(&event);
    while (event.type != GR_EVENT_TYPE_EXPOSURE);
    if (seglInit(window) < 0)
        return 1;
    pixels = seglMemory(WIDTH * HEIGHT * 4);
    if (!pixels)
        return 1;
    memset(pixels, 0, WIDTH * HEIGHT * 4);

    /* the picture is a texture where it lies, stretched over the window */
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    seglTexturePointer(pixels, WIDTH, HEIGHT, GL_RGBA);
    seglSize(&width, &height);
    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixx(whole);
    glDisable(GL_DEPTH_TEST);
    seglQuad(corners, uv, texture, 255, 255, 0);
    seglSwap();

    /* as many workers as the machine gives; what they will read and write is touched first */
    workers = mcw_open(mcw_most());
    mcw_touch(&pixels, sizeof pixels);
    mcw_touch(&now, sizeof now);
    printf("shader: %d worker cores\n", workers);
    since = milliseconds();
    for (;;) {
        GrCheckNextEvent(&event);
        if (event.type == GR_EVENT_TYPE_CLOSE_REQ || (event.type == GR_EVENT_TYPE_KEY_DOWN && event.keystroke.ch == 'q'))
            break;
        now += 0.05f;
        if (workers == 0)
            pieces(0, 1);
        for (k = 1; k <= workers; k++)
            mcw_post(k, pieces, (uint32_t)(k - 1), (uint32_t)workers);
        for (k = 1; k <= workers; k++)
            mcw_wait(k);
        seglSwapAgain();
        if (++pictures % 50 == 0) {
            unsigned ms = milliseconds() - since;

            printf("shader: 50 pictures in %u ms: %u a second\n", ms, ms ? 50000 / ms : 0);
            fflush(stdout);
            since += ms;
        }
    }
    mcw_close();
    GrClose();
    return 0;
}
