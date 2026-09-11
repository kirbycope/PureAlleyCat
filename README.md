# purealleycat

Alley Cat (1984) as an embeddable C library, shaped after
[PureDOOM](https://github.com/Daivuk/PureDOOM): the library is the engine, and you bring the
original data. Here the data file is the game's own `CAT.EXE`, which holds the code and all 29 KB
of its artwork, so nothing copyrighted ships here.

```c
#define ALLEYCAT_IMPLEMENTATION   /* in exactly one translation unit */
#include "PureAlleyCat.h"

alleycat_init(exe_bytes, exe_size);
while (running) {
    alleycat_key(ALLEYCAT_KEY_LEFT, held);
    alleycat_update();                       /* one 18.2 Hz tick */
    draw(alleycat_framebuffer());            /* 320x200, one byte per pixel, values 0-3 */
}
```

**One header, nothing else.** `PureAlleyCat.h` is the whole library, stb-style: define
`ALLEYCAT_IMPLEMENTATION` in exactly one translation unit and that unit gets the interpreter,
exactly as PureDOOM's `DOOM_IMPLEMENTATION` works. Every other unit includes it for the
declarations alone.

No allocation, no file I/O, no dependency beyond `<stdint.h>` and `<string.h>`. One megabyte of
emulated memory lives in BSS, so it is a large static object rather than a heap user.

```bash
zig cc demo.c -o alleycat_demo -O2 -std=c99
./alleycat_demo /path/to/CAT.EXE 2000000 screen.ppm
```

Vendoring it into a host works the same way PureDOOM does in `godot-doom-gdextension`: drop the
header into `thirdparty/`, and add a one-line unit that defines `ALLEYCAT_IMPLEMENTATION` and
includes it.

Builds clean at `-O2 -std=c99` with no warnings.

## What it is, honestly

An 8086 real-mode interpreter plus the handful of BIOS calls and ports this one game touches. It
is not a source port: Alley Cat's source was never released, and a decompilation of hand-written
assembly loses the register-passing conventions the whole program is built on.

What that gives up is readable game logic. What it buys is that **it runs the real thing, exactly**,
and has the same integration shape as PureDOOM, so it drops into a host the same way.

It is also the harness for doing better. With a working reference you can replace one emulated
routine at a time with native C and compare frames, which is the only sane order to attempt that in.

## The platform surface is tiny

Everything the game asks of the machine:

| Surface | What for |
| --- | --- |
| `int 0x1a` | BIOS tick counter, the game clock |
| `int 0x10` | set the video mode |
| `int 0x11` | equipment list, for the game-port check |
| port `0x61`, `0x42`, `0x43` | PC speaker gate and PIT channel 2, the tone |
| port `0x40`, `0x43` | PIT channel 0, the stopwatch it times the joystick with |
| port `0x201` | the game port |
| port `0x3da` | CGA status, polled for vertical retrace |
| port `0x60` + `int 9` | the keyboard, through a handler the game installs itself |

There is no `int 0x21` anywhere, so there is no DOS file I/O to emulate.

## Verification

A Python interpreter (`tools/emu8086.py`) was written first and validated by running the real
`CAT.EXE` through to its attract screen and decoding the framebuffer out of emulated memory. This
library is a port of that, and the two are compared pixel-by-pixel at two million instructions:

```
PureAlleyCat vs Python reference: 0 differing pixels of 64000
IDENTICAL
```

Two independent implementations agreeing bit-for-bit covers the MZ loader and its nine
relocations, flag semantics, mod/rm decoding, the string operations the sprite blitter is built
from, the interleaved CGA banks, 2bpp unpacking and the palette.

The clock is driven by instructions retired rather than wall time, so runs are deterministic and
two of the same length can be diffed. That is what makes the comparison meaningful.

```bash
python tools/emu8086.py /path/to/CAT.EXE --steps 2000000 --shot reference.png
```

## Graphics format

CGA mode 4: 320x200, two bits per pixel, four pixels to a byte, 80 bytes a scanline. The
framebuffer is interleaved into two banks, even scanlines from offset `0x0000` and odd from
`0x2000`.

Sprites are drawn by a routine that takes its geometry in `CX`: `CL` is the width **in words** and
`CH` the height in rows, so every sprite is a multiple of 8 pixels wide. Transparency is not
stored anywhere; the blitter derives a mask at draw time from the pixel values, with colour index
0 acting as the key.

`alleycat_framebuffer()` hands back the decoded 320x200 bytes and `alleycat_palette()` the four
colours, so a host never needs to know any of that.

## API

| Function | |
| --- | --- |
| `alleycat_init(exe, size)` | load `CAT.EXE` and reset; 0 on success |
| `alleycat_update()` | run one tick |
| `alleycat_run(n)` | run exactly n instructions |
| `alleycat_framebuffer()` | 320x200 bytes, each 0-3 |
| `alleycat_palette(out[4])` | the four colours as `0xRRGGBB` |
| `alleycat_key(scancode, down)` | queue a key |
| `alleycat_joystick_present(on)` | report a game adapter in the BIOS equipment list |
| `alleycat_joystick(x, y, b1, b2)` | where the stick is, each axis -1, 0 or 1 |
| `alleycat_ready()` | non-zero once a graphics mode is set |
| `alleycat_text_row(n)`, `alleycat_text_rows()` | the BIOS text screen the setup questions print to |
| `alleycat_screen_painted()` | non-zero framebuffer bytes, so a host knows which screen is up |
| `alleycat_audio_read(out, n)`, `alleycat_audio_rate()` | drain generated PC speaker samples |
| `alleycat_speaker_on()`, `alleycat_speaker_hz()` | what the speaker is being asked to do |
| `alleycat_instructions()`, `alleycat_status()` | diagnostics |
| `alleycat_fault_opcode()`, `alleycat_fault_address()` | what stopped a run, if anything |

Keys go through the game's own INT 9 handler, because it installs one and reads port 0x60 directly
rather than calling INT 16h. `alleycat_key` queues a set-1 make or break code and raises interrupt
9 once interrupts are enabled, which is what the hardware did.

## The game port

Answering yes to "Do you want to use a joystick (Y/N)?" puts the game down a path with three parts
to it, and all three have to work or the answer does nothing.

It reads the BIOS equipment list through INT 11h and gives up unless bit 12, the game adapter, is
set: that is what `alleycat_joystick_present` turns on. It then times the port itself. Writing to
`0x201` fires a one-shot per axis, bits 0-3 read high while each is charging, and the game measures
how long that takes against PIT channel 0, which it latches with `out 0x43, 0` and reads a byte at
a time from port `0x40`. It buckets the result at 1286 and 2586 counts, so each axis resolves to
three positions and no more; `alleycat_joystick` takes -1, 0 or 1 and the library picks a charge
time inside the matching bucket. Buttons are active low in bits 4 and 5.

That is why channel 0 has to be a real down-counter at 1.193182 MHz rather than any old changing
number: the game's stopwatch is the counter, and a value that moves the wrong way or at the wrong
rate puts every reading in the wrong bucket.

The stick is a level rather than an event, because the game samples the port whenever it likes, so
a host sets it every frame and the library reports the current position:

```c
alleycat_joystick_present(1);      /* before alleycat_init, which writes the equipment word */
alleycat_init(exe, size);
while (running) {
    alleycat_joystick(pad_x, pad_y, pad_button_a, pad_button_b);
    alleycat_update();
}
```

## Known gaps

- **The palette is fixed** to CGA palette 1, high intensity. A program can change it through port
  `0x3d9`; this one does not appear to, but the write would not be honoured if it did.
- **Only joystick A exists.** Bits 2 and 3 of port `0x201`, the second stick's axes, always read
  low. The game never looks at them.

## Provenance and licence

The library is MIT, and it is original work: an interpreter and a host shim, written here.

It contains no part of Alley Cat. The game was written by Bill Williams and published by Synapse
Software / Atari, and is still under copyright; you supply your own copy of `CAT.EXE` and the
`.gitignore` here is set up to keep one from being committed by accident. The reverse engineering
that established the CGA format and the platform surface lives separately, in `alley-decomp`,
because that work is derived from the copyrighted binary and this is not.
