// Dear ImGui's platform and renderer for this machine (docs/imgui.md).
#include "imgui_impl_shaderemu.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include "font_baked.h"
#include "imgui_format.h"
extern "C" {
#include <GLES/segl.h>
#include <nano-X.h>
}

static GR_WINDOW_ID window;
static GLuint font_texture;
static struct timeval last_frame;

// ---- text from numbers ----

// Dear ImGui's two format functions, which imgui.cpp is built without (build.sh): the common
// formats are written here (imgui_format.h) and the C library does the rest.
int ImFormatStringV(char* buf, size_t buf_size, const char* fmt, va_list args)
{
    int w = -1;
    if (buf && buf_size) {
        va_list same;
        va_copy(same, args);
        w = format_quick(buf, buf_size, fmt, same);
        va_end(same);
    }
    if (w < 0) w = vsnprintf(buf, buf_size, fmt, args);
    if (buf == NULL) return w;
    if (w == -1 || w >= (int)buf_size) w = (int)buf_size - 1;
    buf[w] = 0;
    return w;
}

int ImFormatString(char* buf, size_t buf_size, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int w = ImFormatStringV(buf, buf_size, fmt, args);
    va_end(args);
    return w;
}

// ---- input ----

// The machine's own words (docs/input.md): from 0x20 the pointer's place, its buttons and the
// count of keys, and at 0x34 the clock in milliseconds.
static const volatile unsigned* control(int offset) { return (const volatile unsigned*)((const char*)seglPalette() - 0x400 + offset); }
static unsigned seen[4], asked_at;
static int asks = 8;

static ImGuiKey key_of(unsigned ch, bool ctrl)
{
    switch (ch) {
        case MWKEY_TAB: return ImGuiKey_Tab;
        case MWKEY_LEFT: return ImGuiKey_LeftArrow;
        case MWKEY_RIGHT: return ImGuiKey_RightArrow;
        case MWKEY_UP: return ImGuiKey_UpArrow;
        case MWKEY_DOWN: return ImGuiKey_DownArrow;
        case MWKEY_PAGEUP: return ImGuiKey_PageUp;
        case MWKEY_PAGEDOWN: return ImGuiKey_PageDown;
        case MWKEY_HOME: return ImGuiKey_Home;
        case MWKEY_END: return ImGuiKey_End;
        case MWKEY_INSERT: return ImGuiKey_Insert;
        case MWKEY_DELETE: return ImGuiKey_Delete;
        case MWKEY_BACKSPACE: return ImGuiKey_Backspace;
        case MWKEY_ENTER: return ImGuiKey_Enter;
        case MWKEY_ESCAPE: return ImGuiKey_Escape;
        case ' ': return ImGuiKey_Space;
        case MWKEY_LCTRL: return ImGuiKey_LeftCtrl;
        case MWKEY_RCTRL: return ImGuiKey_RightCtrl;
        case MWKEY_LSHIFT: return ImGuiKey_LeftShift;
        case MWKEY_RSHIFT: return ImGuiKey_RightShift;
        case MWKEY_LALT: return ImGuiKey_LeftAlt;
        case MWKEY_RALT: return ImGuiKey_RightAlt;
    }
    // the keyboard driver hands a letter with Ctrl over as its control character
    if (ctrl && ch >= 1 && ch <= 26) return (ImGuiKey)(ImGuiKey_A + ch - 1);
    if (ch >= 'a' && ch <= 'z') return (ImGuiKey)(ImGuiKey_A + ch - 'a');
    if (ch >= 'A' && ch <= 'Z') return (ImGuiKey)(ImGuiKey_A + ch - 'A');
    if (ch >= '0' && ch <= '9') return (ImGuiKey)(ImGuiKey_0 + ch - '0');
    return ImGuiKey_None;
}

static void key_event(const GR_EVENT_KEYSTROKE& k, bool down)
{
    ImGuiIO& io = ImGui::GetIO();
    bool ctrl = (k.modifiers & MWKMOD_CTRL) != 0;
    io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
    io.AddKeyEvent(ImGuiMod_Shift, (k.modifiers & MWKMOD_SHIFT) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (k.modifiers & MWKMOD_ALT) != 0);
    ImGuiKey key = key_of(k.ch, ctrl);
    if (key != ImGuiKey_None) io.AddKeyEvent(key, down);
    if (down && !ctrl && k.ch >= 32 && k.ch < 127) io.AddInputCharacter(k.ch);
}

// ---- the font ----

// Dear ImGui's default font as the build wrote it down (font_baked.h), put where the atlas
// would have built it. False when the program has fonts or settings of its own.
static bool baked_font(ImFontAtlas* atlas)
{
    if (atlas->ConfigData.Size || atlas->Fonts.Size || atlas->CustomRects.Size || atlas->Flags || atlas->TexDesiredWidth ||
        atlas->TexGlyphPadding != 1 || atlas->FontBuilderIO || atlas->FontBuilderFlags || strcmp(IMGUI_VERSION, BAKED_VERSION))
        return false;
    // the settings AddFontDefault() gives it; the font's file is not here, so nothing can build it again
    static const char no_file[4] = {0};
    const float* f = (const float*)baked_floats;
    ImFontConfig cfg;
    cfg.OversampleH = cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    cfg.SizePixels = 13.0f;
    strcpy(cfg.Name, "ProggyClean.ttf, 13px");
    cfg.EllipsisChar = (ImWchar)0x0085;
    cfg.GlyphOffset.y = 1.0f;
    cfg.GlyphRanges = atlas->GetGlyphRangesDefault();
    cfg.FontData = (void*)no_file;
    cfg.FontDataSize = sizeof no_file;
    cfg.FontDataOwnedByAtlas = false;
    ImFont* font = atlas->AddFont(&cfg);
    font->FontSize = cfg.SizePixels;
    font->ContainerAtlas = atlas;
    font->Ascent = f[0], font->Descent = f[1];
    font->MetricsTotalSurface = BAKED_SURFACE;
    font->Glyphs.resize(BAKED_GLYPHS);
    memcpy(font->Glyphs.Data, baked_glyphs, sizeof baked_glyphs);
    font->BuildLookupTable();
    for (int i = 0; i < 2; i++) {
        ImFontAtlasCustomRect* r = atlas->GetCustomRectByIndex(atlas->AddCustomRectRegular(baked_rects[i][0], baked_rects[i][1]));
        r->X = baked_rects[i][2], r->Y = baked_rects[i][3];
    }
    atlas->PackIdMouseCursors = BAKED_CURSORS, atlas->PackIdLines = BAKED_LINES;
    atlas->TexWidth = BAKED_W, atlas->TexHeight = BAKED_H;
    atlas->TexUvScale = ImVec2(f[2], f[3]), atlas->TexUvWhitePixel = ImVec2(f[4], f[5]);
    memcpy(atlas->TexUvLines, f + 6, sizeof atlas->TexUvLines);
    atlas->TexReady = true;
    return true;
}

static int unlike(const void* a, const void* b, size_t bytes) { return memcmp(a, b, bytes) != 0; }

// IMGUI_FONT_CHECK=1: the atlas builds the font as it used to, and every part is compared.
static void check_font(ImFontAtlas* have)
{
    ImFontAtlas built;
    unsigned char* pixels;
    int width, height;
    built.GetTexDataAsAlpha8(&pixels, &width, &height);
    const ImFont *a = have->Fonts[0], *b = built.Fonts[0];
    const ImFontConfig *c = a->ConfigData, *d = b->ConfigData;
    int picture = width != BAKED_W || height != BAKED_H || unlike(pixels, baked_alpha, sizeof baked_alpha);
    int glyphs = a->Glyphs.Size != b->Glyphs.Size || unlike(a->Glyphs.Data, b->Glyphs.Data, a->Glyphs.Size * sizeof(ImFontGlyph));
    int tables = a->IndexAdvanceX.Size != b->IndexAdvanceX.Size || a->IndexLookup.Size != b->IndexLookup.Size ||
                 unlike(a->IndexAdvanceX.Data, b->IndexAdvanceX.Data, a->IndexAdvanceX.Size * sizeof(float)) ||
                 unlike(a->IndexLookup.Data, b->IndexLookup.Data, a->IndexLookup.Size * sizeof(ImWchar)) ||
                 unlike(a->Used4kPagesMap, b->Used4kPagesMap, sizeof a->Used4kPagesMap);
    int font = a->FallbackAdvanceX != b->FallbackAdvanceX || a->FontSize != b->FontSize || a->FallbackChar != b->FallbackChar ||
               a->FallbackGlyph - a->Glyphs.Data != b->FallbackGlyph - b->Glyphs.Data || a->EllipsisChar != b->EllipsisChar ||
               a->EllipsisCharCount != b->EllipsisCharCount || a->EllipsisWidth != b->EllipsisWidth ||
               a->EllipsisCharStep != b->EllipsisCharStep || a->Scale != b->Scale || a->Ascent != b->Ascent ||
               a->Descent != b->Descent || a->MetricsTotalSurface != b->MetricsTotalSurface ||
               a->ConfigDataCount != b->ConfigDataCount || a->DirtyLookupTables != b->DirtyLookupTables ||
               a->ContainerAtlas != have || have->Fonts.Size != built.Fonts.Size;
    int atlas = have->TexWidth != built.TexWidth || have->TexHeight != built.TexHeight || unlike(&have->TexUvScale, &built.TexUvScale, 8) ||
                unlike(&have->TexUvWhitePixel, &built.TexUvWhitePixel, 8) ||
                unlike(have->TexUvLines, built.TexUvLines, sizeof have->TexUvLines) ||
                have->PackIdMouseCursors != built.PackIdMouseCursors || have->PackIdLines != built.PackIdLines ||
                have->CustomRects.Size != built.CustomRects.Size || have->TexReady != built.TexReady ||
                have->ConfigData.Size != built.ConfigData.Size;
    for (int i = 0; i < have->CustomRects.Size && !atlas; i++) {
        const ImFontAtlasCustomRect &r = have->CustomRects[i], &t = built.CustomRects[i];
        atlas = r.Width != t.Width || r.Height != t.Height || r.X != t.X || r.Y != t.Y || r.GlyphID != t.GlyphID || r.Font || t.Font;
    }
    int settings = strcmp(c->Name, d->Name) || c->SizePixels != d->SizePixels || c->OversampleH != d->OversampleH ||
                   c->OversampleV != d->OversampleV || c->PixelSnapH != d->PixelSnapH || unlike(&c->GlyphOffset, &d->GlyphOffset, 8) ||
                   unlike(&c->GlyphExtraSpacing, &d->GlyphExtraSpacing, 8) || c->GlyphRanges != d->GlyphRanges ||
                   c->GlyphMinAdvanceX != d->GlyphMinAdvanceX || c->GlyphMaxAdvanceX != d->GlyphMaxAdvanceX ||
                   c->MergeMode != d->MergeMode || c->FontBuilderFlags != d->FontBuilderFlags || c->FontNo != d->FontNo ||
                   c->RasterizerMultiply != d->RasterizerMultiply || c->RasterizerDensity != d->RasterizerDensity ||
                   c->EllipsisChar != d->EllipsisChar || c->DstFont != a || d->DstFont != b;
    if (picture | glyphs | tables | font | atlas | settings)
        fprintf(stderr, "imgui: the font written down is NOT the one the atlas builds: picture %d, glyphs %d, tables %d, font %d, atlas %d, settings %d\n",
                picture, glyphs, tables, font, atlas, settings);
    else
        fprintf(stderr, "imgui: the font written down is the one the atlas builds (%d x %d, %d glyphs)\n", width, height, a->Glyphs.Size);
}

bool ImGui_ImplShaderEmu_Init(unsigned int nano_x_window)
{
    ImGuiIO& io = ImGui::GetIO();
    window = nano_x_window;
    io.BackendPlatformName = io.BackendRendererName = "imgui_impl_shaderemu";
    GrSelectEvents(window, GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP |
                               GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_POSITION |
                               GR_EVENT_MASK_MOUSE_EXIT);
    glGenTextures(1, &font_texture);
    glBindTexture(GL_TEXTURE_2D, font_texture);
    if (!io.Fonts->IsBuilt() && baked_font(io.Fonts)) {
        // the device's words: white, and how transparent in the top byte. Most of the picture is empty.
        ImU32 *texels = (ImU32*)seglMemory(BAKED_W * BAKED_H * 4), *to = texels;
        if (!texels) return false;
        for (unsigned four : baked_alpha) {
            if (!four) to[0] = to[1] = to[2] = to[3] = ~0u;
            else to[0] = ~(four << 24), to[1] = ~(four >> 8 << 24), to[2] = ~(four >> 16 << 24), to[3] = ~(four >> 24 << 24);
            to += 4;
        }
        seglTexturePointer(texels, BAKED_W, BAKED_H, GL_RGBA);
        if (getenv("IMGUI_FONT_CHECK")) check_font(io.Fonts);
    } else {
        // a font of the program's own: bytes of red, green, blue and alpha, which the library makes the device's words
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        io.Fonts->ClearTexData();
    }
    io.Fonts->SetTexID((ImTextureID)font_texture);
    gettimeofday(&last_frame, nullptr);
    return font_texture != 0;
}

bool ImGui_ImplShaderEmu_NewFrame()
{
    ImGuiIO& io = ImGui::GetIO();
    GR_EVENT event;
    bool open = true;
    // The window system is asked for events when the input words have changed, a few frames
    // more (an event may be on its way) and twice a second: a question that has no answer
    // is two system calls and two changes of task all the same.
    const volatile unsigned* input = control(0x20);
    const unsigned clock = *control(0x34);
    for (int i = 0; i < 4; i++)
        if (seen[i] != input[i]) seen[i] = input[i], asks = 8;
    if (!asks && clock - asked_at >= 500) asks = 1;
    while (asks) {
        GrCheckNextEvent(&event);
        if (event.type == GR_EVENT_TYPE_NONE) {
            asks--, asked_at = clock;
            break;
        }
        switch (event.type) {
            case GR_EVENT_TYPE_CLOSE_REQ: open = false; break;
            case GR_EVENT_TYPE_UPDATE:
                if (event.update.utype == GR_UPDATE_SIZE) seglWindowChanged();
                break;
            case GR_EVENT_TYPE_MOUSE_POSITION: io.AddMousePosEvent((float)event.mouse.x, (float)event.mouse.y); break;
            case GR_EVENT_TYPE_MOUSE_EXIT: io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); break;
            case GR_EVENT_TYPE_BUTTON_DOWN:
            case GR_EVENT_TYPE_BUTTON_UP: {
                bool down = event.type == GR_EVENT_TYPE_BUTTON_DOWN;
                // a notch of the wheel is a button going down (docs/input.md)
                if (event.button.buttons & GR_BUTTON_SCROLLUP) io.AddMouseWheelEvent(0, 1);
                if (event.button.buttons & GR_BUTTON_SCROLLDN) io.AddMouseWheelEvent(0, -1);
                io.AddMousePosEvent((float)event.button.x, (float)event.button.y);
                if (event.button.changebuttons & GR_BUTTON_L) io.AddMouseButtonEvent(0, down);
                if (event.button.changebuttons & GR_BUTTON_R) io.AddMouseButtonEvent(1, down);
                if (event.button.changebuttons & GR_BUTTON_M) io.AddMouseButtonEvent(2, down);
                break;
            }
            case GR_EVENT_TYPE_KEY_DOWN: key_event(event.keystroke, true); break;
            case GR_EVENT_TYPE_KEY_UP: key_event(event.keystroke, false); break;
        }
    }
    int width, height;
    seglSize(&width, &height);
    io.DisplaySize = ImVec2((float)width, (float)height);
    struct timeval now;
    gettimeofday(&now, nullptr);
    float passed = (float)(now.tv_sec - last_frame.tv_sec) + (float)(now.tv_usec - last_frame.tv_usec) * 1e-6f;
    io.DeltaTime = passed > 0 ? passed : 1.0f / 60;
    last_frame = now;
    return open;
}

// ---- drawing ----

// A corner as ImGui has it (ImDrawVert's layout).
struct corner { float x, y, u, v; ImU32 colour; };

// A vertex goes to the GPU as four words (docs/gpu.md, a tagged vertex): ImGui's x and y as
// the floats they are, 0, and the texture coordinates in 1,024ths with a tag, which is the
// number of the vertex's colour in a table of the colours the frame has used.
static ImU32* table;                    // the table being filled: 256 words in the frame
static int colours;                     // how many it has
static ImU32 slot_colour[256];          // the table's colours by a hash of them
static unsigned char slot_tag[256], slot_used[256];

// The copy's own state, kept here between its pieces (docs/imgui.md, "The copy").
static struct {
    ImU32 *to, *first, *last;   // the next corner's place, the run's first, the last place a cut triangle may begin
    int quads;                  // the run being written: four corners a quad (1), or three a triangle (0)
    GLuint name;                // the texture of ImGui's command
    ImU32 colour[3], tag[3];    // the colours looked up last, the newest first, and their tags as a vertex has them:
    bool tagged;                // (once there are any) a bar's, its text's, and what a shape's edge fades to
    ImU32 left, top, wide, high;    // the command's clip rectangle, for inside() below
    const ImDrawIdx* next_lone; // triangles one after the other in a run of quads: where the next would begin,
    int lone;                   // and how many there are so far
} out;

static bool new_table()
{
    table = (ImU32*)seglScreenTable();
    colours = 0;
    out.tagged = false;
    memset(slot_used, 0, sizeof slot_used);
    return table != nullptr;
}

// A colour and its tag become the newest of the three remembered, in place of entry `old`.
static inline void newest(ImU32 colour, ImU32 tag, int old)
{
    for (; old > 0; old--) out.colour[old] = out.colour[old - 1], out.tag[old] = out.tag[old - 1];
    out.colour[0] = colour, out.tag[0] = tag;
}

// Is `colour` one of the two remembered before the newest? Then it is the newest now.
static __attribute__((noinline)) bool recalled(ImU32 colour)
{
    for (int i = 1; i < 3; i++)
        if (colour == out.colour[i]) return newest(colour, out.tag[i], i), true;
    return false;
}

// The tag of a colour as a vertex has it, into `word`. False when the table is full.
static inline bool tag_of(ImU32 colour, ImU32* word)
{
    if (out.tagged) {
        if (colour == out.colour[0]) return *word = out.tag[0], true;
        if (recalled(colour)) return *word = out.tag[0], true;
    }
    unsigned at = (colour * 2654435761u) >> 24;
    while (slot_used[at] && slot_colour[at] != colour) at = (at + 1) & 255;
    if (!slot_used[at]) {
        if (colours == 256) return false;
        slot_used[at] = 1;
        slot_colour[at] = colour;
        slot_tag[at] = (unsigned char)colours;
        // ImGui's is alpha, blue, green, red from the top; the device's transparency, red, green, blue
        table[colours++] = (255 - (colour >> 24)) << 24 | (colour & 255) << 16 | (colour & 0xff00) | (colour >> 16 & 255);
    }
    *word = (ImU32)slot_tag[at] << 24;
    if (!out.tagged) out.colour[1] = out.colour[2] = colour, out.tag[1] = out.tag[2] = *word, out.tagged = true;
    newest(colour, *word, 2);
    return true;
}

// A texture coordinate (a float's bits) in 1,024ths.
static inline ImU32 texels(ImU32 coordinate)
{
    float f;
    memcpy(&f, &coordinate, 4);
    return (ImU32)(int)(f * 1024.0f + 0.5f);
}

static inline ImU32* put(ImU32* to, const ImU32* from, ImU32 tag)
{
    to[0] = from[0], to[1] = from[1], to[2] = 0;
    to[3] = texels(from[2]) | texels(from[3]) << 12 | tag;
    return to + 4;
}

static void more_room()
{
    GLsizei room = 0;
    out.to = out.first = (ImU32*)seglScreenCompactSpace(&room);
    out.last = out.to + 4 * (room - 24);    // (a triangle cut into a fan is 24 corners at most)
}

// What is written of the run becomes a command of the device's.
static void run_done()
{
    seglScreenCompactUsed((GLsizei)(out.to - out.first) / 4, out.quads, out.name, (const unsigned int*)table);
}

// The run ends, and one of quads or of triangles begins in the room there is now.
static void flush(int next_quads)
{
    run_done();
    out.quads = next_quads;
    more_room();
}

static bool full_table()
{
    run_done();
    if (!new_table()) return false;
    more_room();
    return true;
}

// A few triangles among quads (an arrow in a list) are quads whose last two corners are one,
// which draws the triangle and nothing more: no run of their own, and none after them.
// Many in a row (a plot's line) are a run. True: the triangle at `index` is to be a quad.
static inline bool lone_as_quad(const ImDrawIdx* index)
{
    if (index != out.next_lone) out.lone = 0;
    out.next_lone = index + 3;
    return ++out.lone <= 16;
}

// A quad or (no `s`) a triangle that is all inside its clip rectangle, the long way: its
// colours looked up, and a run of the other kind begun if it must be. False: the table is full.
static bool coloured(const ImDrawIdx* index, const ImU32* p, const ImU32* q, const ImU32* r, const ImU32* s)
{
    const bool quad = s != nullptr;
    ImU32 a, b, c, d;
    if (!quad) s = r;
    // (in the corners' order: the table has its colours in the order they were first seen)
    bool fit = tag_of(p[4], &a);
    fit &= tag_of(q[4], &b);
    fit &= tag_of(r[4], &c);
    fit &= tag_of(s[4], &d);
    if (!fit) return false;
    const bool as_quad = quad || (out.quads && lone_as_quad(index));
    if (out.quads != as_quad) flush(as_quad);
    ImU32* to = put(put(put(out.to, p, a), q, b), r, c);
    out.to = as_quad ? put(to, s, d) : to;
    return true;
}

static corner between(const corner& a, const corner& b, float t)
{
    corner c = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t, a.colour};
    if (a.colour != b.colour) {
        c.colour = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            float from = (float)(a.colour >> shift & 255), to = (float)(b.colour >> shift & 255);
            c.colour |= (ImU32)(from + (to - from) * t + 0.5f) << shift;
        }
    }
    return c;
}

// The part of a polygon on the inner side of one edge of the clip rectangle: x or y at least
// (or at most) `limit`. The device has no scissor, so a triangle that crosses one is cut here.
static int cut(const corner* in, int count, corner* out, bool is_y, bool at_least, float limit)
{
    int n = 0;
    for (int i = 0; i < count; i++) {
        const corner &a = in[i], &b = in[(i + 1) % count];
        float pa = is_y ? a.y : a.x, pb = is_y ? b.y : b.x;
        bool a_in = at_least ? pa >= limit : pa <= limit, b_in = at_least ? pb >= limit : pb <= limit;
        if (a_in) out[n++] = a;
        if (a_in != b_in) out[n++] = between(a, b, (limit - pa) / (pb - pa));
    }
    return n;
}

static inline float least(float a, float b, float c)
{
    float m = a < b ? a : b;
    return m < c ? m : c;
}

// A triangle that may reach over its clip rectangle: by its box, drawn whole, or left out, or
// cut by each edge in turn (up to seven corners, drawn as a fan). False when the table is full.
static bool edge_triangle(const ImVec4& clip, const ImDrawIdx* index, const ImU32* p, const ImU32* q, const ImU32* r)
{
    const corner *a = (const corner*)p, *b = (const corner*)q, *c = (const corner*)r;
    float x0 = least(a->x, b->x, c->x), x1 = -least(-a->x, -b->x, -c->x);
    float y0 = least(a->y, b->y, c->y), y1 = -least(-a->y, -b->y, -c->y);
    if (x0 >= clip.x && x1 <= clip.z && y0 >= clip.y && y1 <= clip.w) return coloured(index, p, q, r, nullptr);
    if (!(x1 >= clip.x && x0 <= clip.z && y1 >= clip.y && y0 <= clip.w)) return true;
    corner tri[3] = {*a, *b, *c}, even[8], odd[8];
    int count = cut(tri, 3, even, false, true, clip.x);
    ImU32 fan[8];
    bool fits = true;
    count = cut(even, count, odd, false, false, clip.z);
    count = cut(odd, count, even, true, true, clip.y);
    count = cut(even, count, odd, true, false, clip.w);
    for (int k = 0; k < count; k++) fits &= tag_of(odd[k].colour, &fan[k]);
    if (!fits) return false;
    if (out.quads) flush(0);
    for (int k = 1; k + 1 < count; k++)
        out.to = put(put(put(out.to, (const ImU32*)&odd[0], fan[0]), (const ImU32*)&odd[k], fan[k]), (const ImU32*)&odd[k + 1], fan[k + 1]);
    return true;
}

// A quad that may reach over its clip rectangle, by its box. 1: drawn whole, or left out;
// 0: its two triangles are to be looked at one by one; -1: the table is full.
static int edge_quad(const ImVec4& clip, const ImDrawIdx* index, const ImU32* p, const ImU32* q, const ImU32* r, const ImU32* s)
{
    const corner *a = (const corner*)p, *b = (const corner*)q, *c = (const corner*)r, *d = (const corner*)s;
    float x0 = least(a->x, b->x, c->x), x1 = -least(-a->x, -b->x, -c->x);
    float y0 = least(a->y, b->y, c->y), y1 = -least(-a->y, -b->y, -c->y);
    x0 = x0 < d->x ? x0 : d->x, x1 = x1 > d->x ? x1 : d->x;
    y0 = y0 < d->y ? y0 : d->y, y1 = y1 > d->y ? y1 : d->y;
    if (x0 >= clip.x && x1 <= clip.z && y0 >= clip.y && y1 <= clip.w) return coloured(index, p, q, r, s) ? 1 : -1;
    return !(x1 >= clip.x && x0 <= clip.z && y1 >= clip.y && y0 <= clip.w);
}

static inline ImU32 bits(float f)
{
    ImU32 u;
    memcpy(&u, &f, 4);
    return u;
}

// The clip rectangle is kept as the bits of its floats, which are in the floats' order when
// none is negative: a corner is inside when its own bits are within `wide` and `high` of the
// left and the top. (With 0 and 0 no corner is, and the floats are asked.)
static inline bool inside(const ImU32* corner, ImU32 left, ImU32 top, ImU32 wide, ImU32 high)
{
    return corner[0] - left < wide && corner[1] - top < high;
}

// One triangle or quad the long way: whatever it is. How many indices it was, or 0 when the
// table is full.
static __attribute__((noinline)) int one_shape(const ImVec4& clip, const char* base, const ImDrawIdx* index, const ImDrawIdx* end)
{
    const ImU32 *p = (const ImU32*)(base + index[0] * sizeof(ImDrawVert)), *q = (const ImU32*)(base + index[1] * sizeof(ImDrawVert)),
                *r = (const ImU32*)(base + index[2] * sizeof(ImDrawVert));
    const ImU32 left = out.left, top = out.top, wide = out.wide, high = out.high;
    const bool three = inside(p, left, top, wide, high) && inside(q, left, top, wide, high) && inside(r, left, top, wide, high);
    // two triangles on corners 0 1 2 and 0 2 3, as ImGui makes its rectangles and letters: a quad
    if (index + 5 < end && index[3] == index[0] && index[4] == index[2]) {
        const ImU32* s = (const ImU32*)(base + index[5] * sizeof(ImDrawVert));
        if (three && inside(s, left, top, wide, high)) return coloured(index, p, q, r, s) ? 6 : 0;
        int whole = edge_quad(clip, index, p, q, r, s);
        if (whole) return whole > 0 ? 6 : 0;
    }
    return (three ? coloured(index, p, q, r, nullptr) : edge_triangle(clip, index, p, q, r)) ? 3 : 0;
}

// One of ImGui's commands: its triangles into the frame, as the runs of quads and of triangles
// they make. Nearly all are inside the clip rectangle and of the two colours looked up last:
// those are written by the two loops here, a run of each kind, and the rest by one_shape().
// False: no table.
static __attribute__((noinline)) bool copy_command(const ImDrawCmd& cmd, const char* base, const ImDrawIdx* index)
{
    const ImDrawIdx* const end = index + cmd.ElemCount;
    const ImU32 left = bits(cmd.ClipRect.x), top = bits(cmd.ClipRect.y), right = bits(cmd.ClipRect.z), bottom = bits(cmd.ClipRect.w);
    const bool ordered = !((left | top | right | bottom) >> 31) && right >= left && bottom >= top;
    const ImU32 wide = ordered ? right - left + 1 : 0, high = ordered ? bottom - top + 1 : 0;
    out.left = left, out.top = top, out.wide = wide, out.high = high;
    // (a multiplication is one instruction here, and the compiler would shift and add instead)
    ImU32 stride = sizeof(ImDrawVert);
    __asm__("" : "+r"(stride));
    for (;;) {
        ImU32* to = out.to;
        ImU32* const last = out.last;
        const ImU32 colour = out.colour[0], tag = out.tag[0];
        bool again = false, stopped = false;
        if (out.tagged && out.quads) {
            // two triangles on corners 0 1 2 and 0 2 3, as ImGui makes its rectangles and letters: a quad
            for (; index + 5 < end && to <= last && index[3] == index[0] && index[4] == index[2]; index += 6, to += 16) {
                const ImU32 *p = (const ImU32*)(base + index[0] * stride), *q = (const ImU32*)(base + index[1] * stride),
                            *r = (const ImU32*)(base + index[2] * stride), *s = (const ImU32*)(base + index[5] * stride);
                stopped = true;
                if (!(inside(p, left, top, wide, high) && inside(q, left, top, wide, high) && inside(r, left, top, wide, high) &&
                      inside(s, left, top, wide, high)))
                    break;
                if (p[4] != colour || q[4] != colour || r[4] != colour || s[4] != colour) {
                    // (a bar, its text, the next bar: the colours take turns)
                    again = q[4] == p[4] && r[4] == p[4] && s[4] == p[4] && recalled(p[4]);
                    break;
                }
                stopped = false;
                const ImU32 u0 = p[2], v0 = p[3], u1 = r[2], v1 = r[3];
                to[0] = p[0], to[1] = p[1], to[2] = 0;
                to[4] = q[0], to[5] = q[1], to[6] = 0;
                to[8] = r[0], to[9] = r[1], to[10] = 0;
                to[12] = s[0], to[13] = s[1], to[14] = 0;
                if (q[2] == u1 && q[3] == v0 && s[2] == u0 && s[3] == v1) {
                    // a rectangle of the texture, as a letter is: two coordinates across and two down
                    const ImU32 a = texels(u0) | tag, b = texels(u1) | tag, c = texels(v0) << 12, d = texels(v1) << 12;
                    to[3] = a | c, to[7] = b | c, to[11] = b | d, to[15] = a | d;
                } else {
                    to[3] = texels(u0) | texels(v0) << 12 | tag, to[7] = texels(q[2]) | texels(q[3]) << 12 | tag;
                    to[11] = texels(u1) | texels(v1) << 12 | tag, to[15] = texels(s[2]) | texels(s[3]) << 12 | tag;
                }
            }
            // a few triangles among the quads, as quads (lone_as_quad())
            if (!stopped) {
                const ImU32 other = out.colour[1], other_tag = out.tag[1];
                for (; index + 2 < end && to <= last; index += 3, to += 16) {
                    if (index + 5 < end && index[3] == index[0] && index[4] == index[2]) {
                        again = true;
                        break;
                    }
                    const ImU32 *p = (const ImU32*)(base + index[0] * stride), *q = (const ImU32*)(base + index[1] * stride),
                                *r = (const ImU32*)(base + index[2] * stride);
                    if (!(inside(p, left, top, wide, high) && inside(q, left, top, wide, high) && inside(r, left, top, wide, high))) break;
                    const ImU32 pc = p[4], qc = q[4], rc = r[4];
                    if ((pc != colour && pc != other) || (qc != colour && qc != other) || (rc != colour && rc != other)) break;
                    if (!lone_as_quad(index)) {
                        out.lone--, out.next_lone = index;      // (one_shape() counts it again)
                        break;
                    }
                    const ImU32 x = r[0], y = r[1], w = texels(r[2]) | texels(r[3]) << 12 | (rc == colour ? tag : other_tag);
                    to[0] = p[0], to[1] = p[1], to[2] = 0, to[3] = texels(p[2]) | texels(p[3]) << 12 | (pc == colour ? tag : other_tag);
                    to[4] = q[0], to[5] = q[1], to[6] = 0, to[7] = texels(q[2]) | texels(q[3]) << 12 | (qc == colour ? tag : other_tag);
                    to[8] = x, to[9] = y, to[10] = 0, to[11] = w;
                    to[12] = x, to[13] = y, to[14] = 0, to[15] = w;
                }
            }
        } else if (out.tagged) {
            const ImU32 other = out.colour[1], other_tag = out.tag[1];
            for (; index + 2 < end && to <= last; index += 3, to += 12) {
                if (index + 5 < end && index[3] == index[0] && index[4] == index[2]) break;
                const ImU32 *p = (const ImU32*)(base + index[0] * stride), *q = (const ImU32*)(base + index[1] * stride),
                            *r = (const ImU32*)(base + index[2] * stride);
                if (!(inside(p, left, top, wide, high) && inside(q, left, top, wide, high) && inside(r, left, top, wide, high))) break;
                const ImU32 pc = p[4], qc = q[4], rc = r[4];
                if ((pc != colour && pc != other) || (qc != colour && qc != other) || (rc != colour && rc != other)) break;
                to[0] = p[0], to[1] = p[1], to[2] = 0, to[3] = texels(p[2]) | texels(p[3]) << 12 | (pc == colour ? tag : other_tag);
                to[4] = q[0], to[5] = q[1], to[6] = 0, to[7] = texels(q[2]) | texels(q[3]) << 12 | (qc == colour ? tag : other_tag);
                to[8] = r[0], to[9] = r[1], to[10] = 0, to[11] = texels(r[2]) | texels(r[3]) << 12 | (rc == colour ? tag : other_tag);
            }
        }
        out.to = to;
        if (!(index + 2 < end && to <= last)) return true;
        if (again) continue;
        int indices = one_shape(cmd.ClipRect, base, index, end);
        if (!indices && !full_table()) return false;    // (with a new table, the same one again)
        index += indices;
    }
}

void ImGui_ImplShaderEmu_RenderDrawData(ImDrawData* draw_data)
{
    static_assert(sizeof(ImDrawVert) == sizeof(corner) && sizeof(ImDrawIdx) == 2, "a corner is an ImDrawVert");
    if (!new_table()) return;
    out.quads = 0, out.next_lone = nullptr;
    more_room();
    for (int n = 0; n < draw_data->CmdListsCount && out.to; n++) {
        const ImDrawList* list = draw_data->CmdLists[n];
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState) cmd.UserCallback(list, &cmd);
                continue;
            }
            out.name = (GLuint)cmd.TextureId;
            if (!copy_command(cmd, (const char*)(list->VtxBuffer.Data + cmd.VtxOffset), list->IdxBuffer.Data + cmd.IdxOffset)) return;
            // a command of the device's for every one of ImGui's that drew: each has one texture
            flush(out.quads);
        }
    }
}
