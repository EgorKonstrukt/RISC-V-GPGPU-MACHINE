// RV32IMAF for Linux without an MMU, in one compute shader thread: no fast path, no caches.
// The machine of docs/system/nommu.md: entered in supervisor mode, SBI answered here,
// the CSRs as fields (CSR_FIELDS), the console as plain memory (docs/machine/console.md).
// Not done: fflags, fused rounding of fmadd, denormals (GPUs flush them), misaligned jumps.

RWByteAddressBuffer Ram   : register(u0);   // guest RAM from 0x80000000
RWByteAddressBuffer State : register(u1);   // r, csr, pc priv fcsr res, tcmp lo hi, memop src dst n, instructions run
ByteAddressBuffer   Rom   : register(t0);   // the ROM from 0x40000000

cbuffer Pass : register(b0) {
    uint TimeLo, TimeHi, Rtc0, Rtc1, Steps, RamBytes, Seed;
};

static uint r[65];    // x0-x31, then f0-f31 as bits; [64] takes writes that must not land
static uint csr[16];  // slot (n >> 3 & 8) | (n & 7): status 0, ie 4, tvec 5, counteren 6,
                      // scratch 8, epc 9, cause 10, tval 11, ip 12; s* and m* share a slot
static uint pc, priv, fcsr, res, tcmp_lo, tcmp_hi, msrc, mdst, mn;
static bool trapped;
static uint trap_cause, trap_val;

void raise(uint cause, uint val) {
    if (!trapped) { trapped = true; trap_cause = cause; trap_val = val; }
}

uint read_word(uint a) {   // a is word-aligned
    if (a - 0x80000000u < RamBytes) return Ram.Load(a - 0x80000000u);
    if (a - 0x40000000u < 0x40000000u) return Rom.Load(a - 0x40000000u);
    return a == 0x030007f8u ? Rtc0 : a == 0x030007fcu ? Rtc1 : 0;
}

void write_word(uint a, uint v, uint m) {   // a is word-aligned; only RAM takes writes
    if (a - 0x80000000u >= RamBytes) return;
    a -= 0x80000000u;
    Ram.Store(a, m == ~0u ? v : (Ram.Load(a) & ~m) | (v & m));
}

// size in bytes: 1, 2 or 4. Misaligned accesses are put together from two words.
uint load(uint a, uint size) {
    uint sh = (a & 3) * 8, v = read_word(a & ~3u) >> sh;
    if ((a & 3) + size > 4) v |= read_word((a & ~3u) + 4) << (32 - sh);
    return size == 4 ? v : v & ((1u << (size * 8)) - 1);
}

void store(uint a, uint v, uint size) {
    uint m = size == 4 ? ~0u : (1u << (size * 8)) - 1, sh = (a & 3) * 8;
    write_word(a & ~3u, v << sh, m << sh);
    if ((a & 3) + size > 4) write_word((a & ~3u) + 4, v >> (32 - sh), m >> (32 - sh));
}

// The mask of a CSR kept in csr[]: 0 for one that is not. sstatus, sie and sip show part
// of mstatus, mie and mip; slots 5, 6 and 8 to 11 are whole.
uint csr_mask(uint n) {
    uint s = (n >> 3 & 8) | (n & 7);
    if ((n & ~0x47u) == 0x100) return s == 0 ? 0xde162 : (s & 7) == 4 ? 0x222 : (0xf60 >> s & 1) != 0 ? ~0u : 0;
    return n == 0x300 || n == 0x304 || n == 0x344 ? ~0u : 0;
}

uint csr_read(uint n) {
    uint s = (n >> 3 & 8) | (n & 7), v = csr[s] & csr_mask(n);
    if (s == 0 && (v & 0x6000) != 0) v |= 0x80006000;   // FS reads dirty whenever it is on
    if (n - 1 < 3) v = n == 1 ? fcsr & 0x1f : n == 2 ? fcsr >> 5 : fcsr;
    if (n == 0x301) v = 0x40141101;                     // misa, as the machine reports it
    if (n == 0x0bf) v = Seed ^ (pc * 2654435761u);
    if (n == 0xc01) v = TimeLo;
    if (n == 0xc81) v = TimeHi;
    return v;
}

// 0x0b1-0x0b3 and 1 (copy) or 2 (fill) to 0x0b0: the kernel's memcpy and memset hooks.
void csr_write(uint n, uint v) {
    uint s = (n >> 3 & 8) | (n & 7), m = csr_mask(n);
    csr[s] = (csr[s] & ~m) | (v & m);
    if (n - 1 < 3) fcsr = n == 1 ? (fcsr & 0xe0) | (v & 0x1f) : n == 2 ? (fcsr & 0x1f) | ((v & 7) << 5) : v & 0xff;
    if (n == 0x0b1) msrc = v;
    if (n == 0x0b2) mdst = v;
    if (n == 0x0b3) mn = v;
    if (n == 0x0b0)
        for (uint i = 0; i < mn; i += 4) write_word(mdst + i, v == 2 ? msrc : read_word(msrc + i), ~0u);
}

bool timer_due() {
    return (tcmp_lo | tcmp_hi) != 0 && (TimeHi > tcmp_hi || (TimeHi == tcmp_hi && TimeLo >= tcmp_lo));
}

// Every trap goes to the supervisor; an interrupt's cause has bit 31 set.
void take_trap(uint cause, uint val, uint epc) {
    csr[9] = epc;
    csr[10] = cause;
    csr[11] = val;
    pc = (csr[5] & 3) != 0 && (cause >> 31) != 0 ? (csr[5] & ~3u) + 4 * (cause & 31) : csr[5] & ~3u;
    csr[0] = (csr[0] & ~0x122u) | ((csr[0] & 2) << 4) | ((priv & 1) << 8);   // SPIE, SIE 0, SPP
    priv = 1;
}

bool interrupt() {
    uint p = csr[12] & csr[4] & 0x222;
    if (p == 0 || (priv == 1 && (csr[0] & 2) == 0)) return false;
    uint bit = (p & 0x200) ? 9 : (p & 2) ? 1 : 5;   // SEIP, SSIP, STIP
    if (bit != 5) csr[12] &= ~(1u << bit);          // STIP stays until the next set_timer
    take_trap(0x80000000u | bit, 0, pc);
    return true;
}

void sbi_call() {
    uint ext = r[17], a0 = r[10], fn = r[16];
    if (ext == 0x54494D45 || ext == 0) {   // set_timer
        tcmp_lo = a0;
        tcmp_hi = r[11];
        csr[12] = timer_due() ? csr[12] | 0x20 : csr[12] & ~0x20u;
    }
    uint val = ext != 0x10 ? 0 : fn == 0 ? 2 : fn == 1 ? 0x454d55 : fn == 2 ? 1 :
               (fn == 3 && (a0 == 0x54494D45 || a0 <= 2)) ? 1 : 0;
    if (ext < 0x10) { r[10] = val; return; }
    r[10] = ext == 0x10 || ext == 0x54494D45 ? 0 : 0xfffffffeu;
    r[11] = val;
}

uint mulhu(uint a, uint b) {
    uint ll = (a & 0xffff) * (b & 0xffff), lh = (a & 0xffff) * (b >> 16), hl = (a >> 16) * (b & 0xffff);
    uint mid = (ll >> 16) + (lh & 0xffff) + (hl & 0xffff);
    return (a >> 16) * (b >> 16) + (lh >> 16) + (hl >> 16) + (mid >> 16);
}

uint op_m(uint f3, uint a, uint b) {
    int sa = asint(a), sb = asint(b);
    if (f3 == 0) return a * b;
    if (f3 < 4) return mulhu(a, b) - (f3 != 3 && sa < 0 ? b : 0) - (f3 == 1 && sb < 0 ? a : 0);
    bool ovf = a == 0x80000000u && b == ~0u;
    if (f3 == 4) return b == 0 ? ~0u : ovf ? a : asuint(sa / sb);
    if (f3 == 5) return b == 0 ? ~0u : a / b;
    if (f3 == 6) return b == 0 ? a : ovf ? 0 : asuint(sa % sb);
    return b == 0 ? a : a % b;
}

bool is_nan(uint v) { return (v & 0x7fffffffu) > 0x7f800000u; }

uint fclass(uint v) {
    uint e = (v >> 23) & 0xff, m = v & 0x7fffff, k;
    if (e == 0xff) return m == 0 ? (v >> 31 ? 1u : 128u) : (m & 0x400000) ? 512u : 256u;
    k = e != 0 ? 1 : m != 0 ? 2 : 3;   // normal, subnormal, zero: bits 1-3 negative, 6-4 positive
    return v >> 31 ? 1u << k : 1u << (7 - k);
}

uint fcvt_w(uint bits, uint rm, bool u) {
    float v = asfloat(bits);
    v = rm == 1 ? trunc(v) : rm == 2 ? floor(v) : rm == 3 ? ceil(v) : rm == 4 ? sign(v) * floor(abs(v) + 0.5) : round(v);
    if (is_nan(bits)) return u ? ~0u : 0x7fffffffu;
    if (u) return v <= 0 ? 0 : v >= 4294967296.0 ? ~0u : (uint)v;
    return v >= 2147483648.0 ? 0x7fffffffu : v <= -2147483648.0 ? 0x80000000u : asuint((int)v);
}

// OP-FP; a is an integer register for fcvt.s.w(u) and fmv.w.x, b is always a float one.
uint op_fp(uint w, uint a, uint b) {
    uint f7 = w >> 25, f3 = (w >> 12) & 7, rs2 = (w >> 20) & 31;
    precise float fa = asfloat(a), fb = asfloat(b);
    bool nan = is_nan(a) || is_nan(b);
    if (f7 == 0x00) return asuint(fa + fb);
    if (f7 == 0x04) return asuint(fa - fb);
    if (f7 == 0x08) return asuint(fa * fb);
    if (f7 == 0x0c) return asuint(fa / fb);
    if (f7 == 0x2c) return asuint(sqrt(fa));
    if (f7 == 0x10) return (a & 0x7fffffffu) | ((f3 == 0 ? b : f3 == 1 ? ~b : a ^ b) & 0x80000000u);
    if (f7 == 0x14) {   // fmin, fmax: a NaN gives the other, -0 is below +0
        bool max_ = f3 == 1;
        return is_nan(a) && is_nan(b) ? 0x7fc00000u : is_nan(a) ? b : is_nan(b) ? a :
               fa == fb ? (max_ ? a & b : a | b) : (max_ ? fa > fb : fa < fb) ? a : b;
    }
    if (f7 == 0x60) return fcvt_w(a, f3 == 7 ? fcsr >> 5 : f3, rs2 == 1);
    if (f7 == 0x68) return asuint(rs2 == 1 ? (float)a : (float)asint(a));
    if (f7 == 0x70) return f3 == 0 ? a : fclass(a);
    if (f7 == 0x78) return a;
    if (f7 == 0x50) return !nan && (f3 == 2 ? fa == fb : f3 == 1 ? fa < fb : fa <= fb) ? 1u : 0u;
    raise(2, w);
    return 0;
}

uint imm_i(uint w) { return asuint(asint(w) >> 20); }
uint imm_s(uint w) { return asuint(asint(w & 0xfe000000u) >> 20) | ((w >> 7) & 31); }

// Runs the instruction at pc. Returns 0 to go on, 1 when interrupts must be looked at again
// (a CSR, a trap, sret, SBI), 2 when the pass should end (wfi).
uint step() {
    trapped = false;
    uint w = read_word(pc);
    uint opc = w & 0x7f, rd = (w >> 7) & 31, f3 = (w >> 12) & 7, f7 = w >> 25;
    bool fp = (opc & 0x60) == 0x40;   // fmadd family and op-fp
    uint a = r[((w >> 15) & 31) + (fp && (opc != 0x53 || (f7 & 0x68) != 0x68) ? 32 : 0)];
    uint b = r[((w >> 20) & 31) + (fp || opc == 0x27 ? 32 : 0)];
    uint i = imm_i(w), next = pc + 4, v = 0;
    uint dst = rd;   // 64: nothing is written; 32 + n: f register n
    uint ret = 0;

    if (opc == 0x13 || opc == 0x33) {   // op-imm, op
        bool reg = opc == 0x33;
        uint y = reg ? b : i, s = y & 31;
        bool alt = (f7 & 0x20) != 0;
        v = f3 == 0 ? (reg && alt ? a - y : a + y) : f3 == 1 ? a << s :
            f3 == 2 ? (asint(a) < asint(y) ? 1u : 0u) : f3 == 3 ? (a < y ? 1u : 0u) :
            f3 == 4 ? a ^ y : f3 == 5 ? (alt ? asuint(asint(a) >> s) : a >> s) : f3 == 6 ? a | y : a & y;
        if (reg && f7 == 1) v = op_m(f3, a, b);
    } else if (opc == 0x03 || opc == 0x07) {   // loads, flw
        uint s = 32 - (8u << (f3 & 3));
        v = load(a + i, 1u << (f3 & 3));
        v = (f3 & 4) != 0 ? v : asuint(asint(v << s) >> s);
        dst += opc == 0x07 ? 32 : 0;
    } else if (opc == 0x23 || opc == 0x27) {   // stores, fsw
        store(a + imm_s(w), b, 1u << (f3 & 3));
        dst = 64;
    } else if (opc == 0x63) {   // branches
        bool t = (f3 & 4) == 0 ? a == b : (f3 & 2) != 0 ? a < b : asint(a) < asint(b);
        if (t != ((f3 & 1) != 0))
            next = pc + (asuint(asint(w & 0x80000000u) >> 19) | ((w & 0x80) << 4) | ((w >> 20) & 0x7e0) | ((w >> 7) & 0x1e));
        dst = 64;
    } else if (opc == 0x6f) {   // jal
        v = next;
        next = pc + (asuint(asint(w & 0x80000000u) >> 11) | (w & 0xff000) | ((w >> 9) & 0x800) | ((w >> 20) & 0x7fe));
    } else if (opc == 0x67) {   // jalr
        v = next;
        next = (a + i) & ~1u;
    } else if (opc == 0x37 || opc == 0x17) {   // lui, auipc
        v = (w & 0xfffff000u) + (opc == 0x17 ? pc : 0);
    } else if (opc == 0x2f) {   // lr, sc, amo*
        uint f5 = f7 >> 2, old = load(a, 4);
        v = f5 == 2 ? old : f5 == 3 ? (res == a ? 0u : 1u) : old;
        uint y = f5 == 1 ? b : f5 == 0 ? old + b : f5 == 4 ? old ^ b : f5 == 0xc ? old & b : f5 == 8 ? old | b :
                 f5 == 0x10 ? (asint(old) < asint(b) ? old : b) : f5 == 0x14 ? (asint(old) > asint(b) ? old : b) :
                 f5 == 0x18 ? min(old, b) : max(old, b);
        if (f5 != 2 && (f5 != 3 || res == a)) store(a, f5 == 3 ? b : y, 4);
        res = f5 == 2 ? a : f5 == 3 ? ~0u : res;
    } else if (fp) {   // fmadd family, op-fp
        if (opc == 0x53) {
            v = op_fp(w, a, b);
            dst += (f7 & 0x48) == 0x40 ? 0 : 32;   // feq/flt/fle, fcvt.w(u), fmv.x.w and fclass go to x
        } else {
            precise float p = asfloat(a) * asfloat(b), c = asfloat(r[32 + (w >> 27)]);
            v = asuint(opc == 0x43 ? p + c : opc == 0x47 ? p - c : opc == 0x4b ? c - p : -p - c);
            dst += 32;
        }
    } else if (opc == 0x73 && f3 != 0) {   // csrrw, csrrs, csrrc and their immediates
        uint n = w >> 20, src = (f3 & 4) != 0 ? (w >> 15) & 31 : a;
        v = csr_read(n);
        if ((f3 & 3) == 1) csr_write(n, src);
        else if (((w >> 15) & 31) != 0) csr_write(n, (f3 & 3) == 2 ? v | src : v & ~src);
        ret = 1;
    } else if (opc == 0x73) {
        dst = 64;
        ret = 1;
        if (w == 0x00000073) {   // ecall: the supervisor's is a call to SBI
            if (priv == 1) sbi_call(); else raise(8, 0);
        } else if (w == 0x00100073) {   // ebreak
            raise(3, pc);
        } else if (w == 0x10200073) {   // sret: SIE = SPIE, SPIE = 1, SPP = 0
            next = csr[9];
            priv = (csr[0] >> 8) & 1;
            csr[0] = (csr[0] & ~0x122u) | ((csr[0] >> 4) & 2) | 0x20;
        } else if (w == 0x10500073) {   // wfi
            ret = 2;
        } else if (f7 != 0x09) {        // (sfence.vma: nothing)
            raise(2, w);
        }
    } else if (opc == 0x0f) {   // fence, fence.i
        dst = 64;
    } else {
        raise(2, w);
    }

    if (trapped) {
        take_trap(trap_cause, trap_val, pc);
        return 1;
    }
    r[dst == 0 ? 64 : dst] = v;
    pc = next;
    return ret;
}

// The host starts a hart with every word 0 but pc = 0x80400000, priv = 1 (supervisor),
// x11 = 0x82200000 (the device tree) and res = ~0, and dispatches (1, 1, 1) a pass.
[numthreads(1, 1, 1)]
void tick(uint3 id : SV_DispatchThreadID) {
    uint k;
    for (k = 0; k < 64; k++) r[k] = State.Load(4 * k);
    for (k = 0; k < 16; k++) csr[k] = State.Load(256 + 4 * k);
    uint4 p = State.Load4(320), q = State.Load4(336);
    pc = p.x; priv = p.y; fcsr = p.z; res = p.w;
    tcmp_lo = q.x; tcmp_hi = q.y; msrc = q.z; mdst = q.w; mn = State.Load(352);

    if (timer_due()) csr[12] |= 0x20;
    bool look = true;
    uint n = 0;
    for (; n < Steps; n++) {
        if (look && interrupt()) continue;
        uint s = step();
        if (s == 2) { n++; break; }
        look = s == 1;
    }

    for (k = 0; k < 64; k++) State.Store(4 * k, r[k]);
    for (k = 0; k < 16; k++) State.Store(256 + 4 * k, csr[k]);
    State.Store4(320, uint4(pc, priv, fcsr, res));
    State.Store4(336, uint4(tcmp_lo, tcmp_hi, msrc, mdst));
    State.Store(352, mn);
    State.Store(356, n);   // fewer than Steps: the pass ended on wfi
}
