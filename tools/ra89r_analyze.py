#!/usr/bin/env python3
"""
ra89r_analyze.py -- static analysis of a decoded RA89R flash image.

    python3 tools/ra89r_analyze.py work/FIRMWARE_RA89R_20260203_V49.bin

Reads the image plus its .meta sidecar written by ra89r.py, then

  * scans strings (ASCII, UTF-16LE, CJK),
  * finds function entry points from the vector table and from every BL/BLX
    target in a 2-byte-granularity sweep,
  * recursively disassembles each function with capstone, resolving
    PC-relative literal pools (so pool words are not mistaken for code),
  * annotates peripheral accesses using the PY32F403 register map taken from
    PY32F4xx_Firmware/Drivers/CMSIS/Device/PUYA/PY32F403/Include/py32f403xD.h.

Outputs into <image dir>/analysis/ (override with --outdir):

    listing.txt      annotated disassembly, one line per instruction
    functions.csv    function, size, callees, peripherals, string refs
    strings.txt      string address, encoding, text
    peripherals.txt  every known-peripheral access, by function
    datarefs.txt     every reference to a flash/data address

This is a heuristic tool: it aims to be greppable and honest about what it
could not resolve, not to be a decompiler.
"""

import argparse
import csv
import os
import re
import struct
import sys

from capstone import (Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS,
                      CS_GRP_RET)
from capstone.arm import ARM_OP_IMM, ARM_OP_MEM, ARM_OP_REG, ARM_REG_PC

MAGIC = b"RA89RMETA3"

# ---------------------------------------------------------------------------
# PY32F403 register map (from the vendor CMSIS header, py32f403xD.h)
# ---------------------------------------------------------------------------

GPIO_REGS = {0x00: "MODER", 0x04: "OTYPER", 0x08: "OSPEEDR", 0x0C: "PUPDR",
             0x10: "IDR", 0x14: "ODR", 0x18: "BSRR", 0x1C: "LCKR",
             0x20: "AFRL", 0x24: "AFRH", 0x28: "BRR"}
SPI_REGS = {0x00: "CR1", 0x04: "CR2", 0x08: "SR", 0x0C: "DR", 0x10: "CRCPR",
            0x14: "RXCRCR", 0x18: "TXCRCR", 0x1C: "I2SCFGR", 0x20: "I2SPR"}
USART_REGS = {0x00: "SR", 0x04: "DR", 0x08: "BRR", 0x0C: "CR1", 0x10: "CR2",
              0x14: "CR3", 0x18: "GTPR"}
DMA_REGS = {0x00: "CCR", 0x04: "CNDTR", 0x08: "CPAR", 0x0C: "CMAR"}
TIM_REGS = {0x00: "CR1", 0x04: "CR2", 0x08: "SMCR", 0x0C: "DIER", 0x10: "SR",
            0x14: "EGR", 0x18: "CCMR1", 0x1C: "CCMR2", 0x20: "CCER", 0x24: "CNT",
            0x28: "PSC", 0x2C: "ARR", 0x30: "RCR", 0x34: "CCR1", 0x38: "CCR2",
            0x3C: "CCR3", 0x40: "CCR4", 0x44: "BDTR", 0x48: "DCR", 0x4C: "DMAR",
            0x50: "OR"}
RCC_REGS = {0x00: "CR", 0x04: "ICSCR", 0x08: "CFGR", 0x0C: "CIER", 0x10: "CIFR",
            0x14: "CICR", 0x18: "AHBRSTR", 0x20: "AHBENR", 0x24: "APBENR1",
            0x28: "APBENR2", 0x2C: "APBRSTR1", 0x30: "APBRSTR2", 0x34: "BDCR",
            0x38: "CSR"}
FLASH_REGS = {0x00: "ACR", 0x08: "KEYR", 0x0C: "OPTKEYR", 0x10: "SR",
              0x14: "CR", 0x20: "OPTR", 0x2C: "WRPR"}

# Base addresses and register offsets are taken verbatim from
# PY32F4xx_Firmware/Drivers/CMSIS/Device/PUYA/PY32F403/Include/py32f403xD.h
# (an earlier revision of this table guessed an STM32-like map and mislabelled
# RCC: AHB2ENR is 0x34, APB2ENR 0x18, and 0x40010000 is SYSCFG, not TIM6).
PERIPH = {
    0x40021000: ("RCC", {0x00: "CR", 0x04: "CFGR", 0x08: "CIR", 0x0C: "APB2RSTR",
                         0x10: "APB1RSTR", 0x14: "AHB1ENR", 0x18: "APB2ENR",
                         0x1C: "APB1ENR", 0x20: "BDCR", 0x24: "CSR",
                         0x28: "CFGR1", 0x2C: "AHB1RSTR", 0x30: "AHB2RSTR",
                         0x34: "AHB2ENR", 0x38: "CFGR2"}),
    0x48000000: ("GPIOA", GPIO_REGS),
    0x48000400: ("GPIOB", GPIO_REGS),
    0x48000800: ("GPIOC", GPIO_REGS),
    0x48000C00: ("GPIOD", GPIO_REGS),
    0x48001000: ("GPIOE", GPIO_REGS),
    0x40013000: ("SPI1", SPI_REGS),
    0x40003800: ("SPI2", SPI_REGS),
    0x40003C00: ("SPI3", SPI_REGS),
    0x40013800: ("USART1", USART_REGS),
    0x40004400: ("USART2", USART_REGS),
    0x40004800: ("USART3", USART_REGS),
    0x40004C00: ("USART4", USART_REGS),
    0x40005000: ("USART5", USART_REGS),
    0x40012C00: ("TIM1", TIM_REGS),
    0x40000000: ("TIM2", TIM_REGS),
    0x40000400: ("TIM3", TIM_REGS),
    0x40000800: ("TIM4", TIM_REGS),
    0x40000C00: ("TIM5", TIM_REGS),
    0x40001000: ("TIM6", TIM_REGS),
    0x40001400: ("TIM7", TIM_REGS),
    0x40013400: ("TIM8", TIM_REGS),
    0x40014C00: ("TIM9", TIM_REGS),
    0x40015000: ("TIM10", TIM_REGS),
    0x40015400: ("TIM11", TIM_REGS),
    0x40001800: ("TIM12", TIM_REGS),
    0x40001C00: ("TIM13", TIM_REGS),
    0x40002000: ("TIM14", TIM_REGS),
    0x40012400: ("ADC1", {}),
    0x40012800: ("ADC2", {}),
    0x40013C00: ("ADC3", {}),
    0x40005400: ("I2C1", {0x00: "CR1", 0x04: "CR2", 0x08: "OAR1", 0x0C: "OAR2",
                          0x10: "DR", 0x14: "SR1", 0x18: "SR2", 0x1C: "CCR",
                          0x20: "TRISE"}),
    0x40005800: ("I2C2", {}),
    0x40020000: ("DMA1", {}),
    0x40020400: ("DMA2", {}),
    0x40022000: ("FLASH", FLASH_REGS),
    0x40010400: ("EXTI", {0x00: "IMR", 0x04: "EMR", 0x08: "RTSR", 0x0C: "FTSR",
                          0x10: "SWIER", 0x14: "PR"}),
    0x40010000: ("SYSCFG", {}),
    0x40023000: ("CRC", {0x00: "DR", 0x04: "IDR", 0x08: "CR"}),
    0x40003000: ("IWDG", {0x00: "KR", 0x04: "PR", 0x08: "RLR", 0x0C: "SR"}),
    0x40002C00: ("WWDG", {0x00: "CR", 0x04: "CFR", 0x08: "SR"}),
    0x40002800: ("RTC", {}),
    0x40007000: ("PWR", {0x00: "CR", 0x04: "CSR"}),
}
for _c in range(7):
    PERIPH[0x40020008 + 0x14 * _c] = ("DMA1_CH%d" % (_c + 1), DMA_REGS)
    PERIPH[0x40020408 + 0x14 * _c] = ("DMA2_CH%d" % (_c + 1), DMA_REGS)


def periph_name(addr):
    """Name the register at addr, or return ''."""
    if 0x40000000 <= addr < 0x50000000:
        for base in sorted(PERIPH, reverse=True):
            if base <= addr < base + 0x400:
                name, regs = PERIPH[base]
                off = addr - base
                if off == 0:
                    return name
                if off in regs:
                    return "%s_%s" % (name, regs[off])
                return "%s+0x%02X" % (name, off)
        return "PERIPH+0x%X" % (addr - 0x40000000)
    if 0x20000000 <= addr < 0x20020000:
        return "SRAM+0x%X" % (addr - 0x20000000)
    if 0x08000000 <= addr < 0x08060000:
        return "flash"
    return ""


# ---------------------------------------------------------------------------
# image
# ---------------------------------------------------------------------------

class Image(object):
    def __init__(self, bin_path, meta_path=None):
        with open(bin_path, "rb") as f:
            self.data = f.read()
        meta_path = meta_path or bin_path + ".meta"
        with open(meta_path, "rb") as f:
            blob = f.read()
        if not blob.startswith(MAGIC):
            raise SystemExit("%s is not a sidecar written by ra89r.py" % meta_path)
        off = len(MAGIC)
        self.baseline, self.base, self.size, self.nrec = \
            struct.unpack_from("<BIII", blob, off)
        if len(self.data) != self.size:
            raise SystemExit("image is %d bytes, sidecar says %d"
                             % (len(self.data), self.size))
        self.end = self.base + len(self.data)

    def off(self, addr):
        return addr - self.base

    def contains(self, addr, size=1):
        return self.base <= addr and addr + size <= self.end

    def read32(self, addr):
        return struct.unpack_from("<I", self.data, self.off(addr))[0]

    def is_code(self, addr):
        return self.contains(addr, 2) and self.base <= addr < self.end


# ---------------------------------------------------------------------------
# strings
# ---------------------------------------------------------------------------

def scan_strings(img, minlen=4):
    out = []
    for m in re.finditer(rb"[\x20-\x7e]{%d,}" % minlen, img.data):
        out.append((img.base + m.start(), "ascii", m.group().decode("ascii")))
    for m in re.finditer(rb"(?:[\x20-\x7e][\x00]){%d,}" % minlen, img.data):
        out.append((img.base + m.start(), "utf16", m.group().decode("utf-16le")))
    pat = re.compile(rb"(?:[\x00-\xff][\x4e-\x9f]){%d,}" % minlen)
    for m in pat.finditer(img.data):
        raw = m.group()
        out.append((img.base + m.start(), "utf16-cjk",
                    raw.decode("utf-16le", "replace")))
    out.sort()
    return out


# ---------------------------------------------------------------------------
# analyzer
# ---------------------------------------------------------------------------

class Analyzer(object):
    def __init__(self, img):
        self.img = img
        self.md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
        self.md.detail = True
        self.funcs = {}
        self.calls = {}
        self.periph_refs = []
        self.data_refs = []
        self.strings = {}
        self.listing = []

    # -- entry points ---------------------------------------------------------

    def vector_entries(self):
        ents = []
        for i in range(0, 0x40 * 4, 4):
            if not self.img.contains(self.img.base + i, 4):
                break
            w = self.img.read32(self.img.base + i)
            if self.img.is_code(w & ~1):
                ents.append(w & ~1)
        return ents

    def branch_targets(self):
        """BL/BLX targets and push{..,lr} prologues found by a 2-byte sweep."""
        img = self.img
        found = set()
        for off in range(0, len(img.data) - 4, 2):
            addr = img.base + off
            for insn in self.md.disasm(img.data[off:off + 4], addr):
                if insn.mnemonic in ("bl", "blx") and insn.operands and \
                        insn.operands[0].type == ARM_OP_IMM:
                    t = insn.operands[0].imm & ~1
                    if img.is_code(t):
                        found.add(t)
                elif insn.mnemonic == "push" and "lr" in insn.op_str:
                    found.add(addr)
                break
        return found

    def pointer_tables(self, minrun=4):
        """Runs of >=minrun consecutive words that all point into code.

        These are dispatch/task/vector tables: the entries are real function
        starts with no direct BL from anywhere.
        """
        img = self.img
        out = set()
        i = 0
        n = len(img.data) // 4
        run = []
        while i < n:
            w = img.read32(img.base + 4 * i)
            if (w & 1) and img.is_code(w & ~1) and (w & ~1) != 0:
                run.append((i, w & ~1))
            else:
                if len(run) >= minrun:
                    out.update(t for _, t in run)
                run = []
            i += 1
        if len(run) >= minrun:
            out.update(t for _, t in run)
        return out

    def thunk_targets(self, entries):
        """Veneers of the form `ldr rX, =code; bx rX` are function entries too."""
        out = set()
        for start in list(entries):
            insns = self.disasm_function(start, set())
            if len(insns) <= 3:
                for insn in insns:
                    if insn.mnemonic in ("ldr", "movw", "movs") and \
                            insn.operands and insn.operands[0].type == ARM_OP_REG:
                        lit = None
                        ops = insn.operands
                        if len(ops) > 1 and ops[1].type == ARM_OP_MEM and \
                                ops[1].mem.base == ARM_REG_PC:
                            lit = ((insn.address + 4) & ~3) + ops[1].mem.disp
                        if lit is not None and self.img.contains(lit, 4):
                            v = self.img.read32(lit) & ~1
                            if self.img.is_code(v) and v not in entries:
                                out.add(v)
        return out

    # -- disassembly ----------------------------------------------------------

    def disasm_function(self, start, entries=None):
        img = self.img
        seen = set()
        pool = set()
        work = [start & ~1]
        insns = []
        while work:
            a = work.pop()
            steps = 0
            while img.is_code(a) and a not in seen and a not in pool:
                if a != (start & ~1) and entries and a in entries:
                    break
                steps += 1
                if steps > 4096:
                    break
                seen.add(a)
                insn = None
                for insn in self.md.disasm(img.data[img.off(a):img.off(a) + 4], a):
                    break
                if insn is None:
                    break
                insns.append(insn)
                mnem, ops = insn.mnemonic, insn.operands
                if mnem in ("ldr", "ldrh", "ldrb", "vldr") and len(ops) > 1 and \
                        ops[1].type == ARM_OP_MEM and ops[1].mem.base == ARM_REG_PC:
                    lit = ((insn.address + 4) & ~3) + ops[1].mem.disp
                    for k in range(4):
                        pool.add(lit + k)
                if mnem == "b" and ops and ops[0].type == ARM_OP_IMM:
                    t = ops[0].imm & ~1
                    if img.is_code(t):
                        work.append(t)
                    break
                if mnem in ("cbz", "cbnz"):
                    tgt = next((o.imm for o in ops if o.type == ARM_OP_IMM), None)
                    if tgt is not None and img.is_code(tgt & ~1):
                        work.append(tgt & ~1)
                elif mnem in ("bx", "blx", "udf", "bkpt", "trap", "tbb", "tbh"):
                    break
                elif mnem in ("pop", "ldm", "ldmia") and "pc" in insn.op_str:
                    break
                elif insn.group(CS_GRP_RET):
                    break
                a = insn.address + insn.size
        return insns

    # -- annotation -----------------------------------------------------------

    def annotate(self, func, insns):
        lines = []
        regs = {}
        for insn in insns:
            mnem, ops = insn.mnemonic, insn.operands
            note = []
            addr = None
            if mnem in ("ldr", "ldrh", "ldrb") and len(ops) > 1 and \
                    ops[1].type == ARM_OP_MEM and ops[1].mem.base == ARM_REG_PC:
                lit = ((insn.address + 4) & ~3) + ops[1].mem.disp
                if self.img.contains(lit, 4) and ops[0].type == ARM_OP_REG:
                    val = self.img.read32(lit)
                    regs[ops[0].reg] = ("lit", val)
                    note.append("=0x%08X" % val)
                    pn = periph_name(val)
                    if 0x40000000 <= val < 0x50000000 and pn:
                        note.append(pn)
                    if self.img.contains(val, 2):
                        note.append("->0x%08X" % val)
                        self.data_refs.append((func, insn.address, val, "literal"))
            elif mnem in ("mov", "movs", "movw") and len(ops) > 1 and \
                    ops[0].type == ARM_OP_REG and ops[1].type == ARM_OP_IMM:
                regs[ops[0].reg] = ("imm", ops[1].imm)
            elif mnem in ("mov", "movs") and len(ops) > 1 and \
                    ops[0].type == ARM_OP_REG and ops[1].type == ARM_OP_REG:
                regs[ops[0].reg] = regs.get(ops[1].reg, ("reg", None))
            elif mnem in ("add", "adds", "sub", "subs", "addw", "subw") and \
                    len(ops) > 2 and ops[0].type == ARM_OP_REG and \
                    ops[1].type == ARM_OP_REG and ops[2].type == ARM_OP_IMM:
                base = regs.get(ops[1].reg)
                if base and base[0] == "lit":
                    v = base[1] + (ops[2].imm if mnem.startswith("add") else -ops[2].imm)
                    regs[ops[0].reg] = ("lit", v)
                    note.append("=0x%08X" % v)
                    pn = periph_name(v)
                    if pn:
                        note.append(pn)
            elif mnem in ("ldr", "ldrb", "ldrh", "str", "strb", "strh", "ldrsb",
                          "ldrsh") and len(ops) > 1 and ops[1].type == ARM_OP_MEM:
                mem = ops[1].mem
                base = regs.get(mem.base) if mem.base else None
                if base and base[0] == "lit" and mem.index == 0:
                    addr = base[1] + mem.disp
                    pn = periph_name(addr)
                    if pn and 0x40000000 <= addr < 0x50000000:
                        note.append(pn)
                        self.periph_refs.append((func, insn.address,
                                                 "%s %s" % (mnem, pn)))
                    else:
                        note.append("0x%08X" % addr)
                        self.data_refs.append((func, insn.address, addr, mnem))
                    if mnem in ("ldr", "ldrh", "ldrb") and \
                            self.img.contains(addr, 4) and ops[0].type == ARM_OP_REG:
                        regs[ops[0].reg] = ("lit", self.img.read32(addr))
                if ops[0].type == ARM_OP_REG and mnem.startswith(("str", "strb")):
                    regs.pop(ops[0].reg, None)
                elif ops[0].type == ARM_OP_REG:
                    regs[ops[0].reg] = ("reg", None)
            elif mnem in ("push", "pop", "stmdb", "ldmia", "stmia", "stm") and ops:
                # registers are clobbered by restores
                if mnem.startswith(("pop", "ldm")) and "pc" not in insn.op_str:
                    for op in ops:
                        if op.type == ARM_OP_REG:
                            regs.pop(op.reg, None)
            elif ops and ops[0].type == ARM_OP_REG and mnem not in ("cmp", "tst",
                                                                    "adr", "push", "pop"):
                regs.pop(ops[0].reg, None)
            if mnem in ("bl", "blx") and ops and ops[0].type == ARM_OP_IMM:
                t = ops[0].imm & ~1
                self.calls.setdefault(func, set()).add(t)
                note.append("call 0x%08X" % t)
                if t in self.strings:
                    note.append('"%s"' % self.strings[t][0][1][:40])
            if mnem == "bl" and ops and ops[0].type == ARM_OP_REG:
                note.append("call ptr")
            lines.append("  %08X  %-10s %-8s %-34s%s" % (
                insn.address, insn.bytes.hex(), mnem, insn.op_str,
                " ; " + " ".join(note) if note else ""))
        return lines

    # -- driver ---------------------------------------------------------------

    def run(self, outdir):
        for addr, kind, text in scan_strings(self.img):
            self.strings.setdefault(addr, []).append((kind, text))

        entries = set(self.vector_entries()) | self.branch_targets()
        tables = self.pointer_tables()
        entries |= tables
        for _ in range(4):
            extra = self.thunk_targets(entries) | (self.pointer_tables() - entries)
            if not extra:
                break
            entries |= extra
        entries = sorted(entries)
        self.entries = set(entries)
        for start in entries:
            insns = self.disasm_function(start, self.entries)
            if not insns:
                continue
            info = self.funcs.setdefault(start, {"insns": 0, "strings": set()})
            info["insns"] += len(insns)
            self.listing.append("; ---- function 0x%08X (%d instructions) ----"
                                % (start, len(insns)))
            self.listing.extend(self.annotate(start, insns))
            self.listing.append("")

        for func, at, target, kind in self.data_refs:
            if target in self.strings:
                self.funcs.setdefault(func, {"insns": 0, "strings": set()})
                for k, t in self.strings[target]:
                    self.funcs[func]["strings"].add("%s@%08X" % (t[:40], target))

        if not os.path.isdir(outdir):
            os.makedirs(outdir)
        with open(os.path.join(outdir, "listing.txt"), "w") as f:
            f.write("\n".join(self.listing) + "\n")
        with open(os.path.join(outdir, "strings.txt"), "w") as f:
            for addr in sorted(self.strings):
                for kind, text in self.strings[addr]:
                    f.write("%08X  %-10s %s\n" % (addr, kind, text))
        with open(os.path.join(outdir, "peripherals.txt"), "w") as f:
            for func, at, ref in sorted(self.periph_refs):
                f.write("%08X  %08X  %s\n" % (func, at, ref))
        with open(os.path.join(outdir, "datarefs.txt"), "w") as f:
            for func, at, target, kind in sorted(self.data_refs):
                if 0x40000000 <= target < 0x50000000:
                    continue
                label = self.strings[target][0][1][:60] if target in self.strings else ""
                f.write("%08X  %08X  ->%08X  %-6s %s\n" % (func, at, target, kind, label))
        with open(os.path.join(outdir, "functions.csv"), "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["address", "insns", "callees", "peripherals", "strings"])
            for addr in sorted(self.funcs):
                callees = sorted(self.calls.get(addr, ()))
                per = sorted({r.split()[1] for (fn, at, r) in self.periph_refs
                              if fn == addr})
                w.writerow(["0x%08X" % addr, self.funcs[addr]["insns"],
                            " ".join("0x%08X" % c for c in callees),
                            " ".join(per),
                            " | ".join(sorted(self.funcs[addr]["strings"]))])

        print("pointer-table entries: %d" % len(tables))
        print("functions: %d, instructions: %d"
              % (len(self.funcs), sum(i["insns"] for i in self.funcs.values())))
        print("strings: %d, peripheral accesses: %d, data refs: %d"
              % (len(self.strings), len(self.periph_refs), len(self.data_refs)))
        print("output -> %s" % outdir)


def main(argv=None):
    ap = argparse.ArgumentParser(description="RA89R image analyzer")
    ap.add_argument("image", help="decoded .bin (needs the .meta sidecar)")
    ap.add_argument("--meta")
    ap.add_argument("--outdir")
    a = ap.parse_args(argv)
    outdir = a.outdir or os.path.join(
        os.path.dirname(os.path.abspath(a.image)), "analysis")
    Analyzer(Image(a.image, a.meta)).run(outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
