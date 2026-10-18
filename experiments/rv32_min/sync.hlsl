// rvc_harness --compute: the compute processor's RAM (a buffer) kept in step with the state
// texture, where the shader machine's devices read and write it (README.md, "The whole machine").
// After the processor's pass: what it stored goes into the texture, in 4 MB bands, with the state
// texels the commit and the host read. After the devices' passes: what they wrote comes back.

RWByteAddressBuffer Ram   : register(u0);   // guest RAM from 0x80000000: the processor's three images (rv32_min_fast.hlsl)
RWByteAddressBuffer State : register(u1);   // the cores' words (rv32_min*.hlsl, 512 bytes a core), core 0's then these:
RWTexture2D<uint4>  Out   : register(u2);   // the state texture the commit reads (export)
RWByteAddressBuffer Dec   : register(u3);   // the processor's decoded instructions: what is imported is cleared
RWByteAddressBuffer Marks : register(u4);   // its page marks, then a word a 4 MB band stored to (merge)
RWByteAddressBuffer Args  : register(u5);   // the processor's dispatches: Args[3] the pages of this frame (export_pages)
Texture2D<uint4>    In    : register(t0);   // the state texture as the devices left it (import)

cbuffer Sync : register(b0) {
    uint Mask;    // sync_count: the bands that may go into the texture now
    uint Mode;    // import_rows: 0 the texels From to From + Count, 1 the picture the commit copied back
    uint From, Count;   // sync_count and sync_cores: Count is how many cores there are
    uint Export;  // export_rows: the bands below it (the fast shader lists the GPU's pages itself: 24)
};

#define S_PC     320   // the processor's pc
#define S_RAN    356   // instructions its last pass ran
#define S_DIRTY  360   // bands it stored to, not yet in the texture
#define S_NOW    364   // bands put into the texture this frame
#define S_LAST   368   // and in the frame before
#define S_INSTR  372   // instructions so far (low, high), passes so far (a core's own, the processor keeps them)
#define S_MODE   392   // a worker's: parked 0, running 1, asleep 2, stopped 3 or 4
#define BANDS    (65536u * 4)   // the band words in Marks the harness takes (rv32_min_fast.hlsl, M_HARNESS)
#define M_EXP    (32768u * 4)   // and a word a page listed in this frame, the list (M_EXP, M_FRAME there)
#define M_FRAME  ((65536u + 64 + 32768 + 1024) * 4)
#define S_COPY   384   // the picture copied back: its first byte in RAM, its bytes
#define RAM_ROWS 4032u // the texture's rows of RAM: 126 MB from row 64
#define RAM_BYTES (RAM_ROWS * 32768u)
#define CTRL     0x07000000u   // the control words (docs/machine/gpu.md), in RAM
#define STATS_ALL 0x076ba000u  // every core's count, then a texel of bits for every 32 cores (src/gpu.h)

// Once a frame: which bands go into the texture, as the merges marked them.
[numthreads(1, 1, 1)]
void sync_count() {
    uint dirty = State.Load(S_DIRTY);
    for (uint b = 0; b < 32; b++) {
        dirty |= Marks.Load(BANDS + 4 * b) != 0 ? 1u << b : 0u;
        Marks.Store(BANDS + 4 * b, 0);
    }
    State.Store(S_DIRTY, dirty & ~Mask);
    State.Store(S_LAST, State.Load(S_NOW));
    State.Store(S_NOW, dirty & Mask);
}

// A word of RAM as every image has it.
void put(uint o, uint v) {
    Ram.Store(o, v);
    Ram.Store(RAM_BYTES + o, v);
    Ram.Store(2 * RAM_BYTES + o, v);
}

// After the control words came back: what the cores are, as the shader machine's control pass
// says it (src/gpu.h, MC_STATS) and rvc_cpu's control_pass: how many cores the geometry has
// (a worker is there when the geometry is not being laid out anew and it is within its count),
// which run and which sleep, every core's instructions, and how many there could be. A worker the
// geometry does not have is parked. A thread a core. And the frame's list of pages starts anew.
groupshared uint flags[256];
[numthreads(256, 1, 1)]
void sync_cores(uint c : SV_GroupIndex) {
    uint shaping = Ram.Load(CTRL + 0x3d0) & 1, there = Ram.Load(CTRL + 0x3d4), bits = Ram.Load(CTRL + 0x3d8);
    bool some = shaping == 0 && bits - 3 <= 3 && Count > 1;
    uint mode = c == 0 ? 1 : State.Load(512 * c + S_MODE);
    bool here = c == 0 || (some && c < Count && c <= there);
    if (!here && c < Count) { State.Store(512 * c + S_MODE, 0); mode = 0; }
    flags[c] = (here && mode != 0 ? 1u : 0u) | (here && mode == 2 ? 2u : 0u);
    uint n = c < Count ? State.Load(512 * c + S_INSTR) : 0;
    put(STATS_ALL + 4 * c, n);
    if (c < 16) put(CTRL + 0x380 + 4 * c, n);
    GroupMemoryBarrierWithGroupSync();
    if (c < 8) {
        uint running = 0, asleep = 0;
        for (uint k = 0; k < 32; k++) {
            running |= (flags[32 * c + k] & 1) << k;
            asleep |= (flags[32 * c + k] >> 1) << k;
        }
        put(STATS_ALL + 0x400 + 16 * c, running);
        put(STATS_ALL + 0x404 + 16 * c, asleep);
        if (c == 0) {
            put(CTRL + 0x3c4, running);
            put(CTRL + 0x3c8, asleep);
            put(CTRL + 0x3cc, Count);
            put(CTRL + 0x3c0, some ? min(Count - 1, there) + 1 : 1);
            Marks.Store(BANDS + 4 * (CTRL >> 22), 1);
            Marks.Store(BANDS + 4 * (STATS_ALL >> 22), 1);
            // every core's count goes into the texture with the next frame's pages, where the host
            // reads those of cores 16 on (the fast shader exports the GPU's memory only as pages)
            Marks.Store(M_FRAME, STATS_ALL >> 12);
            Marks.Store(M_EXP + 4 * (STATS_ALL >> 12), 1);
            Args.Store(12, 1);
        }
    }
}

// The core's state rows as the tick would leave them for the commit and the host, and nothing
// else in them: no write cache (the commit applies none), no stall, no copy left to it.
// (28,0): stall, instructions, commits; (36,0).a pc; (11,0).a the UART's last byte (none);
// (41,0): the bands this frame's commit carries over, and the last frame's with the GPU's.
[numthreads(64, 1, 1)]
void sync_state(uint3 g : SV_GroupID, uint x : SV_GroupIndex) {
    uint y = g.y;
    uint4 v = 0;
    if (y == 0 && x == 11) v.a = 0xffffffffu;
    if (y == 0 && x == 28) v = uint4(0, State.Load(S_INSTR), State.Load(S_INSTR + 8), 0);
    if (y == 0 && x == 36) v.a = State.Load(S_PC);
    if (y == 0 && x == 41) v = uint4(0, State.Load(S_NOW), State.Load(S_LAST) | 0xff000000u, 0);
    Out[uint2(x, y)] = v;
}

// RAM's rows of the bands in S_NOW: a group a quarter row (dispatch 8 x 4032).
[numthreads(256, 1, 1)]
void export_rows(uint3 g : SV_GroupID, uint t : SV_GroupIndex) {
    uint row = g.y, x = g.x * 256 + t;
    if (((State.Load(S_NOW) >> (row >> 7)) & 1) == 0 || (row >> 7) >= Export) return;
    Out[uint2(x, 64 + row)] = Ram.Load4((row * 2048 + x) * 16);
}

// The pages the processor stored to in this frame (merge_list's frame list): a group a page
// (DispatchIndirect from Args[3]), its 256 texels into the texture.
[numthreads(256, 1, 1)]
void export_pages(uint3 g : SV_GroupID, uint t : SV_GroupIndex) {
    uint page = Marks.Load(M_FRAME + 4 * g.x), texel = page * 256 + t;
    if (t == 0) Marks.Store(M_EXP + 4 * page, 0);
    if (texel < RAM_ROWS * 2048) Out[uint2(texel % 2048, 64 + texel / 2048)] = Ram.Load4(texel * 16);
}

// The copy the commit made of the list drawn last, as rvc_harness --cpu reads it back: the
// display's framebuffer, or a window's rectangle (GPU_COPY, GPU_DISPLAY in src/gpu.h).
[numthreads(1, 1, 1)]
void import_plan() {
    uint4 copy = Ram.Load4(CTRL + 0x70);
    uint at = 0x87001000u, bytes = Ram.Load(CTRL + 4) * Ram.Load(CTRL + 8) * 4;
    if ((copy.x & 4) != 0) {
        uint width = copy.z & 0xffff, height = copy.z >> 16;
        at = (copy.y & 0x7ffffffcu) | 0x80000000u;
        bytes = max(copy.w, width) * height * 4;
    }
    uint end = 0x80000000u + RAM_ROWS * 32768;
    if (copy.x == 0 || at < 0x86000000u || at >= end || bytes == 0 || bytes > end - at) at = 0x80000000u, bytes = 0;
    State.Store2(S_COPY, uint2(at - 0x80000000u, bytes));
}

// Texels of the texture into RAM: those From to From + Count, or (Mode 1) the words of the
// picture import_plan found, a group 256 texels.
[numthreads(256, 1, 1)]
void import_rows(uint3 g : SV_GroupID, uint t : SV_GroupIndex) {
    uint n = g.x * 256 + t;
    if (Mode == 0) {
        if (n >= Count) return;
        uint texel = From + n;
        uint4 v = In.Load(int3(texel % 2048, 64 + texel / 2048, 0));
        Ram.Store4(texel * 16, v);   // (into every image: core 0's, the workers', the last pass's)
        Ram.Store4(RAM_BYTES + texel * 16, v);
        Ram.Store4(2 * RAM_BYTES + texel * 16, v);
        Dec.Store4(texel * 32, uint4(0, 0, 0, 0));
        Dec.Store4(texel * 32 + 16, uint4(0, 0, 0, 0));
        if (texel != 0) Dec.Store2(texel * 32 - 8, uint2(0, 0));   // (a pair that ends in it)
        return;
    }
    uint2 copy = State.Load2(S_COPY);
    uint texel = copy.x / 16 + n;
    if (copy.y == 0 || texel * 16 >= copy.x + copy.y) return;
    uint4 v = In.Load(int3(texel % 2048, 64 + texel / 2048, 0));
    [unroll] for (uint k = 0; k < 4; k++) {
        uint a = texel * 16 + 4 * k;
        if (a >= copy.x && a < copy.x + copy.y) {
            Ram.Store(a, v[k]);
            Ram.Store(RAM_BYTES + a, v[k]);
            Ram.Store(2 * RAM_BYTES + a, v[k]);
            Dec.Store(a * 2, 0);
            Dec.Store(a * 2 - 8, 0);
        }
    }
}
