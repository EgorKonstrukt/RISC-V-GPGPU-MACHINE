#include "memview.h"

#include <d3dcompiler.h>

#include <cstring>
#include <algorithm>
#include <filesystem>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

const char* kShader = R"(
Texture2D<uint4> Cur : register(t0);
Texture2D<uint4> Prev : register(t1);
Texture2D<float> Heat : register(t2);
Texture2D<float4> Text : register(t3);
Texture2D<float4> Gpu : register(t4);    // the GPU device's colour target (display mode 3)
cbuffer C : register(b0) {
    float2 WinSize;
    uint Frame;
    uint Strips;       // RAM is cut into this many vertical strips shown side by side
    uint TexWidth;
    uint RamRows;
    uint StateRows;
    float Inset;       // side of the CPU state inset in window pixels
    float2 TextSize;   // used size of the text texture; 0 = no text
    float Bar;         // height of the info bar under the memory image
    float Split;       // the display is left of this x, the memory image right of it
    float Ui;          // window pixels a 96 dpi pixel
    float MemOff;      // first RAM row the memory image shows
    float MemSpan;     // RAM rows the memory image shows
    float Glow;        // heat glow intensity (0 off)
    float ColOff;      // first RAM texel column shown (the loupe's window)
    float ColSpan;     // RAM texel columns shown
    float Reserved0, Reserved1;
};

float4 vs(uint id : SV_VertexID) : SV_Position {
    return float4((id == 1) ? 3.0 : -1.0, (id == 2) ? -3.0 : 1.0, 0, 1);
}

uint hash(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

uint peak(uint v) {
    return max(max(v & 0xff, (v >> 8) & 0xff), max((v >> 16) & 0xff, v >> 24));
}

// A texel as a colour: the largest byte of word 0, of word 1 and of words 2-3, lifted off black
// so that any texel holding data shows, whichever of its bytes are in use.
float3 bytes(uint4 t) {
    return float3(peak(t.r), peak(t.g), peak(t.b | t.a)) / 255.0 * 0.88 + 0.12;
}

uint word(uint4 t, uint i) {
    return i == 0 ? t.r : i == 1 ? t.g : i == 2 ? t.b : t.a;
}

// RAM texel by index (physical address - 0x80000000) / 16.
uint4 ram(uint index) {
    return Cur.Load(int3(index % TexWidth, StateRows + index / TexWidth, 0));
}

float3 rgb(uint v) {
    return float3((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff) / 255.0;
}

// The guest's display (docs/machine/display.md): control words at 0x87000000 (mode, width, height),
// palette at 0x87000400, pixels from 0x87001000.
static const uint DispCtrl = 0x700000, DispPalette = 0x700040, DispPixels = 0x700100;
static const uint DispCursor = 0x700004;   // x, y, on, address of a 32x32 image

// One pixel of the display as the guest made it, the cursor over it; `c` where no layer is.
float3 pixelAt(int2 at, uint4 ctrl, float3 c) {
    uint mode = ctrl.r, w = ctrl.g;
    uint4 cursor = ram(DispCursor);
    if (cursor.b & 1) {   // the bits above are its hot spot, for a host that draws it itself
        // drawn over whatever the display shows; image words with a zero top byte are clear
        int2 d = at - int2(asint(cursor.r), asint(cursor.g));
        if (d.x >= 0 && d.y >= 0 && d.x < 32 && d.y < 32) {
            uint a = (cursor.a & 0x7fffffff) + 4 * (uint)(d.y * 32 + d.x);
            uint v = word(ram(a >> 4), (a >> 2) & 3);
            if ((v >> 24) != 0) return rgb(v);
        }
    }
    if (mode == 4) {
        // layers: a table of rectangles of RAM, the last one on top (docs/machine/display.md)
        uint table = (ctrl.a & 0x7fffffff) >> 4, count = min(ram(table).r, 16u);
        for (uint n = count; n > 0; n--) {
            uint4 box = ram(table + 2 * n - 1);
            int2 d = at - int2(asint(box.r), asint(box.g));
            if (d.x >= 0 && d.y >= 0 && d.x < (int)box.b && d.y < (int)box.a) {
                uint word_at = (ram(table + 2 * n).r & 0x7fffffff) + 4 * (uint)(d.y * (int)box.b + d.x);
                return rgb(word(ram(word_at >> 4), (word_at >> 2) & 3));
            }
        }
        return c;
    }
    if (mode == 3) return Gpu.Load(int3(at, 0)).rgb;                         // the GPU's picture
    uint i = (uint)at.y * w + (uint)at.x;
    if (mode == 1) return rgb(word(ram(DispPixels + i / 4), i & 3));          // 0x00RRGGBB
    uint b = (word(ram(DispPixels + i / 16), (i / 4) & 3) >> (8 * (i & 3))) & 0xff;
    return rgb(word(ram(DispPalette + b / 4), b & 3));                        // palette index
}

float3 display(float2 p, float2 size) {
    float3 c = float3(0.035, 0.038, 0.047);
    uint4 ctrl = ram(DispCtrl);
    uint mode = ctrl.r, w = ctrl.g, h = ctrl.b;
    if (mode < 1 || mode > 4 || w == 0 || h == 0 || w > 2048 || h > 2048) return c;
    // as large as the panel holds it, centred (the tick's pointer, gpu.h, by the same rule)
    float scale = min(size.x / w, size.y / h);
    float2 corner = (size - float2(w, h) * scale) * 0.5;
    float2 lo = (floor(p) - corner) / scale, hi = lo + 1 / scale;   // this pixel's box, in display pixels
    if (hi.x <= 0 || hi.y <= 0 || lo.x >= w || lo.y >= h) return c;
    // the box filter: each display pixel weighted by how much of this pixel it covers
    float2 a = max(lo, 0), b = min(hi, float2(w, h));
    int2 first = int2(floor(a)), last = min(int2(ceil(b)) - 1, int2(first + 7));
    float3 sum = 0;
    float total = 0;
    for (int y = first.y; y <= last.y; y++) {
        float wy = min(b.y, y + 1) - max(a.y, y);
        for (int x = first.x; x <= last.x; x++) {
            float wt = (min(b.x, x + 1) - max(a.x, x)) * wy;
            sum += pixelAt(int2(x, y), ctrl, c) * wt;
            total += wt;
        }
    }
    float inside = total * scale * scale;   // the share of this pixel the picture covers
    return lerp(c, sum / max(total, 1e-6), saturate(inside));
}

struct Out {
    float4 color : SV_Target0;
    float heat : SV_Target1;
};

Out ps(float4 pos : SV_Position) {
    Out o;
    uint2 px = (uint2)pos.xy;
    float memHeight = WinSize.y - Bar;     // memory occupies everything above the info bar

    // Info bar along the bottom: counters on the left, CPU state area on the right.
    if (pos.y >= memHeight) {
        o.heat = 0;
        float3 c = float3(0.055, 0.062, 0.075);
        if (pos.y < memHeight + 1) c = float3(0.22, 0.24, 0.28);   // separator line
        float2 t = pos.xy - float2(12 * Ui, memHeight + 10 * Ui);
        if (TextSize.x > 0 && t.x >= 0 && t.y >= 0 && t.x < TextSize.x && t.y < TextSize.y) {
            c = lerp(c, float3(0.93, 0.95, 0.98), Text.Load(int3(t, 0)).g);
        }
        float2 inset = pos.xy - float2(WinSize.x - Inset - 12 * Ui, memHeight + (Bar - Inset) * 0.5);
        if (inset.x >= -1 && inset.y >= -1 && inset.x <= Inset && inset.y <= Inset) {
            if (inset.x < 0 || inset.y < 0 || inset.x >= Inset || inset.y >= Inset) {
                c = float3(0.75, 0.78, 0.85);                         // frame
            } else {
                int2 st = int2(inset / Inset * StateRows);
                uint4 a = Cur.Load(int3(st, 0)), b = Prev.Load(int3(st, 0));
                c = bytes(a) * 0.8 + 0.06;
                if (any(a != b)) c = lerp(c, float3(0.35, 0.85, 1.0), 0.6);
            }
        }
        o.color = float4(c, 1);
        return o;
    }

    // The display, left of the memory image.
    if (pos.x < Split) {
        o.heat = 0;
        float3 c = display(pos.xy, float2(Split, memHeight));
        if (pos.x >= Split - 1) c = float3(0.22, 0.24, 0.28);    // separator line
        o.color = float4(c, 1);
        return o;
    }

    // Heat fades in place and bleeds a little into neighbours, so a single written texel
    // shows up as a small dot rather than one pixel.
    float prevHeat = Heat.Load(int3(px, 0)) * 0.955;
    float around = max(max(Heat.Load(int3(px + int2(1, 0), 0)), Heat.Load(int3(px - int2(1, 0), 0))),
                       max(Heat.Load(int3(px + int2(0, 1), 0)), Heat.Load(int3(px - int2(0, 1), 0))));
    prevHeat = max(prevHeat, around * 0.5);

    // RAM: strip s covers rows [s, s+1) * rowsPerStrip, columns [ColOff, ColOff + ColSpan)
    float rowsPerStrip = MemSpan / Strips;
    float memWidth = WinSize.x - Split;
    float sx = (pos.x - Split) / memWidth * Strips;
    uint strip = min((uint)sx, Strips - 1);
    float2 texel = float2(ColOff + frac(sx) * ColSpan, StateRows + MemOff + (strip + pos.y / memHeight) * rowsPerStrip);
    float2 footprint = float2(ColSpan * Strips / memWidth, rowsPerStrip / memHeight);

    // A window pixel covers several texels when minified: a 4x4 grid of taps over its
    // footprint, nudged per frame so larger footprints are covered over time. Magnified
    // (a pixel is a texel or less) it is one point tap: no smoothing. The heat's spread
    // into neighbours is the glow and goes with it (Glow 0: pure point sampling).
    bool changed = false;
    uint4 first = 0;
    float heat;
    if (footprint.x <= 1.0 && footprint.y <= 1.0 && Glow <= 0.0) {
        int2 t = clamp(int2(texel + 0.5), int2(0, StateRows), int2(TexWidth - 1, StateRows + RamRows - 1));
        uint4 a = Cur.Load(int3(t, 0)), b = Prev.Load(int3(t, 0));
        if (any(a != b)) changed = true;
        first = a;
        heat = max(changed ? 1.0 : 0.0, prevHeat);
    } else {
        uint h = hash(px.x + px.y * 4099u + Frame * 7919u);
        float2 nudge = float2(h & 0xffff, h >> 16) / 65536.0 * 0.25;
        for (uint k = 0; k < 16; k++) {
            float2 cell = (float2(k & 3, k >> 2) + nudge * 4.0) / 4.0;
            int2 t = clamp(int2(texel + (cell - 0.5) * footprint), int2(0, StateRows), int2(TexWidth - 1, StateRows + RamRows - 1));
            uint4 a = Cur.Load(int3(t, 0)), b = Prev.Load(int3(t, 0));
            if (any(a != b)) changed = true;
            if (k == 5) first = a;
        }
        heat = max(changed ? 1.0 : 0.0, prevHeat);
        if (Glow > 0.0) heat = max(heat, around * 0.5);
        else heat = max(changed ? 1.0 : 0.0, Heat.Load(int3(px, 0)) * 0.955);
    }
    float3 base = any(first != 0) ? bytes(first) : float3(0.02, 0.025, 0.04);
    float3 glow = lerp(float3(1.0, 0.45, 0.05), float3(1.0, 0.95, 0.8), saturate(heat * heat));
    o.color = float4(lerp(base, glow, saturate(heat * Glow)), 1);
    o.heat = heat;
    return o;
}
)";

// Keys typed into the window, as the bytes a terminal would send, until someone takes them.
std::string g_typed, g_hostKeys;
// The same keyboard as raw press and release events, and the pointer, for the input device.
std::vector<uint32_t> g_keyEvents;
int g_pointerX = 0, g_pointerY = 0;
unsigned g_buttons = 0;
HWND g_window = nullptr;
bool g_held[128];             // keys down now, by Linux key code
float g_ui = 1;               // the window's dpi over 96
float g_dispShare = (float)kMemoryViewDispW / (kMemoryViewDispW + kMemoryViewMemW);   // the display's part of the width

bool g_imgui = false;
MemoryViewText g_cols;
std::string g_forced;
std::string g_pendingCapture;
RECT g_dispRect{0, 0, 0, 0};
bool g_dispValid = false;
float g_dispShownW = 1, g_dispShownH = 1;
UINT g_viewW = 0, g_viewH = 0;
bool g_freeze = false;
int g_vol = 10;
int g_cores = 256;   // the machine's cores, as the Settings panel wants them (needs a restart)
bool g_soundOff = false;
// Radiation (fault injection) + paint, for the DX12 compute backend (live, no restart).
// Addresses are guest-physical; the backend converts to RAM offsets.
bool g_radEnable = false;
int g_radFlips = 10;   // bit flips a frame, spread over the two ranges
uint32_t g_radR0S = 0x80400000u, g_radR0E = 0x86000000u;   // RAM low
uint32_t g_radR1S = 0x88000000u, g_radR1E = 0x92000000u;   // RAM high
bool g_radProtect = true;   // skip the boot image (the kernel)
uint32_t g_radPaint = ~0u;  // a 16-byte texel to paint this frame, ~0u none
uint32_t g_radPaintVal = 0xffu;
bool g_radArmed = false;    // the cursor paints while held
uint32_t g_radImageBase = 0x80000000u, g_radImageSize = 0;   // the boot image, set by the backend
uint64_t g_radFlipsDone = 0;
uint32_t g_radLastPaint = ~0u;
char g_line[1024]{};
float g_uiScale = 1.0f;
float g_scaleApplied = 1.0f;
int g_memZoom = 1;
float g_memOff = 0.0f;
float g_memGlow = 1.2f;   // heat glow intensity in the memory view (0 off)
// The loupe's region (a square of RAM texels around the cursor), set by the Memory panel.
int g_loupeCX = 1024, g_loupeCY = 64 + 4608, g_loupeN = 64;
uint32_t g_loupeAddr = ~0u;
ID3D11ShaderResourceView* g_gpuShown = nullptr;
float g_stateUv[4]{0, 0, 0, 0};
float g_lastPx = 0, g_lastPy = 0;
bool g_dragArm = false;

static void uiLoadCfg() {
    FILE* f = fopen("logs/imgui_harness.cfg", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        float v = 0;
        int z = 0;
        if (sscanf(line, "ui_scale=%f", &v) == 1) {
            if (v >= 0.5f && v <= 3.0f) g_uiScale = v;
        } else if (sscanf(line, "mem_zoom=%d", &z) == 1) {
            if (z >= 1 && z <= 64) g_memZoom = z;
        } else if (sscanf(line, "mem_off=%f", &v) == 1) {
            if (v >= 0) g_memOff = v;
        } else if (sscanf(line, "mem_glow=%f", &v) == 1) {
            if (v >= 0 && v <= 2.0f) g_memGlow = v;
        } else if (sscanf(line, "cores=%d", &z) == 1) {
            if (z >= 1 && z <= 256) g_cores = z;
        }
    }
    fclose(f);
}

static void uiSaveCfg() {
    FILE* f = fopen("logs/imgui_harness.cfg", "w");
    if (!f) return;
    fprintf(f, "ui_scale=%.3f\nmem_zoom=%d\nmem_off=%.0f\nmem_glow=%.2f\ncores=%d\n", (double)g_uiScale, g_memZoom, (double)g_memOff, (double)g_memGlow, g_cores);
    fclose(f);
}

static void uiApplyScale(float s) {
    float f = s / g_scaleApplied;
    if (f != 1.0f) {
        ImGui::GetStyle().ScaleAllSizes(f);
        ImGui::GetStyle().FontScaleMain = s;
        g_scaleApplied = s;
    }
}

// Windows scan code (set 1, plus the extended flag) to Linux key code. The main block of the
// keyboard has the same numbers in both; the extended keys do not.
unsigned linuxKey(unsigned scan, bool extended) {
    if (!extended) return scan < 0x59 ? scan : 0;
    switch (scan) {
        case 0x1c: return 96;    // keypad Enter
        case 0x1d: return 97;    // right Ctrl
        case 0x35: return 98;    // keypad /
        case 0x38: return 100;   // right Alt
        case 0x47: return 102;   // Home
        case 0x48: return 103;   // Up
        case 0x49: return 104;   // Page Up
        case 0x4b: return 105;   // Left
        case 0x4d: return 106;   // Right
        case 0x4f: return 107;   // End
        case 0x50: return 108;   // Down
        case 0x51: return 109;   // Page Down
        case 0x52: return 110;   // Insert
        case 0x53: return 111;   // Delete
        case 0x5b: return 125;   // left Windows key
        default: return 0;
    }
}

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_imgui && ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 0;
    if (m == WM_CHAR) {
        if (g_imgui) {
            ImGuiIO& io = ImGui::GetIO();
            if (io.WantTextInput) return 0;
        }
        wchar_t ch = (wchar_t)w;
        if (ch == 8) g_typed += '';              // Backspace, as a Linux terminal sends it
        else if (ch < 0x80) g_typed += (char)ch;      // includes Enter (CR) and Ctrl+letter
        else g_typed += narrow(std::wstring(1, ch));
        return 0;
    }
    if ((m == WM_KEYDOWN || m == WM_KEYUP) && (w == VK_F9 || w == VK_F11 || w == VK_F12) && GetKeyState(VK_CONTROL) < 0) {
        if (m == WM_KEYDOWN) g_hostKeys += w == VK_F9 ? 's' : w == VK_F11 ? '-' : '+';
        return 0;
    }
    if (m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) {
        bool down = m == WM_KEYDOWN || m == WM_SYSKEYDOWN, repeat = down && (l & (1 << 30)) != 0;
        unsigned code = linuxKey((unsigned)(l >> 16) & 0xff, (l & (1 << 24)) != 0);
        if (g_imgui) {
            ImGuiIO& io = ImGui::GetIO();
            if (io.WantTextInput) {
                if (code && code < 128) g_held[code] = down;
                if (m == WM_SYSKEYDOWN && w == VK_F4) return DefWindowProcW(h, m, w, l);
                return 0;
            }
        }
        if (code && !repeat) g_keyEvents.push_back(code | (down ? 0x80000000u : 0));   // the guest repeats keys itself
        if (code && code < 128) g_held[code] = down;
        if (m == WM_KEYDOWN) {
            const char* seq = w == VK_UP ? "\x1b[A" : w == VK_DOWN ? "\x1b[B" : w == VK_RIGHT ? "\x1b[C" : w == VK_LEFT ? "\x1b[D" :
                              w == VK_HOME ? "\x1b[H" : w == VK_END ? "\x1b[F" : w == VK_DELETE ? "\x1b[3~" : nullptr;
            if (seq) g_typed += seq;
        }
        // Alt and F10 are the guest's keys, not Windows' menu (which beeps and takes the next ones); Alt+F4 closes
        if (m == WM_SYSKEYDOWN && w == VK_F4) return DefWindowProcW(h, m, w, l);
        return 0;
    }
    if (m == WM_SYSCHAR) return 0;
    if (m == WM_KILLFOCUS) {
        // keys held when the window lost the keyboard are let go, or the guest would hold them
        for (unsigned k = 0; k < 128; ++k)
            if (g_held[k]) g_keyEvents.push_back(k), g_held[k] = false;
    }
    if (m == WM_DPICHANGED) {
        g_ui = HIWORD(w) / 96.0f;
        const RECT* r = (const RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    if (m == WM_MOUSEMOVE || (m >= WM_LBUTTONDOWN && m <= WM_MBUTTONDBLCLK)) {
        g_pointerX = (short)LOWORD(l);
        g_pointerY = (short)HIWORD(l);
        g_buttons = ((w & MK_LBUTTON) ? 1 : 0) | ((w & MK_RBUTTON) ? 2 : 0) | ((w & MK_MBUTTON) ? 4 : 0);
        bool pressed = m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN;
        bool inDisplay = g_imgui && g_dispValid && g_pointerX >= g_dispRect.left && g_pointerX < g_dispRect.right &&
                         g_pointerY >= g_dispRect.top && g_pointerY < g_dispRect.bottom;
        if (pressed && (!g_imgui || inDisplay)) {
            SetCapture(h);
            g_dragArm = true;
        } else if (g_buttons == 0 && m != WM_MOUSEMOVE) ReleaseCapture();
        return 0;
    }
    if (m == WM_MOUSEWHEEL) {
        // a notch is an event of its own in the key ring (docs/machine/input.md): 0x3fe up, 0x3ff down
        static int turned = 0;
        turned += GET_WHEEL_DELTA_WPARAM(w);
        for (; turned >= WHEEL_DELTA; turned -= WHEEL_DELTA) g_keyEvents.push_back(0x800003feu);
        for (; turned <= -WHEEL_DELTA; turned += WHEEL_DELTA) g_keyEvents.push_back(0x800003ffu);
        return 0;
    }
    if (m == WM_CLOSE) {
        // Flag it for render(); the emulator keeps running without the view.
        SetPropW(h, L"closed", (HANDLE)1);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

using Constants = MemoryViewConstants;

}  // namespace

const char* memoryViewShader() { return kShader; }

float memoryViewUi() { return g_ui; }

// Whether a view shows ImGui panels (input goes to ImGui when it wants it).
void memoryViewSetImgui(bool imgui) { g_imgui = imgui; }
// The kept UI settings (scale/zoom/offset), for a view that starts after them.
void memoryViewLoadCfg() { uiLoadCfg(); }
// The view's window size, for the pointer mapping.
void memoryViewSetViewSize(UINT w, UINT h) { g_viewW = w; g_viewH = h; }
// The machine's cores, as the Settings panel wants them (needs a restart).
int memoryViewCoreCount() { return g_cores; }
// The memory view's heat glow intensity (0 off), as the Settings panel wants it.
float memoryViewGlow() { return g_memGlow; }
// The loupe's region (a square of RAM texels around the cursor), set by the Memory panel.
void memoryViewLoupe(int* cx, int* cy, int* n) {
    if (cx) *cx = g_loupeCX;
    if (cy) *cy = g_loupeCY;
    if (n) *n = g_loupeN;
}
// Radiation (fault injection) + paint, for the DX12 compute backend (live, no restart).
bool memoryViewRadiation(MemoryViewRadiation& out) {
    // scripted runs (no one to click the panel): RVC_RAD_FLIPS flips a frame (enables),
    // RVC_RAD_PAINT a guest-physical address to paint every frame, RVC_RAD_PAINTVAL its byte
    static bool envApplied = false;
    if (!envApplied) {
        envApplied = true;
        if (const char* e = getenv("RVC_RAD_FLIPS")) {
            int f = atoi(e);
            if (f < 0) f = 0;
            if (f > 100000) f = 100000;
            if (f > 0) { g_radEnable = true; g_radFlips = f; }
        }
        if (const char* e = getenv("RVC_RAD_PAINT")) {
            unsigned long a = strtoul(e, nullptr, 16);
            if (a >= 0x80000000ul && a <= 0xfffffffful) { g_radArmed = true; g_radPaint = (uint32_t)a; }
        }
        if (const char* e = getenv("RVC_RAD_PAINTVAL")) g_radPaintVal = (uint32_t)strtoul(e, nullptr, 16) & 0xffu;
        if (const char* e = getenv("RVC_RAD_PROTECT")) g_radProtect = atoi(e) != 0;
        if (getenv("RVC_RAD_FLIPS") || getenv("RVC_RAD_PAINT"))
            fprintf(stderr, "[harness] radiation override: flips=%d protect=%d paint=%08X\n",
                    g_radEnable ? g_radFlips : 0, g_radProtect ? 1 : 0, g_radArmed ? g_radPaint : ~0u);
    }
    out.enable = g_radEnable;
    out.flips = g_radEnable ? g_radFlips : 0;
    out.r0s = g_radR0S; out.r0e = g_radR0E;
    out.r1s = g_radR1S; out.r1e = g_radR1E;
    out.protectImage = g_radProtect;
    out.paint = g_radArmed ? g_radPaint : ~0u;
    out.paintVal = g_radPaintVal;
    out.paintArmed = g_radArmed;
    out.imageBase = g_radImageBase; out.imageSize = g_radImageSize;
    out.flipsDone = g_radFlipsDone;
    out.lastPaint = g_radLastPaint;
    // (paint works with radiation off: the cursor is its own switch)
    return out.flips > 0 || out.paint != ~0u;
}
void memoryViewRadiationDone(uint64_t flips) { g_radFlipsDone += flips; }
void memoryViewRadiationImage(uint32_t base, uint32_t size) { g_radImageBase = base; g_radImageSize = size; }
// The cores' decoded state, for the CPU State panel (pushed by the DX12 compute backend).
static std::vector<MemoryViewCore> g_cpuCores;
static int g_cpuSel = 0;
void memoryViewSetCores(const MemoryViewCore* cores, int n) {
    if (!cores || n <= 0) { g_cpuCores.clear(); return; }
    g_cpuCores.assign(cores, cores + n);
    if (g_cpuSel >= n) g_cpuSel = 0;
}
int memoryViewCpuCount() { return (int)g_cpuCores.size(); }
bool memoryViewCpuState(int core, MemoryViewCore& out) {
    if (core < 0 || core >= (int)g_cpuCores.size()) return false;
    out = g_cpuCores[(size_t)core];
    return true;
}
static const char* cpuModeName(uint32_t m) {
    switch (m) {
    case 0: return "parked";
    case 1: return "running";
    case 2: return "asleep";
    case 3: return "stopped fault";
    case 4: return "stopped call";
    default: return "?";
    }
}
static char cpuPrivName(uint32_t p) { return p == 0 ? 'U' : p == 1 ? 'S' : p == 3 ? 'M' : '?'; }
// The CPU State panel: every core's mode/pc/instructions plus the selected core's
// registers. Shared by both views.
static void cpuStatePanel() {
    if (!ImGui::Begin("CPU State")) { ImGui::End(); return; }
    int n = (int)g_cpuCores.size();
    if (n <= 0) {
        ImGui::TextWrapped("Detailed state needs the DX12 compute machine (the default).");
        ImGui::End();
        return;
    }
    if (g_cpuSel < 0) g_cpuSel = 0;
    if (g_cpuSel >= n) g_cpuSel = n - 1;
    ImGui::PushItemWidth(-1);
    if (ImGui::SliderInt("##core", &g_cpuSel, 0, n - 1, "core %d")) {}
    ImGui::PopItemWidth();
    const MemoryViewCore& c = g_cpuCores[(size_t)g_cpuSel];
    ImGui::Text("core %d: %s", g_cpuSel, cpuModeName(c.mode));
    ImGui::SameLine();
    ImGui::Text("pc %08X", c.pc);
    ImGui::SameLine();
    ImGui::Text("priv %c (%u)", cpuPrivName(c.priv), c.priv);
    ImGui::Text("%llu instructions", (unsigned long long)c.instr);
    ImGui::SameLine();
    ImGui::Text("timer %08X%08X", c.tcmpHi, c.tcmpLo);
    if (c.mode == 2) ImGui::Text("sleeps on job %08X", c.job);
    if (c.mode >= 3) ImGui::Text("stopped with %06X", c.seen & 0xffffffu);
    if (ImGui::BeginChild("regs", ImVec2(0, 148), false, ImGuiWindowFlags_HorizontalScrollbar)) {
        for (int r = 0; r < 32; r += 4) {
            ImGui::Text("x%-2d %08X  x%-2d %08X  x%-2d %08X  x%-2d %08X", r, c.r[r], r + 1, c.r[r + 1],
                        r + 2, c.r[r + 2], r + 3, c.r[r + 3]);
        }
    }
    ImGui::EndChild();
    if (ImGui::BeginChild("fregs", ImVec2(0, 148), false, ImGuiWindowFlags_HorizontalScrollbar)) {
        for (int r = 0; r < 32; r += 4) {
            ImGui::Text("f%-2d %08X  f%-2d %08X  f%-2d %08X  f%-2d %08X", r, c.r[32 + r], r + 1, c.r[32 + r + 1],
                        r + 2, c.r[32 + r + 2], r + 3, c.r[32 + r + 3]);
        }
    }
    ImGui::EndChild();
    if (ImGui::BeginChild("csrs", ImVec2(0, 86), false, ImGuiWindowFlags_HorizontalScrollbar)) {
        for (int r = 0; r < 16; r += 4) {
            ImGui::Text("csr%-2d %08X  csr%-2d %08X  csr%-2d %08X  csr%-2d %08X", r, c.csr[r], r + 1, c.csr[r + 1],
                        r + 2, c.csr[r + 2], r + 3, c.csr[r + 3]);
        }
    }
    ImGui::EndChild();
    if (ImGui::BeginChild("cores", ImVec2(0, 0), false)) {
        ImGuiListClipper clipper;
        clipper.Begin(n);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const MemoryViewCore& k = g_cpuCores[(size_t)i];
                char buf[64];
                snprintf(buf, sizeof buf, "%3d %-13s %08X %10llu", i, cpuModeName(k.mode), k.pc,
                         (unsigned long long)k.instr);
                if (ImGui::Selectable(buf, i == g_cpuSel)) g_cpuSel = i;
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();
}
// The Console panel's scrollback: complete parsed lines plus the partial current one.
// (The overlay's 8-line snapshot stays as is for the info bar.)
static std::vector<std::string> g_conLines;
static std::string g_conCur;
static const size_t kConKeep = 5000;   // lines of scrollback
struct ConWrapped { size_t orig; std::string text; };   // a displayed row (a whole line or a wrap chunk)
static std::vector<ConWrapped> g_conWrapped;
static bool g_conDirty = true;   // lines/params changed: rebuild wrapped, restick to bottom
static bool g_conAuto = true;
static bool g_conWrap = true;
static int g_conWrapCol = 120;
static float g_conScale = 1.0f;
static int g_conSelA = -1, g_conSelB = -1;   // selected wrapped rows [min, max], -1 none
static size_t conTotal() { return g_conLines.size() + (g_conCur.empty() ? 0 : 1); }
static const std::string& conLine(size_t i) { return i < g_conLines.size() ? g_conLines[i] : g_conCur; }
void memoryViewAppendConsole(const std::string& delta) {
    // (the overlay's parsing, per delta: escapes stripped, \n new line, \t spaces, \b backspace)
    for (size_t k = 0; k < delta.size(); ++k) {
        unsigned char ch = (unsigned char)delta[k];
        if (ch == 0x1b) {
            if (k + 1 < delta.size() && delta[k + 1] == '[') {
                for (k += 2; k < delta.size() && !isalpha((unsigned char)delta[k]); ++k) {}
            } else if (k + 1 < delta.size() && (delta[k + 1] == '(' || delta[k + 1] == ')')) {
                k += 2;
            } else if (k + 1 < delta.size()) {
                k += 1;
            }
        } else if (ch == '\n') {
            g_conLines.push_back(g_conCur);
            g_conCur.clear();
            if (g_conCur.capacity() > 65536) g_conCur.shrink_to_fit();
        } else if (ch == '\t') {
            g_conCur += "    ";
        } else if (ch == '\b') {
            if (!g_conCur.empty()) g_conCur.pop_back();
        } else if (ch >= 0x20 && ch < 0x7f) {
            g_conCur += (char)ch;
            if (g_conCur.size() > 65536) { g_conLines.push_back(g_conCur); g_conCur.clear(); }
        }
    }
    if (g_conLines.size() > kConKeep) {
        g_conLines.erase(g_conLines.begin(), g_conLines.begin() + (g_conLines.size() - kConKeep));
        g_conSelA = g_conSelB = -1;   // indices shifted: drop the selection
    }
    g_conDirty = true;
}
static void conRebuild() {
    g_conWrapped.clear();
    size_t total = conTotal();
    for (size_t i = 0; i < total; ++i) {
        const std::string& l = conLine(i);
        if (!g_conWrap || (int)l.size() <= g_conWrapCol || g_conWrapCol < 16) {
            g_conWrapped.push_back({i, l});
        } else {
            for (size_t o = 0; o < l.size(); o += (size_t)g_conWrapCol)
                g_conWrapped.push_back({i, l.substr(o, (size_t)g_conWrapCol)});
        }
    }
    g_conDirty = false;
}
static void conCopySelection() {
    if (g_conSelA < 0 || g_conWrapped.empty()) return;
    int a = g_conSelA < g_conSelB ? g_conSelA : g_conSelB;
    int b = g_conSelA < g_conSelB ? g_conSelB : g_conSelA;
    if (a < 0) a = 0;
    if (b >= (int)g_conWrapped.size()) b = (int)g_conWrapped.size() - 1;
    std::string out;
    size_t lastOrig = (size_t)-1;
    for (int i = a; i <= b; ++i) {
        size_t o = g_conWrapped[(size_t)i].orig;
        if (o == lastOrig) continue;   // a wrapped line copies once, whole
        lastOrig = o;
        if (!out.empty()) out += "\n";
        out += conLine(o);
    }
    ImGui::SetClipboardText(out.c_str());
}
float memoryViewUiScale() { return g_uiScale; }
void memoryViewApplyScale(float s) { uiApplyScale(s); }
// A BMP the Machine panel asked for, since the last call (empty: none).
std::string memoryViewTakePendingCapture() {
    std::string path;
    path.swap(g_pendingCapture);
    return path;
}
// The panels' text columns (Machine, Console, input), with the volume the first one names.
void memoryViewSetColumns(const MemoryViewText& columns) {
    g_cols = columns;
    if (!columns.empty()) {
        for (auto& line : columns[0]) {
            size_t p = line.find("volume ");
            if (p != std::string::npos) {
                int v = atoi(line.c_str() + p + 7);
                if (v >= 0 && v <= 100) g_vol = v;
            }
            if (line.find("sound off") != std::string::npos) g_soundOff = true;
            if (line.find("sound playing") != std::string::npos || line.find("sound idle") != std::string::npos ||
                line.find("sound not played") != std::string::npos)
                g_soundOff = false;
        }
    }
}

UINT memoryViewBar() { return (UINT)(kMemoryViewBar * g_ui + 0.5f); }

float memoryViewSplit(UINT width) { return floorf(width * g_dispShare); }

// The Console panel: the guest's full log with scrollback, selectable lines (click,
// Shift+click, Ctrl+C or the Copy button), text scaling and wrapping. Shared by both views.
static void consolePanel() {
    if (!ImGui::Begin("Console")) { ImGui::End(); return; }
    if (ImGui::Checkbox("Autoscroll", &g_conAuto)) { if (g_conAuto) g_conDirty = true; }
    ImGui::SameLine();
    if (ImGui::Checkbox("Wrap", &g_conWrap)) { g_conSelA = g_conSelB = -1; g_conDirty = true; }
    if (g_conWrap) {
        ImGui::SameLine();
        ImGui::PushItemWidth(120);
        if (ImGui::SliderInt("column", &g_conWrapCol, 40, 240)) {
            if (g_conWrapCol < 16) g_conWrapCol = 16;
            g_conSelA = g_conSelB = -1;
            g_conDirty = true;
        }
        ImGui::PopItemWidth();
    }
    ImGui::SameLine();
    ImGui::PushItemWidth(120);
    ImGui::SliderFloat("text", &g_conScale, 0.5f, 2.0f, "%.2fx");
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Copy")) conCopySelection();
    ImGui::SameLine();
    if (ImGui::Button("Copy all")) {
        std::string out;
        size_t total = conTotal();
        for (size_t i = 0; i < total; ++i) {
            if (i) out += "\n";
            out += conLine(i);
        }
        ImGui::SetClipboardText(out.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        g_conLines.clear();
        g_conCur.clear();
        g_conWrapped.clear();
        g_conSelA = g_conSelB = -1;
        g_conDirty = true;
    }
    ImGui::SameLine();
    ImGui::Text("%u lines", (unsigned)conTotal());
    bool newLines = g_conDirty;
    if (g_conDirty) conRebuild();
    float inputH = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("conscroll", ImVec2(0, -inputH), false, ImGuiWindowFlags_HorizontalScrollbar);
    float prevScale = ImGui::GetStyle().FontScaleMain;
    ImGui::GetStyle().FontScaleMain = prevScale * g_conScale;
    ImGuiListClipper clipper;
    clipper.Begin((int)g_conWrapped.size());
    int selMin = -1, selMax = -1;
    if (g_conSelA >= 0 && !g_conWrapped.empty()) {
        selMin = g_conSelA < g_conSelB ? g_conSelA : g_conSelB;
        selMax = g_conSelA < g_conSelB ? g_conSelB : g_conSelA;
        if (selMin < 0) selMin = 0;
        if (selMax >= (int)g_conWrapped.size()) selMax = (int)g_conWrapped.size() - 1;
    }
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            bool sel = selMin >= 0 && i >= selMin && i <= selMax;
            std::string label = g_conWrapped[(size_t)i].text;
            if (label.empty()) label = "##con" + std::to_string(i);
            else label += "##con" + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), sel)) {
                if (ImGui::GetIO().KeyShift && g_conSelA >= 0) g_conSelB = i;
                else { g_conSelA = i; g_conSelB = i; }
            }
        }
    }
    // (Ctrl+C copies the selection unless typing in the input line below)
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) conCopySelection();
    if (g_conAuto && g_conSelA < 0 && newLines) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::GetStyle().FontScaleMain = prevScale;
    ImGui::PushItemWidth(-1);
    if (ImGui::InputText("##line", g_line, sizeof g_line, ImGuiInputTextFlags_EnterReturnsTrue)) {
        g_forced.append(g_line);
        g_forced.push_back('\r');
        g_line[0] = 0;
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::PopItemWidth();
    ImGui::End();
}
// Paint: a Memory image's mouse position to a guest-physical RAM address (a 16-byte texel),
// for the full-RAM view (2 strips side by side, MemOff 0). ~0u when outside RAM.
// (mx,my): the mouse; (x0,y0,x1,y1): the image's rect; half: 0 the left half (u0..um),
// 1 the right (um..1.0); splitPx/barPx/memPx, viewW/viewH, texWidth/stateRows/ramRows: the layout.
static uint32_t memoryPaintAddress(float mx, float my, float x0, float y0, float x1, float y1,
        int half, float splitPx, float memPx, UINT viewW, UINT viewH, UINT texWidth, UINT stateRows, UINT ramRows) {
    if (x1 <= x0 || y1 <= y0) return ~0u;
    float f = (mx - x0) / (x1 - x0), g = (my - y0) / (y1 - y0);
    if (f < 0 || f > 1 || g < 0 || g > 1) return ~0u;
    float memWidth = (float)viewW - splitPx;
    if (memWidth <= 0 || memPx <= 0 || texWidth == 0 || ramRows == 0) return ~0u;
    float u0 = splitPx / (float)viewW, um = u0 + (1.0f - u0) * 0.5f;
    float v1 = memPx / (float)viewH;
    float u = half == 0 ? u0 + f * (um - u0) : um + f * (1.0f - um);
    float v = g * v1;
    float sx = ((u * (float)viewW) - splitPx) / memWidth * 2.0f;
    int strip = (int)sx;
    if (strip < 0) strip = 0;
    if (strip > 1) strip = 1;
    float frac = sx - (float)strip;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    float rowsPerStrip = (float)ramRows / 2.0f;
    int tx = (int)(frac * (float)texWidth);
    int ty = (int)((float)stateRows + ((float)strip + v * (float)viewH / memPx) * rowsPerStrip);
    if (tx < 0) tx = 0;
    if (tx >= (int)texWidth) tx = (int)texWidth - 1;
    if (ty < (int)stateRows) ty = (int)stateRows;
    if (ty >= (int)(stateRows + ramRows)) ty = (int)(stateRows + ramRows) - 1;
    uint32_t index = (uint32_t)(ty - (int)stateRows) * texWidth + (uint32_t)tx;
    return 0x80000000u + index * 16;
}

std::string memoryViewTakeHostKeys() {
    std::string keys;
    keys.swap(g_hostKeys);
    return keys;
}

std::string memoryViewTakeKeys() {
    std::string keys;
    keys.swap(g_forced);
    if (g_imgui) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput) {
            g_typed.clear();
            return keys;
        }
    }
    std::string typed;
    typed.swap(g_typed);
    keys += typed;
    return keys;
}

MemoryViewInput memoryViewTakeInput() {
    MemoryViewInput in;
    if (g_imgui) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput) g_keyEvents.clear();
    }
    in.keys.swap(g_keyEvents);
    if (g_imgui && g_dispValid) {
        float split = memoryViewSplit(g_viewW);
        float memH = (float)g_viewH - (float)memoryViewBar();
        bool inside = g_pointerX >= g_dispRect.left && g_pointerX < g_dispRect.right &&
                      g_pointerY >= g_dispRect.top && g_pointerY < g_dispRect.bottom;
        if (inside || (g_buttons && g_dragArm)) {
            float lx = (float)(g_pointerX - g_dispRect.left);
            float ly = (float)(g_pointerY - g_dispRect.top);
            float dw = g_dispShownW > 0 ? g_dispShownW : 1;
            float dh = g_dispShownH > 0 ? g_dispShownH : 1;
            if (lx < 0) lx = 0;
            if (lx > dw) lx = dw;
            if (ly < 0) ly = 0;
            if (ly > dh) ly = dh;
            g_lastPx = lx * split / dw;
            g_lastPy = ly * memH / dh;
        }
        if (!g_buttons) g_dragArm = false;
        in.x = g_lastPx;
        in.y = g_lastPy;
        in.buttons = (inside || g_dragArm) ? g_buttons : 0;
        in.panelW = split;
        in.panelH = memH;
        return in;
    }
    RECT rc{};
    if (!g_window || !IsWindow(g_window) || !GetClientRect(g_window, &rc) || rc.bottom <= (LONG)memoryViewBar()) return in;
    in.x = (float)g_pointerX;
    in.y = (float)g_pointerY;
    in.buttons = g_buttons;
    in.panelW = memoryViewSplit((UINT)rc.right);   // as the views' `split`
    in.panelH = (float)(rc.bottom - (LONG)memoryViewBar());
    return in;
}

HWND memoryViewCreateWindow() {
    // the window's pixels are the screen's: Windows stretched a window of a process that does
    // not say so, and the display came out blurred
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_ui = GetDpiForSystem() / 96.0f;
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"ShaderEmuMemoryView";
    RegisterClassW(&wc);
    // the display panel as large as a 4:3 desktop fits, the memory strips beside it, the info
    // bar below (the picture fills the panel, resampled; gpu.h maps the pointer the same way)
    RECT work{0, 0, 1920, 1080};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    LONG bar = (LONG)memoryViewBar(), frame = (LONG)(48 * g_ui), side = (LONG)(24 * g_ui), memLeast = (LONG)(320 * g_ui);
    LONG roomW = work.right - work.left - side, roomH = work.bottom - work.top - frame - bar;
    LONG panelH = roomH, panelW = (std::min)(roomW - memLeast, panelH * 4 / 3);
    LONG memW = (std::max)(memLeast, (std::min)(roomW - panelW, panelH * 3 / 4));
    g_dispShare = (float)panelW / (panelW + memW) + 0.5f / (panelW + memW);
    RECT r{0, 0, panelW + memW, panelH + bar};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForSystem());
    LONG x = work.left + ((work.right - work.left) - (r.right - r.left)) / 2, y = work.top;
    return g_window = CreateWindowExW(0, wc.lpszClassName, L"memory", WS_OVERLAPPEDWINDOW, (std::max)(work.left, x), y,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
}

bool memoryViewWriteBmp(const std::string& path, UINT width, UINT height, const uint8_t* rgba, size_t pitch) {
    std::vector<uint8_t> file(54 + (size_t)width * height * 4, 0);
    uint8_t* h = file.data();
    auto put32 = [&](size_t off, uint32_t v) { memcpy(h + off, &v, 4); };
    h[0] = 'B'; h[1] = 'M';
    put32(2, (uint32_t)file.size());
    put32(10, 54);
    put32(14, 40);
    put32(18, width);
    put32(22, (uint32_t)(-(int32_t)height));  // top-down
    h[26] = 1; h[28] = 32;
    for (UINT y = 0; y < height; ++y) {
        const uint8_t* src = rgba + (size_t)y * pitch;
        uint8_t* dst = h + 54 + (size_t)y * width * 4;
        for (UINT x = 0; x < width; ++x) {  // RGBA -> BGRA
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = 255;
        }
    }
    return writeFileBinary(path, file.data(), file.size());
}

bool memoryViewText(const MemoryViewText& columns, std::vector<uint8_t>& out, UINT& usedW, UINT& usedH) {
    const UINT kW = kMemoryViewTextW, kH = kMemoryViewTextH, kLine = (UINT)(19 * g_ui + 0.5f);
    bool ok = false;
    // White text on black in a memory bitmap; the shader uses one channel as coverage.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)kW;
    bi.bmiHeader.biHeight = -(LONG)kH;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HFONT font = CreateFontW(-(int)(15 * g_ui + 0.5f), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (dc && bmp && font && bits) {
        HGDIOBJ oldBmp = SelectObject(dc, bmp), oldFont = SelectObject(dc, font);
        memset(bits, 0, (size_t)kW * kH * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        SIZE m{};
        GetTextExtentPoint32W(dc, L"M", 1, &m);
        int charW = m.cx > 0 ? m.cx : 8;
        UINT rows = 0;
        for (size_t col = 0; col < columns.size() && col < 3; ++col) {
            UINT n = 0;
            for (auto& line : columns[col]) {
                if ((n + 1) * kLine > kH) break;
                std::wstring w = widen(line);
                TextOutW(dc, (int)(kMemoryViewColumnX[col] * charW), (int)(n * kLine), w.c_str(), (int)w.size());
                ++n;
            }
            if (n > rows) rows = n;
        }
        GdiFlush();
        out.assign((const uint8_t*)bits, (const uint8_t*)bits + (size_t)kW * kH * 4);
        ok = true;
        usedW = kW;
        usedH = rows * kLine;
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBmp);
    }
    if (font) DeleteObject(font);
    if (bmp) DeleteObject(bmp);
    if (dc) DeleteDC(dc);
    return ok;
}

bool MemoryView::init(ID3D11Device* dev, UINT texWidth, UINT texHeight, UINT stateRows, std::string& err) {
    dev_ = dev;
    texWidth_ = texWidth;
    texHeight_ = texHeight;
    stateRows_ = stateRows;

    hwnd_ = memoryViewCreateWindow();
    if (!hwnd_) {
        err = "cannot create the memory view window";
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory> factory;
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = hwnd_;
    sd.Windowed = TRUE;
    HRESULT hr = dev_.As(&dxgiDev);
    if (SUCCEEDED(hr)) hr = dxgiDev->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateSwapChain(dev_.Get(), &sd, &swap_);
    if (SUCCEEDED(hr)) factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(hr)) {
        err = "memory view swapchain: " + hrToString(hr);
        return false;
    }

    ComPtr<ID3DBlob> vsb, psb, errors;
    hr = D3DCompile(kShader, strlen(kShader), "memview", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &errors);
    if (SUCCEEDED(hr)) hr = D3DCompile(kShader, strlen(kShader), "memview", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &errors);
    if (FAILED(hr)) {
        err = "memory view shader: " + (errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : hrToString(hr));
        return false;
    }
    dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
    dev_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (sizeof(Constants) + 15) & ~15u;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev_->CreateBuffer(&bd, nullptr, &cb_);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    dev_->CreateRasterizerState(&rd, &raster_);

    D3D11_TEXTURE2D_DESC rd2{};
    rd2.Width = texWidth;
    rd2.Height = texHeight;
    rd2.MipLevels = rd2.ArraySize = 1;
    rd2.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    rd2.SampleDesc.Count = 1;
    rd2.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = dev_->CreateTexture2D(&rd2, nullptr, &refTex_);
    if (SUCCEEDED(hr)) hr = dev_->CreateShaderResourceView(refTex_.Get(), nullptr, &refSrv_);
    if (FAILED(hr)) {
        err = "memory view reference texture: " + hrToString(hr);
        return false;
    }

    if (!createTargets(err)) return false;
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);  // never takes the keyboard from the console
    CreateDirectoryA("logs", nullptr);
    uiLoadCfg();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "logs/imgui_harness.ini";
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    uiApplyScale(g_uiScale);
    ComPtr<ID3D11DeviceContext> imm;
    dev_->GetImmediateContext(&imm);
    if (ImGui_ImplWin32_Init(hwnd_) && ImGui_ImplDX11_Init(dev_.Get(), imm.Get())) {
        imgui_ = true;
        g_imgui = true;
    }
    return true;
}

bool MemoryView::createTargets(std::string& err) {
    backRtv_.Reset();
    backTex_.Reset();
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    width_ = (UINT)(rc.right > 1 ? rc.right : 1);
    height_ = (UINT)(rc.bottom > 1 ? rc.bottom : 1);
    HRESULT hr = swap_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(hr)) hr = swap_->GetBuffer(0, IID_PPV_ARGS(&backTex_));
    if (SUCCEEDED(hr)) hr = dev_->CreateRenderTargetView(backTex_.Get(), nullptr, &backRtv_);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width_;
    td.Height = height_;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16_FLOAT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    for (int i = 0; i < 2 && SUCCEEDED(hr); ++i) {
        heatTex_[i].Reset(); heatSrv_[i].Reset(); heatRtv_[i].Reset();
        hr = dev_->CreateTexture2D(&td, nullptr, &heatTex_[i]);
        if (SUCCEEDED(hr)) hr = dev_->CreateShaderResourceView(heatTex_[i].Get(), nullptr, &heatSrv_[i]);
        if (SUCCEEDED(hr)) hr = dev_->CreateRenderTargetView(heatTex_[i].Get(), nullptr, &heatRtv_[i]);
    }
    if (SUCCEEDED(hr)) {
        viewTex_.Reset(); viewSrv_.Reset(); viewRtv_.Reset();
        D3D11_TEXTURE2D_DESC vd{};
        vd.Width = width_;
        vd.Height = height_;
        vd.MipLevels = vd.ArraySize = 1;
        vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        vd.SampleDesc.Count = 1;
        vd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        hr = dev_->CreateTexture2D(&vd, nullptr, &viewTex_);
        if (SUCCEEDED(hr)) hr = dev_->CreateShaderResourceView(viewTex_.Get(), nullptr, &viewSrv_);
        if (SUCCEEDED(hr)) hr = dev_->CreateRenderTargetView(viewTex_.Get(), nullptr, &viewRtv_);
        g_viewW = width_;
        g_viewH = height_;
    }
    if (FAILED(hr)) {
        err = "memory view targets: " + hrToString(hr);
        return false;
    }
    heatNeedsClear_ = true;
    return true;
}

// The docked panels both memory views show, after the backend's NewFrame and before its
// Render (memview.h). Backend-agnostic: the textures arrive as ImTextureID (a D3D11 SRV
// or a D3D12 GPU descriptor handle).
void memoryViewPanels(void* view, void* heat, void* gpu, void* loupe, UINT width, UINT height,
                      UINT texH, UINT stateRows, UINT texWidth) {
    static bool dockBuilt = false;
    ImGuiID dockId = 0;
    {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        ImGui::Begin("HarnessDockHost", nullptr, hostFlags);
        ImGui::PopStyleVar(3);
        dockId = ImGui::GetID("HarnessDock");
        if (!dockBuilt) {
            dockBuilt = true;
            if (!std::filesystem::exists("logs/imgui_harness.ini")) {
                ImGui::DockBuilderRemoveNode(dockId);
                ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockId, vp->Size);
                ImGuiID right = 0, bottom = 0, left = 0;
                ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Right, 0.30f, &right, &left);
                ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.32f, &bottom, &left);
                ImGuiID bottomLeft = 0, bottomRight = 0;
                ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.45f, &bottomRight, &bottomLeft);
                ImGui::DockBuilderDockWindow("Display", left);
                ImGui::DockBuilderDockWindow("Memory", right);
                ImGui::DockBuilderDockWindow("Heat", right);
                ImGui::DockBuilderDockWindow("Loupe", right);
                ImGui::DockBuilderDockWindow("CPU State", right);
                ImGui::DockBuilderDockWindow("GPU Target", right);
                ImGui::DockBuilderDockWindow("Console", bottomLeft);
                ImGui::DockBuilderDockWindow("Machine", bottomRight);
                ImGui::DockBuilderDockWindow("Settings", bottomRight);
                ImGui::DockBuilderDockWindow("Radiation", bottomRight);
                ImGui::DockBuilderFinish(dockId);
            }
        }
        ImGui::DockSpace(dockId);
        ImGui::End();
    }
    float splitPx = memoryViewSplit(width);
    float barPx = (float)memoryViewBar();
    float memPx = (float)height - barPx;
    float barRowsF = (float)(texH - stateRows);
    float insetPx = barPx - 16 * g_ui;
    if (insetPx > 0) {
        float bx0 = (float)width - insetPx - 12 * g_ui;
        float by0 = memPx + (barPx - insetPx) * 0.5f;
        g_stateUv[0] = bx0 / (float)width;
        g_stateUv[1] = by0 / (float)height;
        g_stateUv[2] = (bx0 + insetPx) / (float)width;
        g_stateUv[3] = (by0 + insetPx) / (float)height;
    }
    bool dispOpen = ImGui::Begin("Display");
    if (dispOpen) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float s = (std::min)(avail.x / splitPx, avail.y / memPx);
        if (s > 16) s = 16;
        if (s > 0 && splitPx > 0 && memPx > 0) {
            ImVec2 size(splitPx * s, memPx * s);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - size.x) * 0.5f);
            ImGui::Image((ImTextureID)view, size, ImVec2(0, 0), ImVec2(splitPx / (float)width, memPx / (float)height));
            ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
            POINT p0{(LONG)mn.x, (LONG)mn.y}, p1{(LONG)mx.x, (LONG)mx.y};
            ScreenToClient(g_window, &p0);
            ScreenToClient(g_window, &p1);
            g_dispRect.left = p0.x;
            g_dispRect.top = p0.y;
            g_dispRect.right = p1.x;
            g_dispRect.bottom = p1.y;
            g_dispShownW = size.x;
            g_dispShownH = size.y;
            g_dispValid = true;
        } else g_dispValid = false;
    } else g_dispValid = false;
    ImGui::End();
    bool memOpen = ImGui::Begin("Memory");
    if (memOpen) {
        float mw = (float)width - splitPx;
        float totalRows = (float)(texH - stateRows);
        float halfSpan = totalRows / (float)(g_memZoom > 0 ? g_memZoom : 1) * 0.5f;
        ImGui::BeginChild("ramScroll", ImVec2(0, 0), false);
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float s = avail.x / (mw * 0.5f);
        if (s > 0 && mw > 0 && memPx > 0) {
            float u0 = splitPx / (float)width;
            float um = u0 + (1.0f - u0) * 0.5f;
            float v1 = memPx / (float)height;
            ImVec2 size(avail.x, avail.x * memPx / (mw * 0.5f));
            ImGui::Text("rows %.0f..%.0f", (double)g_memOff, (double)(g_memOff + halfSpan));
            ImGui::Image((ImTextureID)view, size, ImVec2(u0, 0), ImVec2(um, v1));
            {
                // the loupe follows the cursor (its square of texels is rendered separately)
                ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax(), mp = ImGui::GetMousePos();
                if (mp.x >= mn.x && mp.x <= mx.x && mp.y >= mn.y && mp.y <= mx.y) {
                    uint32_t a = memoryPaintAddress(mp.x, mp.y, mn.x, mn.y, mx.x, mx.y, 0,
                        splitPx, memPx, width, height, texWidth, stateRows, texH - stateRows);
                    if (a != ~0u) {
                        uint32_t idx = (a - 0x80000000u) / 16;
                        g_loupeCX = (int)(idx % texWidth);
                        g_loupeCY = (int)(stateRows + idx / texWidth);
                        g_loupeAddr = a;
                    }
                }
            }
            if (g_radArmed && ImGui::IsMouseDown(0)) {
                ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax(), mp = ImGui::GetMousePos();
                if (mp.x >= mn.x && mp.x <= mx.x && mp.y >= mn.y && mp.y <= mx.y) {
                    uint32_t a = memoryPaintAddress(mp.x, mp.y, mn.x, mn.y, mx.x, mx.y, 0,
                        splitPx, memPx, width, height, texWidth, stateRows, texH - stateRows);
                    if (a != ~0u) { g_radPaint = a; g_radLastPaint = a; }
                }
            }
            ImGui::Text("rows %.0f..%.0f", (double)(g_memOff + halfSpan), (double)(g_memOff + halfSpan * 2));
            ImGui::Image((ImTextureID)view, size, ImVec2(um, 0), ImVec2(1.0f, v1));
            {
                ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax(), mp = ImGui::GetMousePos();
                if (mp.x >= mn.x && mp.x <= mx.x && mp.y >= mn.y && mp.y <= mx.y) {
                    uint32_t a = memoryPaintAddress(mp.x, mp.y, mn.x, mn.y, mx.x, mx.y, 1,
                        splitPx, memPx, width, height, texWidth, stateRows, texH - stateRows);
                    if (a != ~0u) {
                        uint32_t idx = (a - 0x80000000u) / 16;
                        g_loupeCX = (int)(idx % texWidth);
                        g_loupeCY = (int)(stateRows + idx / texWidth);
                        g_loupeAddr = a;
                    }
                }
            }
            if (g_radArmed && ImGui::IsMouseDown(0)) {
                ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax(), mp = ImGui::GetMousePos();
                if (mp.x >= mn.x && mp.x <= mx.x && mp.y >= mn.y && mp.y <= mx.y) {
                    uint32_t a = memoryPaintAddress(mp.x, mp.y, mn.x, mn.y, mx.x, mx.y, 1,
                        splitPx, memPx, width, height, texWidth, stateRows, texH - stateRows);
                    if (a != ~0u) { g_radPaint = a; g_radLastPaint = a; }
                }
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
    if (ImGui::Begin("Heat")) {
        ImGui::BeginChild("heatScroll", ImVec2(0, 0), false);
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float mw = (float)width - splitPx;
        if (avail.x > 0 && mw > 0 && memPx > 0) {
            float u0 = splitPx / (float)width;
            float um = u0 + (1.0f - u0) * 0.5f;
            float v1 = memPx / (float)height;
            ImVec2 size(avail.x, avail.x * memPx / (mw * 0.5f));
            ImGui::Image((ImTextureID)heat, size, ImVec2(u0, 0), ImVec2(um, v1));
            ImGui::Image((ImTextureID)heat, size, ImVec2(um, 0), ImVec2(1.0f, v1));
        }
        ImGui::EndChild();
    }
    ImGui::End();
    if (ImGui::Begin("Loupe")) {
        if (loupe) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            float s = avail.x < 256.0f ? avail.x : 256.0f;
            if (s > 0) ImGui::Image((ImTextureID)loupe, ImVec2(s, s), ImVec2(0, 0), ImVec2(1, 1));
        } else {
            ImGui::TextWrapped("No loupe (needs the DX12 view).");
        }
        if (g_loupeAddr != ~0u) ImGui::Text("0x%08X", g_loupeAddr);
        else ImGui::Text("hover Memory to aim");
        ImGui::PushItemWidth(-1);
        ImGui::SliderInt("##loupeN", &g_loupeN, 16, 128, "%d texels");
        if (g_loupeN < 16) g_loupeN = 16;
        if (g_loupeN > 128) g_loupeN = 128;
        ImGui::PopItemWidth();
    }
    ImGui::End();
    cpuStatePanel();
    if (ImGui::Begin("GPU Target")) {
        if (gpu) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            if (avail.x > 0) ImGui::Image((ImTextureID)gpu, ImVec2(avail.x, avail.x), ImVec2(0, 0), ImVec2(1, 1));
        } else {
            ImGui::Text("no GPU target");
        }
    }
    ImGui::End();
    consolePanel();
    if (ImGui::Begin("Machine")) {
        if (!g_cols.empty())
            for (auto& l : g_cols[0]) ImGui::TextUnformatted(l.c_str());
        if (g_cols.size() > 2)
            for (auto& l : g_cols[2]) ImGui::TextUnformatted(l.c_str());
        ImGui::Checkbox("Freeze view", &g_freeze);
        if (ImGui::Button("Capture view BMP")) g_pendingCapture = "logs/imgui_view.bmp";
        if (ImGui::Button("Reset layout")) {
            DeleteFileA("logs/imgui_harness.ini");
            ImGui::DockBuilderRemoveNode(dockId);
            dockBuilt = false;
        }
    }
    ImGui::End();
    if (ImGui::Begin("Settings")) {
        float scale = g_uiScale;
        if (ImGui::SliderFloat("UI scale", &scale, 0.5f, 2.0f, "%.2f")) {
            g_uiScale = scale;
            uiApplyScale(g_uiScale);
            uiSaveCfg();
        }
        bool sound = !g_soundOff;
        if (ImGui::Checkbox("Sound", &sound)) g_hostKeys.push_back('s');
        int vol = g_vol;
        if (ImGui::SliderInt("Volume", &vol, 0, 100)) {
            int steps = (vol - g_vol + (vol > g_vol ? 2 : -2)) / 5;
            for (int i = 0; i < abs(steps); ++i) g_hostKeys.push_back(vol > g_vol ? '+' : '-');
            g_vol = vol;
        }
        float spanF = barRowsF / (float)(g_memZoom > 0 ? g_memZoom : 1);
        if (spanF < 1) spanF = 1;
        float maxOffSet = barRowsF - spanF;
        if (maxOffSet < 0) maxOffSet = 0;
        if (ImGui::SliderInt("Memory zoom", &g_memZoom, 1, 64)) {
            if (g_memOff > maxOffSet) g_memOff = maxOffSet;
            uiSaveCfg();
        }
        if (ImGui::SliderFloat("Memory offset", &g_memOff, 0, maxOffSet, "%.0f")) uiSaveCfg();
        if (ImGui::Button("Memory 1:1")) {
            g_memZoom = 1;
            g_memOff = 0;
            uiSaveCfg();
        }
        ImGui::Text("rows %.0f..%.0f of %.0f", (double)g_memOff, (double)(g_memOff + spanF), (double)barRowsF);
        if (ImGui::SliderFloat("Memory glow", &g_memGlow, 0.0f, 2.0f, "%.2f")) uiSaveCfg();
        int cores = g_cores;
        if (ImGui::SliderInt("Cores", &cores, 1, 256)) {
            if (cores < 1) cores = 1;
            if (cores > 256) cores = 256;
            if (cores != g_cores) {
                g_cores = cores;
                uiSaveCfg();
            }
        }
        ImGui::Text("takes effect on restart");
    }
    ImGui::End();
    if (ImGui::Begin("Radiation")) {
        ImGui::TextWrapped("Single-bit faults in RAM (DX12 compute) and paint with the cursor. Protect the boot image to keep the kernel alive. Flips below 0x88000000 show in the view; all flips affect execution.");
        ImGui::Checkbox("Enable radiation", &g_radEnable);
        ImGui::SliderInt("Flips a frame", &g_radFlips, 0, 1000);
        static char b0s[16] = "80400000", b0e[16] = "86000000", b1s[16] = "88000000", b1e[16] = "92000000";
        unsigned long v = 0;
        if (ImGui::InputText("Range 0 start##rad", b0s, sizeof b0s,
                ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
            v = strtoul(b0s, nullptr, 16);
            if (v <= 0xfffffffful) g_radR0S = (uint32_t)v;
        }
        if (ImGui::InputText("Range 0 end##rad", b0e, sizeof b0e,
                ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
            v = strtoul(b0e, nullptr, 16);
            if (v <= 0xfffffffful) g_radR0E = (uint32_t)v;
        }
        if (ImGui::InputText("Range 1 start##rad", b1s, sizeof b1s,
                ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
            v = strtoul(b1s, nullptr, 16);
            if (v <= 0xfffffffful) g_radR1S = (uint32_t)v;
        }
        if (ImGui::InputText("Range 1 end##rad", b1e, sizeof b1e,
                ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
            v = strtoul(b1e, nullptr, 16);
            if (v <= 0xfffffffful) g_radR1E = (uint32_t)v;
        }
        if (ImGui::Button("Whole RAM")) {
            g_radR0S = 0x80400000u; g_radR0E = 0x86000000u;
            g_radR1S = 0x88000000u; g_radR1E = 0x92000000u;
            sprintf(b0s, "80400000"); sprintf(b0e, "86000000");
            sprintf(b1s, "88000000"); sprintf(b1e, "92000000");
        }
        ImGui::Checkbox("Protect boot image (kernel)", &g_radProtect);
        if (g_radImageSize > 0)
            ImGui::Text("protected: %08X..%08X (%u MB)", g_radImageBase,
                        g_radImageBase + g_radImageSize, g_radImageSize >> 20);
        else
            ImGui::Text("protected: (image not reported yet)");
        ImGui::Separator();
        ImGui::Checkbox("Paint with cursor", &g_radArmed);
        int bv = (int)(g_radPaintVal & 0xffu);
        if (ImGui::SliderInt("Brush byte", &bv, 0, 255, "0x%02X")) g_radPaintVal = (uint32_t)bv;
        ImGui::Text("click/drag on Memory to paint a 16-byte texel");
        ImGui::Separator();
        ImGui::Text("flips done: %llu", (unsigned long long)g_radFlipsDone);
        if (g_radLastPaint != ~0u) ImGui::Text("last paint: %08X", g_radLastPaint);
        else ImGui::Text("last paint: none");
        ImGui::Text("DX12 compute only (the default); nothing on D3D11.");
        ImGui::TextWrapped("Scripts: RVC_RAD_FLIPS=N, RVC_RAD_PAINT=0x..., RVC_RAD_PAINTVAL=0x.., RVC_RAD_PROTECT=0/1");
    }
    ImGui::End();
    // the cursor's paint lasts while the button is held (a click paints at least a frame)
    if (!ImGui::IsMouseDown(0)) g_radPaint = ~0u;
}

bool MemoryView::render(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* cur, bool present, ID3D11ShaderResourceView* gpu) {
    if (!hwnd_) return false;
    MSG msg;
    while (PeekMessageW(&msg, hwnd_, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (GetPropW(hwnd_, L"closed")) {
        close();
        return false;
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return true;  // minimised
    std::string err;
    if ((UINT)rc.right != width_ || (UINT)rc.bottom != height_) {
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        if (!createTargets(err)) return true;
    }
    if (heatNeedsClear_) {
        const float zero[4] = {0, 0, 0, 0};
        for (auto& r : heatRtv_) ctx->ClearRenderTargetView(r.Get(), zero);
        heatNeedsClear_ = false;
    }

    Constants c{};
    c.winSize[0] = (float)width_;
    c.winSize[1] = (float)height_;
    c.frame = frame_++;
    c.strips = 2;
    c.texWidth = texWidth_;
    c.ramRows = texHeight_ - stateRows_;
    c.stateRows = stateRows_;
    c.bar = (float)memoryViewBar();
    c.inset = c.bar - 16 * g_ui;
    c.split = memoryViewSplit(width_);
    c.ui = g_ui;
    float ramRowsF = (float)(texHeight_ - stateRows_);
    if (g_memZoom < 1) g_memZoom = 1;
    if (g_memZoom > 64) g_memZoom = 64;
    float memSpanF = ramRowsF / (float)g_memZoom;
    if (memSpanF < 1) memSpanF = 1;
    float maxOffF = ramRowsF - memSpanF;
    if (maxOffF < 0) maxOffF = 0;
    if (g_memOff > maxOffF) g_memOff = maxOffF;
    if (g_memOff < 0) g_memOff = 0;
    c.memOff = g_memOff;
    c.memSpan = memSpanF;
    c.glow = g_memGlow;
    c.colOff = 0;
    c.colSpan = (float)texWidth_;
    c.reserved[0] = c.reserved[1] = 0;
    c.textSize[0] = (float)textW_;
    c.textSize[1] = (float)textH_;
    ctx->UpdateSubresource(cb_.Get(), 0, nullptr, &c, 0, 0);

    int dst = 1 - heatCur_;
    ID3D11ShaderResourceView* nulls[5] = {};
    ctx->PSSetShaderResources(0, 5, nulls);
    if (!g_freeze) {
        ID3D11RenderTargetView* rtvs[2] = {viewRtv_.Get(), heatRtv_[dst].Get()};
        ctx->OMSetRenderTargets(2, rtvs, nullptr);
        D3D11_VIEWPORT vp{0, 0, (float)width_, (float)height_, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(raster_.Get());
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(nullptr, 0);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs_.Get(), nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->PSSetShader(ps_.Get(), nullptr, 0);
        ID3D11Buffer* cb = cb_.Get();
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ID3D11ShaderResourceView* srvs[5] = {cur, refValid_ ? refSrv_.Get() : cur, heatSrv_[heatCur_].Get(), textSrv_.Get(), gpu};
        ctx->PSSetShaderResources(0, 5, srvs);
        ctx->Draw(3, 0);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ctx->PSSetShaderResources(0, 5, nulls);
        heatCur_ = dst;
        ComPtr<ID3D11Resource> curRes;
        cur->GetResource(&curRes);
        ctx->CopyResource(refTex_.Get(), curRes.Get());
        refValid_ = true;
    } else {
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ctx->PSSetShaderResources(0, 5, nulls);
    }

    if (!imgui_ || !viewSrv_) {
        ctx->CopyResource(backTex_.Get(), viewTex_.Get());
        if (present) swap_->Present(0, 0);
        return true;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    static bool dockBuilt = false;
    ImGuiID dockId = 0;
    {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        ImGui::Begin("HarnessDockHost", nullptr, hostFlags);
        ImGui::PopStyleVar(3);
        dockId = ImGui::GetID("HarnessDock");
        if (!dockBuilt) {
            dockBuilt = true;
            if (!std::filesystem::exists("logs/imgui_harness.ini")) {
                ImGui::DockBuilderRemoveNode(dockId);
                ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockId, vp->Size);
                ImGuiID right = 0, bottom = 0, left = 0;
                ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Right, 0.30f, &right, &left);
                ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.32f, &bottom, &left);
                ImGuiID bottomLeft = 0, bottomRight = 0;
                ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.45f, &bottomRight, &bottomLeft);
                ImGui::DockBuilderDockWindow("Display", left);
                ImGui::DockBuilderDockWindow("Memory", right);
                ImGui::DockBuilderDockWindow("Heat", right);
                ImGui::DockBuilderDockWindow("Loupe", right);
                ImGui::DockBuilderDockWindow("CPU State", right);
                ImGui::DockBuilderDockWindow("GPU Target", right);
                ImGui::DockBuilderDockWindow("Console", bottomLeft);
                ImGui::DockBuilderDockWindow("Machine", bottomRight);
                ImGui::DockBuilderDockWindow("Settings", bottomRight);
                ImGui::DockBuilderDockWindow("Radiation", bottomRight);
                ImGui::DockBuilderFinish(dockId);
            }
        }
        ImGui::DockSpace(dockId);
        ImGui::End();
    }
    float splitPx = memoryViewSplit(width_);
    float barPx = (float)memoryViewBar();
    float memPx = (float)height_ - barPx;
    g_gpuShown = gpu;
    float insetPx = c.inset;
    if (insetPx > 0) {
        float bx0 = (float)width_ - insetPx - 12 * g_ui;
        float by0 = memPx + (barPx - insetPx) * 0.5f;
        g_stateUv[0] = bx0 / (float)width_;
        g_stateUv[1] = by0 / (float)height_;
        g_stateUv[2] = (bx0 + insetPx) / (float)width_;
        g_stateUv[3] = (by0 + insetPx) / (float)height_;
    }
    bool dispOpen = ImGui::Begin("Display");
    if (dispOpen) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float s = (std::min)(avail.x / splitPx, avail.y / memPx);
        if (s > 16) s = 16;
        if (s > 0 && splitPx > 0 && memPx > 0) {
            ImVec2 size(splitPx * s, memPx * s);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - size.x) * 0.5f);
            ImGui::Image((ImTextureID)viewSrv_.Get(), size, ImVec2(0, 0), ImVec2(splitPx / (float)width_, memPx / (float)height_));
            ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
            POINT p0{(LONG)mn.x, (LONG)mn.y}, p1{(LONG)mx.x, (LONG)mx.y};
            ScreenToClient(hwnd_, &p0);
            ScreenToClient(hwnd_, &p1);
            g_dispRect.left = p0.x;
            g_dispRect.top = p0.y;
            g_dispRect.right = p1.x;
            g_dispRect.bottom = p1.y;
            g_dispShownW = size.x;
            g_dispShownH = size.y;
            g_dispValid = true;
        } else g_dispValid = false;
    } else g_dispValid = false;
    ImGui::End();
    bool memOpen = ImGui::Begin("Memory");
    if (memOpen) {
        float mw = (float)width_ - splitPx;
        float totalRows = (float)(texHeight_ - stateRows_);
        float halfSpan = totalRows / (float)(g_memZoom > 0 ? g_memZoom : 1) * 0.5f;
        ImGui::BeginChild("ramScroll", ImVec2(0, 0), false);
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float s = avail.x / (mw * 0.5f);
        if (s > 0 && mw > 0 && memPx > 0) {
            float u0 = splitPx / (float)width_;
            float um = u0 + (1.0f - u0) * 0.5f;
            float v1 = memPx / (float)height_;
            ImVec2 size(avail.x, avail.x * memPx / (mw * 0.5f));
            ImGui::Text("rows %.0f..%.0f", (double)g_memOff, (double)(g_memOff + halfSpan));
            ImGui::Image((ImTextureID)viewSrv_.Get(), size, ImVec2(u0, 0), ImVec2(um, v1));
            ImGui::Text("rows %.0f..%.0f", (double)(g_memOff + halfSpan), (double)(g_memOff + halfSpan * 2));
            ImGui::Image((ImTextureID)viewSrv_.Get(), size, ImVec2(um, 0), ImVec2(1.0f, v1));
        }
        ImGui::EndChild();
    }
    ImGui::End();
    if (ImGui::Begin("Heat")) {
        ImGui::BeginChild("heatScroll", ImVec2(0, 0), false);
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float mw = (float)width_ - splitPx;
        if (avail.x > 0 && mw > 0 && memPx > 0) {
            float u0 = splitPx / (float)width_;
            float um = u0 + (1.0f - u0) * 0.5f;
            float v1 = memPx / (float)height_;
            ImVec2 size(avail.x, avail.x * memPx / (mw * 0.5f));
            ImGui::Image((ImTextureID)heatSrv_[heatCur_].Get(), size, ImVec2(u0, 0), ImVec2(um, v1));
            ImGui::Image((ImTextureID)heatSrv_[heatCur_].Get(), size, ImVec2(um, 0), ImVec2(1.0f, v1));
        }
        ImGui::EndChild();
    }
    ImGui::End();
    cpuStatePanel();
    if (ImGui::Begin("GPU Target")) {
        if (g_gpuShown) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            if (avail.x > 0) ImGui::Image((ImTextureID)g_gpuShown, ImVec2(avail.x, avail.x), ImVec2(0, 0), ImVec2(1, 1));
        } else {
            ImGui::Text("no GPU target");
        }
    }
    ImGui::End();
    consolePanel();
    if (ImGui::Begin("Machine")) {
        if (!g_cols.empty())
            for (auto& l : g_cols[0]) ImGui::TextUnformatted(l.c_str());
        if (g_cols.size() > 2)
            for (auto& l : g_cols[2]) ImGui::TextUnformatted(l.c_str());
        ImGui::Checkbox("Freeze view", &g_freeze);
        if (ImGui::Button("Capture view BMP")) g_pendingCapture = "logs/imgui_view.bmp";
        if (ImGui::Button("Reset layout")) {
            DeleteFileA("logs/imgui_harness.ini");
            ImGui::DockBuilderRemoveNode(dockId);
            dockBuilt = false;
        }
    }
    ImGui::End();
    if (ImGui::Begin("Settings")) {
        float scale = g_uiScale;
        if (ImGui::SliderFloat("UI scale", &scale, 0.5f, 2.0f, "%.2f")) {
            g_uiScale = scale;
            uiApplyScale(g_uiScale);
            uiSaveCfg();
        }
        bool sound = !g_soundOff;
        if (ImGui::Checkbox("Sound", &sound)) g_hostKeys.push_back('s');
        int vol = g_vol;
        if (ImGui::SliderInt("Volume", &vol, 0, 100)) {
            int steps = (vol - g_vol + (vol > g_vol ? 2 : -2)) / 5;
            for (int i = 0; i < abs(steps); ++i) g_hostKeys.push_back(vol > g_vol ? '+' : '-');
            g_vol = vol;
        }
        float spanF = ramRowsF / (float)(g_memZoom > 0 ? g_memZoom : 1);
        if (spanF < 1) spanF = 1;
        float maxOffSet = ramRowsF - spanF;
        if (maxOffSet < 0) maxOffSet = 0;
        if (ImGui::SliderInt("Memory zoom", &g_memZoom, 1, 64)) {
            if (g_memOff > maxOffSet) g_memOff = maxOffSet;
            uiSaveCfg();
        }
        if (ImGui::SliderFloat("Memory offset", &g_memOff, 0, maxOffSet, "%.0f")) uiSaveCfg();
        if (ImGui::Button("Memory 1:1")) {
            g_memZoom = 1;
            g_memOff = 0;
            uiSaveCfg();
        }
        ImGui::Text("rows %.0f..%.0f of %.0f", (double)g_memOff, (double)(g_memOff + spanF), (double)ramRowsF);
        if (ImGui::SliderFloat("Memory glow", &g_memGlow, 0.0f, 2.0f, "%.2f")) uiSaveCfg();
    }
    ImGui::End();
    ImGui::Render();
    ImDrawData* draw = ImGui::GetDrawData();
    if (!draw || draw->TotalVtxCount == 0) {
        ctx->CopyResource(backTex_.Get(), viewTex_.Get());
        if (present) swap_->Present(0, 0);
        return true;
    }
    ID3D11RenderTargetView* back = backRtv_.Get();
    ctx->OMSetRenderTargets(1, &back, nullptr);
    const float clearCol[4] = {0.03f, 0.03f, 0.04f, 1.0f};
    ctx->ClearRenderTargetView(back, clearCol);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (!g_pendingCapture.empty()) {
        D3D11_TEXTURE2D_DESC td{};
        viewTex_->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        bool done = false;
        if (SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, &staging))) {
            ctx->CopyResource(staging.Get(), viewTex_.Get());
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                done = memoryViewWriteBmp(g_pendingCapture, td.Width, td.Height, (const uint8_t*)m.pData, m.RowPitch);
                ctx->Unmap(staging.Get(), 0);
            }
        }
        (void)done;
        g_pendingCapture.clear();
    }
    if (present) swap_->Present(0, 0);
    return true;
}

bool MemoryView::capture(ID3D11DeviceContext* ctx, const std::string& path) {
    if (!viewTex_) return false;
    D3D11_TEXTURE2D_DESC td{};
    viewTex_->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &staging))) return false;
    ctx->CopyResource(staging.Get(), viewTex_.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    bool ok = memoryViewWriteBmp(path, td.Width, td.Height, (const uint8_t*)m.pData, m.RowPitch);
    ctx->Unmap(staging.Get(), 0);
    return ok;
}

void MemoryView::setText(ID3D11DeviceContext* ctx, const MemoryViewText& columns) {
    if (!hwnd_) return;
    g_cols = columns;
    if (!columns.empty()) {
        for (auto& line : columns[0]) {
            size_t p = line.find("volume ");
            if (p != std::string::npos) {
                int v = atoi(line.c_str() + p + 7);
                if (v >= 0 && v <= 100) g_vol = v;
            }
            if (line.find("sound off") != std::string::npos) g_soundOff = true;
            if (line.find("sound playing") != std::string::npos || line.find("sound idle") != std::string::npos ||
                line.find("sound not played") != std::string::npos)
                g_soundOff = false;
        }
    }
    const UINT kW = kMemoryViewTextW, kH = kMemoryViewTextH;
    if (!textTex_) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kW;
        td.Height = kH;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &textTex_)) ||
            FAILED(dev_->CreateShaderResourceView(textTex_.Get(), nullptr, &textSrv_)))
            return;
    }
    std::vector<uint8_t> bits;
    if (memoryViewText(columns, bits, textW_, textH_)) ctx->UpdateSubresource(textTex_.Get(), 0, nullptr, bits.data(), kW * 4, 0);
}

void MemoryView::setTitle(const std::string& title) {
    if (hwnd_) SetWindowTextW(hwnd_, widen(title).c_str());
}

void MemoryView::close() {
    if (!hwnd_) return;
    if (imgui_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imgui_ = false;
        g_imgui = false;
    }
    viewSrv_.Reset();
    viewRtv_.Reset();
    viewTex_.Reset();
    backRtv_.Reset();
    backTex_.Reset();
    swap_.Reset();
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}
