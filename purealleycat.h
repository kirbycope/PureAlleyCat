/* purealleycat - Alley Cat (1984) as an embeddable C library.
 *
 * Shaped after PureDOOM: the library is the engine, and you supply the original data. Here the
 * data file is the game's own CAT.EXE, which holds both the code and all 29 KB of artwork, so
 * nothing copyrighted ships with the library.
 *
 *     #include "purecat.h"
 *
 *     purecat_init(exe_bytes, exe_size);
 *     while (running) {
 *         purecat_key(PURECAT_KEY_LEFT, held);
 *         purecat_update();                        // one frame
 *         draw(purecat_framebuffer());             // 320x200, one byte per pixel, values 0-3
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
#ifndef PURECAT_H
#define PURECAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURECAT_WIDTH  320
#define PURECAT_HEIGHT 200

/* Scancodes the game reads. It installs its own INT 9 handler and reads port 0x60 directly, so
 * these are IBM PC set-1 make codes; purecat_key feeds them through the same path. */
enum {
    PURECAT_KEY_UP    = 0x48,
    PURECAT_KEY_DOWN  = 0x50,
    PURECAT_KEY_LEFT  = 0x4B,
    PURECAT_KEY_RIGHT = 0x4D,
    PURECAT_KEY_ALT   = 0x38,
    PURECAT_KEY_ESC   = 0x01,
    PURECAT_KEY_CTRL  = 0x1D,
    PURECAT_KEY_S     = 0x1F,
    PURECAT_KEY_M     = 0x32,
    PURECAT_KEY_Y     = 0x15,
    PURECAT_KEY_N     = 0x31,
};

/* Loads CAT.EXE and resets the machine. Returns 0 on success, non-zero if it is not an MZ image.
 * The bytes are copied in, so the caller may free them afterwards. */
int purecat_init(const void *exe, int exe_size);

/* Runs approximately one 18.2 Hz tick worth of instructions. */
void purecat_update(void);

/* Runs an explicit number of instructions; purecat_update is a wrapper on this. */
void purecat_run(int instructions);

/* 320x200, one byte per pixel, each 0-3 indexing the CGA palette. Decoded from the emulated
 * framebuffer, so it is valid after any purecat_update. */
const uint8_t *purecat_framebuffer(void);

/* The four RGB triples for the palette the game selected, as 0xRRGGBB. */
void purecat_palette(uint32_t out[4]);

/* Press or release a key. `down` is non-zero for a press. */
void purecat_key(int scancode, int down);

/* Non-zero once the program has set a graphics mode, i.e. it is past its startup checks. */
int purecat_ready(void);

/* Total instructions retired, and the last stop reason, for diagnostics. */
uint64_t purecat_instructions(void);
const char *purecat_status(void);

/* -1 if none; otherwise the opcode that stopped the run, and its CS:IP packed as 0xSSSSIIII. */
int purecat_fault_opcode(void);
uint32_t purecat_fault_address(void);

#ifdef __cplusplus
}
#endif

#endif /* PURECAT_H */
