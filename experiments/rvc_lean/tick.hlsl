// The tick: RV32IMAF for Linux without an MMU, every core of the machine, in one fragment shader
// (README.md). Written in the shape of experiments/rv32_min/rv32_min_fast.hlsl: one loop that
// tests the kinds of instruction most common first, arithmetic as one test with the result
// chosen by selects, one indexed register write; a general step of its own for the rest. The
// state texture is the machine's as before (docs/machine/multicore.md): RAM from row 64, and in
// rows 0-63 core 0's block and the workers' strip; what a core stores goes into its write cache
// (three tables of buckets of three texels), and the commit pass puts it into RAM.
//
// A pixel of the tick is eight texels of a core's state, one in each of eight targets; every
// pixel of a core runs the whole pass and writes its own eight. TICK_UNPACK compiles the pass
// that copies the targets into the state texture.

Texture2D<uint4> _SelfTexture2D;
Texture2D<float4> _Data_MTD_R, _Data_MTD_G, _Data_MTD_B, _Data_MTD_A;   // the ROM at 0x40000000
Texture2D<float4> _Data_DTB_R, _Data_DTB_G, _Data_DTB_B, _Data_DTB_A;   // the device tree at 0x1020
uniform uint _Init, _InitRaw, _Ticks, _TicksDivisor, _RTC0, _RTC1, _PlayerID;
uniform uint _TimePass;   // this frame's round (--tick-rounds): the clock a tick on for each, as passes are told apart by it
uniform uint _TickFull;   // the rounds that always run (--tick-split); a later one only for core 0's rest of its frame,
                          // and for the workers only when core 0 ended the round before early (texel 39.z)
#define MORE_ROUND (_TimePass >= max(_TickFull, 1) && TEX(uint2(39, 0)).z == 0)
uniform float4 _Time;
#ifdef NO_DOUBLES
uniform uint _HostMtimeLo, _HostMtimeHi;
#endif

#ifndef CORES
#define CORES 256
#endif
#define TEX(p) _SelfTexture2D[p]
#define RAM_ADDR(t) uint2((t) % 2048, 64 + (t) / 2048)
#ifndef STATE_TEX_HEIGHT
#define STATE_TEX_HEIGHT 9280u   // the state texture's rows: 64 of state, then RAM to 0x92000000
#endif
#define RAM_MAX (2048u * (STATE_TEX_HEIGHT - 64u) * 16u)
// A core's state, as texels numbered 64 to a row of core 0's block: 0-2 the CSRs, 8 the store a
// full cache had no room for and the privilege, 10 the timer's compare, 28 the stall, the count
// and the passes, 29-36 x1-x31 and pc, 37 the reservation and a copy's source and destination,
// 38 its length, 41 the stores' addresses or-ed, their 4 MB bands and the worker's word, from 44
// the write cache (a texel of tags, then three texels, a bucket), then the float registers.
#define L1_AT 44
#define FP_AT 492                                    // after core 0's 112 buckets: the numbering's (lin_of)
#ifndef CORE0_BITS
#define CORE0_BITS 6                                 // core 0's cache: tables of 2^6 (its floats then right after)
#endif
#define CORE0_ROWS ((L1_AT + BUCKETS(CORE0_BITS) * 4 + 8 + 63) / 64)
#define STATE_ROWS 16
#define BUCKETS(bits) ((7u << (bits)) >> 2)          // tables of 2^bits, 2^(bits-1) and 2^(bits-2)
#define H1(t, n) ((((t) >> 3) ^ ((t) << ((n) - 3)) ^ ((t) >> (n))) & ((1u << (n)) - 1))
#define H2(t, n) ((((t) >> 2) ^ ((t) << ((n) - 2)) ^ ((t) >> ((n) + 3))) & ((1u << (n)) - 1))
#define STALL_MEM 3
#define STALL_FENCE 7
#define STALL_COPY 8
#define STALL_WFI 9
#define STALL_FILL 10
#define MBOX 0x06C00000u                             // the workers' mailboxes, as a RAM offset
#define MC_START 0x5553434du
#define MC_STOP 0x5453434du

// ---- The geometry (docs/machine/multicore.md, "The geometry"): the workers' state is tiles of
// 8 x 8 texels in the strip right of core 0's block, 248 to a band; in the targets a worker is
// a block of columns. Workers 1 to g_nb are big (caches of g_bb bits); the rest have .b's.
#define GEO_TEXEL 0x70003du
#define STRIP_TILES 248
#define STRIP_BANDS ((CORES + 47) / 49)
#define TICK_W 1040
#define TICK_GAP 8
#define TICK_COLS (TICK_W - 8 - TICK_GAP)
#define TICK_BANDS ((CORES + 126) / 128)
static uint4 g;
static uint g_count, g_rows, g_cols, g_nb, g_bb, g_rows_b, g_cols_b, g_per0;
static uint core, bits, tile0, rows_now, cols_now;   // the core whose state is read

uint rows_of(uint b) { return b == 0 ? 0 : (L1_AT + 8 + BUCKETS(b) * 4 + 63) / 64; }
uint cols_wide(uint rows, uint block, uint pad) { return (rows + pad + block - 1) / block * block; }
void geo_read() {
    g = TEX(RAM_ADDR(GEO_TEXEL));
    uint ws = (g.a >> 16) & 0xff, wb = g.a >> 24;
    g_rows = g.b < 3 || g.b > 6 ? 0 : rows_of(g.b);
    g_cols = ws != 0 && ws <= 64 ? cols_wide(g_rows, ws, 0) : cols_wide(g_rows, 4, 2);
    g_count = 0; g_nb = 0; g_per0 = 0;
    g_bb = (g.a >> 8) & 0xff;
    if ((g.a & 0xff) != 0 && g_bb >= 3 && g_bb <= 6 && g_rows != 0) {
        g_rows_b = rows_of(g_bb);
        g_cols_b = wb != 0 && wb <= 64 ? cols_wide(g_rows_b, wb, 0) : cols_wide(g_rows_b, 8, 0);
        g_nb = min(min(min(g.a & 0xff, STRIP_TILES / g_rows_b), TICK_COLS / g_cols_b), min(g.g, (uint)CORES - 1));
        if (g_nb != 0) g_per0 = (TICK_COLS - g_nb * g_cols_b) / g_cols;
    }
    uint sb0 = g_nb != 0 ? 1 : 0;
    if (g_rows != 0 && (g.r & 1) == 0)
        g_count = g_nb + min(min(g.g, (uint)CORES - 1) - g_nb,
                             min((STRIP_BANDS - sb0) * (STRIP_TILES / g_rows), (TICK_BANDS - sb0) * (TICK_COLS / g_cols) + g_per0));
}
void select_core(uint k) {
    core = k;
    bool big = k != 0 && k <= g_nb;
    bits = k == 0 ? CORE0_BITS : big ? g_bb : k <= g_count ? g.b : 0;
    rows_now = big ? g_rows_b : g_rows;
    cols_now = big ? g_cols_b : g_cols;
    uint j = k - 1 - g_nb, per = STRIP_TILES / max(g_rows, 1);
    tile0 = k == 0 ? 0 : big ? (k - 1) * g_rows_b : ((g_nb != 0 ? 1 : 0) + j / per) * STRIP_TILES + (j % per) * g_rows;
}
uint2 tile_at(uint t) { return uint2(64 + 8 * (t % STRIP_TILES), 8 * (t / STRIP_TILES)); }
uint2 block_of(uint k) {
    if (k <= g_nb) return uint2(8 + TICK_GAP + (k - 1) * g_cols_b, 0);
    uint j = k - 1 - g_nb, per = TICK_COLS / max(g_cols, 1), sb0 = g_nb != 0 ? 1 : 0, below = (TICK_BANDS - sb0) * per;
    if (j >= below) return uint2(8 + TICK_GAP + g_nb * g_cols_b + (j - below) * g_cols, 0);
    return uint2(8 + TICK_GAP + (j % per) * g_cols, (sb0 + j / per) * STATE_ROWS);
}
// the worker whose block holds the targets' pixel, selected, and which of its tiles that is; 0: none
uint core_in(uint2 p, out uint tile) {
    tile = 0;
    if (p.x < 8 + TICK_GAP || (p.y % STATE_ROWS) >= 8 || g_count == 0) return 0;
    uint band = p.y / STATE_ROWS, xx = p.x - 8 - TICK_GAP, per = TICK_COLS / g_cols, sb0 = g_nb != 0 ? 1 : 0, k;
    if (g_nb != 0 && band == 0 && xx / g_cols_b < g_nb) {
        k = xx / g_cols_b + 1; tile = xx % g_cols_b;
    } else {
        if (g_nb != 0 && band == 0) xx -= g_nb * g_cols_b;
        uint i = xx / g_cols;
        if (i >= (g_nb != 0 && band == 0 ? g_per0 : per)) return 0;
        k = g_nb + (g_nb != 0 && band == 0 ? (TICK_BANDS - sb0) * per : (band - sb0) * per) + i + 1;
        tile = xx % g_cols;
    }
    if (k > g_count) return 0;
    select_core(k);
    return k;
}
// the worker whose tiles the strip's tile t is one of, selected (the unpack's question); 0: none
uint core_at(uint t) {
    uint band = t / STRIP_TILES, i, k;
    if (g_nb != 0 && band == 0) {
        i = (t % STRIP_TILES) / max(g_rows_b, 1);
        if (i >= g_nb) return 0;
        k = i + 1;
    } else {
        uint per = STRIP_TILES / max(g_rows, 1);
        i = (t % STRIP_TILES) / max(g_rows, 1);
        if (i >= per) return 0;
        k = g_nb + (band - (g_nb != 0 ? 1 : 0)) * per + i + 1;
    }
    if (k > g_count) return 0;
    select_core(k);
    return k;
}
// Texel lin (core 0's numbering) of the selected core: a worker keeps its floats after its cache.
uint4 st(uint lin) {
    uint end = L1_AT + BUCKETS(bits) * 4, w = lin >= FP_AT ? end + lin - FP_AT : lin;
    if (lin >= end && lin < FP_AT) return 0;
    if (core == 0) return TEX(uint2(w & 63, w >> 6));
    uint t = tile0 + (w >> 6), i = w & 63;
    return TEX(tile_at(t) + uint2(i & 7, i >> 3));
}
// which texel (core 0's numbering) texel w of the selected core's rows is; 4095 for none
uint lin_of(uint w) {
    uint end = L1_AT + BUCKETS(bits) * 4;
    return w < end ? w : w < end + 8 ? FP_AT + w - end : 4095;
}

uint idx(uint4 v, uint i) { return (i & 2) ? ((i & 1) ? v.w : v.z) : ((i & 1) ? v.y : v.x); }
uint4 put(uint4 v, uint i, uint x) { return uint4(i == 0 ? x : v.x, i == 1 ? x : v.y, i == 2 ? x : v.z, i == 3 ? x : v.w); }
uint unpack(float4 f) { return uint(f.a * 255.0 + 0.5) << 24 | uint(f.b * 255.0 + 0.5) << 16 | uint(f.g * 255.0 + 0.5) << 8 | uint(f.r * 255.0 + 0.5); }

// ---- The processor's state in this pass
static uint xr[65];   // x0-x31, f0-f31 as bits, and [64] for writes that must not land
static uint pc, priv, stall, clock, mstatus, mie, mip, stvec, sscratch, sepc, scause, stval, scounteren, fcsr;
static uint tcmp_lo, tcmp_hi, now_lo, now_hi, res_en, res_addr, msrc, mdst, mn, ovf_addr, ovf_val, bloom, dirty, mc_word;
static bool quiet;            // nothing the general step looks at after an instruction has changed
static bool trapped;
static uint tcause, tval;
static uint budget;           // the fast loop's instructions at most (a store with no room makes it 0)
static uint used;             // core 0: its instructions in this frame's rounds before this one (texel 39.y)
static bool early;            // core 0: this round ended before its instructions did (39.z)

// ---- Memory. The cache is a local array of the pixel (c), its tags at 4b, its texels after;
// a bit a bucket says it was written in this pass (it starts undefined otherwise). The bits are a
// vector in registers: as a local array of four they cost a load what the card's local memory costs.
#define C_N 448
static uint4 occv = 0;
#define OCC(b) (((idx(occv, (b) >> 5) >> ((b) & 31)) & 1) != 0)
#define OCC_SET(b) occv = put(occv, (b) >> 5, idx(occv, (b) >> 5) | (1u << ((b) & 31)))
static uint dr_t = 0xffffffff;   // the RAM texel read last for data, as the texture has it
static uint4 dr_v;
uint bucket(uint t, uint k) {
    uint n = 1u << bits;
    return k == 0 ? t & (n - 1) : k == 1 ? n + H1(t, bits - 1) : n + (n >> 1) + (H2(t, bits - 1) & ((n >> 2) - 1));
}
// Most searches are for a texel just used (the next word of a save, a structure, a copy): the two
// found or made last are remembered with where they are, and the texel looked for last and not found.
// (A bit a texel by a hash, saying the cache was given it, spared searches and cost more than they
// did: its 32 words were a local array, read on every load the bloom let through.)
static uint m1_t = 0xffffffff, m1_at = 0, m2_t = 0xffffffff, m2_at = 0, absent_t = 0xffffffff;   // (absent_t: looked for last and not there)
void remember(uint t, uint at) { m2_t = m1_t; m2_at = m1_at; m1_t = t; m1_at = at; }
uint slot(uint4 tags, uint tag) { return tags.x == tag ? 1 : tags.y == tag ? 2 : tags.z == tag ? 3 : 0; }
// where the cache holds RAM texel t (4b + entry), or 0: a table is looked in when the one before is full
uint find(inout uint4 c[C_N], uint t) {
    // (the tables one after another, each one's bucket worked out only when it is looked in: rvc_opt's l1_find)
    uint n = 1u << bits, b = t & (n - 1), found = 0;
    bool more = false;
    [branch] if (OCC(b)) {
        uint4 tags = c[4 * b];
        uint e = slot(tags, t + 1);
        found = e != 0 ? 4 * b + e : 0;
        more = e == 0 && tags.z != 0;
    }
    [branch] if (more) {
        b = n + H1(t, bits - 1);
        more = false;
        [branch] if (OCC(b)) {
            uint4 tags = c[4 * b];
            uint e = slot(tags, t + 1);
            found = e != 0 ? 4 * b + e : 0;
            more = e == 0 && tags.z != 0;
        }
        [branch] if (more) {
            b = n + (n >> 1) + (H2(t, bits - 1) & ((n >> 2) - 1));
            [branch] if (OCC(b)) {
                uint e = slot(c[4 * b], t + 1);
                found = e != 0 ? 4 * b + e : 0;
            }
        }
    }
    return found;
}
// a word of RAM (o its offset, on a word); the texture is asked first, the cache meanwhile
uint ram_word(inout uint4 c[C_N], uint o) {
    uint t = o >> 4;
    [branch] if (t != dr_t) { dr_v = TEX(RAM_ADDR(t)); dr_t = t; }
    bool maybe = (o & bloom) == o;
    uint at = 0;
    [branch] if (maybe) {
        at = t == m1_t ? m1_at : t == m2_t ? m2_at : 0;
        [branch] if (at == 0 && t != absent_t) {
            at = find(c, t);
            if (at != 0) remember(t, at); else absent_t = t;
        }
    }
    uint4 tx = dr_v;
    [branch] if (at != 0) tx = c[at];   // (an if: a ?: read the cache's entry on every load, there or not)
    uint w = idx(tx, (o >> 2) & 3);
    return maybe && at == 0 && ovf_addr == o ? ovf_val : w;
}
// `bits` into the word at RAM offset o under `mask`: the tables looked in one after another, each
// one's bucket worked out only when the one before is full; a whole word into a texel the cache
// has is written without reading what was there (rvc_opt's shape, mem.h)
void ram_store(inout uint4 c[C_N], uint o, uint v, uint mask) {
    if (o >= RAM_MAX) return;
    uint t = o >> 4, wi = (o >> 2) & 3, tag = t + 1, n = 1u << bits, b0 = 0, b1 = 0, f0 = 0, f1 = 0;
    uint4 tags0 = 0, tags1 = 0;
    uint at = t == m1_t ? m1_at : t == m2_t ? m2_at : 0;
    [branch] if (at == 0) {
        b0 = t & (n - 1);
        [branch] if (OCC(b0)) tags0 = c[4 * b0];
        uint e0 = slot(tags0, tag), e1 = 0;
        f0 = slot(tags0, 0);
        [branch] if (e0 == 0 && f0 == 0) {
            b1 = n + H1(t, bits - 1);
            [branch] if (OCC(b1)) tags1 = c[4 * b1];
            e1 = slot(tags1, tag); f1 = slot(tags1, 0);
            [branch] if (e1 == 0 && f1 == 0) {
                b1 = n + (n >> 1) + (H2(t, bits - 1) & ((n >> 2) - 1));
                tags1 = 0;
                [branch] if (OCC(b1)) tags1 = c[4 * b1];
                e1 = slot(tags1, tag); f1 = slot(tags1, 0);
            }
        }
        at = e0 != 0 ? 4 * b0 + e0 : e1 != 0 ? 4 * b1 + e1 : 0;
        if (at != 0) remember(t, at);
    }
    [branch] if (at != 0 && mask == 0xffffffff) {
        bloom |= o;
        [branch] if (wi < 2) { if (wi == 0) c[at].x = v; else c[at].y = v; }
        else { if (wi == 2) c[at].z = v; else c[at].w = v; }
        return;
    }
    uint4 texel;
    [branch] if (at != 0) texel = c[at];
    else {
        [branch] if (t != dr_t) { dr_v = TEX(RAM_ADDR(t)); dr_t = t; }
        texel = dr_v;
    }
    uint cur = at == 0 && ovf_addr == o && (o & bloom) == o ? ovf_val : idx(texel, wi);
    uint val = (cur & ~mask) | (v & mask);
    if (val == cur) return;
    bloom |= o;
    dirty |= 1u << ((o >> 22) & 31);
    texel = put(texel, wi, val);
    [branch] if (at != 0) {
        c[at] = texel;
    } else if (f0 != 0 || f1 != 0) {
        uint b = f0 != 0 ? b0 : b1, f = f0 != 0 ? f0 : f1;
        c[4 * b] = put(f0 != 0 ? tags0 : tags1, f - 1, tag);
        c[4 * b + f] = texel;
        OCC_SET(b);
        remember(t, 4 * b + f);
        absent_t = 0xffffffff;
    } else {
        ovf_addr = o; ovf_val = val;   // made by the commit, which ends the pass here
        stall = STALL_MEM; budget = 0;
    }
}
float4 rom4(Texture2D<float4> r, Texture2D<float4> gg, Texture2D<float4> b, Texture2D<float4> a, uint2 p, uint i) {
    [branch] if (i < 2) return i == 0 ? r[p] : gg[p];
    return i == 2 ? b[p] : a[p];
}
// a word anywhere (a on a word): RAM, the ROM, the device tree, the clock chip, the timer
uint load_word(inout uint4 c[C_N], uint a) {
    [branch] if (a & 0x80000000) {
        uint o = a & 0x7fffffff;
        return o < RAM_MAX ? ram_word(c, o) : 0;
    }
    [branch] if (a & 0x40000000) {
        uint2 d; _Data_MTD_R.GetDimensions(d.x, d.y);
        uint t = (a - 0x40000000) >> 4;
        return unpack(rom4(_Data_MTD_R, _Data_MTD_G, _Data_MTD_B, _Data_MTD_A, uint2(t % d.x, d.y - 1 - t / d.x), (a >> 2) & 3));
    }
    [branch] if (a - 0x1020 < 0xfe0)
        return unpack(rom4(_Data_DTB_R, _Data_DTB_G, _Data_DTB_B, _Data_DTB_A, uint2((a - 0x1020) >> 4, 0), (a >> 2) & 3));
    return a == 0x030007f8 ? _RTC0 : a == 0x030007fc ? _RTC1 : a == 0x02004000 ? tcmp_lo : a == 0x02004004 ? tcmp_hi :
           a == 0x0200bff8 ? now_lo : a == 0x0200bffc ? now_hi : 0;
}
// a store of `size` bytes anywhere: RAM (a byte at a time when it is not one whole word), the timer
void store_any(inout uint4 c[C_N], uint a, uint v, uint size) {
    [branch] if ((a & 0x80000000) == 0) {
        quiet = false;
        if (size == 4 && a == 0x02004000) tcmp_lo = v;
        if (size == 4 && a == 0x02004004) tcmp_hi = v;
        return;
    }
    bool whole = size == 4 && (a & 3) == 0;
    [loop] for (uint i = 0; i < (whole ? 1 : size); i++) {
        uint o = (a + i) & 0x7fffffff, sh = (o & 3) * 8;
        ram_store(c, o & ~3u, whole ? v : ((v >> (8 * i)) & 0xff) << sh, whole ? 0xffffffff : 0xffu << sh);
    }
}

// ---- Instructions. The fetch reads the texture (never the cache: code a pass stores runs from
// the next, after fence.i, which ends the pass); a texel is four instructions, the next is read too.
static uint fw_t = 0xffffffff;
static uint pre_w;           // the word the fast loop fetched and left to the general step
static bool pre_valid = false;
static uint4 fw0, fw1;
uint fetch(uint a) {
    uint t = (a & 0x7fffffff) >> 4;
    // (no test of "this texel is the window's": a jump within the texel reads it again, 3 in 1,000,
    // and every other entry passes a test less; rvc_opt's cpu.h)
    [branch] if (t == fw_t + 1) fw0 = fw1; else fw0 = TEX(RAM_ADDR(t));   // (an if: a ?: would read the texel either way)
    fw1 = TEX(RAM_ADDR(t + 1)); fw_t = t;
    return idx(fw0, (a >> 2) & 3);
}
uint imm_i(uint w) { return asuint(asint(w) >> 20); }
uint imm_s(uint w) { return (asuint(asint(w) >> 20) & 0xffffffe0) | ((w >> 7) & 31); }
uint imm_b(uint w) { return asuint(asint(w & 0x80000000) >> 19) | ((w & 0x80) << 4) | ((w >> 20) & 0x7e0) | ((w >> 7) & 0x1e); }
uint imm_j(uint w) { return asuint(asint(w & 0x80000000) >> 11) | (w & 0xff000) | ((w >> 9) & 0x800) | ((w >> 20) & 0x7fe); }
uint mulhu(uint a, uint b) {
    uint al = a & 0xffff, ah = a >> 16, bl = b & 0xffff, bh = b >> 16, lh = al * bh, hl = ah * bl;
    return ah * bh + (lh >> 16) + (hl >> 16) + ((((al * bl) >> 16) + (lh & 0xffff) + (hl & 0xffff)) >> 16);
}
// M by its kind, a product first: a division is dear, and one result made of eight costs every mul all of them
uint op_m(uint f3, uint a, uint b) {
    [branch] if (f3 == 0) return a * b;
    [branch] if (f3 < 4) {
        uint hu = mulhu(a, b);
        return f3 == 1 ? hu - ((a >> 31) ? b : 0) - ((b >> 31) ? a : 0) : f3 == 2 ? hu - ((a >> 31) ? b : 0) : hu;
    }
    bool zero = b == 0, wraps = a == 0x80000000 && b == 0xffffffff;
    int sa = asint(a), sb = asint(zero || wraps ? 1 : b);
    uint safe = zero ? 1 : b;
    [branch] if (f3 == 4) return zero ? 0xffffffff : wraps ? a : asuint(sa / sb);
    [branch] if (f3 == 5) return zero ? 0xffffffff : a / safe;
    [branch] if (f3 == 6) return zero ? a : wraps ? 0 : asuint(sa % sb);
    return zero ? a : a % safe;
}
// The floats that are signs, the lesser or greater, comparisons and moves (the fast loop's)
uint fp_light(uint w, uint ra, uint rb, out bool to_f, out bool ok) {
    uint f7 = w >> 25, f3 = (w >> 12) & 7;
    precise float a = asfloat(ra), b = asfloat(rb);
    bool an = (ra & 0x7fffffff) > 0x7f800000, bn = (rb & 0x7fffffff) > 0x7f800000, less = a < b || (ra == 0x80000000 && rb == 0);
    to_f = f7 == 0x10 || f7 == 0x14 || f7 == 0x78;
    ok = f7 == 0x10 || f7 == 0x50 ? f3 < 3 : f7 == 0x14 ? f3 < 2 : ((w >> 20) & 31) == 0 && f3 == 0;
    return f7 == 0x10 ? (ra & 0x7fffffff) | ((f3 == 0 ? rb : f3 == 1 ? ~rb : ra ^ rb) & 0x80000000)
         : f7 == 0x14 ? (an ? (bn ? 0x7fc00000 : rb) : bn ? ra : ((less == (f3 == 0)) ? ra : rb))
         : f7 == 0x50 ? ((!an && !bn && (f3 == 2 ? a == b : f3 == 1 ? a < b : a <= b)) ? 1 : 0) : ra;
}
// Every other float of opcode 0x53, every result made and one chosen (the general step's). The
// card divides by multiplying with 1 / b: two ordinary numbers have their exponents moved to the middle.
uint fp_all(uint w, uint ra, uint rb, out bool to_f, out bool ok) {
    uint f7 = w >> 25, f3 = (w >> 12) & 7, sel = (w >> 20) & 31, ea = (ra >> 23) & 0xff, eb = (rb >> 23) & 0xff;
    precise float a = asfloat(ra), b = asfloat(rb);
    bool an = (ra & 0x7fffffff) > 0x7f800000;
    int de = (int)ea - (int)eb, ha = clamp(de, -126, 127);
    precise float da = asfloat((ra & 0x807fffff) | ((uint)(127 + ha) << 23)), db = asfloat((rb & 0x807fffff) | ((uint)(127 - (de - ha)) << 23));
    precise float q = (ea - 1 < 254 && eb - 1 < 254) ? da / db : a / b;
    precise float sum = a + b, dif = a - b, prod = a * b, root = sqrt(a);
    uint ar = asuint(f7 == 0 ? sum : f7 == 4 ? dif : f7 == 8 ? prod : f7 == 0x0c ? q : root);
    ar = (ar & 0x7fffffff) > 0x7f800000 ? 0x7fc00000 : ar;
    float whole = f3 == 1 ? trunc(a) : f3 == 2 ? floor(a) : f3 == 3 ? ceil(a) : round(a);
    uint to_s = (an || whole >= 2147483648.0) ? 0x7fffffff : whole <= -2147483648.0 ? 0x80000000 : asuint((int)whole);
    uint to_u = (an || whole >= 4294967296.0) ? 0xffffffff : whole <= 0.0 ? 0 : whole >= 2147483648.0 ? 0x80000000 + (uint)(whole - 2147483648.0) : (uint)whole;
    bool neg_in = sel == 0 && (ra >> 31) != 0, neg = (ra >> 31) != 0;
    uint from = asuint((float)(neg_in ? 0 - ra : ra)) | (neg_in ? 0x80000000 : 0), man = ra & 0x7fffff, ex = (ra >> 23) & 0xff;
    uint cls = ex == 0xff ? (man != 0 ? ((man & 0x400000) ? 0x200 : 0x100) : (neg ? 1 : 0x80))
             : ex == 0 ? (man != 0 ? (neg ? 4 : 0x20) : (neg ? 8 : 0x10)) : (neg ? 2 : 0x40);
    uint light = fp_light(w, ra, rb, to_f, ok);
    bool arith = (f7 & 0x73) == 0 || f7 == 0x2c;
    to_f = to_f || arith || f7 == 0x68;
    ok = arith ? (f7 != 0x2c || sel == 0) : (f7 == 0x60 || f7 == 0x68) ? sel < 2 : f7 == 0x70 ? (sel == 0 && f3 < 2) :
         (f7 == 0x10 || f7 == 0x14 || f7 == 0x50 || f7 == 0x78) && ok;
    return arith ? ar : f7 == 0x60 ? (sel == 0 ? to_s : to_u) : f7 == 0x68 ? from : f7 == 0x70 && f3 == 1 ? cls : light;
}

// The CSRs Linux uses, as fields; the machine's own read as zeros, a write to one not kept goes nowhere.
uint csr_read(uint n) {
    uint fs = (mstatus & 0x6000) != 0 ? 0x80006000 : 0, r = asuint(_Time.w);   // FS reads dirty whenever it is on
    r ^= r << 13; r ^= r >> 17; r ^= r << 5;
    return n == 0x301 ? 0x40141101 : n == 1 ? fcsr & 0x1f : n == 2 ? (fcsr >> 5) & 7 : n == 3 ? fcsr & 0xff :
           n == 0x100 ? (mstatus & 0xde162) | fs : n == 0x300 ? mstatus | fs : n == 0x104 ? mie & 0x222 : n == 0x304 ? mie :
           n == 0x144 ? mip & 0x222 : n == 0x344 ? mip : n == 0x105 ? stvec : n == 0x106 ? scounteren :
           n == 0x140 ? sscratch : n == 0x141 ? sepc : n == 0x142 ? scause : n == 0x143 ? stval :
           n == 0xc01 ? now_lo : n == 0xb00 || n == 0xc00 ? clock : n == 0xf14 ? core :
           n == 0x0bf ? r : n == 0x0be ? _PlayerID : 0;
}
void csr_write(uint n, uint v) {
    // (only what interrupts see, and the copy and fill, which stall: sscratch, sepc and the rest leave the next instruction to the fast loop)
    if ((n & 0xfff) == 0x100 || n == 0x300 || (n & 0xdff) == 0x104 || (n & 0xdff) == 0x144 || n == 0x0b0) quiet = false;
    if (n - 1 < 3) fcsr = n == 1 ? (fcsr & ~0x1fu) | (v & 0x1f) : n == 2 ? (fcsr & ~0xe0u) | ((v & 7) << 5) : v & 0xff;
    if (n == 0x100) mstatus = (mstatus & ~0xde162u) | (v & 0xde162);
    if (n == 0x300) mstatus = v;
    if (n == 0x104 || n == 0x304) mie = n == 0x304 ? v : (mie & ~0x222u) | (v & 0x222);
    if (n == 0x144 || n == 0x344) mip = n == 0x344 ? v : (mip & ~0x222u) | (v & 0x222);
    if (n == 0x105) stvec = v;
    if (n == 0x106) scounteren = v;
    if (n == 0x140) sscratch = v;
    if (n == 0x141) sepc = v;
    if (n == 0x142) scause = v;
    if (n == 0x143) stval = v;
    if (n == 0x0b1) msrc = v;
    if (n == 0x0b2) mdst = v;
    if (n == 0x0b3) mn = v;
    if (n == 0x0b0) stall = v == 2 ? STALL_FILL : STALL_COPY;   // the kernel's copy and fill: the commit makes them
}
bool timer_due() { return (tcmp_lo | tcmp_hi) != 0 && (now_hi > tcmp_hi || (now_hi == tcmp_hi && now_lo >= tcmp_lo)); }
// the supervisor's calls to firmware, answered here (docs/machine/boot.md)
void sbi() {
    uint ext = xr[17], a0 = xr[10], fn = xr[16];
    if (ext == 0x54494D45 || ext == 0) { tcmp_lo = a0; tcmp_hi = xr[11]; mip = timer_due() ? mip | 0x20 : mip & ~0x20u; }
    uint val = ext != 0x10 ? 0 : fn == 0 ? 2 : fn == 1 ? 0x454d55 : fn == 2 ? 1 : (fn == 3 && (a0 == 0x54494D45 || a0 <= 2)) ? 1 : 0;
    if (ext < 0x10) xr[10] = val;
    else { xr[10] = ext == 0x10 || ext == 0x54494D45 ? 0 : 0xfffffffe; xr[11] = val; }
    quiet = false;
}
void raise(uint cause, uint val) { if (!trapped) { trapped = true; tcause = cause; tval = val; } }

// ---- The fast loop: what cannot trap or change what interrupts see, back to back; anything
// else leaves it, and the general step takes that instruction. Returns how many ran.
uint fast(inout uint4 c[C_N], uint room) {
    uint n = 0;
    budget = room;
    bool same = false;   // the last stayed within its texel: this one is the next word of it
    uint4 cur = 0;
    [loop] while (n < budget) {
        [branch] if (!same) { fetch(pc); uint s = (pc >> 2) & 3; cur = (s & 2) ? ((s & 1) ? fw0.wxyz : fw0.zwxy) : ((s & 1) ? fw0.yzwx : fw0); }
        uint w = cur.x, opc = w & 0x7f, f3 = (w >> 12) & 7, rd = (w >> 7) & 31;
        uint a = xr[(w >> 15) & 31], b = xr[(w >> 20) & 31], next = pc + 4, v = 0, dst = rd == 0 ? 64 : rd;
        bool ok = true;
        // arithmetic, loads, stores and floats a group, in it arithmetic and loads one (rvc_opt's tree)
        bool alu = (w & 0x5f) == 0x13 && (w & 0xfe00007f) != 0x02000033, ld = opc == 0x03;   // (flw and fsw are the floats')
        [branch] if (alu || ld || opc == 0x23 || (w & 0x5f) == 0x07 || opc == 0x53) {
            [branch] if (alu || ld) {
                [branch] if (alu) {
                    // op-imm and op: every result made, one chosen; signed is unsigned with the signs turned over
                    bool reg = (w & 0x20) != 0, alt = (w & 0x40000000) != 0;
                    uint y = reg ? b : imm_i(w), s = y & 31, turn = (f3 & 1) ? 0 : 0x80000000, f7 = w >> 25;
                    uint r01 = (f3 & 1) ? a << s : ((reg && alt) ? a - y : a + y), r23 = (a ^ turn) < (y ^ turn) ? 1 : 0;
                    uint r45 = (f3 & 1) ? (alt ? asuint(asint(a) >> s) : a >> s) : a ^ y, r67 = (f3 & 1) ? a & y : a | y;
                    v = (f3 & 4) ? ((f3 & 2) ? r67 : r45) : ((f3 & 2) ? r23 : r01);
                    ok = (((f7 == 0 ? 0xffffu : f7 == 0x20 ? 0x21fdu : 0x00ddu) >> (((w >> 2) & 8) | f3)) & 1) != 0;
                } else {
                    // lb lh lw lbu lhu, within one word of RAM
                    uint va = a + imm_i(w), up = 24 - 8 * (f3 & 3) - ((f3 & 3) == 2 ? 8 : 0);
                    ok = va != 0 && ((0x37u >> f3) & 1) != 0 && (va & 3) + (1u << (f3 & 3)) <= 4 && (va ^ 0x80000000) < RAM_MAX;
                    [branch] if (ok) {
                        uint x = ram_word(c, va & 0x7ffffffc) >> ((va & 3) * 8) << up;
                        v = (f3 & 4) ? x >> up : asuint(asint(x) >> up);
                    }
                }
            } else {
                [branch] if (opc == 0x23) {
                    // sb sh sw, within one word of RAM
                    uint sa = a + imm_s(w), off = sa & 3;
                    ok = f3 < 3 && (sa & 0x80000000) != 0 && off + (1u << f3) <= 4;
                    [branch] if (ok) ram_store(c, sa & 0x7ffffffc, b << (off * 8), (f3 == 2 ? 0xffffffff : f3 == 1 ? 0xffff : 0xff) << (off * 8));
                    dst = 64;
                } else [branch] if (opc == 0x53) {
                    // (opcode 0x53 before flw and fsw: a program has more of them, rvc_opt's order) floats: a sum, a difference, a product, and the light ones; the rest are the general step's
                    uint f7 = w >> 25, fa = a, fb = xr[32 + ((w >> 20) & 31)];
                    [branch] if ((f7 & 0x68) != 0x68) fa = xr[32 + ((w >> 15) & 31)];   // (an if: a ?: read the register either way)
                    bool to_f = true;
                    [branch] if ((f7 & 0x73) == 0 && f7 != 0x0c) {
                        precise float x = asfloat(fa), y = asfloat(fb), sum = x + y, dif = x - y, prod = x * y;
                        v = asuint(f7 == 0 ? sum : f7 == 4 ? dif : prod);
                        v = (v & 0x7fffffff) > 0x7f800000 ? 0x7fc00000 : v;
                    } else [branch] if (((0x50100030u >> (f7 >> 2)) & 1) != 0 && (f7 & 3) == 0 && (f7 != 0x70 || f3 == 0)) {
                        v = fp_light(w, fa, fb, to_f, ok);
                    } else {
                        v = fp_all(w, fa, fb, to_f, ok);   // the divisions, roots and conversions too: a float never leaves the loop
                    }
                    dst = to_f ? 32 + rd : dst;
                } else [branch] if (opc == 0x07) {
                    // flw, a word of RAM
                    uint va = a + imm_i(w);
                    ok = va != 0 && f3 == 2 && (va & 0x80000003) == 0x80000000 && (va & 0x7ffffffc) < RAM_MAX;
                    [branch] if (ok) v = ram_word(c, va & 0x7ffffffc);
                    dst = 32 + rd;
                } else {
                    // fsw, a word of RAM
                    uint sa = a + imm_s(w);
                    ok = f3 == 2 && (sa & 0x80000003) == 0x80000000;
                    [branch] if (ok) ram_store(c, sa & 0x7ffffffc, xr[32 + ((w >> 20) & 31)], 0xffffffff);
                    dst = 64;
                }
            }
        } else [branch] if (opc == 0x63) {
            bool lt = (f3 & 2) ? a < b : asint(a) < asint(b);
            next = (((f3 & 4) ? lt : a == b) != ((f3 & 1) != 0)) ? pc + imm_b(w) : next;
            ok = (f3 & 6) != 2 && (next & 3) == 0;
            dst = 64;
        } else [branch] if (opc == 0x37 || opc == 0x17 || opc == 0x6f || (opc == 0x67 && f3 == 0)) {
            bool jump = (w & 0x40) != 0;
            v = jump ? pc + 4 : (w & 0xfffff000) + (opc == 0x17 ? pc : 0);
            next = opc == 0x6f ? pc + imm_j(w) : opc == 0x67 ? a + imm_i(w) : next;
            ok = (next & 3) == 0;
        } else [branch] if ((w & 0x707f) == 0x202f) {
            // amoadd amoswap lr sc amoxor amoor amoand on a word of RAM (the others are the general step's)
            uint f5 = w >> 27, o = a & 0x7ffffffc;
            bool lr = f5 == 2, sc = f5 == 3, sc_ok = res_en != 0 && res_addr == a;
            ok = a != 0 && (a & 0x80000003) == 0x80000000 && o < RAM_MAX && f5 < 16 && ((0x111fu >> f5) & 1) != 0 && (!lr || ((w >> 20) & 31) == 0);
            [branch] if (ok) {
                uint old = sc ? 0 : ram_word(c, o);
                [branch] if (sc ? sc_ok : !lr) ram_store(c, o, f5 == 0 ? old + b : f5 == 4 ? old ^ b : f5 == 8 ? old | b : f5 == 12 ? old & b : b, 0xffffffff);
                res_en = lr ? 1 : sc && sc_ok ? 0 : res_en;
                res_addr = lr ? a : res_addr;
                v = sc ? (sc_ok ? 0 : 1) : old;
            }
        } else [branch] if (opc == 0x33 && (w >> 25) == 1) {
            v = op_m(f3, a, b);
        } else [branch] if (opc == 0x73 && (f3 & 3) != 0) {
            // csrrw, csrrs, csrrc and their immediates on a CSR nothing else looks at (sscratch, sepc, scause,
            // stval, stvec, scounteren, the float flags, time, the hart): the kernel's trap entry and exit are
            // full of them, and each was a general step
            uint cn = w >> 20, src = (f3 & 4) ? (w >> 15) & 31 : a;
            bool wr_ = (f3 & 3) == 1 || src != 0;
            bool plain = (cn - 0x140 < 4) || cn == 0x105 || cn == 0x106 || (cn - 1 < 3) || cn == 0xc01 || cn == 0xf14;
            ok = plain && ((cn >> 8) & 3) <= priv && !(wr_ && ((cn >> 10) & 3) == 3);
            [branch] if (ok) {
                v = csr_read(cn);
                if (wr_) csr_write(cn, (f3 & 3) == 1 ? src : (f3 & 3) == 2 ? v | src : v & ~src);
            }
        } else ok = false;
        if (!ok) { pre_w = w; pre_valid = true; break; }
        xr[dst] = v;
        same = next == pc + 4 && (next & 12) != 0;
        cur = cur.yzwx;
        pc = next;
        n++;
    }
    clock += n;
    return n;
}

// ---- The general step: one instruction of any kind, then the traps and interrupts.
void worker_fault(inout uint4 c[C_N]);
void step(inout uint4 c[C_N]) {
    clock++;
    trapped = false;
    uint w;
    [branch] if (pre_valid) w = pre_w; else w = fetch(pc);
    pre_valid = false;
    uint opc = w & 0x7f, f3 = (w >> 12) & 7, rd = (w >> 7) & 31, f7 = w >> 25;
    uint a = xr[(w >> 15) & 31], b = xr[(w >> 20) & 31], next = pc + 4, v = 0, dst = rd == 0 ? 64 : rd;
    if ((pc & 3) != 0) { raise(0, pc); w = 0; opc = 0x7f; }
    // a read for loads and atomics, put together from two words when it is not on one (at 0: nothing)
    uint ra = (opc & 0x7b) == 0x03 ? a + imm_i(w) : (w & 0x202f) == 0x202f ? a : 0, mv = 0;
    [branch] if (ra != 0) {
        uint sh = (ra & 3) * 8;
        [loop] for (uint h = 0; h < (sh != 0 ? 2u : 1u); h++) {
            uint x = load_word(c, (ra & ~3u) + 4 * h);
            mv |= h == 0 ? x >> sh : x << (32 - sh);
        }
    }
    [branch] if (opc == 0x13 || (opc == 0x33 && f7 != 1)) {
        uint f6 = w >> 26, y = opc == 0x13 ? imm_i(w) : b, s = y & 31;
        bool sub = opc == 0x33 && f7 == 0x20;
        v = f3 == 0 ? (sub ? a - y : a + y) : f3 == 1 ? a << s : f3 == 2 ? (asint(a) < asint(y) ? 1 : 0) : f3 == 3 ? (a < y ? 1 : 0) :
            f3 == 4 ? a ^ y : f3 == 5 ? ((w & 0x40000000) ? asuint(asint(a) >> s) : a >> s) : f3 == 6 ? a | y : a & y;
        bool okk = opc == 0x13 ? ((f3 & 3) != 1 || f6 == 0 || (f3 == 5 && f6 == 0x10)) : (f7 == 0 || (f7 == 0x20 && (f3 == 0 || f3 == 5)));
        if (!okk) raise(2, w);
    } else [branch] if (opc == 0x33) {
        v = op_m(f3, a, b);
    } else [branch] if (opc == 0x03 || opc == 0x07) {
        uint up = 24 - 8 * (f3 & 3) - ((f3 & 3) == 2 ? 8 : 0), x = mv << up;
        v = (f3 & 4) ? x >> up : asuint(asint(x) >> up);
        dst = opc == 0x07 ? 32 + rd : dst;
        if (opc == 0x07 ? f3 != 2 : ((0x37u >> f3) & 1) == 0) raise(2, w);
    } else [branch] if (opc == 0x23 || opc == 0x27) {
        if (opc == 0x27 ? f3 != 2 : f3 > 2) raise(2, w);
        else store_any(c, a + imm_s(w), opc == 0x27 ? xr[32 + ((w >> 20) & 31)] : b, 1u << f3);
        dst = 64;
    } else [branch] if (opc == 0x63) {
        bool lt = (f3 & 2) ? a < b : asint(a) < asint(b);
        next = (((f3 & 4) ? lt : a == b) != ((f3 & 1) != 0)) ? pc + imm_b(w) : next;
        if ((f3 & 6) == 2) raise(2, w);
        dst = 64;
    } else [branch] if (opc == 0x37 || opc == 0x17 || opc == 0x6f || (opc == 0x67 && f3 == 0)) {
        v = (w & 0x40) ? pc + 4 : (w & 0xfffff000) + (opc == 0x17 ? pc : 0);
        next = opc == 0x6f ? pc + imm_j(w) : opc == 0x67 ? a + imm_i(w) : next;
    } else [branch] if (opc == 0x2f && f3 == 2) {
        // lr, sc and the atomics, on the word read above
        uint f5 = w >> 27, x = f5 == 1 ? b : f5 == 0 ? mv + b : f5 == 4 ? mv ^ b : f5 == 12 ? mv & b : f5 == 8 ? mv | b :
                 f5 == 16 ? (asint(b) < asint(mv) ? b : mv) : f5 == 20 ? (asint(b) > asint(mv) ? b : mv) : f5 == 24 ? min(b, mv) : max(b, mv);
        bool sc_ok = res_en != 0 && res_addr == a;
        v = f5 == 3 ? (sc_ok ? 0 : 1) : mv;
        if (f5 == 2) { res_en = 1; res_addr = a; }
        if (f5 == 3 && sc_ok) res_en = 0;
        if (f5 > 28 || ((f5 & 3) != 0 && f5 > 3) || (f5 == 2 && ((w >> 20) & 31) != 0)) raise(2, w);
        else if (f5 != 2 && (f5 != 3 || sc_ok)) store_any(c, a, f5 == 3 ? b : x, 4);
    } else [branch] if (opc == 0x53) {
        bool to_f, ok;
        v = fp_all(w, (f7 & 0x68) == 0x68 ? a : xr[32 + ((w >> 15) & 31)], xr[32 + ((w >> 20) & 31)], to_f, ok);
        dst = to_f ? 32 + rd : dst;
        if (!ok) raise(2, w);
    } else [branch] if ((w & 0x06000073) == 0x43) {
        // fmadd, fmsub, fnmsub, fnmadd: a product and a sum, each rounded
        precise float x = asfloat(xr[32 + ((w >> 15) & 31)]), y = asfloat(xr[32 + ((w >> 20) & 31)]), z = asfloat(xr[32 + (w >> 27)]);
        precise float p = x * y, r = ((w >> 2) & 3) == 0 ? p + z : ((w >> 2) & 3) == 1 ? p - z : ((w >> 2) & 3) == 2 ? z - p : (-p) - z;
        v = (asuint(r) & 0x7fffffff) > 0x7f800000 ? 0x7fc00000 : asuint(r);
        dst = 32 + rd;
    } else [branch] if (opc == 0x73) {
        // every system instruction reads the CSR its top bits name, and traps where it may not
        uint n = w >> 20, src = (f3 & 4) ? (w >> 15) & 31 : a;
        dst = 64;
        [branch] if (((n >> 8) & 3) > priv) {
            raise(2, pc);
        } else if (f3 != 0) {
            v = csr_read(n);
            dst = rd == 0 ? 64 : rd;
            [branch] if ((f3 & 3) == 1 || src != 0) {
                if (((n >> 10) & 3) == 3) raise(2, pc);
                else if (n != 0x180) csr_write(n, (f3 & 3) == 1 ? src : (f3 & 3) == 2 ? v | src : v & ~src);
            }
        } else if (w == 0x00000073) {
            if (priv == 1) sbi(); else raise(8, pc);
        } else if (w == 0x00100073) {
            if (core != 0) { mc_word = 1 << 8; stall = STALL_WFI; }   // ebreak parks a worker
        } else if (w == 0x10200073) {
            uint s = csr_read(0x100);
            csr_write(0x100, (s & ~0x20122u) | (((s >> 5) & 1) << 1) | 0x20);
            priv = (s >> 8) & 1;
            next = sepc;
        } else if (w == 0x10500073) {
            stall = STALL_WFI;
        } else if ((w & 0xfe007fff) != 0x12000073 && w != 0x30200073 && w != 0x00200073) {
            raise(2, w);
        }
    } else [branch] if (opc == 0x0f && f3 < 2) {
        // fence.i ends the pass, and pause: a worker sleeps on its job word
        dst = 64;
        if (f3 == 1) stall = STALL_FENCE;
        if (w == 0x0100000f) {
            stall = STALL_WFI;
            if (core != 0) mc_word = 3 | (TEX(RAM_ADDR((MBOX + 0x4000 + 64 * core) >> 4)).x << 8);
        }
    } else {
        raise(2, w);
    }
    if (!trapped) xr[dst] = v;
    if (quiet && !trapped) { pc = next; return; }
    [branch] if (core != 0 && trapped) {
        // a worker is given no trap: it stops where it is, its record in its mailbox (docs/machine/multicore.md)
        worker_fault(c);
        trapped = false;
        next = pc;
    }
    bool no_trap = !trapped, handled = false;
    uint pend = 0;
    [branch] if (!trapped) {
        uint m = mip & mie & 0x222;
        pend = (m & 0x200) ? 0x200 : (m & 2) ? 2 : (m & 0x20) ? 0x20 : 0;
        if (pend != 0) { trapped = true; tcause = 0x80000000 | firstbitlow(pend); tval = 0; }
    }
    [branch] if (trapped) {
        // every trap is the supervisor's; an interrupt waits while it has them off
        handled = no_trap == false || priv != 1 || (mstatus & 2) != 0;
        [branch] if (handled) {
            sepc = no_trap ? next : pc;
            scause = tcause;
            stval = tval;
            next = (stvec & 3) != 0 ? (stvec & ~3u) + 4 * (tcause & 0xffff) : stvec;
            mstatus = (mstatus & ~0x122u) | ((mstatus & 2) << 4) | ((priv & 1) << 8);
            priv = 1;
        }
        if (handled && no_trap && pend != 0x20) mip &= ~pend;   // (the timer's bit stays until the next deadline)
    }
    quiet = no_trap && !handled && stall == 0 && (next & 3) == 0;   // (a jump not to a word: the next step traps)
    pc = next;
}
// The record of a worker's trap, the count last; a record that does not all fit is made in the next pass.
void worker_fault(inout uint4 c[C_N]) {
    uint box = MBOX + 0x4000 + 64 * core, seen = TEX(RAM_ADDR((box >> 4) + 3)).x, call = MBOX + 0x2000 + 32 * core, k;
    bool is_call = tcause == 8, room = stall == 0;
    // a call's a7 and a0-a5, the cause, the address, pc, and the count last (one store, made in a loop;
    // each word worked out where it is stored: no array beside the registers and the cache)
    uint n = is_call ? 7 : 0;
    [loop] for (k = 0; k < n + 4; k++) {
        if (k == n + 3 && stall != 0) break;
        uint j = k - n;
        uint a = k < n ? call + 4 * k : box + (j == 3 ? 32 : 36 + 4 * j);
        uint v = k < n ? (k == 0 ? xr[17] : xr[9 + k]) : j == 0 ? tcause : j == 1 ? tval : j == 2 ? pc : seen + 1;
        ram_store(c, a, v, 0xffffffff);
    }
    if (stall == 0) { mc_word = (is_call ? 9 : 5) | (seen << 8); stall = STALL_WFI; }
    else if (room) ovf_addr = 0xffffffff;
}

// ---- The pass
void load_state() {
    uint4 t0 = st(0), t1 = st(1), t2 = st(2), t8 = st(8), t28 = st(28), t37 = st(37);
    uint k;
    mstatus = t0.x; mie = t0.y; mip = t0.z; stvec = t0.w;
    sscratch = t1.x; sepc = t1.y; scause = t1.z; stval = t1.w;
    scounteren = t2.x; fcsr = t2.y;
    priv = t8.z; ovf_addr = 0xffffffff;
    tcmp_lo = st(10).y; tcmp_hi = st(10).z;
    stall = t28.x; clock = t28.y;
    [loop] for (k = 0; k < 8; k++) {
        uint4 r = st(29 + k), f = st(FP_AT + k);
        xr[4 * k + 1] = r.x; xr[4 * k + 2] = r.y; xr[4 * k + 3] = r.z; xr[4 * k + 4] = r.w;
        xr[32 + 4 * k] = f.x; xr[33 + 4 * k] = f.y; xr[34 + 4 * k] = f.z; xr[35 + 4 * k] = f.w;
    }
    xr[0] = 0; pc = st(36).w; xr[32] = st(FP_AT).x;   // (texel 36's last word is pc, not f0)
    res_en = t37.x; res_addr = t37.y; msrc = t37.z; mdst = t37.w; mn = st(38).z;
    mc_word = st(41).w;
    used = core == 0 && _TimePass != 0 ? st(39).y : 0;
#ifdef NO_DOUBLES
    now_lo = _HostMtimeLo; now_hi = _HostMtimeHi;
#else
    double mt = (double)_Time.x * 1000000.0 * 0.1;   // (as the machine has always worked it out)
    now_lo = (uint)floor(mt - 4294967296.0 * floor(mt / 4294967296.0)); now_hi = (uint)(mt / 4294967296.0);
#endif
    now_lo += _TimePass;
    now_hi += now_lo < _TimePass ? 1 : 0;
    if (timer_due()) mip |= 0x20;
}
// a worker's start, stop or answer, from its mailbox as the last commit left it
void worker_enter() {
    bool called = (mc_word & 9) == 9;
    if (mc_word & 14) mc_word = 1;
    uint4 box = TEX(RAM_ADDR((MBOX >> 4) + core));
    if (box.x == MC_START) {
        pc = box.y; xr[2] = box.z; xr[10] = core; priv = 0; stall = 0; mc_word = 1;
    } else if (box.x == MC_STOP || (mc_word & 1) == 0) {
        if (mc_word & 1) mc_word = 2 << 8;
        stall = STALL_WFI;
    } else if (called) {
        xr[10] = TEX(RAM_ADDR(((MBOX + 0x4000 + 64 * core) >> 4) + 3)).y;
        pc += 4;
    }
}
// whether the selected worker has nothing to do in this pass (its pixels then keep what they hold)
bool worker_idle() {
    uint start = TEX(RAM_ADDR((MBOX >> 4) + core)).x, word = st(41).w, box = (MBOX + 0x4000 + 64 * core) >> 4;
    if (start == MC_START || (start == MC_STOP && (word & 1) != 0)) return false;
    if ((word & 1) == 0) return true;
    if (word & 12) return (TEX(RAM_ADDR(box + 3)).x & 0xffffff) == (word >> 8);
    return (word & 2) != 0 && (TEX(RAM_ADDR(box)).x & 0xffffff) == (word >> 8);
}
// texel lin of the state after this pass
uint4 state_out(inout uint4 c[C_N], uint lin) {
    uint end = L1_AT + BUCKETS(bits) * 4;
    [branch] if (lin >= L1_AT && lin < end) {
        uint off = lin - L1_AT, bk = off >> 2;
        if (!OCC(bk)) return 0;
        uint4 tags = c[4 * bk];
        return (off & 3) == 0 ? tags : idx(tags, (off & 3) - 1) == 0 ? (uint4)0 : c[off];
    }
    if (lin >= FP_AT) return uint4(xr[32 + 4 * (lin - FP_AT)], xr[33 + 4 * (lin - FP_AT)], xr[34 + 4 * (lin - FP_AT)], xr[35 + 4 * (lin - FP_AT)]);
    if (lin >= 29 && lin < 37) return uint4(xr[4 * (lin - 29) + 1], xr[4 * (lin - 29) + 2], xr[4 * (lin - 29) + 3], lin == 36 ? pc : xr[4 * (lin - 29) + 4]);
    uint4 was = st(lin);
    return lin == 0 ? uint4(mstatus, mie, mip, stvec) : lin == 1 ? uint4(sscratch, sepc, scause, stval) : lin == 2 ? uint4(scounteren, fcsr, 0, 0) :
           lin == 8 ? uint4(ovf_addr, ovf_val, priv, 0) : lin == 10 ? uint4(0, tcmp_lo, tcmp_hi, 0) : lin == 11 ? uint4(0, 0, 0, 0xffffffff) :
           lin == 28 ? uint4(stall, clock, was.z, 0) : lin == 37 ? uint4(res_en, res_addr, msrc, mdst) : lin == 38 ? uint4(0, 0, mn, 0) :
           lin == 39 ? uint4(was.x, used, early ? 1 : 0, 0) : lin == 40 ? uint4(0, 0, stall, 0) : lin == 41 ? uint4(bloom, dirty, was.z, mc_word) : lin < 41 ? (uint4)0 : was;
}

struct v2f { float4 vertex : SV_POSITION; };
v2f vert(uint id : SV_VertexID) { v2f o; o.vertex = float4(0, 0, 0, 1); return o; }
// A quad for core 0's rectangle, and one for each worker with something to run (16 a primitive).
void quad(inout TriangleStream<v2f> s, float x, float y, float w, float h, float2 size) {
    for (uint k = 0; k < 4; k++) {
        float2 p = float2(x + (k & 1) * w, y + (k >> 1) * h) / size;
        v2f o; o.vertex = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
        s.Append(o);
    }
    s.RestartStrip();
}
[maxvertexcount(64)]
void tick_geom(triangle v2f IN[3], uint prim : SV_PrimitiveID, inout TriangleStream<v2f> s) {
    if (prim >= (CORES + 15) / 16) return;
#ifdef TICK_UNPACK
    float2 size; _SelfTexture2D.GetDimensions(size.x, size.y);
    if (prim == 0) quad(s, 0, 0, 64, CORE0_ROWS, size);
    if (_Init) return;
    geo_read();
    if ((g.r & 1) != 0) { if (prim == 0) quad(s, 64, 0, 8 * STRIP_TILES, 8 * STRIP_BANDS, size); return; }
#else
    float2 size = float2(TICK_W, STATE_ROWS * TICK_BANDS);
    if (prim == 0) quad(s, 0, 0, 8, CORE0_ROWS, size);
    if (_Init) return;
    geo_read();
    if ((g.r & 1) != 0) { if (prim == 0) quad(s, 8, 0, TICK_W - 8, STATE_ROWS * TICK_BANDS, size); return; }
#endif
    for (uint n = 0; n < 16; n++) {
        uint k = prim * 16 + n;
        if (k == 0 || k > g_count) continue;
        select_core(k);
        if (worker_idle() || MORE_ROUND) continue;   // (its state stays as the commit left it)
#ifdef TICK_UNPACK
        uint2 at = tile_at(tile0);
        quad(s, at.x, at.y, 8 * rows_now, 8, size);
#else
        uint2 at = block_of(k);
        quad(s, at.x, at.y, cols_now, 8, size);
#endif
    }
}

#ifdef TICK_UNPACK
Texture2D<uint4> _TickOut0, _TickOut1, _TickOut2, _TickOut3, _TickOut4, _TickOut5, _TickOut6, _TickOut7;
uint4 frag(v2f i) : SV_Target {
    uint2 p = uint2(i.vertex.xy);
    uint3 at = uint3(p.x >> 3, p.y, 0);
    if (p.x >= 64) {
        geo_read();
        uint tile = ((p.x - 64) >> 3) + STRIP_TILES * (p.y >> 3);
        if ((g.r & 1) != 0 || core_at(tile) == 0) return 0;
        at = uint3(block_of(core) + uint2(tile - tile0, p.y & 7), 0);
    }
    uint k = p.x & 7;
    [branch] if (k < 4) return k == 0 ? _TickOut0.Load(at) : k == 1 ? _TickOut1.Load(at) : k == 2 ? _TickOut2.Load(at) : _TickOut3.Load(at);
    return k == 4 ? _TickOut4.Load(at) : k == 5 ? _TickOut5.Load(at) : k == 6 ? _TickOut6.Load(at) : _TickOut7.Load(at);
}
#else
struct tick_out { uint4 t0 : SV_Target0; uint4 t1 : SV_Target1; uint4 t2 : SV_Target2; uint4 t3 : SV_Target3;
                  uint4 t4 : SV_Target4; uint4 t5 : SV_Target5; uint4 t6 : SV_Target6; uint4 t7 : SV_Target7; };
tick_out frag(v2f i) {
    uint2 p = uint2(i.vertex.xy);
    p.y %= STATE_ROWS * TICK_BANDS;
    uint4 o[8], c[C_N];
    uint k, w0 = p.y * 64 + p.x * 8;   // the pixel's first texel, in its core's rows
    uint2 own = uint2(p.x * 8, p.y);   // and where that is in the state texture
    geo_read();
    select_core(0);
    bool none = false;
    [branch] if (p.x >= 8 || p.y >= STATE_ROWS) {
        uint tile;
        none = _Init != 0 || core_in(p, tile) == 0 || tile >= rows_now;
        w0 = (tile << 6) + ((p.y & 7) << 3);
        own = tile_at(tile0 + tile) + uint2(0, p.y & 7);
    } else none = p.y >= CORE0_ROWS;
    bool keep = !none && !_Init && core != 0 && (worker_idle() || MORE_ROUND);
    bool any = false;
    for (k = 0; k < 8; k++) any = any || lin_of(w0 + k) < FP_AT + 8;
    [branch] if (!none && !keep && any && !(_Init && _InitRaw)) {
        if (_Init) {
            // as firmware would enter the kernel: supervisor mode, the device tree's address in a1
            stall = 0; clock = 0; priv = 1; pc = 0x80400000; ovf_addr = 0xffffffff;
            for (k = 0; k < 65; k++) xr[k] = 0;
            xr[11] = 0x82200000;
        } else {
            load_state();
            if (core != 0) worker_enter();
            // a round after the first (--tick-rounds) gives core 0 what is left of its frame's instructions,
            // and the workers all of theirs: core 0 runs as with a round a frame, the workers it waits for more
            uint n = 0, most = max(_Ticks / max(_TicksDivisor, 1), 2);
            most = core != 0 ? most : min(most, most * max(_TickFull, 1) - min(used, most * max(_TickFull, 1)));
            [loop] while (n < most && stall == 0) {
                [branch] if (quiet) n += fast(c, most - n);
                if (n >= most || stall != 0) break;
                step(c);
                n++;
            }
            used += n;
            early = n < most;
        }
    }
    for (k = 0; k < 8; k++) {
        uint lin = lin_of(w0 + k);
        o[k] = none ? (uint4)0 : (_Init && _InitRaw) ? st(lin) : keep || lin >= FP_AT + 8 ? TEX(own + uint2(k, 0)) : state_out(c, lin);
    }
    tick_out r;
    r.t0 = o[0]; r.t1 = o[1]; r.t2 = o[2]; r.t3 = o[3]; r.t4 = o[4]; r.t5 = o[5]; r.t6 = o[6]; r.t7 = o[7];
    return r;
}
#endif
