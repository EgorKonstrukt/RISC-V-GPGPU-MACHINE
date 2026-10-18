// RV32IMAF for Linux without an MMU, a core a thread group of one thread (README.md): core 0
// the kernel's, entered in supervisor mode, SBI answered here; the others workers in user mode
// on the mailboxes of docs/machine/multicore.md. The CSRs as fields, the console as plain memory
// (docs/machine/console.md). The fast path runs from decoded instructions (Dec, below).
// RAM is three images (Ram): core 0's, the workers', and both as the last pass left them. A
// core reads and writes its own; `merge`, after every pass, takes into all three what each
// side stored, word by word: so a core sees another's stores from the next pass on, all of
// them at once, as on the shader machine (src/mc.h), which a GPU's memory gives no other way.
// Not done: fflags, fused rounding of fmadd, denormals (GPUs flush them), misaligned jumps.

RWByteAddressBuffer Ram   : register(u0);   // guest RAM from 0x80000000: three images of RamBytes
RWByteAddressBuffer State : register(u1);   // 512 bytes a core (words below)
RWByteAddressBuffer Dec   : register(u2);   // decoded instructions (decode, below)
RWByteAddressBuffer Marks : register(u3);   // the pages stored to in this pass, and lists of them (M_ below)
RWByteAddressBuffer Args  : register(u4);   // merge_list's dispatch: how many pages to merge
ByteAddressBuffer   Rom   : register(t0);   // the ROM from 0x40000000

cbuffer Pass : register(b0) {
    uint TimeLo, TimeHi, Rtc0, Rtc1, Steps, RamBytes, Seed;
    uint AllRun;   // every core runs on its own from its words, in supervisor mode (a measurement)
};

// A core's words in State, from hart * 512 (W_ in bytes): r 0-63, csr 64-79, pc priv fcsr res,
// the timer's compare, the copy's source, destination and length, the instructions of its last
// pass (89); core 0's 90-92 are the harness's; 93-95 the instructions so far (low, high) and
// passes; a worker's 98, 99 and 101 its mailbox's.
#define W_RAN    356
#define W_INSTR  372
#define W_MODE   392   // a worker: parked 0, running 1, asleep 2, stopped at a fault 3 or a call 4
#define W_SEEN   396   // the resume word it stopped with
#define W_JOB    404   // the job word it sleeps on
#define M_PAGE    0                            // Marks (bytes): a word a 4 KB page stored to in this pass,
#define M_EXP     (32768 * 4)                  // a word a page in this frame's list, the 4 MB bands for
#define M_HARNESS (65536 * 4)                  // the harness, the pages merge takes, a word for 32 pages
#define M_LIST    ((65536 + 64) * 4)           // of M_PAGE that says one is marked, and this frame's
#define M_GROUP   ((65536 + 64 + 32768) * 4)   // list of the GPU memory's pages stored to
#define M_FRAME   ((65536 + 64 + 32768 + 1024) * 4)
#define MBOX     0x06C00000u   // the workers' mailbox page, in RAM (linux/kernel/shaderemu_gpu.c)
#define MC_START 0x5553434Du
#define MC_STOP  0x5453434Du

#define DEC_AT(a) (((a) - 0x80000000u) << 1)   // a RAM address's entry (8 bytes a word)
#define DEC_RO(o) ((o) << 1)                       // a RAM offset's

groupshared uint r[65];    // x0-x31, then f0-f31 as bits; [64] takes writes that must not land
static uint csr[16];  // slot (n >> 3 & 8) | (n & 7): status 0, ie 4, tvec 5, counteren 6,
                      // scratch 8, epc 9, cause 10, tval 11, ip 12; s* and m* share a slot
static uint pc, priv, fcsr, res, tcmp_lo, tcmp_hi, msrc, mdst, mn;
static bool trapped;
static uint hart, base;   // this core's number and its words in State
static uint why;          // a pass ended on: 1 pause, 2 ebreak, 3 a fault or call (a worker's)
static uint view;         // this core's image of RAM (a byte offset into Ram)
static uint marked;       // the page this core marked last (each store marks its page once in a row)
static uint ran;      // instructions of this pass before the one in step()
static uint trap_cause, trap_val;

void raise(uint cause, uint val) {
    if (!trapped) { trapped = true; trap_cause = cause; trap_val = val; }
}

uint read_word(uint a) {   // a is word-aligned
    uint o = a - 0x80000000u;
    if (o < RamBytes) return Ram.Load(view + o);
    if (a - 0x40000000u < 0x40000000u) return Rom.Load(a - 0x40000000u);
    return a == 0x030007f8u ? Rtc0 : a == 0x030007fcu ? Rtc1 : 0;
}

void mark(uint page) {   // (two cores store the same 1: no atomics wanted)
    marked = page;
    Marks.Store(M_PAGE + page * 4, 1);
    Marks.Store(M_GROUP + (page >> 5) * 4, 1);
}

void write_word(uint a, uint v, uint m) {   // a is word-aligned; only RAM takes writes
    if (a - 0x80000000u >= RamBytes) return;
    a -= 0x80000000u;
    if ((a >> 12) != marked) mark(a >> 12);
    Dec.Store(DEC_RO(a), 0);
    Ram.Store(view + a, m == ~0u ? v : (Ram.Load(view + a) & ~m) | (v & m));
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
    // cycle and instret: this core's instructions so far
    uint lo = State.Load(base + W_INSTR) + ran;
    if (n == 0xc00 || n == 0xc02) v = lo;
    if (n == 0xc80 || n == 0xc82) v = State.Load(base + W_INSTR + 4) + (lo < ran ? 1u : 0u);
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

// fcvt.w.s and fcvt.wu.s: rounded as the instruction says, then clamped. Both are made and one
// chosen, a uint from 2^31 up in two parts, as the tick has it (fxc made a conversion here a
// signed one, and fcvt.w.s of a negative number gave 0).
uint fcvt_w(uint bits, uint rm, bool u) {
    float v = asfloat(bits);
    v = rm == 1 ? trunc(v) : rm == 2 ? floor(v) : rm == 3 ? ceil(v) : rm == 4 ? sign(v) * floor(abs(v) + 0.5) : round(v);
    bool nan = is_nan(bits);
    uint to_s = (nan || v >= 2147483648.0) ? 0x7fffffffu : v <= -2147483648.0 ? 0x80000000u : asuint((int)v);
    uint to_u = (nan || v >= 4294967296.0) ? ~0u : v <= 0.0 ? 0u : v >= 2147483648.0 ? 0x80000000u + (uint)(v - 2147483648.0) : (uint)v;
    return u ? to_u : to_s;
}

// The card divides by multiplying with 1 / b, which is nothing when b is large: two ordinary
// numbers are divided with their exponents moved towards the middle (the tick's way).
float fdiv(uint a, uint b) {
    uint ea = (a >> 23) & 0xff, eb = (b >> 23) & 0xff;
    int de = (int)ea - (int)eb, ha = clamp(de, -126, 127);
    float da = asfloat((a & 0x807fffffu) | ((uint)(127 + ha) << 23)), db = asfloat((b & 0x807fffffu) | ((uint)(127 - (de - ha)) << 23));
    return ea - 1 < 254 && eb - 1 < 254 ? da / db : asfloat(a) / asfloat(b);
}

// OP-FP; a is an integer register for fcvt.s.w(u) and fmv.w.x, b is always a float one.
uint op_fp(uint w, uint a, uint b) {
    uint f7 = w >> 25, f3 = (w >> 12) & 7, rs2 = (w >> 20) & 31;
    float fa = asfloat(a), fb = asfloat(b);
    bool nan = is_nan(a) || is_nan(b);
    if (f7 == 0x00) return asuint(fa + fb);
    if (f7 == 0x04) return asuint(fa - fb);
    if (f7 == 0x08) return asuint(fa * fb);
    if (f7 == 0x0c) return asuint(fdiv(a, b));
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
uint imm_b(uint w) { return asuint(asint(w & 0x80000000u) >> 19) | ((w & 0x80) << 4) | ((w >> 20) & 0x7e0) | ((w >> 7) & 0x1e); }
uint imm_j(uint w) { return asuint(asint(w & 0x80000000u) >> 11) | (w & 0xff000) | ((w >> 9) & 0x800) | ((w >> 20) & 0x7fe); }

// op-imm and op (opc 0x13, 0x33) with the M extension.
uint alu(uint opc, uint f3, uint f7, uint a, uint b, uint i) {
    bool reg = opc == 0x33, alt = (f7 & 0x20) != 0;
    uint y = reg ? b : i, s = y & 31;
    if (reg && f7 == 1) return op_m(f3, a, b);
    return f3 == 0 ? (reg && alt ? a - y : a + y) : f3 == 1 ? a << s :
           f3 == 2 ? (asint(a) < asint(y) ? 1u : 0u) : f3 == 3 ? (a < y ? 1u : 0u) :
           f3 == 4 ? a ^ y : f3 == 5 ? (alt ? asuint(asint(a) >> s) : a >> s) : f3 == 6 ? a | y : a & y;
}

bool taken(uint f3, uint a, uint b) {
    return ((f3 & 4) == 0 ? a == b : (f3 & 2) != 0 ? a < b : asint(a) < asint(b)) != ((f3 & 1) != 0);
}

// fmadd, fmsub, fnmsub, fnmadd (opc 0x43, 0x47, 0x4b, 0x4f): rounded twice.
uint fmadd(uint opc, uint a, uint b, uint c_bits) {
    float p = asfloat(a) * asfloat(b), c = asfloat(c_bits);
    return asuint(opc == 0x43 ? p + c : opc == 0x47 ? p - c : opc == 0x4b ? c - p : -p - c);
}

// A worker's trap: a record in its mailbox for core 0 (rvc_cpu.cpp, mc_fault), and it waits for
// the resume word; ebreak parks it.
void worker_stop(uint cause, uint val) {
    uint job = MBOX + 0x4000 + 64 * hart, seen = read_word(0x80000000u + job + 48);
    why = cause == 3 && read_word(pc) == 0x00100073u ? 2 : 3;
    if (why == 2) return;
    if (cause == 8) {
        uint call = MBOX + 0x2000 + 32 * hart;
        write_word(0x80000000u + call, r[17], ~0u);
        for (uint k = 0; k < 6; k++) write_word(0x80000000u + call + 4 + 4 * k, r[10 + k], ~0u);
    }
    write_word(0x80000000u + job + 36, cause, ~0u);
    write_word(0x80000000u + job + 40, val, ~0u);
    write_word(0x80000000u + job + 44, pc, ~0u);
    write_word(0x80000000u + job + 32, seen + 1, ~0u);
    State.Store(base + W_MODE, cause == 8 ? 4u : 3u);
    State.Store(base + W_SEEN, seen);
}

// Runs the instruction at pc. Returns 0 to go on, 1 when interrupts must be looked at again
// (a CSR, a trap, sret, SBI), 2 when the pass should end (wfi, pause).
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
        v = alu(opc, f3, f7, a, b, i);
    } else if (opc == 0x03 || opc == 0x07) {   // loads, flw
        uint s = 32 - (8u << (f3 & 3));
        v = load(a + i, 1u << (f3 & 3));
        v = (f3 & 4) != 0 ? v : asuint(asint(v << s) >> s);
        dst += opc == 0x07 ? 32 : 0;
    } else if (opc == 0x23 || opc == 0x27) {   // stores, fsw
        store(a + imm_s(w), b, 1u << (f3 & 3));
        dst = 64;
    } else if (opc == 0x63) {   // branches
        if (taken(f3, a, b)) next = pc + imm_b(w);
        dst = 64;
    } else if (opc == 0x6f) {   // jal
        v = next;
        next = pc + imm_j(w);
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
            v = fmadd(opc, a, b, r[32 + (w >> 27)]);
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
    } else if (opc == 0x0f) {   // fence; pause and fence.i end the pass, as on the shader machine
        dst = 64;
        ret = w == 0x0100000fu || f3 == 1 ? 2 : 0;
        why = w == 0x0100000fu ? 1 : 0;
    } else {
        raise(2, w);
    }

    if (trapped) {
        if (hart != 0 && AllRun == 0) { worker_stop(trap_cause, trap_val); return 2; }
        take_trap(trap_cause, trap_val, pc);
        return 1;
    }
    r[dst == 0 ? 64 : dst] = v;
    pc = next;
    return ret;
}

// Decoded instructions (Dec): 8 bytes for each word of RAM, `kind | rd << 6 | rs1 << 13 |
// rs2 << 20 | rs3 << 27` (register numbers as r[] has them: 32 + n a float one, 64 none; an
// immediate operand has rs2 0, which is x0) and an immediate (a branch's or jump's target,
// lui's and auipc's value, a float instruction's word). 0: not made yet. A store to a word
// clears its entry, so an entry is the word that is in RAM now. The fast path tests the kinds
// the most common first (counted on a shell loop, sha256sum and gzip): add with the other
// arithmetic, one test for both (10 to 25% faster than add alone first), then lw, the branches,
// sw, the narrow loads and stores, jal, lui and auipc, jalr.
#define K_ADD    1   // add, addi, mv, li of 12 bits
#define K_LW     2   // lw, flw
#define K_SW     3   // sw, fsw
#define K_BR     4   // + 0..5: beq bne blt bge bltu bgeu
#define K_JAL   10
#define K_ALU   11   // + 0..8: sll slt sltu xor srl or and sub sra, of rs1 and rs2 + imm
#define K_LDS   20   // + 0..3: lb lh lbu lhu
#define K_STS   24   // + 0..1: sb sh
#define K_LI    26   // lui, auipc
#define K_JALR  27
#define K_M     28   // + f3: mul mulh mulhsu mulhu div divu rem remu
#define K_FP    36   // op-fp
#define K_FMA   37   // fmadd and its kin
#define K_SLOW  63   // the general path's

uint2 decode(uint o) {
    uint at = o + 0x80000000u, w = Ram.Load(view + o), opc = w & 0x7f, f3 = (w >> 12) & 7, f7 = w >> 25;
    uint rd = (w >> 7) & 31, rs1 = (w >> 15) & 31, rs2 = (w >> 20) & 31, k = K_SLOW, imm = imm_i(w);
    // sll slt sltu xor srl or and by f3 (add is K_ADD); sub and sra by hand
    uint opk = f3 == 0 ? K_ADD : K_ALU + f3 - 1;
    bool fd = false;
    if (opc == 0x13) {
        k = f3 == 5 && f7 == 0x20 ? K_ALU + 8 : opk;
        if ((f3 & 3) == 1) {
            imm &= 31;
            if (f7 != 0 && !(f3 == 5 && f7 == 0x20)) k = K_SLOW;
        }
        rs2 = 0;
    } else if (opc == 0x33) {
        k = f7 == 0 ? opk : f7 == 1 ? K_M + f3 : f7 == 0x20 && f3 == 0 ? K_ALU + 7 : f7 == 0x20 && f3 == 5 ? K_ALU + 8 : K_SLOW;
        imm = 0;
    } else if (opc == 0x63) {
        k = (f3 & 6) == 2 ? K_SLOW : K_BR + f3 - (f3 >= 4 ? 2 : 0);
        imm = at + imm_b(w);
        rd = 64;
        if (imm - 0x80000000u >= RamBytes) k = K_SLOW;   // (the fast path's targets are in RAM)
    } else if (opc == 0x03 || (opc == 0x07 && f3 == 2)) {
        k = f3 == 2 ? K_LW : f3 < 2 ? K_LDS + f3 : f3 == 4 || f3 == 5 ? K_LDS + f3 - 2 : K_SLOW;
        fd = opc == 0x07;
    } else if (opc == 0x23 || (opc == 0x27 && f3 == 2)) {
        k = f3 == 2 ? K_SW : f3 < 2 ? K_STS + f3 : K_SLOW;
        imm = imm_s(w);
        rs2 += opc == 0x27 ? 32 : 0;
        rd = 64;
    } else if (opc == 0x37 || opc == 0x17) {
        k = K_LI;
        imm = (w & 0xfffff000u) + (opc == 0x17 ? at : 0);
    } else if (opc == 0x6f) {
        imm = at + imm_j(w);
        k = imm - 0x80000000u < RamBytes ? K_JAL : K_SLOW;
    } else if (opc == 0x67 && f3 == 0) {
        k = K_JALR;
    } else if (opc == 0x53 && (f7 & 3) == 0 && ((0x5510083fu >> (f7 >> 2)) & 1) != 0) {
        k = K_FP;
        imm = w;
        rs1 += (f7 & 0x68) != 0x68 ? 32 : 0;
        rs2 += 32;
        fd = (f7 & 0x48) != 0x40;
    } else if ((opc & 0x73) == 0x43) {
        k = K_FMA;
        imm = w;
        rs1 += 32;
        rs2 += 32;
        fd = true;
    }
    rd = fd ? rd + 32 : rd == 0 ? 64 : rd;
    return uint2(k | rd << 6 | rs1 << 13 | rs2 << 20 | (w >> 27) << 27, imm);
}

// A store the fast path made: the page is marked, the entry of the word cleared.
void stored(uint ro) {
    if ((ro >> 12) != marked) mark(ro >> 12);
    Dec.Store(DEC_RO(ro & ~3u), 0);
}

// The fast path, from the decoded entries: the inner loop runs what cannot trap or reach a
// CSR, testing the kinds most common first, and reads the entries of pc + 4 and of a branch's
// target while it runs the one of pc; an entry to make, M and the floats are the outer loop's;
// the rest is the general path's. Returns the step count reached; the instruction at pc is
// then the general path's.
uint fast_run(uint n) {
    [loop]
    while (n < Steps) {
        if (pc - 0x80000000u >= RamBytes) break;
        uint2 e = Dec.Load2(DEC_AT(pc));
        [loop]
        for (; n < Steps; n++) {
            {
                uint2 en = Dec.Load2(DEC_AT(pc + 4)), et = Dec.Load2(min(e.y - 0x80000000u, RamBytes - 4) << 1);
                uint k = e.x & 63, imm = e.y, next = pc + 4, v = 0;
                uint a = r[(e.x >> 13) & 127], b = r[(e.x >> 20) & 127];
                [branch] if (k == K_ADD || k - K_ALU < 9) {
                    uint y = b + imm, s = y & 31, f = k - K_ALU;
                    v = (((f == 0 ? a << s : 0u) | (f == 1 ? (asint(a) < asint(y) ? 1u : 0u) : 0u)) | ((f == 2 ? (a < y ? 1u : 0u) : 0u) | (f == 3 ? a ^ y : 0u))) |
                        (((f == 4 ? a >> s : 0u) | (f == 5 ? a | y : 0u)) | ((f == 6 ? a & y : 0u) | (f == 7 ? a - y : 0u))) | ((f == 8 ? asuint(asint(a) >> s) : 0u) | (k == K_ADD ? a + y : 0u));
                } else [branch] if (k == K_LW) {
                    uint ro = a + imm - 0x80000000u;
                    if (ro >= RamBytes || (ro & 3) != 0) break;
                    v = Ram.Load(view + ro);
                } else [branch] if (k - K_BR < 6) {
                    uint f = k - K_BR;
                    bool lt = (f & 4) != 0 ? a < b : asint(a) < asint(b);
                    next = ((f < 2 ? a == b : lt) != ((f & 1) != 0)) ? imm : next;
                } else [branch] if (k == K_SW) {
                    uint ro = a + imm - 0x80000000u;
                    if (ro >= RamBytes || (ro & 3) != 0) break;
                    Ram.Store(view + ro, b);
                    stored(ro);
                    if (ro == pc + 4 - 0x80000000u) en = 0;   // (the next instruction itself)
                } else [branch] if (k - K_LDS < 6) {
                    // lb lh lbu lhu, sb sh
                    uint f = k < K_STS ? k - K_LDS : k - K_STS, size = 1u << (f & 1), ad = a + imm, ro = ad - 0x80000000u;
                    if (ro >= RamBytes || (ad & (size - 1)) != 0) break;
                    uint sh = (ad & 3) * 8, word = Ram.Load(view + (ro & ~3u)), s = 32 - 8 * size;
                    [branch] if (k < K_STS) {
                        v = word >> sh << s;
                        v = (f & 2) != 0 ? v >> s : asuint(asint(v) >> s);
                    } else {
                        uint m = (0xffffffffu >> s) << sh;
                        Ram.Store(view + (ro & ~3u), (word & ~m) | ((b << sh) & m));
                        stored(ro);
                        if ((ro & ~3u) == pc + 4 - 0x80000000u) en = 0;
                    }
                } else [branch] if (k == K_JAL) {
                    v = next;
                    next = imm;
                } else [branch] if (k == K_LI) {
                    v = imm;
                } else [branch] if (k == K_JALR) {
                    // a jump by register: the next entry is read now
                    next = (a + imm) & ~1u;
                    r[(e.x >> 6) & 127] = pc + 4;
                    pc = next;
                    if (next - 0x80000000u >= RamBytes) { n++; break; }
                    e = Dec.Load2(DEC_AT(next));
                    continue;
                } else break;   // (0: no entry yet; M, the floats, the general path's)
                r[(e.x >> 6) & 127] = v;
                e = next == pc + 4 ? en : et;
                pc = next;
                    }
            if (++n >= Steps) break;
            {
                uint2 en = Dec.Load2(DEC_AT(pc + 4)), et = Dec.Load2(min(e.y - 0x80000000u, RamBytes - 4) << 1);
                uint k = e.x & 63, imm = e.y, next = pc + 4, v = 0;
                uint a = r[(e.x >> 13) & 127], b = r[(e.x >> 20) & 127];
                [branch] if (k == K_ADD || k - K_ALU < 9) {
                    uint y = b + imm, s = y & 31, f = k - K_ALU;
                    v = (((f == 0 ? a << s : 0u) | (f == 1 ? (asint(a) < asint(y) ? 1u : 0u) : 0u)) | ((f == 2 ? (a < y ? 1u : 0u) : 0u) | (f == 3 ? a ^ y : 0u))) |
                        (((f == 4 ? a >> s : 0u) | (f == 5 ? a | y : 0u)) | ((f == 6 ? a & y : 0u) | (f == 7 ? a - y : 0u))) | ((f == 8 ? asuint(asint(a) >> s) : 0u) | (k == K_ADD ? a + y : 0u));
                } else [branch] if (k == K_LW) {
                    uint ro = a + imm - 0x80000000u;
                    if (ro >= RamBytes || (ro & 3) != 0) break;
                    v = Ram.Load(view + ro);
                } else [branch] if (k - K_BR < 6) {
                    uint f = k - K_BR;
                    bool lt = (f & 4) != 0 ? a < b : asint(a) < asint(b);
                    next = ((f < 2 ? a == b : lt) != ((f & 1) != 0)) ? imm : next;
                } else [branch] if (k == K_SW) {
                    uint ro = a + imm - 0x80000000u;
                    if (ro >= RamBytes || (ro & 3) != 0) break;
                    Ram.Store(view + ro, b);
                    stored(ro);
                    if (ro == pc + 4 - 0x80000000u) en = 0;   // (the next instruction itself)
                } else [branch] if (k - K_LDS < 6) {
                    // lb lh lbu lhu, sb sh
                    uint f = k < K_STS ? k - K_LDS : k - K_STS, size = 1u << (f & 1), ad = a + imm, ro = ad - 0x80000000u;
                    if (ro >= RamBytes || (ad & (size - 1)) != 0) break;
                    uint sh = (ad & 3) * 8, word = Ram.Load(view + (ro & ~3u)), s = 32 - 8 * size;
                    [branch] if (k < K_STS) {
                        v = word >> sh << s;
                        v = (f & 2) != 0 ? v >> s : asuint(asint(v) >> s);
                    } else {
                        uint m = (0xffffffffu >> s) << sh;
                        Ram.Store(view + (ro & ~3u), (word & ~m) | ((b << sh) & m));
                        stored(ro);
                        if ((ro & ~3u) == pc + 4 - 0x80000000u) en = 0;
                    }
                } else [branch] if (k == K_JAL) {
                    v = next;
                    next = imm;
                } else [branch] if (k == K_LI) {
                    v = imm;
                } else [branch] if (k == K_JALR) {
                    // a jump by register: the next entry is read now
                    next = (a + imm) & ~1u;
                    r[(e.x >> 6) & 127] = pc + 4;
                    pc = next;
                    if (next - 0x80000000u >= RamBytes) { n++; break; }
                    e = Dec.Load2(DEC_AT(next));
                    continue;
                } else break;   // (0: no entry yet; M, the floats, the general path's)
                r[(e.x >> 6) & 127] = v;
                e = next == pc + 4 ? en : et;
                pc = next;
                    }
        }
        if (n >= Steps) break;
        if (pc - 0x80000000u >= RamBytes) break;
        // an entry to make, M and the floats; anything else is the general path's
        e = Dec.Load2(DEC_AT(pc));
        [branch] if (e.x == 0) {
            Dec.Store2(DEC_AT(pc), decode(pc - 0x80000000u));
            continue;
        }
        uint k = e.x & 63, a = r[(e.x >> 13) & 127], b = r[(e.x >> 20) & 127], v;
        [branch] if (k - K_M < 8) v = op_m(k - K_M, a, b);
        else [branch] if (k == K_FP) v = op_fp(e.y, a, b);
        else [branch] if (k == K_FMA) v = fmadd(e.y & 0x7f, a, b, r[32 + (e.x >> 27)]);
        else break;
        r[(e.x >> 6) & 127] = v;
        pc += 4;
        n++;
    }
    return n;
}

// A worker's mailbox as the pass found it (src/mc.h, mc_idle and mc_enter): whether it runs in
// this pass. While its start texel is "MCSU" (pc, stack, page table) it starts there, whatever it
// was doing, in user mode with its number in a0 (core 0 takes the word away once the worker has
// said it runs); "MCST" parks it; a sleeping one wakes when its job word is no longer the one it
// slept on, a stopped one when the resume word is no longer the one it stopped with (a call's
// answer in a0).
bool worker_wakes() {
    uint start = view + MBOX + 16 * hart, job = view + MBOX + 0x4000 + 64 * hart, mode = State.Load(base + W_MODE);
    uint go = Ram.Load(start);
    if (go == MC_START) {
        uint k;
        for (k = 0; k < 80; k++) State.Store(base + 4 * k, 0);
        State.Store(base + 4 * 2, Ram.Load(start + 8));
        State.Store(base + 4 * 10, hart);
        State.Store(base + 256, 0x6000);   // floats on
        State.Store4(base + 320, uint4(Ram.Load(start + 4), 0, 0, ~0u));
        State.Store4(base + 336, uint4(0, 0, 0, 0));
        State.Store(base + 352, 0);
        mode = 1;
    } else if (go == MC_STOP || mode == 0) {
        mode = 0;
    } else if (mode == 2) {
        mode = Ram.Load(job) != State.Load(base + W_JOB) ? 1 : 2;
    } else if (mode >= 3 && ((Ram.Load(job + 48) ^ State.Load(base + W_SEEN)) & 0xffffff) != 0) {
        if (mode == 4) {
            State.Store(base + 4 * 10, Ram.Load(job + 52));
            State.Store(base + 320, State.Load(base + 320) + 4);
        }
        mode = 1;
    }
    State.Store(base + W_MODE, mode);
    return mode == 1;
}

// The host starts core 0 with every word 0 but pc = 0x80400000, priv = 1 (supervisor),
// x11 = 0x82200000 (the device tree) and res = ~0, and dispatches a group for every core a pass.
[numthreads(1, 1, 1)]
void tick(uint3 g : SV_GroupID) {
    hart = g.x;
    base = hart * 512;
    view = hart == 0 ? 0 : RamBytes;
    marked = ~0u;
    if (hart != 0 && AllRun == 0 && !worker_wakes()) return;
    uint k;
    for (k = 0; k < 64; k++) r[k] = State.Load(base + 4 * k);
    for (k = 0; k < 16; k++) csr[k] = State.Load(base + 256 + 4 * k);
    uint4 p = State.Load4(base + 320), q = State.Load4(base + 336);
    pc = p.x; priv = p.y; fcsr = p.z; res = p.w;
    tcmp_lo = q.x; tcmp_hi = q.y; msrc = q.z; mdst = q.w; mn = State.Load(base + 352);
    why = 0;

    if (timer_due()) csr[12] |= 0x20;
    bool look = true;
    uint n = 0;
    [loop]
    while (n < Steps) {
        if (look && (hart == 0 || AllRun != 0) && interrupt()) { n++; continue; }
        n = fast_run(n);
        if (n >= Steps) break;
        ran = n;
        uint s = step();
        n++;
        if (s == 2) break;
        look = s == 1;
    }

    for (k = 0; k < 64; k++) State.Store(base + 4 * k, r[k]);
    for (k = 0; k < 16; k++) State.Store(base + 256 + 4 * k, csr[k]);
    State.Store4(base + 320, uint4(pc, priv, fcsr, res));
    State.Store4(base + 336, uint4(tcmp_lo, tcmp_hi, msrc, mdst));
    State.Store(base + 352, mn);
    State.Store(base + W_RAN, n);   // fewer than Steps: the pass ended on wfi, pause, fence.i, a stop
    uint lo = State.Load(base + W_INSTR) + n;
    State.Store(base + W_INSTR + 4, State.Load(base + W_INSTR + 4) + (lo < n ? 1u : 0u));
    State.Store(base + W_INSTR, lo);
    State.Store(base + W_INSTR + 8, State.Load(base + W_INSTR + 8) + 1);
    // a worker's pause puts it to sleep on its job word as the pass found it; ebreak parks it
    if (hart != 0 && AllRun == 0 && why == 1) { State.Store(base + W_MODE, 2); State.Store(base + W_JOB, Ram.Load(view + MBOX + 0x4000 + 64 * hart)); }
    if (hart != 0 && AllRun == 0 && why == 2) State.Store(base + W_MODE, 0);
}

// After every pass: merge_list puts the 4 KB pages stored to in a list (and the dispatch for
// them in Args, and their 4 MB bands for the harness: sync.hlsl), clearing the marks, reading
// only the words of 32 pages that say one was marked; the GPU memory's pages (bands 24 to 31)
// not yet in this frame's list go into it (its dispatch in Args[3], which sync.hlsl takes and
// clears). merge, a thread group a page of the list, takes each word as core 0 left it if core 0
// changed it, else as the workers left it, into all three images (two cores store to one 16
// bytes in one pass on no machine). A word that changed is no instruction decoded any more.
groupshared uint counts[1024];
[numthreads(1024, 1, 1)]
void merge_list(uint t : SV_GroupIndex) {
    uint first = t * 32, mine = 0, k, frame = Args.Load(12);
    bool any = Marks.Load(M_GROUP + 4 * t) != 0;
    if (any)
        for (k = first; k < first + 32; k++)
            if (Marks.Load(M_PAGE + 4 * k) != 0) mine += k >= 24 * 1024 && Marks.Load(M_EXP + 4 * k) == 0 ? 0x10001u : 1u;
    counts[t] = mine;
    GroupMemoryBarrierWithGroupSync();
    for (uint d = 1; d < 1024; d <<= 1) {   // where this thread's pages go in the lists
        uint add = t >= d ? counts[t - d] : 0;
        GroupMemoryBarrierWithGroupSync();
        counts[t] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    uint at = counts[t] - mine, fat = frame + (at >> 16);
    at &= 0xffff;
    if (any) {
        for (k = first; k < first + 32; k++) {
            if (Marks.Load(M_PAGE + 4 * k) == 0) continue;
            Marks.Store(M_LIST + 4 * at++, k);
            Marks.Store(M_PAGE + 4 * k, 0);
            Marks.Store(M_HARNESS + 4 * (k >> 10), 1);
            if (k < 24 * 1024 || Marks.Load(M_EXP + 4 * k) != 0) continue;   // (Linux's own go as bands)
            Marks.Store(M_EXP + 4 * k, 1);
            Marks.Store(M_FRAME + 4 * fat++, k);
        }
        Marks.Store(M_GROUP + 4 * t, 0);
    }
    if (t == 1023) {
        uint all = counts[1023];
        Args.Store3(0, uint3(all & 0xffff, 1, 1));
        Args.Store3(12, uint3(min(frame + (all >> 16), 32768u), 1, 1));
    }
}
[numthreads(256, 1, 1)]
void merge(uint3 g : SV_GroupID, uint t : SV_GroupIndex) {
    uint page = Marks.Load(M_LIST + 4 * g.x);
    for (uint k = 0; k < 4; k++) {
        uint o = page * 4096 + (t + 256 * k) * 4;
        uint own = Ram.Load(o), theirs = Ram.Load(RamBytes + o), was = Ram.Load(2 * RamBytes + o);
        uint now = own != was ? own : theirs;
        if (now == was) continue;
        Ram.Store(o, now);
        Ram.Store(RamBytes + o, now);
        Ram.Store(2 * RamBytes + o, now);
        Dec.Store(DEC_RO(o), 0);
        Dec.Store(DEC_RO(o - 4), 0);
    }
}
