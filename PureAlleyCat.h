/* PureAlleyCat - Alley Cat (1984) as an embeddable C library.
 *
 * Shaped after PureDOOM: the library is the engine, and you supply the original data. Here the
 * data file is the game's own CAT.EXE, which holds both the code and all 29 KB of artwork, so
 * nothing copyrighted ships with the library.
 *
 *     #include "purecat.h"
 *
 *     alleycat_init(exe_bytes, exe_size);
 *     while (running) {
 *         alleycat_key(ALLEYCAT_KEY_LEFT, held);
 *         alleycat_update();                        // one frame
 *         draw(alleycat_framebuffer());             // 320x200, one byte per pixel, values 0-3
 *     }
 *
 * No allocation, no file I/O, no dependency beyond <stdint.h> and <string.h>. One megabyte of
 * state lives in the library, so it is a big BSS object rather than a heap user.
 *
 * What it actually is: a small 8086 real-mode interpreter plus the handful of BIOS calls and
 * ports the game touches. A source port would be nicer, but the source was never released, and
 * Ghidra's decompilation of hand-written assembly loses the register-passing conventions the
 * program is built on. This runs the real thing, and it is exact.
 */
#ifndef ALLEYCAT_H
#define ALLEYCAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALLEYCAT_WIDTH  320
#define ALLEYCAT_HEIGHT 200

/* Scancodes the game reads. It installs its own INT 9 handler and reads port 0x60 directly, so
 * these are IBM PC set-1 make codes; alleycat_key feeds them through the same path. */
enum {
    ALLEYCAT_KEY_UP    = 0x48,
    ALLEYCAT_KEY_DOWN  = 0x50,
    ALLEYCAT_KEY_LEFT  = 0x4B,
    ALLEYCAT_KEY_RIGHT = 0x4D,
    ALLEYCAT_KEY_ALT   = 0x38,
    ALLEYCAT_KEY_ESC   = 0x01,
    ALLEYCAT_KEY_CTRL  = 0x1D,
    ALLEYCAT_KEY_S     = 0x1F,
    ALLEYCAT_KEY_M     = 0x32,
    ALLEYCAT_KEY_Y     = 0x15,
    ALLEYCAT_KEY_N     = 0x31,
};

/* Loads CAT.EXE and resets the machine. Returns 0 on success, non-zero if it is not an MZ image.
 * The bytes are copied in, so the caller may free them afterwards. */
int alleycat_init(const void *exe, int exe_size);

/* Runs approximately one 18.2 Hz tick worth of instructions. */
void alleycat_update(void);

/* Runs an explicit number of instructions; alleycat_update is a wrapper on this. */
void alleycat_run(int instructions);

/* 320x200, one byte per pixel, each 0-3 indexing the CGA palette. Decoded from the emulated
 * framebuffer, so it is valid after any alleycat_update. */
const uint8_t *alleycat_framebuffer(void);

/* The four RGB triples for the palette the game selected, as 0xRRGGBB. */
void alleycat_palette(uint32_t out[4]);

/* Press or release a key. `down` is non-zero for a press. */
void alleycat_key(int scancode, int down);

/* Non-zero once the program has set a graphics mode, i.e. it is past its startup checks. */
int alleycat_ready(void);

/* Total instructions retired, and the last stop reason, for diagnostics. */
uint64_t alleycat_instructions(void);
const char *alleycat_status(void);

/* -1 if none; otherwise the opcode that stopped the run, and its CS:IP packed as 0xSSSSIIII. */
int alleycat_fault_opcode(void);
uint32_t alleycat_fault_address(void);

#ifdef __cplusplus
}
#endif

#endif /* ALLEYCAT_H */



/* ============================================================================
 *  Implementation.
 *
 *  Define ALLEYCAT_IMPLEMENTATION in exactly one translation unit before
 *  including this header and that unit gets the whole interpreter, the way
 *  PureDOOM's DOOM_IMPLEMENTATION works. Every other unit includes the header
 *  for the declarations alone.
 * ============================================================================ */
#ifdef ALLEYCAT_IMPLEMENTATION

#include <string.h>

/* ---- machine state ------------------------------------------------------------------------ */

#define MEM_SIZE   (1u << 20)
#define LOAD_SEG   0x1000u
#define PSP_SEG    (LOAD_SEG - 0x10u)
#define VIDEO_SEG  0xB800u
#define TICK_STEPS 6000            /* instructions per 18.2 Hz tick, as in the Python reference */

enum { rAX, rCX, rDX, rBX, rSP, rBP, rSI, rDI };
enum { sES, sCS, sSS, sDS };

enum {
    fCF = 0x0001, fPF = 0x0004, fAF = 0x0010, fZF = 0x0040,
    fSF = 0x0080, fTF = 0x0100, fIF = 0x0200, fDF = 0x0400, fOF = 0x0800
};

static uint8_t  MEM[MEM_SIZE];
static uint16_t R[8];
static uint16_t S[4];
static uint16_t IP;
static uint16_t FLAGS = 0x0002;
static uint64_t ICOUNT;
static int      VIDEO_MODE = -1;
static const char *STATUS = "not started";
static uint8_t  FRAME[ALLEYCAT_WIDTH * ALLEYCAT_HEIGHT];
static uint8_t  SCANCODE;
static uint8_t  PORT61;
static uint32_t RETRACE;
static int      STOPPED;
static int      BAD_OP = -1;
static uint16_t BAD_CS, BAD_IP;

/* A tiny key queue: the game installs its own INT 9 handler, so a key is delivered by putting the
 * make/break code where port 0x60 will be read and raising interrupt 9. */
static uint8_t  KEYQ[32];
static int      KEYQ_HEAD, KEYQ_TAIL;

static uint8_t PARITY[256];
static int     PARITY_READY;

static void parity_init(void)
{
    for (int i = 0; i < 256; i++) {
        int bits = 0, v = i;
        while (v) { bits += v & 1; v >>= 1; }
        PARITY[i] = (uint8_t)((bits & 1) == 0);
    }
    PARITY_READY = 1;
}

/* ---- memory --------------------------------------------------------------------------------- */

static uint32_t phys(uint16_t seg, uint16_t off) { return (uint32_t)(((seg << 4) + off) & 0xFFFFF); }
static uint8_t  rd8(uint16_t seg, uint16_t off)  { return MEM[phys(seg, off)]; }
static void     wr8(uint16_t seg, uint16_t off, uint8_t v) { MEM[phys(seg, off)] = v; }

static uint16_t rd16(uint16_t seg, uint16_t off)
{
    return (uint16_t)(MEM[phys(seg, off)] | (MEM[phys(seg, (uint16_t)(off + 1))] << 8));
}

static void wr16(uint16_t seg, uint16_t off, uint16_t v)
{
    MEM[phys(seg, off)] = (uint8_t)v;
    MEM[phys(seg, (uint16_t)(off + 1))] = (uint8_t)(v >> 8);
}

/* 8-bit register halves: index 0-3 are AL CL DL BL, 4-7 are AH CH DH BH. */
static uint8_t get8(int i)
{
    return (uint8_t)((i & 4) ? (R[i & 3] >> 8) : (R[i & 3] & 0xFF));
}

static void set8(int i, uint8_t v)
{
    int j = i & 3;
    if (i & 4) R[j] = (uint16_t)((R[j] & 0x00FF) | (v << 8));
    else       R[j] = (uint16_t)((R[j] & 0xFF00) | v);
}

/* ---- fetch ----------------------------------------------------------------------------------- */

static uint8_t fetch8(void)   { uint8_t v = rd8(S[sCS], IP); IP++; return v; }
static uint16_t fetch16(void) { uint16_t v = rd16(S[sCS], IP); IP = (uint16_t)(IP + 2); return v; }
static int8_t fetchs8(void)   { return (int8_t)fetch8(); }

/* ---- flags ------------------------------------------------------------------------------------ */

static void setf(uint16_t mask, int on) { FLAGS = (uint16_t)(on ? (FLAGS | mask) : (FLAGS & ~mask)); }
static int  getf(uint16_t mask) { return (FLAGS & mask) != 0; }

static void szp(uint32_t v, int w)
{
    setf(fZF, v == 0);
    setf(fSF, (v & (w ? 0x8000u : 0x80u)) != 0);
    setf(fPF, PARITY[v & 0xFF]);
}

static void logic_flags(uint32_t v, int w) { setf(fCF | fOF, 0); szp(v, w); }

static void add_flags(uint32_t a, uint32_t b, uint32_t r, int w, uint32_t carry)
{
    uint32_t mask = w ? 0xFFFFu : 0xFFu;
    uint32_t sign = w ? 0x8000u : 0x80u;
    setf(fCF, (a + b + carry) > mask);
    setf(fAF, ((a ^ b ^ r) & 0x10) != 0);
    setf(fOF, ((~(a ^ b) & (a ^ r)) & sign) != 0);
    szp(r & mask, w);
}

static void sub_flags(uint32_t a, uint32_t b, uint32_t r, int w, uint32_t borrow)
{
    uint32_t mask = w ? 0xFFFFu : 0xFFu;
    uint32_t sign = w ? 0x8000u : 0x80u;
    setf(fCF, (b + borrow) > a);
    setf(fAF, ((a ^ b ^ r) & 0x10) != 0);
    setf(fOF, (((a ^ b) & (a ^ r)) & sign) != 0);
    szp(r & mask, w);
}

/* ---- mod/rm -------------------------------------------------------------------------------------- */

typedef struct { int mod, reg, rm; uint16_t seg, off; } Modrm;

static Modrm decode_modrm(int seg_override)
{
    Modrm m;
    uint8_t byte = fetch8();
    int base_seg;
    int32_t base;

    m.mod = byte >> 6;
    m.reg = (byte >> 3) & 7;
    m.rm  = byte & 7;
    m.seg = 0;
    m.off = 0;
    if (m.mod == 3) return m;

    switch (m.rm) {
    case 0: base = R[rBX] + R[rSI]; base_seg = sDS; break;
    case 1: base = R[rBX] + R[rDI]; base_seg = sDS; break;
    case 2: base = R[rBP] + R[rSI]; base_seg = sSS; break;
    case 3: base = R[rBP] + R[rDI]; base_seg = sSS; break;
    case 4: base = R[rSI];          base_seg = sDS; break;
    case 5: base = R[rDI];          base_seg = sDS; break;
    case 6:
        if (m.mod == 0) { base = fetch16(); base_seg = sDS; }
        else            { base = R[rBP];    base_seg = sSS; }
        break;
    default: base = R[rBX]; base_seg = sDS; break;
    }
    if (m.mod == 1)      base += fetchs8();
    else if (m.mod == 2) base += (int16_t)fetch16();

    if (seg_override >= 0) base_seg = seg_override;
    m.seg = S[base_seg];
    m.off = (uint16_t)base;
    return m;
}

static uint16_t rm_read(int w, const Modrm *m)
{
    if (m->mod == 3) return w ? R[m->rm] : get8(m->rm);
    return w ? rd16(m->seg, m->off) : rd8(m->seg, m->off);
}

static void rm_write(int w, const Modrm *m, uint16_t v)
{
    if (m->mod == 3) { if (w) R[m->rm] = v; else set8(m->rm, (uint8_t)v); }
    else if (w)      wr16(m->seg, m->off, v);
    else             wr8(m->seg, m->off, (uint8_t)v);
}

/* ---- stack ------------------------------------------------------------------------------------------ */

static void push(uint16_t v) { R[rSP] = (uint16_t)(R[rSP] - 2); wr16(S[sSS], R[rSP], v); }
static uint16_t pop(void)    { uint16_t v = rd16(S[sSS], R[rSP]); R[rSP] = (uint16_t)(R[rSP] + 2); return v; }

/* ---- alu / shift -------------------------------------------------------------------------------------- */

/* Returns the result; *store is cleared for cmp, which writes nothing. */
static uint16_t alu(int op, uint32_t a, uint32_t b, int w, int *store)
{
    uint32_t mask = w ? 0xFFFFu : 0xFFu, r;
    uint32_t c;
    *store = 1;
    switch (op) {
    case 0: r = (a + b) & mask;  add_flags(a, b, r, w, 0); return (uint16_t)r;
    case 1: r = a | b;           logic_flags(r, w);        return (uint16_t)r;
    case 2: c = getf(fCF) ? 1u : 0u; r = (a + b + c) & mask; add_flags(a, b, r, w, c); return (uint16_t)r;
    case 3: c = getf(fCF) ? 1u : 0u; r = (a - b - c) & mask; sub_flags(a, b, r, w, c); return (uint16_t)r;
    case 4: r = a & b;           logic_flags(r, w);        return (uint16_t)r;
    case 5: r = (a - b) & mask;  sub_flags(a, b, r, w, 0); return (uint16_t)r;
    case 6: r = a ^ b;           logic_flags(r, w);        return (uint16_t)r;
    default:
        r = (a - b) & mask; sub_flags(a, b, r, w, 0); *store = 0; return (uint16_t)r;
    }
}

static uint16_t shift_op(int op, uint32_t v, int count, int w)
{
    uint32_t mask = w ? 0xFFFFu : 0xFFu;
    uint32_t sign = w ? 0x8000u : 0x80u;
    int c = 0, i;

    count &= 0x1F;
    if (count == 0) return (uint16_t)v;
    for (i = 0; i < count; i++) {
        switch (op) {
        case 0: c = (v & sign) != 0; v = ((v << 1) | (uint32_t)c) & mask; break;          /* rol */
        case 1: c = v & 1; v = ((v >> 1) | (c ? sign : 0)) & mask; break;                  /* ror */
        case 2: { int old = getf(fCF); c = (v & sign) != 0;
                  v = ((v << 1) | (uint32_t)old) & mask; break; }                          /* rcl */
        case 3: { int old = getf(fCF); c = v & 1;
                  v = ((v >> 1) | (old ? sign : 0)) & mask; break; }                       /* rcr */
        case 4: case 6: c = (v & sign) != 0; v = (v << 1) & mask; break;                   /* shl */
        case 5: c = v & 1; v = (v >> 1) & mask; break;                                     /* shr */
        default: c = v & 1; v = ((v >> 1) | (v & sign)) & mask; break;                     /* sar */
        }
        setf(fCF, c);
    }
    if (op == 4 || op == 5 || op == 6 || op == 7) szp(v, w);
    setf(fOF, count == 1 ? (((v ^ (v << 1)) & sign) != 0) : 0);
    return (uint16_t)v;
}

static int condition(int code)
{
    int o = getf(fOF), c = getf(fCF), z = getf(fZF), s = getf(fSF), p = getf(fPF);
    switch (code) {
    case 0:  return o;              case 1:  return !o;
    case 2:  return c;              case 3:  return !c;
    case 4:  return z;              case 5:  return !z;
    case 6:  return c || z;         case 7:  return !(c || z);
    case 8:  return s;              case 9:  return !s;
    case 10: return p;              case 11: return !p;
    case 12: return s != o;         case 13: return s == o;
    case 14: return z || (s != o);  default: return !z && (s == o);
    }
}

/* ---- the platform ------------------------------------------------------------------------------------- */

static uint8_t port_in(uint16_t port)
{
    switch (port) {
    case 0x3DA: RETRACE++; return (uint8_t)((RETRACE & 1) ? 0x09 : 0x00);
    case 0x60:  return SCANCODE;
    case 0x61:  return PORT61;
    case 0x40:  return (uint8_t)(ICOUNT >> 2);
    case 0x201: return 0xF0;
    default:    return 0xFF;
    }
}

static void port_out(uint16_t port, uint8_t value)
{
    if (port == 0x61) PORT61 = value;
}

static void interrupt(int n);

/* Returns 1 if the host answered, 0 to fall through to the interrupt vector table. */
static int bios(int n)
{
    uint8_t ah = get8(4);
    if (n == 0x1A && ah == 0x00) {
        uint32_t ticks = (uint32_t)(ICOUNT / TICK_STEPS);
        R[rCX] = (uint16_t)(ticks >> 16);
        R[rDX] = (uint16_t)ticks;
        set8(rAX, 0);
        return 1;
    }
    if (n == 0x10) {
        if (ah == 0x00) VIDEO_MODE = get8(rAX);
        return 1;
    }
    if (n == 0x11) { R[rAX] = 0x0021; return 1; }
    if (n == 0x12) { R[rAX] = 640;    return 1; }
    if (n == 0x20) { STOPPED = 1; STATUS = "int 20h: program exit"; return 1; }
    if (n == 0x21 && ah == 0x4C) { STOPPED = 1; STATUS = "int 21h/4C: program exit"; return 1; }
    return 0;
}

static void interrupt(int n)
{
    uint16_t off, seg;
    if (bios(n)) return;
    off = rd16(0, (uint16_t)(n * 4));
    seg = rd16(0, (uint16_t)(n * 4 + 2));
    if (seg == 0 && off == 0) return;        /* nothing installed; ignore */
    push(FLAGS);
    push(S[sCS]);
    push(IP);
    setf(fIF, 0);
    S[sCS] = seg;
    IP = off;
}

/* ---- string operations ------------------------------------------------------------------------------------ */

static void string_op(int op, int seg_override, int rep)
{
    int w = op & 1;
    int16_t step = (int16_t)((w ? 2 : 1) * (getf(fDF) ? -1 : 1));
    uint16_t src_seg = S[seg_override >= 0 ? seg_override : sDS];

    if (rep && R[rCX] == 0) return;
    for (;;) {
        switch (op) {
        case 0xA4: case 0xA5: {
            uint16_t v = w ? rd16(src_seg, R[rSI]) : rd8(src_seg, R[rSI]);
            if (w) wr16(S[sES], R[rDI], v); else wr8(S[sES], R[rDI], (uint8_t)v);
            R[rSI] = (uint16_t)(R[rSI] + step);
            R[rDI] = (uint16_t)(R[rDI] + step);
            break;
        }
        case 0xA6: case 0xA7: {
            uint32_t a = w ? rd16(src_seg, R[rSI]) : rd8(src_seg, R[rSI]);
            uint32_t b = w ? rd16(S[sES], R[rDI]) : rd8(S[sES], R[rDI]);
            sub_flags(a, b, (a - b) & (w ? 0xFFFFu : 0xFFu), w, 0);
            R[rSI] = (uint16_t)(R[rSI] + step);
            R[rDI] = (uint16_t)(R[rDI] + step);
            break;
        }
        case 0xAA: case 0xAB:
            if (w) wr16(S[sES], R[rDI], R[rAX]); else wr8(S[sES], R[rDI], get8(rAX));
            R[rDI] = (uint16_t)(R[rDI] + step);
            break;
        case 0xAC: case 0xAD:
            if (w) R[rAX] = rd16(src_seg, R[rSI]); else set8(rAX, rd8(src_seg, R[rSI]));
            R[rSI] = (uint16_t)(R[rSI] + step);
            break;
        default: {
            uint32_t a = w ? R[rAX] : get8(rAX);
            uint32_t b = w ? rd16(S[sES], R[rDI]) : rd8(S[sES], R[rDI]);
            sub_flags(a, b, (a - b) & (w ? 0xFFFFu : 0xFFu), w, 0);
            R[rDI] = (uint16_t)(R[rDI] + step);
            break;
        }
        }
        if (!rep) return;
        R[rCX] = (uint16_t)(R[rCX] - 1);
        if (R[rCX] == 0) return;
        if (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF) {
            /* repe (F3) runs while ZF, repne (F2) while not ZF */
            if ((rep == 0xF3) != getf(fZF)) return;
        }
    }
}

/* ---- execute ------------------------------------------------------------------------------------------------ */

static void execute(uint8_t op, int seg_override, int rep)
{
    Modrm m;

    if (op < 0x40 && (op & 7) < 6) {                 /* the eight ALU ops */
        int kind = op >> 3, form = op & 7, w = form & 1, store;
        uint16_t res;
        if (form < 4) {
            uint32_t a, b;
            m = decode_modrm(seg_override);
            a = rm_read(w, &m);
            b = w ? R[m.reg] : get8(m.reg);
            if (form & 2) {
                res = alu(kind, b, a, w, &store);
                if (store) { if (w) R[m.reg] = res; else set8(m.reg, (uint8_t)res); }
            } else {
                res = alu(kind, a, b, w, &store);
                if (store) rm_write(w, &m, res);
            }
        } else {
            uint32_t imm = w ? fetch16() : fetch8();
            uint32_t a = w ? R[rAX] : get8(rAX);
            res = alu(kind, a, imm, w, &store);
            if (store) { if (w) R[rAX] = res; else set8(rAX, (uint8_t)res); }
        }
        return;
    }

    switch (op) {
    case 0x06: case 0x0E: case 0x16: case 0x1E: push(S[(op >> 3) & 3]); return;
    case 0x07: case 0x17: case 0x1F: S[(op >> 3) & 3] = pop(); return;

    case 0x37: case 0x3F: {                          /* aaa / aas */
        uint8_t al = get8(rAX);
        if ((al & 0x0F) > 9 || getf(fAF)) {
            set8(rAX, (uint8_t)((al + (op == 0x37 ? 6 : -6)) & 0x0F));
            R[rAX] = (uint16_t)(R[rAX] + (op == 0x37 ? 0x100 : -0x100));
            setf(fAF | fCF, 1);
        } else {
            set8(rAX, (uint8_t)(al & 0x0F));
            setf(fAF | fCF, 0);
        }
        return;
    }
    case 0x27: case 0x2F: return;                    /* daa/das: unused by this program */

    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47: {
        int i = op & 7, c = getf(fCF);
        uint32_t a = R[i], r = (a + 1) & 0xFFFF;
        add_flags(a, 1, r, 1, 0); setf(fCF, c); R[i] = (uint16_t)r; return;
    }
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
        int i = op & 7, c = getf(fCF);
        uint32_t a = R[i], r = (a - 1) & 0xFFFF;
        sub_flags(a, 1, r, 1, 0); setf(fCF, c); R[i] = (uint16_t)r; return;
    }
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57: push(R[op & 7]); return;
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: R[op & 7] = pop(); return;

    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t d = fetchs8();
        if (condition(op & 0x0F)) IP = (uint16_t)(IP + d);
        return;
    }

    case 0x80: case 0x81: case 0x82: case 0x83: {
        int w = op & 1, store;
        uint32_t a, imm;
        uint16_t res;
        m = decode_modrm(seg_override);
        a = rm_read(w, &m);
        if (op == 0x81)      imm = fetch16();
        else if (op == 0x83) imm = (uint16_t)(int16_t)fetchs8();
        else                 imm = fetch8();
        res = alu(m.reg, a, imm, w, &store);
        if (store) rm_write(w, &m, res);
        return;
    }

    case 0x84: case 0x85: {
        int w = op & 1;
        uint32_t a, b;
        m = decode_modrm(seg_override);
        a = rm_read(w, &m);
        b = w ? R[m.reg] : get8(m.reg);
        logic_flags(a & b, w);
        return;
    }
    case 0x86: case 0x87: {
        int w = op & 1;
        uint16_t a, b;
        m = decode_modrm(seg_override);
        a = rm_read(w, &m);
        b = w ? R[m.reg] : get8(m.reg);
        rm_write(w, &m, b);
        if (w) R[m.reg] = a; else set8(m.reg, (uint8_t)a);
        return;
    }

    case 0x88: case 0x89: case 0x8A: case 0x8B: {
        int w = op & 1;
        m = decode_modrm(seg_override);
        if (op & 2) {
            uint16_t v = rm_read(w, &m);
            if (w) R[m.reg] = v; else set8(m.reg, (uint8_t)v);
        } else {
            rm_write(w, &m, (uint16_t)(w ? R[m.reg] : get8(m.reg)));
        }
        return;
    }
    case 0x8C: m = decode_modrm(seg_override); rm_write(1, &m, S[m.reg & 3]); return;
    case 0x8E: m = decode_modrm(seg_override); S[m.reg & 3] = rm_read(1, &m); return;
    case 0x8D: m = decode_modrm(seg_override); R[m.reg] = m.off; return;
    case 0x8F: m = decode_modrm(seg_override); rm_write(1, &m, pop()); return;

    case 0x90: return;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        int i = op & 7; uint16_t t = R[rAX]; R[rAX] = R[i]; R[i] = t; return;
    }
    case 0x98: { uint8_t v = get8(rAX); R[rAX] = (uint16_t)((v & 0x80) ? (v | 0xFF00) : v); return; }
    case 0x99: R[rDX] = (uint16_t)((R[rAX] & 0x8000) ? 0xFFFF : 0); return;
    case 0x9A: { uint16_t off = fetch16(), seg = fetch16();
                 push(S[sCS]); push(IP); S[sCS] = seg; IP = off; return; }
    case 0x9C: push(FLAGS); return;
    case 0x9D: FLAGS = (uint16_t)(pop() | 0x0002); return;
    case 0x9E: FLAGS = (uint16_t)((FLAGS & 0xFF00) | (get8(4) & 0xD5) | 0x02); return;
    case 0x9F: set8(4, (uint8_t)(FLAGS & 0xFF)); return;

    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        int w = op & 1;
        uint16_t off = fetch16();
        uint16_t sg = S[seg_override >= 0 ? seg_override : sDS];
        if (op & 2) { if (w) wr16(sg, off, R[rAX]); else wr8(sg, off, get8(rAX)); }
        else        { if (w) R[rAX] = rd16(sg, off); else set8(rAX, rd8(sg, off)); }
        return;
    }

    case 0xA4: case 0xA5: case 0xA6: case 0xA7:
    case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(op, seg_override, rep); return;

    case 0xA8: case 0xA9: {
        int w = op & 1;
        uint32_t imm = w ? fetch16() : fetch8();
        uint32_t a = w ? R[rAX] : get8(rAX);
        logic_flags(a & imm, w); return;
    }

    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7: set8(op & 7, fetch8()); return;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: R[op & 7] = fetch16(); return;

    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int w = op & 1, count;
        m = decode_modrm(seg_override);
        if (op == 0xC0 || op == 0xC1)      count = fetch8();
        else if (op == 0xD0 || op == 0xD1) count = 1;
        else                               count = get8(rCX);
        rm_write(w, &m, shift_op(m.reg, rm_read(w, &m), count, w));
        return;
    }

    case 0xC2: case 0xC3: {
        uint16_t n = (op == 0xC2) ? fetch16() : 0;
        IP = pop(); R[rSP] = (uint16_t)(R[rSP] + n); return;
    }
    case 0xCA: case 0xCB: {
        uint16_t n = (op == 0xCA) ? fetch16() : 0;
        IP = pop(); S[sCS] = pop(); R[rSP] = (uint16_t)(R[rSP] + n); return;
    }

    case 0xC4: case 0xC5:
        m = decode_modrm(seg_override);
        R[m.reg] = rd16(m.seg, m.off);
        S[op == 0xC4 ? sES : sDS] = rd16(m.seg, (uint16_t)(m.off + 2));
        return;

    case 0xC6: case 0xC7: {
        int w = op & 1;
        uint16_t imm;
        m = decode_modrm(seg_override);
        imm = (uint16_t)(w ? fetch16() : fetch8());
        rm_write(w, &m, imm); return;
    }

    case 0xCC: interrupt(3); return;
    case 0xCD: interrupt(fetch8()); return;
    case 0xCE: if (getf(fOF)) interrupt(4); return;
    case 0xCF: IP = pop(); S[sCS] = pop(); FLAGS = (uint16_t)(pop() | 0x0002); return;
    case 0xD4: case 0xD5: fetch8(); return;
    case 0xD7: set8(rAX, rd8(S[seg_override >= 0 ? seg_override : sDS],
                             (uint16_t)(R[rBX] + get8(rAX)))); return;

    case 0xE0: case 0xE1: case 0xE2: {
        int8_t d = fetchs8();
        int z;
        R[rCX] = (uint16_t)(R[rCX] - 1);
        z = getf(fZF);
        if (R[rCX] != 0 && (op == 0xE2 || (op == 0xE1 && z) || (op == 0xE0 && !z)))
            IP = (uint16_t)(IP + d);
        return;
    }
    case 0xE3: { int8_t d = fetchs8(); if (R[rCX] == 0) IP = (uint16_t)(IP + d); return; }

    case 0xE4: case 0xE5: case 0xEC: case 0xED: {
        int w = op & 1;
        uint16_t port = (op < 0xEC) ? fetch8() : R[rDX];
        uint8_t v = port_in(port);
        if (w) R[rAX] = (uint16_t)(v | (port_in((uint16_t)(port + 1)) << 8));
        else   set8(rAX, v);
        return;
    }
    case 0xE6: case 0xE7: case 0xEE: case 0xEF: {
        int w = op & 1;
        uint16_t port = (op < 0xEE) ? fetch8() : R[rDX];
        port_out(port, get8(rAX));
        if (w) port_out((uint16_t)(port + 1), (uint8_t)(R[rAX] >> 8));
        return;
    }

    case 0xE8: { int16_t d = (int16_t)fetch16(); push(IP); IP = (uint16_t)(IP + d); return; }
    case 0xE9: { int16_t d = (int16_t)fetch16(); IP = (uint16_t)(IP + d); return; }
    case 0xEA: { uint16_t off = fetch16(), seg = fetch16(); S[sCS] = seg; IP = off; return; }
    case 0xEB: { int8_t d = fetchs8(); IP = (uint16_t)(IP + d); return; }

    case 0xF4: STOPPED = 1; STATUS = "hlt"; return;
    case 0xF5: setf(fCF, !getf(fCF)); return;
    case 0xF8: setf(fCF, 0); return;
    case 0xF9: setf(fCF, 1); return;
    case 0xFA: setf(fIF, 0); return;
    case 0xFB: setf(fIF, 1); return;
    case 0xFC: setf(fDF, 0); return;
    case 0xFD: setf(fDF, 1); return;

    case 0xF6: case 0xF7: {
        int w = op & 1;
        uint32_t a, mask = w ? 0xFFFFu : 0xFFu;
        m = decode_modrm(seg_override);
        a = rm_read(w, &m);
        switch (m.reg) {
        case 0: case 1: {
            uint32_t imm = w ? fetch16() : fetch8();
            logic_flags(a & imm, w); return;
        }
        case 2: rm_write(w, &m, (uint16_t)(~a & mask)); return;
        case 3: {
            uint32_t r = (uint32_t)(-(int32_t)a) & mask;
            sub_flags(0, a, r, w, 0); setf(fCF, a != 0);
            rm_write(w, &m, (uint16_t)r); return;
        }
        case 4: {
            uint32_t hi;
            if (w) { uint32_t p = (uint32_t)R[rAX] * a;
                     R[rAX] = (uint16_t)p; R[rDX] = (uint16_t)(p >> 16); hi = R[rDX]; }
            else   { uint32_t p = (uint32_t)get8(rAX) * a;
                     R[rAX] = (uint16_t)p; hi = (p >> 8) & 0xFF; }
            setf(fCF | fOF, hi != 0); return;
        }
        case 5: {
            int32_t sa = (int32_t)((a & (w ? 0x8000u : 0x80u)) ? (int32_t)a - (int32_t)(mask + 1) : (int32_t)a);
            if (w) { int32_t sb = (int16_t)R[rAX]; int32_t p = sa * sb;
                     R[rAX] = (uint16_t)p; R[rDX] = (uint16_t)(p >> 16);
                     setf(fCF | fOF, p != (int16_t)p); }
            else   { int32_t sb = (int8_t)get8(rAX); int32_t p = sa * sb;
                     R[rAX] = (uint16_t)p; setf(fCF | fOF, p != (int8_t)p); }
            return;
        }
        case 6: {
            if (a == 0) { interrupt(0); return; }
            if (w) { uint32_t n = ((uint32_t)R[rDX] << 16) | R[rAX];
                     uint32_t q = n / a, rem = n % a;
                     if (q > 0xFFFF) { interrupt(0); return; }
                     R[rAX] = (uint16_t)q; R[rDX] = (uint16_t)rem; }
            else   { uint32_t n = R[rAX], q = n / a, rem = n % a;
                     if (q > 0xFF) { interrupt(0); return; }
                     set8(rAX, (uint8_t)q); set8(4, (uint8_t)rem); }
            return;
        }
        default: {
            int32_t sa = (int32_t)((a & (w ? 0x8000u : 0x80u)) ? (int32_t)a - (int32_t)(mask + 1) : (int32_t)a);
            if (sa == 0) { interrupt(0); return; }
            if (w) { int32_t n = (int32_t)(((uint32_t)R[rDX] << 16) | R[rAX]);
                     int32_t q = n / sa, rem = n - q * sa;
                     R[rAX] = (uint16_t)q; R[rDX] = (uint16_t)rem; }
            else   { int32_t n = (int16_t)R[rAX];
                     int32_t q = n / sa, rem = n - q * sa;
                     set8(rAX, (uint8_t)q); set8(4, (uint8_t)rem); }
            return;
        }
        }
    }

    case 0xFE: case 0xFF: {
        int w = op & 1;
        uint32_t a;
        m = decode_modrm(seg_override);
        a = rm_read(w, &m);
        switch (m.reg) {
        case 0: { int c = getf(fCF); uint32_t r = (a + 1) & (w ? 0xFFFFu : 0xFFu);
                  add_flags(a, 1, r, w, 0); setf(fCF, c); rm_write(w, &m, (uint16_t)r); return; }
        case 1: { int c = getf(fCF); uint32_t r = (a - 1) & (w ? 0xFFFFu : 0xFFu);
                  sub_flags(a, 1, r, w, 0); setf(fCF, c); rm_write(w, &m, (uint16_t)r); return; }
        case 2: push(IP); IP = (uint16_t)a; return;
        case 3: push(S[sCS]); push(IP);
                IP = rd16(m.seg, m.off); S[sCS] = rd16(m.seg, (uint16_t)(m.off + 2)); return;
        case 4: IP = (uint16_t)a; return;
        case 5: IP = rd16(m.seg, m.off); S[sCS] = rd16(m.seg, (uint16_t)(m.off + 2)); return;
        default: push((uint16_t)a); return;
        }
    }

    default:
        STOPPED = 1;
        STATUS = "unimplemented opcode";
        BAD_OP = op;
        BAD_CS = S[sCS];
        BAD_IP = (uint16_t)(IP - 1);
        return;
    }
}

static void step(void)
{
    int seg_override = -1, rep = 0;
    uint8_t op;
    for (;;) {
        op = fetch8();
        if (op == 0x26) { seg_override = sES; continue; }
        if (op == 0x2E) { seg_override = sCS; continue; }
        if (op == 0x36) { seg_override = sSS; continue; }
        if (op == 0x3E) { seg_override = sDS; continue; }
        if (op == 0xF0) continue;
        if (op == 0xF2 || op == 0xF3) { rep = op; continue; }
        break;
    }
    ICOUNT++;
    execute(op, seg_override, rep);
}

/* ---- public API --------------------------------------------------------------------------------------------- */

int alleycat_init(const void *exe_bytes, int exe_size)
{
    const uint8_t *exe = (const uint8_t *)exe_bytes;
    uint16_t pages, relocations, header_pars, bytes_last, table;
    uint32_t header, size;
    uint16_t i;

    if (!PARITY_READY) parity_init();
    if (exe_size < 32 || exe[0] != 'M' || exe[1] != 'Z') return 1;

    memset(MEM, 0, sizeof MEM);
    memset(R, 0, sizeof R);
    memset(S, 0, sizeof S);
    FLAGS = 0x0002; IP = 0; ICOUNT = 0; VIDEO_MODE = -1; STOPPED = 0;
    SCANCODE = 0; PORT61 = 0; RETRACE = 0; KEYQ_HEAD = KEYQ_TAIL = 0;
    STATUS = "running";

#define U16(o) ((uint16_t)(exe[(o)] | (exe[(o) + 1] << 8)))
    bytes_last  = U16(2);
    pages       = U16(4);
    relocations = U16(6);
    header_pars = U16(8);
    table       = U16(24);
    header = (uint32_t)header_pars * 16;
    size = (uint32_t)(pages - 1) * 512 + (bytes_last ? bytes_last : 512) - header;
    if (header + size > (uint32_t)exe_size) size = (uint32_t)exe_size - header;

    memcpy(MEM + ((uint32_t)LOAD_SEG << 4), exe + header, size);

    for (i = 0; i < relocations; i++) {
        uint16_t off = U16(table + i * 4);
        uint16_t seg = U16(table + i * 4 + 2);
        uint32_t at = (((uint32_t)(LOAD_SEG + seg)) << 4) + off;
        uint16_t fixed = (uint16_t)((MEM[at] | (MEM[at + 1] << 8)) + LOAD_SEG);
        MEM[at] = (uint8_t)fixed;
        MEM[at + 1] = (uint8_t)(fixed >> 8);
    }

    S[sCS] = (uint16_t)(LOAD_SEG + U16(22));
    IP     = U16(20);
    S[sSS] = (uint16_t)(LOAD_SEG + U16(14));
    R[rSP] = U16(16);
    S[sDS] = S[sES] = PSP_SEG;
#undef U16

    /* A PSP whose first bytes are INT 20h, so a program that returns into it exits. */
    MEM[(uint32_t)PSP_SEG << 4] = 0xCD;
    MEM[((uint32_t)PSP_SEG << 4) + 1] = 0x20;
    /* BIOS data area: equipment word reports a CGA in 80x25. */
    MEM[0x410] = 0x21;
    MEM[0x411] = 0x00;
    return 0;
}

void alleycat_run(int instructions)
{
    int i;
    for (i = 0; i < instructions && !STOPPED; i++) {
        /* Deliver a queued key through the game's own INT 9 handler when it will accept one. */
        if (KEYQ_HEAD != KEYQ_TAIL && getf(fIF)) {
            SCANCODE = KEYQ[KEYQ_HEAD];
            KEYQ_HEAD = (KEYQ_HEAD + 1) % (int)sizeof KEYQ;
            interrupt(9);
        }
        step();
    }
}

void alleycat_update(void) { alleycat_run(TICK_STEPS); }

void alleycat_key(int scancode, int down)
{
    int next = (KEYQ_TAIL + 1) % (int)sizeof KEYQ;
    if (next == KEYQ_HEAD) return;                      /* queue full: drop it */
    KEYQ[KEYQ_TAIL] = (uint8_t)(down ? (scancode & 0x7F) : (scancode | 0x80));
    KEYQ_TAIL = next;
}

const uint8_t *alleycat_framebuffer(void)
{
    int y, xb, pair;
    for (y = 0; y < ALLEYCAT_HEIGHT; y++) {
        /* CGA mode 4 interleave: even scanlines at +0x0000, odd at +0x2000, 80 bytes a row. */
        uint32_t row = ((uint32_t)VIDEO_SEG << 4) + ((y & 1) ? 0x2000u : 0u) + (uint32_t)(y >> 1) * 80u;
        for (xb = 0; xb < 80; xb++) {
            uint8_t byte = MEM[row + xb];
            for (pair = 0; pair < 4; pair++)
                FRAME[y * ALLEYCAT_WIDTH + xb * 4 + pair] = (uint8_t)((byte >> (6 - pair * 2)) & 3);
        }
    }
    return FRAME;
}

void alleycat_palette(uint32_t out[4])
{
    /* CGA palette 1, high intensity: black, light cyan, light magenta, white. Which palette the
     * hardware shows is set through port 0x3D9, which this game leaves at the default. */
    out[0] = 0x000000; out[1] = 0x55FFFF; out[2] = 0xFF55FF; out[3] = 0xFFFFFF;
}

int alleycat_ready(void) { return VIDEO_MODE >= 0; }
uint64_t alleycat_instructions(void) { return ICOUNT; }
const char *alleycat_status(void) { return STATUS; }
int alleycat_fault_opcode(void) { return BAD_OP; }
uint32_t alleycat_fault_address(void) { return ((uint32_t)BAD_CS << 16) | BAD_IP; }

#endif /* ALLEYCAT_IMPLEMENTATION */
