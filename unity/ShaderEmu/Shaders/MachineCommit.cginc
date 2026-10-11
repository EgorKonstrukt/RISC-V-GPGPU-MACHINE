// The commit's own code (experiments/rvc_opt/main.shader's Commit pass), for Machine.shader's
// Commit, which draws the state rows and whole bands, and MachineCommitPoints.shader, which
// draws what the cores stored as points. Include after UnityCG.cginc and MachineBlit.cginc.
float _Init, _InitRaw;

Texture2D<float4> _Data_RAM_R;
Texture2D<float4> _Data_RAM_G;
Texture2D<float4> _Data_RAM_B;
Texture2D<float4> _Data_RAM_A;
Texture2D<float4> _Data_RAM_RAW;

Texture2D<float4> _Data_MTD_R;
Texture2D<float4> _Data_MTD_G;
Texture2D<float4> _Data_MTD_B;
Texture2D<float4> _Data_MTD_A;

// A core's state as the tick left it (docs/multicore.md): core 0's block, or a
// worker's strip of tiles in the band, as the geometry says. Everything else as it
// was: the tick does not write RAM. As experiments/rvc_opt/main.shader's commit.
static uint2 state_off = uint2(0, 0);
static uint mc_core = 0;
static uint mc_bits_now = 6;    // the write cache's size of the core looked at
static uint mc_size_now = 4096; // and how many texels of state it has
static uint mc_tile0 = 0;       // and, a worker's, the first of its tiles
static uint4 mc_geo;            // the geometry's texel
uint4 mc_state_read(uint2 pos) {
    if (mc_core == 0) return state_after_tick(pos + state_off);
    uint w = pos.y * 64 + pos.x;
    if (w >= mc_size_now) return (uint4)0;
    uint t = mc_tile0 + (w >> 6), i = w & 63;
    return state_after_tick(uint2(64 + ((t % 248) << 3) + (i & 7), ((t / 248) << 3) + (i >> 3)));
}
#define STATE_TEX_HART(pos, hartidx) mc_state_read(uint2(pos))
#define STATE_TEX(pos) mc_state_read(uint2(pos))
#define RAM_TEX(pos) (_SelfTexture2D[pos])   // the tick does not write RAM

static uint2 s_dim;
static uint2 m_dim;

#include "helpers.cginc"
#include "src/types.cginc" // includes fb.h

// The GPU device's picture, for copying into RAM (docs/gpu.md).
Texture2D<float4> _GpuTarget;
#define GPU_STATE _SelfTexture2D
#define GPU_WRITEBACK
#include "src/gpu.cginc"
#include "src/zap.cginc"   // the host's hand in RAM, for one commit (docs/danger.md)

// The bands this commit draws whole: what the GPU's picture copied back wrote. What the
// cores stored is drawn as points (MachineCommitPoints.shader), not as bands.
uint commit_bands_changed() {
    if (_Init) return 0xffffffff;
    return gpu_copy_bands() | zap_bands();
}

static uint commit_core = 0;   // (MachineCommitPoints.shader) whose store the point drawn is
// A texel of the state after this commit: the state rows from the tick's, RAM from the
// cores' stores.
uint4 commit_texel(uint2 pos) {
    _SelfTexture2D.GetDimensions(s_dim.x, s_dim.y);
    _Data_MTD_R.GetDimensions(m_dim.x, m_dim.y);

    if (_Init && _InitRaw) {
        float3 raw[6];
        for (uint off = 0; off < 6; off++) {
            raw[off] = _Data_RAM_RAW[pos + uint2((off * s_dim.x) % (s_dim.x*3), (off / 3) * s_dim.y)].rgb;
        }
        uint4 data = unpack_uint4(raw);
        return data;
    }

    uint4 picture;
    if (!_Init && gpu_writeback(pos, picture)) return picture;
    uint4 result;
    MC_GEO_READ
    if (pos.y < 64) {
        // a core's own state, as with one core
        uint hb = 0;
        uint4 own = state_after_tick(pos);
        mc_select(0);
        if (pos.x >= MC_STRIP_X) {
            uint tile = ((pos.x - MC_STRIP_X) >> 3) + MC_STRIP_TILES * (pos.y >> 3);
            hb = mc_core_at(tile);
            if (hb == 0) return (uint4)0;
            uint w = ((tile - mc_tile0) << 6) + ((pos.y & 7) << 3) + (pos.x & 7);
            pos = uint2(w & 63, w >> 6);
        }
        pos = mc_texel_of(pos);
        // (of a worker's state the commit changes three texels: the rest is as it is)
        if (hb != 0 && (pos.y != 0 || (pos.x != 8 && pos.x != 28 && pos.x != 41))) return own;
        decode_for_commit();
        result = commit(pos, own);
        // A worker's one store that found its cache full is in its state until its next
        // pass rewrites it: gone now, or the commits of a worker asleep would store it again.
        if (hb != 0 && pos.x == 8 && pos.y == 0) result.r = 0xffffffff;
        if (pos.x == 41 && pos.y == 0) result.b = commit_bands_changed();   // for the control pass and the next commit
    } else {
        // RAM: every core's writes, the highest core's last
        result = RAM_TEX(pos);
#ifdef COMMIT_AS_POINTS
        // a point: the store of one core (two may not store to one texel in a pass). A pixel
        // that asked every core took as long as hundreds of the tick's instructions.
        uint first = min(commit_core, mc_count), most = first + 1;
#else
        // (a band drawn whole: what a core stored there is a point, drawn after this)
        uint first = 0, most = _Init ? 1 : 0;
#endif
        bool stored_here = false;
        for (uint h = first; h < most; h++) {
            mc_select(h);
            if (h != 0) {
                // A worker keeps every address it stored to or-ed together in one word:
                // a texel whose address has a bit that word lacks is not its. (A copy
                // or a fill is not in that word.)
                uint stored = STATE_TEX(uint2(41, 0)).r;
                if (stored == 0) continue;
                uint stalled = STATE_TEX(uint2(28, 0)).r;
                if (((RAM_LIN(pos.x, pos.y - 64) << 4) & ~stored) != 0 && stalled != STALL_MEMOP_COPY && stalled != STALL_MEMOP_FILL) continue;
            }
            decode_for_commit();
            result = commit(pos, result);
            stored_here = true;
        }
        mc_select(0);
#ifndef COMMIT_AS_POINTS
        // (in the pass that draws bands whole: a core's store to the same texel is a point drawn after)
        result = zap(pos, result);
#endif
#ifdef COMMIT_AS_POINTS
        // A worker that sat the pass out has its last pass's tags still: its point is of no
        // store, and must not put the texel as it was over what another core's point has drawn.
        if (!stored_here) discard;
#endif
    }
    return result;
}
