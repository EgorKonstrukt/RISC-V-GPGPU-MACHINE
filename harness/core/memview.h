// The machine's window. Left two thirds: the guest's display (docs/machine/display.md), decoded from
// its framebuffer in RAM or taken from the GPU device. Right third: memory live, the RAM part of the state texture
// drawn as colour (the largest byte of each texel's words), with texels written since
// the view's previous frame glowing and fading. Beside it, the guest's display (docs/machine/display.md),
// decoded from the framebuffer it keeps in RAM. A bar underneath, clear of the memory image,
// holds the speed counters and the 64x64 CPU state area magnified. It keeps its own copy of
// the texture to compare against.
// It only reads the state textures. Closing the window closes the view; rvc_harness then quits.
#pragma once

#include "common.h"

#include <d3d11.h>
#include <dxgi.h>

#include <string>
#include <vector>

// Pieces shared with the D3D12 view (rvc_memview12.h): shader source (entry points vs/ps,
// textures t0..t3 = current, previous, heat, text; cbuffer b0), window, text and BMP output.
const UINT kMemoryViewBar = 176;  // info bar at 96 dpi: eight text lines, and the CPU state area at 160 px
const UINT kMemoryViewTextW = 3584, kMemoryViewTextH = 384;   // room for the bar's text at up to 2.33 times that
// Columns of text in the bar under the picture: speed, console, input. The CPU state sits right of them.
const UINT kMemoryViewColumnX[3] = {0, 52, 116};   // in characters: 52 and 64 wide
typedef std::vector<std::vector<std::string>> MemoryViewText;
const UINT kMemoryViewDispW = 1280, kMemoryViewMemW = 640, kMemoryViewHeight = 720;  // initial layout above the bar
struct MemoryViewConstants {
    float winSize[2];
    uint32_t frame, strips, texWidth, ramRows, stateRows;
    float inset;
    float textSize[2];
    float bar;
    float split;   // x where the display ends and the memory image begins
    float ui;      // window pixels a 96 dpi pixel
    float memOff;  // first RAM row the memory image shows
    float memSpan; // RAM rows the memory image shows
    float glow;    // heat glow intensity (0 off)
    float colOff;  // first RAM texel column shown (the loupe's window)
    float colSpan; // RAM texel columns shown
    float reserved[2];
};
// The window's dpi over 96, the bar's height in window pixels, and the x where the display panel ends.
float memoryViewUi();
UINT memoryViewBar();
float memoryViewSplit(UINT width);
const char* memoryViewShader();
// What was typed into the window since the last call, as terminal bytes (arrows as escape
// sequences). Keys arrive when the window's messages are pumped, which render() does.
std::string memoryViewTakeKeys();
// Keys the window keeps for the host, since the last call: 's' Ctrl+F9 (the sound card on or
// off), '-' Ctrl+F11 and '+' Ctrl+F12 (its volume).
std::string memoryViewTakeHostKeys();
// The window's pointer and raw key events, for the machine's input device.
struct MemoryViewInput {
    float x = 0, y = 0;             // pointer, in window pixels over the display panel
    float panelW = 0, panelH = 0;   // the display panel's size; 0 without a window
    unsigned buttons = 0;           // bit 0 left, 1 right, 2 middle
    std::vector<uint32_t> keys;     // since the last call: Linux key code, bit 31 while pressed
};
MemoryViewInput memoryViewTakeInput();
// The docked panels both memory views show (Display, Memory, Heat, CPU State, GPU Target,
// Console, Machine, Settings, Radiation, Loupe), after the backend's NewFrame and before its Render.
// view/heat/gpu/loupe: the rendered view, the heat image, the GPU target and the loupe, as ImTextureID
// (a D3D11 SRV or a D3D12 GPU descriptor handle; loupe null when none); width/height: the window's size;
// texH/stateRows: the state's size, for the memory panel's row labels; texWidth: its width,
// for the paint cursor's pixel-to-address mapping.
void memoryViewPanels(void* view, void* heat, void* gpu, void* loupe, UINT width, UINT height,
                      UINT texH, UINT stateRows, UINT texWidth);
// Whether a view shows ImGui panels (input goes to ImGui when it wants it).
void memoryViewSetImgui(bool imgui);
// Radiation (fault injection) + paint, for the DX12 compute backend (live, no restart).
// Addresses are guest-physical (0x80000000 + RAM offset). The panel owns the settings;
// the backend takes them every frame and reports back.
struct MemoryViewRadiation {
    bool enable = false;
    int flips = 0;              // bit flips this dispatch (paced: per real second, or per frame)
    uint32_t r0s = 0x80400000u, r0e = 0x86000000u;   // RAM low (guest-physical)
    uint32_t r1s = 0x88000000u, r1e = 0x92000000u;   // RAM high (guest-physical)
    bool protectImage = true;   // skip the boot image (the kernel): [imageBase, imageBase + imageSize)
    uint32_t paint = ~0u;       // guest-physical address of a 16-byte texel to paint, ~0u none
    uint32_t paintVal = 0xffu;  // byte to paint it with
    bool paintArmed = false;    // the cursor paints while held (the Memory panel sets paint)
    uint32_t imageBase = 0x80000000u, imageSize = 0;   // the boot image, set by the backend
    uint64_t flipsDone = 0;     // accumulated by the backend, for the status line
    uint32_t lastPaint = ~0u;   // last painted address, for the status line
    // Pacing + stats, filled by memoryViewRadiation every call (the backend ignores them):
    bool perSecond = true;      // rate is flips per real (wall-clock) second, not per frame
    int rate = 0;               // the panel's requested rate (per second, or per frame)
    uint32_t zones = ~0u;       // memory zones to flip in, a bit per memview zone (only those)
    double measuredPerSec = 0;  // dispatched flips per real second, exponentially averaged
    double elapsedSec = 0;      // real seconds while enabled this session
};
// Fill with the panel's settings; true when the backend should run radiate this frame
// (enabled and flips > 0 or a paint is pending). Called by the backend, every frame.
bool memoryViewRadiation(MemoryViewRadiation& out);
// The backend reports flips it executed (accumulates flipsDone).
void memoryViewRadiationDone(uint64_t flips);
// Zeroes the session counters (flips done, elapsed and measured rate).
void memoryViewRadiationReset();
// The backend reports the boot image range (for the protect range's display).
void memoryViewRadiationImage(uint32_t base, uint32_t size);
// A core's decoded state, for the CPU State panel (DX12 compute; the backend pushes
// it every frame from the cores' state buffer). r[0..31] are x, r[32..63] f (bits).
struct MemoryViewCore {
    uint32_t mode = 0;   // 0 parked, 1 running, 2 asleep, 3/4 stopped
    uint32_t pc = 0;
    uint32_t priv = 0;
    uint64_t instr = 0;
    uint32_t seen = 0;   // the resume word it stopped with
    uint32_t job = 0;    // the job word it sleeps on
    uint32_t tcmpLo = 0, tcmpHi = 0;
    uint32_t r[64] = {};
    uint32_t csr[16] = {};
    uint32_t fcsr = 0;   // float control/status (frm + fflags); words 82 of the core's state
};
// The backend pushes all cores' state (empty: not available, e.g. not DX12 compute).
void memoryViewSetCores(const MemoryViewCore* cores, int n);
// How many cores were pushed (0: none yet).
int memoryViewCpuCount();
// The pushed state of one core (false: none).
bool memoryViewCpuState(int core, MemoryViewCore& out);
// The Console panel's full log: the backend appends transcript deltas (the overlay's
// 8-line snapshot stays as is for the info bar). The panel accumulates scrollback.
void memoryViewAppendConsole(const std::string& delta);
// The kept UI settings (scale/zoom/offset), for a view that starts after them.
void memoryViewLoadCfg();
// The view's window size, for the pointer mapping.
void memoryViewSetViewSize(UINT w, UINT h);
// The memory view's heat glow intensity (0 off), as the Settings panel wants it.
float memoryViewGlow();
// How many vertical strips RAM is cut into in the memory image (2/3/4/8 tiles).
int memoryViewStrips();
// The zoomed span in RAM rows for a total (clamped offset + span for the current zoom).
void memoryViewMemRange(float totalRows, float* off, float* span);
// The loupe's region (a square of RAM texels around the cursor), set by the Memory panel.
void memoryViewLoupe(int* cx, int* cy, int* n);
// The machine's cores, as the Settings panel wants them (needs a restart).
int memoryViewCoreCount();
float memoryViewUiScale();
void memoryViewApplyScale(float s);
// A BMP the Machine panel asked for, since the last call (empty: none).
std::string memoryViewTakePendingCapture();
// The panels' text columns (Machine, Console, input), with the volume the first one names.
void memoryViewSetColumns(const MemoryViewText& columns);
// Machine numbers behind the overlay lines above (pushed with them, 4 Hz): the Machine
// panel sections, sparklines and copy text read these; the info bar keeps the strings.
struct MemoryViewStats {
    double ipsAll = 0, ipsCore0 = 0, ipsWorkers = 0;
    unsigned workersRunning = 0;
    double fps = 0, frameMs = 0, instrPerFrame = 0;
    double tickMs = -1, commitMs = -1, gpuMs = 0;   // tick/commit -1: not measured
    int sound = 0;    // 0 none, 1 off, 2 not played, 3 idle, 4 playing
    int volume = 0;
    double upSec = 0, guestSec = 0;
    uint64_t totalInstr = 0, commits = 0;
    uint64_t inputQueued = 0, inputSent = 0, inputTaken = 0;
    int lastKey = 0;
    std::string inputDesc;
};
void memoryViewSetStats(const MemoryViewStats& stats);
// WM_CLOSE sets the window property L"closed" instead of destroying the window.
HWND memoryViewCreateWindow();
// Rasterises the lines into a kMemoryViewTextW x kMemoryViewTextH BGRA bitmap, white on black.
bool memoryViewText(const MemoryViewText& columns, std::vector<uint8_t>& out, UINT& usedW, UINT& usedH);
bool memoryViewWriteBmp(const std::string& path, UINT width, UINT height, const uint8_t* rgba, size_t pitch);

class MemoryView {
public:
    // stateRows: rows at the top of the texture that are CPU state rather than RAM.
    bool init(ID3D11Device* dev, UINT texWidth, UINT texHeight, UINT stateRows, std::string& err);
    // Draws one frame from the current state texture and presents it.
    // Returns false once the window has been closed.
    // present = false leaves the frame in the back buffer for capture().
    // gpu: the GPU device's colour target, shown when the guest selects display mode 3.
    bool render(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* cur, bool present = true,
                ID3D11ShaderResourceView* gpu = nullptr);
    // Saves the back buffer as a 32-bit BMP; call after render(..., false).
    bool capture(ID3D11DeviceContext* ctx, const std::string& path);
    void setTitle(const std::string& title);
    // Lines of text shown in the info bar under the memory image (speed counters).
    void setText(ID3D11DeviceContext* ctx, const MemoryViewText& columns);
    bool open() const { return hwnd_ != nullptr; }
    void close();

private:
    bool createTargets(std::string& err);

    HWND hwnd_ = nullptr;
    ComPtr<ID3D11Device> dev_;
    ComPtr<IDXGISwapChain> swap_;
    ComPtr<ID3D11RenderTargetView> backRtv_;
    ComPtr<ID3D11Texture2D> backTex_;
    ComPtr<ID3D11Texture2D> refTex_;                 // the state as of the previous view frame
    ComPtr<ID3D11ShaderResourceView> refSrv_;
    bool refValid_ = false;
    ComPtr<ID3D11Texture2D> heatTex_[2];
    ComPtr<ID3D11ShaderResourceView> heatSrv_[2];
    ComPtr<ID3D11RenderTargetView> heatRtv_[2];
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11Texture2D> textTex_;                // text box, drawn with GDI
    ComPtr<ID3D11ShaderResourceView> textSrv_;
    UINT textW_ = 0, textH_ = 0;                     // used part of the text texture
    ComPtr<ID3D11Texture2D> viewTex_;
    ComPtr<ID3D11ShaderResourceView> viewSrv_;
    ComPtr<ID3D11RenderTargetView> viewRtv_;
    bool imgui_ = false;
    ComPtr<ID3D11RasterizerState> raster_;
    UINT width_ = 0, height_ = 0;
    UINT texWidth_ = 0, texHeight_ = 0, stateRows_ = 0;
    UINT frame_ = 0;
    int heatCur_ = 0;
    bool heatNeedsClear_ = true;
};
