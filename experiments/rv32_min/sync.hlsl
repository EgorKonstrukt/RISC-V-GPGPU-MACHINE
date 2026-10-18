// rvc_harness --compute: the compute processor's RAM (a buffer) kept in step with the state
// texture, where the shader machine's devices read and write it (README.md, "The whole machine").
// After the processor's pass: what it stored goes into the texture, in 4 MB bands, with the state
// texels the commit and the host read. After the devices' passes: what they wrote comes back.

RWByteAddressBuffer Ram   : register(u0);   // guest RAM from 0x80000000, as the processor has it
RWByteAddressBuffer State : register(u1);   // the processor's words (rv32_min*.hlsl), then these:
RWTexture2D<uint4>  Out   : register(u2);   // the state texture the commit reads (export)
Texture2D<uint4>    In    : register(t0);   // the state texture as the devices left it (import)

cbuffer Sync : register(b0) {
    uint Mask;    // sync_count: the bands that may go into the texture now
    uint Mode;    // import_rows: 0 the texels From to From + Count, 1 the picture the commit copied back
    uint From, Count;
};

#define S_PC     320   // the processor's pc
#define S_RAN    356   // instructions its last pass ran
#define S_DIRTY  360   // bands it stored to, not yet in the texture
#define S_NOW    364   // bands put into the texture this frame
#define S_LAST   368   // and in the frame before
#define S_INSTR  372   // instructions so far (low, high), passes so far
#define S_COPY   384   // the picture copied back: its first byte in RAM, its bytes
#define RAM_ROWS 4032u // the texture's rows of RAM: 126 MB from row 64
#define CTRL     0x07000000u   // the control words (docs/machine/gpu.md), in RAM

// Once a pass: which bands go into the texture, and the counts.
[numthreads(1, 1, 1)]
void sync_count() {
    uint dirty = State.Load(S_DIRTY), ran = State.Load(S_RAN);
    State.Store(S_DIRTY, dirty & ~Mask);
    State.Store(S_LAST, State.Load(S_NOW));
    State.Store(S_NOW, dirty & Mask);
    uint lo = State.Load(S_INSTR) + ran;
    State.Store(S_INSTR + 4, State.Load(S_INSTR + 4) + (lo < ran ? 1u : 0u));
    State.Store(S_INSTR, lo);
    State.Store(S_INSTR + 8, State.Load(S_INSTR + 8) + 1);
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
    if (((State.Load(S_NOW) >> (row >> 7)) & 1) == 0) return;
    Out[uint2(x, 64 + row)] = Ram.Load4((row * 2048 + x) * 16);
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
        Ram.Store4(texel * 16, In.Load(int3(texel % 2048, 64 + texel / 2048, 0)));
        return;
    }
    uint2 copy = State.Load2(S_COPY);
    uint texel = copy.x / 16 + n;
    if (copy.y == 0 || texel * 16 >= copy.x + copy.y) return;
    uint4 v = In.Load(int3(texel % 2048, 64 + texel / 2048, 0));
    [unroll] for (uint k = 0; k < 4; k++) {
        uint a = texel * 16 + 4 * k;
        if (a >= copy.x && a < copy.x + copy.y) Ram.Store(a, v[k]);
    }
}
