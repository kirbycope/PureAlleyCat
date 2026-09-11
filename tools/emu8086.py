#!/usr/bin/env python3
"""A small 8086 real-mode interpreter, enough to run CAT.EXE.

    python tools/emu8086.py original/CAT.EXE --steps 20000000 --shot run.png

Why this exists: the mechanically translated C in `recomp/` compiles and runs but does nothing,
and the reasons are structural (flags that are never computed, registers that do not write back,
segments that are approximated). Debugging that by inspection is guesswork. A real CPU gives a
reference to compare against: run the original here, run the translation beside it, and the first
place the register file diverges is the bug.

It is also a check on the whole analysis. If the interpreter is faithful, the game sets the video
mode and draws; --shot decodes the CGA framebuffer at B800:0000 and writes it out. A recognisable
picture means the loader, the memory model, the CGA decoding and the CPU all agree with reality.

Speed is not the point. This runs a few hundred thousand instructions a second, which is plenty to
reach a title screen and far easier to instrument than something fast.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Register indices, in the order the opcode encoding uses them.
AX, CX, DX, BX, SP, BP, SI, DI = range(8)
ES, CS, SS, DS = range(4)
REG16 = ("AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI")
SREG = ("ES", "CS", "SS", "DS")

CF, PF, AF, ZF, SF, TF, IF, DF, OF = (
    0x0001, 0x0004, 0x0010, 0x0040, 0x0080, 0x0100, 0x0200, 0x0400, 0x0800)

PARITY = bytes(bin(i).count("1") % 2 == 0 for i in range(256))


class Halt(Exception):
    """Raised to stop the run: hlt, or the program returning to the exit stub."""


class CPU:
    def __init__(self) -> None:
        self.mem = bytearray(1 << 20)
        self.r = [0] * 8
        self.s = [0] * 4
        self.ip = 0
        self.flags = 0x0002
        self.halted = False
        self.instructions = 0
        self.int_log: list[tuple[int, int]] = []
        self.call_log: list[tuple[int, int]] = []
        self.trace_calls = 0
        self.unhandled: dict[int, int] = {}
        self.text = [[' '] * 40 for _ in range(25)]
        self.cursor = [0, 0]
        self.palette_call = None
        # PIT channel 0, latched with `out 0x43, 0` and read a byte at a time from port 0x40.
        self.pit0_latch = 0
        self.pit0_high_next = False
        # The game port. joystick_present sets the BIOS equipment bit the game's adapter check
        # looks for; joystick_x and joystick_y are -1, 0 or 1, which is all the game resolves.
        self.joystick_present = False
        self.joystick_x = 0
        self.joystick_y = 0
        self.joystick_buttons = (False, False)
        self.joystick_fired = 0
        self.joystick_timing = False

    # ---- memory -------------------------------------------------------------------------
    def phys(self, seg: int, off: int) -> int:
        return ((seg << 4) + (off & 0xFFFF)) & 0xFFFFF

    def rd8(self, seg: int, off: int) -> int:
        return self.mem[self.phys(seg, off)]

    def rd16(self, seg: int, off: int) -> int:
        a = self.phys(seg, off)
        b = self.phys(seg, off + 1)
        return self.mem[a] | (self.mem[b] << 8)

    def wr8(self, seg: int, off: int, v: int) -> None:
        self.mem[self.phys(seg, off)] = v & 0xFF

    def wr16(self, seg: int, off: int, v: int) -> None:
        self.mem[self.phys(seg, off)] = v & 0xFF
        self.mem[self.phys(seg, off + 1)] = (v >> 8) & 0xFF

    # ---- 8-bit register halves ------------------------------------------------------------
    def get8(self, i: int) -> int:
        return (self.r[i & 3] >> 8) & 0xFF if i & 4 else self.r[i & 3] & 0xFF

    def set8(self, i: int, v: int) -> None:
        v &= 0xFF
        j = i & 3
        if i & 4:
            self.r[j] = (self.r[j] & 0x00FF) | (v << 8)
        else:
            self.r[j] = (self.r[j] & 0xFF00) | v

    # ---- fetch ------------------------------------------------------------------------------
    def fetch8(self) -> int:
        v = self.rd8(self.s[CS], self.ip)
        self.ip = (self.ip + 1) & 0xFFFF
        return v

    def fetch16(self) -> int:
        v = self.rd16(self.s[CS], self.ip)
        self.ip = (self.ip + 2) & 0xFFFF
        return v

    def fetchs8(self) -> int:
        v = self.fetch8()
        return v - 256 if v & 0x80 else v

    # ---- flags -------------------------------------------------------------------------------
    def setf(self, mask: int, on: bool) -> None:
        self.flags = (self.flags | mask) if on else (self.flags & ~mask)

    def getf(self, mask: int) -> bool:
        return bool(self.flags & mask)

    def logic_flags(self, v: int, w: int) -> None:
        self.setf(CF | OF, False)
        self.szp(v, w)

    def szp(self, v: int, w: int) -> None:
        self.setf(ZF, v == 0)
        self.setf(SF, bool(v & (0x8000 if w else 0x80)))
        self.setf(PF, bool(PARITY[v & 0xFF]))

    def add_flags(self, a: int, b: int, r: int, w: int, carry: int = 0) -> None:
        mask = 0xFFFF if w else 0xFF
        sign = 0x8000 if w else 0x80
        self.setf(CF, (a + b + carry) > mask)
        self.setf(AF, ((a ^ b ^ r) & 0x10) != 0)
        self.setf(OF, bool((~(a ^ b) & (a ^ r)) & sign))
        self.szp(r & mask, w)

    def sub_flags(self, a: int, b: int, r: int, w: int, borrow: int = 0) -> None:
        mask = 0xFFFF if w else 0xFF
        sign = 0x8000 if w else 0x80
        self.setf(CF, (b + borrow) > a)
        self.setf(AF, ((a ^ b ^ r) & 0x10) != 0)
        self.setf(OF, bool(((a ^ b) & (a ^ r)) & sign))
        self.szp(r & mask, w)

    # ---- mod/rm ---------------------------------------------------------------------------------
    def modrm(self, seg_override):
        byte = self.fetch8()
        mod, reg, rm = byte >> 6, (byte >> 3) & 7, byte & 7
        if mod == 3:
            return mod, reg, rm, None, None
        if rm == 0:   base, seg = self.r[BX] + self.r[SI], DS
        elif rm == 1: base, seg = self.r[BX] + self.r[DI], DS
        elif rm == 2: base, seg = self.r[BP] + self.r[SI], SS
        elif rm == 3: base, seg = self.r[BP] + self.r[DI], SS
        elif rm == 4: base, seg = self.r[SI], DS
        elif rm == 5: base, seg = self.r[DI], DS
        elif rm == 6:
            if mod == 0:
                base, seg = self.fetch16(), DS
            else:
                base, seg = self.r[BP], SS
        else:         base, seg = self.r[BX], DS
        if mod == 1:
            base += self.fetchs8()
        elif mod == 2:
            base += self.fetch16()
        if seg_override is not None:
            seg = seg_override
        return mod, reg, rm, self.s[seg], base & 0xFFFF

    def rm_read(self, w, mod, rm, seg, off):
        if mod == 3:
            return self.r[rm] if w else self.get8(rm)
        return self.rd16(seg, off) if w else self.rd8(seg, off)

    def rm_write(self, w, mod, rm, seg, off, v):
        if mod == 3:
            if w:
                self.r[rm] = v & 0xFFFF
            else:
                self.set8(rm, v)
        elif w:
            self.wr16(seg, off, v)
        else:
            self.wr8(seg, off, v)

    # ---- stack ------------------------------------------------------------------------------------
    def push(self, v: int) -> None:
        self.r[SP] = (self.r[SP] - 2) & 0xFFFF
        self.wr16(self.s[SS], self.r[SP], v)

    def pop(self) -> int:
        v = self.rd16(self.s[SS], self.r[SP])
        self.r[SP] = (self.r[SP] + 2) & 0xFFFF
        return v

    # ---- arithmetic dispatch ------------------------------------------------------------------------
    def alu(self, op: int, a: int, b: int, w: int) -> int | None:
        mask = 0xFFFF if w else 0xFF
        if op == 0:   r = (a + b) & mask; self.add_flags(a, b, r, w); return r
        if op == 1:   r = a | b;          self.logic_flags(r, w);     return r
        if op == 2:
            c = 1 if self.getf(CF) else 0
            r = (a + b + c) & mask; self.add_flags(a, b, r, w, c);    return r
        if op == 3:
            c = 1 if self.getf(CF) else 0
            r = (a - b - c) & mask; self.sub_flags(a, b, r, w, c);    return r
        if op == 4:   r = a & b;          self.logic_flags(r, w);     return r
        if op == 5:   r = (a - b) & mask; self.sub_flags(a, b, r, w); return r
        if op == 6:   r = a ^ b;          self.logic_flags(r, w);     return r
        r = (a - b) & mask; self.sub_flags(a, b, r, w); return None   # 7: cmp, no write

    def shift(self, op: int, v: int, count: int, w: int) -> int:
        mask = 0xFFFF if w else 0xFF
        bits = 16 if w else 8
        sign = 1 << (bits - 1)
        count &= 0x1F
        if count == 0:
            return v
        for _ in range(count):
            if op == 0:    # rol
                c = bool(v & sign); v = ((v << 1) | c) & mask
            elif op == 1:  # ror
                c = bool(v & 1); v = ((v >> 1) | (sign if c else 0)) & mask
            elif op == 2:  # rcl
                old = self.getf(CF); c = bool(v & sign)
                v = ((v << 1) | old) & mask
            elif op == 3:  # rcr
                old = self.getf(CF); c = bool(v & 1)
                v = ((v >> 1) | (sign if old else 0)) & mask
            elif op in (4, 6):  # shl / sal
                c = bool(v & sign); v = (v << 1) & mask
            elif op == 5:  # shr
                c = bool(v & 1); v = (v >> 1) & mask
            else:          # 7: sar
                c = bool(v & 1); v = ((v >> 1) | (v & sign)) & mask
            self.setf(CF, c)
        if op in (4, 6, 5, 7):
            self.szp(v, w)
        self.setf(OF, bool((v ^ (v << 1)) & sign) if count == 1 else False)
        return v

    # ---- one instruction ----------------------------------------------------------------------
    def step(self) -> None:
        seg_override = None
        rep = None
        while True:
            op = self.fetch8()
            if op == 0x26: seg_override = ES; continue
            if op == 0x2E: seg_override = CS; continue
            if op == 0x36: seg_override = SS; continue
            if op == 0x3E: seg_override = DS; continue
            if op == 0xF0: continue                       # lock: no effect here
            if op in (0xF2, 0xF3): rep = op; continue
            break
        self.instructions += 1
        self.execute(op, seg_override, rep)

    def data_seg(self, seg_override) -> int:
        return self.s[DS if seg_override is None else seg_override]

    def execute(self, op: int, seg_override, rep) -> None:
        r, s = self.r, self.s

        # 00-3F: the eight ALU ops, each in six addressing forms
        if op < 0x40 and (op & 7) < 6:
            kind = op >> 3
            form = op & 7
            if form in (0, 1, 2, 3):
                w = form & 1
                mod, reg, rm, sg, off = self.modrm(seg_override)
                a = self.rm_read(w, mod, rm, sg, off)
                b = r[reg] if w else self.get8(reg)
                if form & 2:                               # reg <- reg op rm
                    res = self.alu(kind, b, a, w)
                    if res is not None:
                        if w: r[reg] = res
                        else: self.set8(reg, res)
                else:                                      # rm <- rm op reg
                    res = self.alu(kind, a, b, w)
                    if res is not None:
                        self.rm_write(w, mod, rm, sg, off, res)
            else:                                          # acc, imm
                w = form & 1
                imm = self.fetch16() if w else self.fetch8()
                a = r[AX] if w else self.get8(AX)
                res = self.alu(kind, a, imm, w)
                if res is not None:
                    if w: r[AX] = res
                    else: self.set8(AX, res)
            return

        if op in (0x06, 0x0E, 0x16, 0x1E):                 # push sreg
            self.push(s[(op >> 3) & 3]); return
        if op in (0x07, 0x17, 0x1F):                       # pop sreg (0x0F is not pop cs on 8086 use)
            s[(op >> 3) & 3] = self.pop(); return

        if op == 0x27 or op == 0x2F or op == 0x37 or op == 0x3F:
            # daa/das/aaa/aas: only aaa appears in this program, and only once.
            al = self.get8(AX)
            if op == 0x37 or op == 0x3F:                   # aaa / aas
                if (al & 0x0F) > 9 or self.getf(AF):
                    delta = 6 if op == 0x37 else -6
                    self.set8(AX, (al + delta) & 0x0F)
                    self.r[AX] = (self.r[AX] + (0x100 if op == 0x37 else -0x100)) & 0xFFFF
                    self.setf(AF | CF, True)
                else:
                    self.set8(AX, al & 0x0F)
                    self.setf(AF | CF, False)
            return

        if 0x40 <= op <= 0x47:                             # inc r16
            i = op & 7; a = r[i]; res = (a + 1) & 0xFFFF
            c = self.getf(CF); self.add_flags(a, 1, res, 1); self.setf(CF, c)
            r[i] = res; return
        if 0x48 <= op <= 0x4F:                             # dec r16
            i = op & 7; a = r[i]; res = (a - 1) & 0xFFFF
            c = self.getf(CF); self.sub_flags(a, 1, res, 1); self.setf(CF, c)
            r[i] = res; return
        if 0x50 <= op <= 0x57: self.push(r[op & 7]); return
        if 0x58 <= op <= 0x5F: r[op & 7] = self.pop(); return

        if 0x70 <= op <= 0x7F:                             # jcc rel8
            d = self.fetchs8()
            if self.cond(op & 0x0F):
                self.ip = (self.ip + d) & 0xFFFF
            return

        if op in (0x80, 0x81, 0x82, 0x83):                 # group1: rm, imm
            w = op & 1
            mod, reg, rm, sg, off = self.modrm(seg_override)
            a = self.rm_read(w, mod, rm, sg, off)
            if op == 0x81:   imm = self.fetch16()
            elif op == 0x83: imm = self.fetchs8() & 0xFFFF
            else:            imm = self.fetch8()
            res = self.alu(reg, a, imm, w)
            if res is not None:
                self.rm_write(w, mod, rm, sg, off, res)
            return

        if op in (0x84, 0x85):                             # test rm, reg
            w = op & 1
            mod, reg, rm, sg, off = self.modrm(seg_override)
            a = self.rm_read(w, mod, rm, sg, off)
            b = r[reg] if w else self.get8(reg)
            self.logic_flags(a & b, w); return

        if op in (0x86, 0x87):                             # xchg rm, reg
            w = op & 1
            mod, reg, rm, sg, off = self.modrm(seg_override)
            a = self.rm_read(w, mod, rm, sg, off)
            b = r[reg] if w else self.get8(reg)
            self.rm_write(w, mod, rm, sg, off, b)
            if w: r[reg] = a
            else: self.set8(reg, a)
            return

        if 0x88 <= op <= 0x8B:                             # mov
            w = op & 1
            mod, reg, rm, sg, off = self.modrm(seg_override)
            if op & 2:
                v = self.rm_read(w, mod, rm, sg, off)
                if w: r[reg] = v
                else: self.set8(reg, v)
            else:
                self.rm_write(w, mod, rm, sg, off, r[reg] if w else self.get8(reg))
            return

        if op == 0x8C:                                     # mov rm, sreg
            mod, reg, rm, sg, off = self.modrm(seg_override)
            self.rm_write(1, mod, rm, sg, off, s[reg & 3]); return
        if op == 0x8E:                                     # mov sreg, rm
            mod, reg, rm, sg, off = self.modrm(seg_override)
            s[reg & 3] = self.rm_read(1, mod, rm, sg, off); return
        if op == 0x8D:                                     # lea
            mod, reg, rm, sg, off = self.modrm(seg_override)
            r[reg] = off & 0xFFFF; return
        if op == 0x8F:                                     # pop rm
            mod, reg, rm, sg, off = self.modrm(seg_override)
            self.rm_write(1, mod, rm, sg, off, self.pop()); return

        if op == 0x90: return                              # nop
        if 0x91 <= op <= 0x97:                             # xchg ax, r16
            i = op & 7; r[AX], r[i] = r[i], r[AX]; return
        if op == 0x98:                                     # cbw
            v = self.get8(AX); r[AX] = (v | 0xFF00) if v & 0x80 else v; return
        if op == 0x99:                                     # cwd
            r[DX] = 0xFFFF if r[AX] & 0x8000 else 0; return
        if op == 0x9A:                                     # call far
            off = self.fetch16(); seg = self.fetch16()
            self.push(s[CS]); self.push(self.ip)
            s[CS], self.ip = seg, off; return
        if op == 0x9C: self.push(self.flags); return       # pushf
        if op == 0x9D: self.flags = self.pop() | 0x0002; return
        if op == 0x9E:                                     # sahf
            self.flags = (self.flags & 0xFF00) | (self.get8(4) & 0xD5) | 0x02; return
        if op == 0x9F: self.set8(4, self.flags & 0xFF); return  # lahf

        if 0xA0 <= op <= 0xA3:                             # mov acc <-> [imm16]
            w = op & 1
            off = self.fetch16()
            sg = self.data_seg(seg_override)
            if op & 2:
                if w: self.wr16(sg, off, r[AX])
                else: self.wr8(sg, off, self.get8(AX))
            else:
                if w: r[AX] = self.rd16(sg, off)
                else: self.set8(AX, self.rd8(sg, off))
            return

        if 0xA4 <= op <= 0xA7 or 0xAA <= op <= 0xAF:       # string ops
            self.string_op(op, seg_override, rep); return

        if op in (0xA8, 0xA9):                             # test acc, imm
            w = op & 1
            imm = self.fetch16() if w else self.fetch8()
            a = r[AX] if w else self.get8(AX)
            self.logic_flags(a & imm, w); return

        if 0xB0 <= op <= 0xB7: self.set8(op & 7, self.fetch8()); return
        if 0xB8 <= op <= 0xBF: r[op & 7] = self.fetch16(); return

        if op in (0xC0, 0xC1, 0xD0, 0xD1, 0xD2, 0xD3):     # shift group
            w = op & 1
            mod, reg, rm, sg, off = self.modrm(seg_override)
            if op in (0xC0, 0xC1):   count = self.fetch8()
            elif op in (0xD0, 0xD1): count = 1
            else:                    count = self.get8(CX)
            v = self.rm_read(w, mod, rm, sg, off)
            self.rm_write(w, mod, rm, sg, off, self.shift(reg, v, count, w)); return

        if op in (0xC2, 0xC3):                             # ret near
            n = self.fetch16() if op == 0xC2 else 0
            self.ip = self.pop()
            self.r[SP] = (self.r[SP] + n) & 0xFFFF; return
        if op in (0xCA, 0xCB):                             # ret far
            n = self.fetch16() if op == 0xCA else 0
            self.ip = self.pop(); s[CS] = self.pop()
            self.r[SP] = (self.r[SP] + n) & 0xFFFF; return

        if op in (0xC4, 0xC5):                             # les / lds
            mod, reg, rm, sg, off = self.modrm(seg_override)
            r[reg] = self.rd16(sg, off)
            s[ES if op == 0xC4 else DS] = self.rd16(sg, off + 2); return

        if op in (0xC6, 0xC7):                             # mov rm, imm
            w = op & 1
            mod, reg, rm, sg, off = self.modrm(seg_override)
            imm = self.fetch16() if w else self.fetch8()
            self.rm_write(w, mod, rm, sg, off, imm); return

        if op == 0xCC: self.interrupt(3); return
        if op == 0xCD: self.interrupt(self.fetch8()); return
        if op == 0xCE:
            if self.getf(OF): self.interrupt(4)
            return
        if op == 0xCF:                                     # iret
            self.ip = self.pop(); s[CS] = self.pop(); self.flags = self.pop() | 0x0002; return

        if op == 0xD4: self.fetch8(); return               # aam, unused here
        if op == 0xD5: self.fetch8(); return               # aad, unused here
        if op == 0xD7:                                     # xlat
            self.set8(AX, self.rd8(self.data_seg(seg_override),
                                   (r[BX] + self.get8(AX)) & 0xFFFF)); return

        if 0xE0 <= op <= 0xE2:                             # loopne / loope / loop
            d = self.fetchs8()
            r[CX] = (r[CX] - 1) & 0xFFFF
            z = self.getf(ZF)
            go = r[CX] != 0 and (op == 0xE2 or (op == 0xE1 and z) or (op == 0xE0 and not z))
            if go: self.ip = (self.ip + d) & 0xFFFF
            return
        if op == 0xE3:                                     # jcxz
            d = self.fetchs8()
            if r[CX] == 0: self.ip = (self.ip + d) & 0xFFFF
            return

        if op in (0xE4, 0xE5, 0xEC, 0xED):                 # in
            w = op & 1
            port = self.fetch8() if op < 0xEC else r[DX]
            v = self.port_in(port)
            if w: r[AX] = v | (self.port_in(port + 1) << 8)
            else: self.set8(AX, v)
            return
        if op in (0xE6, 0xE7, 0xEE, 0xEF):                 # out
            w = op & 1
            port = self.fetch8() if op < 0xEE else r[DX]
            self.port_out(port, self.get8(AX))
            if w: self.port_out(port + 1, (r[AX] >> 8) & 0xFF)
            return

        if op == 0xE8:                                     # call near
            d = self.fetch16()
            self.push(self.ip)
            if self.trace_calls and len(self.call_log) < self.trace_calls:
                self.call_log.append((self.s[CS], (self.ip + (d - 0x10000 if d & 0x8000 else d)) & 0xFFFF))
            self.ip = (self.ip + (d - 0x10000 if d & 0x8000 else d)) & 0xFFFF; return
        if op == 0xE9:                                     # jmp near
            d = self.fetch16()
            self.ip = (self.ip + (d - 0x10000 if d & 0x8000 else d)) & 0xFFFF; return
        if op == 0xEA:                                     # jmp far
            off = self.fetch16(); seg = self.fetch16()
            s[CS], self.ip = seg, off; return
        if op == 0xEB:                                     # jmp short
            d = self.fetchs8(); self.ip = (self.ip + d) & 0xFFFF; return

        if op == 0xF4: raise Halt("hlt")
        if op == 0xF5: self.setf(CF, not self.getf(CF)); return
        if op == 0xF8: self.setf(CF, False); return
        if op == 0xF9: self.setf(CF, True); return
        if op == 0xFA: self.setf(IF, False); return
        if op == 0xFB: self.setf(IF, True); return
        if op == 0xFC: self.setf(DF, False); return
        if op == 0xFD: self.setf(DF, True); return

        if op in (0xF6, 0xF7):                             # group3
            w = op & 1
            mod, reg, rm, sg, off = self.modrm(seg_override)
            a = self.rm_read(w, mod, rm, sg, off)
            mask = 0xFFFF if w else 0xFF
            if reg in (0, 1):                              # test imm
                imm = self.fetch16() if w else self.fetch8()
                self.logic_flags(a & imm, w)
            elif reg == 2:                                 # not
                self.rm_write(w, mod, rm, sg, off, ~a & mask)
            elif reg == 3:                                 # neg
                res = (-a) & mask
                self.sub_flags(0, a, res, w); self.setf(CF, a != 0)
                self.rm_write(w, mod, rm, sg, off, res)
            elif reg == 4:                                 # mul
                if w:
                    p = r[AX] * a; r[AX] = p & 0xFFFF; r[DX] = (p >> 16) & 0xFFFF
                    hi = r[DX]
                else:
                    p = self.get8(AX) * a; r[AX] = p & 0xFFFF; hi = (p >> 8) & 0xFF
                self.setf(CF | OF, hi != 0)
            elif reg == 5:                                 # imul
                sa = a - (mask + 1) if a & (0x8000 if w else 0x80) else a
                if w:
                    b = r[AX]; sb = b - 0x10000 if b & 0x8000 else b
                    p = sa * sb; r[AX] = p & 0xFFFF; r[DX] = (p >> 16) & 0xFFFF
                    ok = p == ((p & 0xFFFF) - 0x10000 if p & 0x8000 else p & 0xFFFF)
                else:
                    b = self.get8(AX); sb = b - 0x100 if b & 0x80 else b
                    p = sa * sb; r[AX] = p & 0xFFFF
                    ok = -128 <= p <= 127
                self.setf(CF | OF, not ok)
            elif reg == 6:                                 # div
                if a == 0: self.interrupt(0); return
                if w:
                    n = (r[DX] << 16) | r[AX]
                    q, rem = divmod(n, a)
                    if q > 0xFFFF: self.interrupt(0); return
                    r[AX], r[DX] = q, rem
                else:
                    n = r[AX]
                    q, rem = divmod(n, a)
                    if q > 0xFF: self.interrupt(0); return
                    self.set8(AX, q); self.set8(4, rem)
            else:                                          # idiv
                if a == 0: self.interrupt(0); return
                sa = a - (mask + 1) if a & (0x8000 if w else 0x80) else a
                if w:
                    n = (r[DX] << 16) | r[AX]
                    if n & 0x80000000: n -= 1 << 32
                    q = int(n / sa); rem = n - q * sa
                    r[AX], r[DX] = q & 0xFFFF, rem & 0xFFFF
                else:
                    n = r[AX]
                    if n & 0x8000: n -= 1 << 16
                    q = int(n / sa); rem = n - q * sa
                    self.set8(AX, q & 0xFF); self.set8(4, rem & 0xFF)
            return

        if op in (0xFE, 0xFF):                             # group4/5
            w = op & 1
            mod, reg, rm, sg, off = self.modrm(seg_override)
            a = self.rm_read(w, mod, rm, sg, off)
            if reg == 0:
                res = (a + 1) & (0xFFFF if w else 0xFF)
                c = self.getf(CF); self.add_flags(a, 1, res, w); self.setf(CF, c)
                self.rm_write(w, mod, rm, sg, off, res)
            elif reg == 1:
                res = (a - 1) & (0xFFFF if w else 0xFF)
                c = self.getf(CF); self.sub_flags(a, 1, res, w); self.setf(CF, c)
                self.rm_write(w, mod, rm, sg, off, res)
            elif reg == 2:                                 # call near rm
                self.push(self.ip); self.ip = a
            elif reg == 3:                                 # call far [rm]
                self.push(s[CS]); self.push(self.ip)
                self.ip = self.rd16(sg, off); s[CS] = self.rd16(sg, off + 2)
            elif reg == 4:                                 # jmp near rm
                self.ip = a
            elif reg == 5:                                 # jmp far [rm]
                self.ip = self.rd16(sg, off); s[CS] = self.rd16(sg, off + 2)
            elif reg == 6:
                self.push(a)
            return

        self.unhandled[op] = self.unhandled.get(op, 0) + 1
        raise Halt(f"unimplemented opcode {op:02X} at {s[CS]:04X}:{(self.ip - 1) & 0xFFFF:04X}")

    def cond(self, code: int) -> bool:
        f = self.flags
        o, c, z, sg, p = bool(f & OF), bool(f & CF), bool(f & ZF), bool(f & SF), bool(f & PF)
        return (o, not o, c, not c, z, not z, c or z, not (c or z),
                sg, not sg, p, not p, sg != o, sg == o,
                z or (sg != o), not z and (sg == o))[code]

    # ---- string operations ---------------------------------------------------------------------
    def string_op(self, op: int, seg_override, rep) -> None:
        w = op & 1
        step = (2 if w else 1) * (-1 if self.getf(DF) else 1)
        src_seg = self.data_seg(seg_override)
        count = self.r[CX] if rep else 1
        if rep and count == 0:
            return
        while True:
            if op in (0xA4, 0xA5):     # movs
                v = self.rd16(src_seg, self.r[SI]) if w else self.rd8(src_seg, self.r[SI])
                if w: self.wr16(self.s[ES], self.r[DI], v)
                else: self.wr8(self.s[ES], self.r[DI], v)
                self.r[SI] = (self.r[SI] + step) & 0xFFFF
                self.r[DI] = (self.r[DI] + step) & 0xFFFF
            elif op in (0xA6, 0xA7):   # cmps
                a = self.rd16(src_seg, self.r[SI]) if w else self.rd8(src_seg, self.r[SI])
                b = self.rd16(self.s[ES], self.r[DI]) if w else self.rd8(self.s[ES], self.r[DI])
                self.sub_flags(a, b, (a - b) & (0xFFFF if w else 0xFF), w)
                self.r[SI] = (self.r[SI] + step) & 0xFFFF
                self.r[DI] = (self.r[DI] + step) & 0xFFFF
            elif op in (0xAA, 0xAB):   # stos
                if w: self.wr16(self.s[ES], self.r[DI], self.r[AX])
                else: self.wr8(self.s[ES], self.r[DI], self.get8(AX))
                self.r[DI] = (self.r[DI] + step) & 0xFFFF
            elif op in (0xAC, 0xAD):   # lods
                if w: self.r[AX] = self.rd16(src_seg, self.r[SI])
                else: self.set8(AX, self.rd8(src_seg, self.r[SI]))
                self.r[SI] = (self.r[SI] + step) & 0xFFFF
            else:                      # AE/AF scas
                a = self.r[AX] if w else self.get8(AX)
                b = self.rd16(self.s[ES], self.r[DI]) if w else self.rd8(self.s[ES], self.r[DI])
                self.sub_flags(a, b, (a - b) & (0xFFFF if w else 0xFF), w)
                self.r[DI] = (self.r[DI] + step) & 0xFFFF
            if not rep:
                return
            self.r[CX] = (self.r[CX] - 1) & 0xFFFF
            if self.r[CX] == 0:
                return
            if op in (0xA6, 0xA7, 0xAE, 0xAF):
                # repe (F3) continues while ZF, repne (F2) while not ZF
                if (rep == 0xF3) != self.getf(ZF):
                    return

    # ---- the platform ---------------------------------------------------------------------------
    def interrupt(self, n: int) -> None:
        self.int_log.append((n, self.get8(4)))
        if self.bios(n):
            return
        # Anything the host does not answer goes through the vector table, which is how the
        # game's own INT 9 handler gets called.
        vector_off = self.rd16(0, n * 4)
        vector_seg = self.rd16(0, n * 4 + 2)
        if vector_seg == 0 and vector_off == 0:
            return
        self.push(self.flags)
        self.push(self.s[CS])
        self.push(self.ip)
        self.setf(IF, False)
        self.s[CS], self.ip = vector_seg, vector_off

    def bios(self, n: int) -> bool:
        ah = self.get8(4)
        if n == 0x1A and ah == 0x00:
            # 18.2 Hz tick. Driven by instructions retired rather than the wall clock, so a run
            # is deterministic and two runs can be compared.
            ticks = self.instructions // 6000
            self.r[CX] = (ticks >> 16) & 0xFFFF
            self.r[DX] = ticks & 0xFFFF
            self.set8(AX, 0)
            return True
        if n == 0x10:
            # The game writes its setup prompts through BIOS teletype, so these have to do
            # something or the questions are invisible and it looks like a hang.
            if ah == 0x00:
                self.video_mode = self.get8(AX)
                self.text = [[' '] * 40 for _ in range(25)]
                self.cursor = [0, 0]
                return True
            if ah == 0x02:                      # set cursor: DH row, DL column
                self.cursor = [self.get8(6), self.get8(2)]
                return True
            if ah == 0x0B:                      # set palette / border
                self.palette_call = (self.get8(7), self.get8(3))
                return True
            if ah == 0x0E:                      # teletype: AL is the character
                ch = self.get8(AX)
                row, col = self.cursor
                if ch == 13:
                    col = 0
                elif ch == 10:
                    row += 1
                elif ch == 8:
                    col = max(0, col - 1)
                else:
                    if row < 25 and col < 40:
                        self.text[row][col] = chr(ch) if 32 <= ch < 127 else '?'
                    col += 1
                if col >= 40:
                    col = 0
                    row += 1
                if row >= 25:                   # scroll
                    self.text.pop(0)
                    self.text.append([' '] * 40)
                    row = 24
                self.cursor = [row, col]
                return True
            return True
        if n == 0x11:
            # CGA 80x25; bit 12 is the game adapter, which the joystick check at 0xD215 wants.
            self.r[AX] = 0x0021 | (0x1000 if self.joystick_present else 0)
            return True
        if n == 0x12:
            self.r[AX] = 640
            return True
        if n == 0x20:
            raise Halt("int 20h: program exit")
        if n == 0x21 and ah == 0x4C:
            raise Halt("int 21h/4C: program exit")
        return False

    def port_in(self, port: int) -> int:
        if port == 0x3DA:
            # CGA status: cycle retrace and display-enable so a poll loop makes progress.
            self.retrace = getattr(self, "retrace", 0) + 1
            return 0x09 if self.retrace & 1 else 0x00
        if port == 0x60:
            return getattr(self, "scancode", 0)
        if port == 0x61:
            return getattr(self, "port61", 0)
        if port == 0x40:
            value = (self.pit0_latch >> 8) if self.pit0_high_next else (self.pit0_latch & 0xFF)
            self.pit0_high_next = not self.pit0_high_next
            if not self.pit0_high_next:
                self.pit0_latch = self.pit0_now()
            return value
        if port == 0x201:
            byte = 0xF0                          # axis pairs low, every button up
            if self.joystick_buttons[0]:
                byte &= ~0x10
            if self.joystick_buttons[1]:
                byte &= ~0x20
            if self.joystick_timing:
                counts = (self.instructions - self.joystick_fired) * 65536 // 6000
                if counts < self.joy_charge(self.joystick_x):
                    byte |= 0x01
                if counts < self.joy_charge(self.joystick_y):
                    byte |= 0x02
                if not byte & 0x03:
                    self.joystick_timing = False
            return byte
        return 0xFF

    def pit0_now(self) -> int:
        """Channel 0 counts down at 1.193182 MHz, wrapping through 65536 every BIOS tick."""
        return (-(self.instructions * 65536 // 6000)) & 0xFFFF

    @staticmethod
    def joy_charge(axis: int) -> int:
        """PIT counts an axis holds its one-shot high. The game buckets at 1286 and 2586."""
        return 600 if axis < 0 else (3200 if axis > 0 else 1900)

    def port_out(self, port: int, value: int) -> None:
        if port == 0x61:
            self.port61 = value
        elif port == 0x43 and value >> 6 == 0:
            self.pit0_latch = self.pit0_now()
            self.pit0_high_next = False
        elif port == 0x201:
            # Any write fires the axis one-shots; the hardware ignores the value.
            self.joystick_fired = self.instructions
            self.joystick_timing = True


LOAD_SEGMENT = 0x1000   # the same base Ghidra used, so addresses match the decompilation
PSP_SEGMENT = LOAD_SEGMENT - 0x10


def load(cpu: CPU, path: Path) -> int:
    exe = path.read_bytes()
    if exe[:2] not in (b"MZ", b"ZM"):
        sys.exit(f"{path}: not an MZ executable")
    u16 = lambda o: exe[o] | (exe[o + 1] << 8)
    pages, relocations, header_pars = u16(4), u16(6), u16(8)
    bytes_last = u16(2) or 512
    header = header_pars * 16
    size = (pages - 1) * 512 + bytes_last - header

    base = LOAD_SEGMENT << 4
    cpu.mem[base:base + size] = exe[header:header + size]

    table = u16(24)
    for i in range(relocations):
        off, seg = u16(table + i * 4), u16(table + i * 4 + 2)
        at = ((LOAD_SEGMENT + seg) << 4) + off
        fixed = (cpu.mem[at] | (cpu.mem[at + 1] << 8)) + LOAD_SEGMENT
        cpu.mem[at] = fixed & 0xFF
        cpu.mem[at + 1] = (fixed >> 8) & 0xFF

    cpu.s[CS] = (LOAD_SEGMENT + u16(22)) & 0xFFFF
    cpu.ip = u16(20)
    cpu.s[SS] = (LOAD_SEGMENT + u16(14)) & 0xFFFF
    cpu.r[SP] = u16(16)
    cpu.s[DS] = cpu.s[ES] = PSP_SEGMENT

    # A PSP with INT 20h at offset 0, so a program that returns to it exits cleanly.
    psp = PSP_SEGMENT << 4
    cpu.mem[psp] = 0xCD
    cpu.mem[psp + 1] = 0x20
    # The BIOS data area: equipment word says CGA 80x25, and the tick counter lives at 0040:006C.
    cpu.mem[0x410] = 0x21
    cpu.mem[0x411] = 0x10 if cpu.joystick_present else 0x00
    return size


def screenshot(cpu: CPU, path: Path, palette: str = "1h") -> bool:
    """Decode the CGA framebuffer at B800:0000 into a PNG. 320x200, 2bpp, two interleaved banks."""
    try:
        from PIL import Image
    except ImportError:
        print("Pillow is not installed; skipping the screenshot")
        return False
    # CGA mode 4 palettes, inlined so this file stands alone in this repository.
    PALETTES = {
        "0l": [(0, 0, 0), (0, 170, 0), (170, 0, 0), (170, 85, 0)],
        "0h": [(0, 0, 0), (85, 255, 85), (255, 85, 85), (255, 255, 85)],
        "1l": [(0, 0, 0), (0, 170, 170), (170, 0, 170), (170, 170, 170)],
        "1h": [(0, 0, 0), (85, 255, 255), (255, 85, 255), (255, 255, 255)],
    }
    colours = PALETTES[palette]
    base = 0xB8000
    image = Image.new("RGB", (320, 200))
    pixels = image.load()
    for y in range(200):
        # even scanlines live at +0x0000 and odd at +0x2000, 80 bytes to a row
        row = base + (0x2000 if y & 1 else 0) + (y >> 1) * 80
        for xb in range(80):
            byte = cpu.mem[row + xb]
            for pair in range(4):
                pixels[xb * 4 + pair, y] = colours[(byte >> (6 - pair * 2)) & 3]
    image = image.resize((640, 400), Image.NEAREST)
    image.save(path)
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("exe", type=Path)
    parser.add_argument("--steps", type=int, default=5_000_000)
    parser.add_argument("--shot", type=Path, help="write the CGA framebuffer here when the run ends")
    parser.add_argument("--trace-calls", type=int, default=0,
                        help="print the first N call targets, for lining up against the C")
    args = parser.parse_args()

    cpu = CPU()
    cpu.video_mode = None
    cpu.trace_calls = args.trace_calls
    size = load(cpu, args.exe)
    print(f"loaded {size:,} bytes, entry {cpu.s[CS]:04X}:{cpu.ip:04X}")

    reason = "step limit"
    try:
        for _ in range(args.steps):
            cpu.step()
    except Halt as stop:
        reason = str(stop)
    except Exception as error:                      # noqa: BLE001 - report and keep the state
        reason = f"{type(error).__name__}: {error}"

    print(f"stopped: {reason}")
    print(f"instructions: {cpu.instructions:,}")
    print(f"video mode set: {cpu.video_mode if cpu.video_mode is not None else 'never'}")
    seen = {}
    for number, ah in cpu.int_log:
        seen[number] = seen.get(number, 0) + 1
    if seen:
        print("interrupts: " + ", ".join(f"{n:02X}h x{c}" for n, c in sorted(seen.items())))
    if cpu.unhandled:
        print("unimplemented opcodes: "
              + ", ".join(f"{o:02X} x{c}" for o, c in sorted(cpu.unhandled.items())))

    painted = sum(1 for b in cpu.mem[0xB8000:0xB8000 + 16384] if b)
    print(f"framebuffer: {painted:,} of 16,384 bytes non-zero")
    if cpu.call_log:
        out = Path("call_trace.txt")
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text("".join(f"{s:04X}:{o:04X}\n" for s, o in cpu.call_log), encoding="utf-8")
        print(f"call trace ({len(cpu.call_log):,}) -> {out}")
    if args.shot and screenshot(cpu, args.shot):
        print(f"screenshot -> {args.shot}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
