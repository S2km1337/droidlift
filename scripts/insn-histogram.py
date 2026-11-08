#!/usr/bin/env python3
"""Static instruction-frequency count over an ARMv7 shared library.

Function starts come from .ARM.exidx and the dynamic symbols; each function is
ARM or Thumb by its symbol's bit 0, else by its first bytes, else like the
function before it. The whole .text is disassembled once per mode with LLVM's
llvm-objdump, and each line is taken from the listing of its function's mode.
Literal pools inside functions are counted as instructions too, so the tail of
the histogram is noisy; the head is what matters.

usage: insn-histogram.py LIB.so [--objdump PATH] [--top N]
"""
import argparse
import collections
import re
import struct
import subprocess


def sections(data):
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    hdrs = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    stroff = hdrs[shstrndx][4]
    out = {}
    for h in hdrs:
        name = data[stroff + h[0]:data.index(b"\0", stroff + h[0])].decode()
        out[name] = h  # name, type, flags, addr, offset, size, link, info, align, entsize
    return out


def exidx_starts(data, secs):
    h = secs[".ARM.exidx"]
    addr, off, size = h[3], h[4], h[5]
    starts = []
    for i in range(0, size, 8):
        w, = struct.unpack_from("<I", data, off + i)
        rel = w & 0x7FFFFFFF
        if rel & 0x40000000:
            rel -= 0x80000000
        starts.append((addr + i + rel) & 0xFFFFFFFF)
    return starts


def dynsym_funcs(data, secs):
    h = secs[".dynsym"]
    off, size = h[4], h[5]
    out = {}
    for i in range(0, size, 16):
        _, value, _, info, _, _ = struct.unpack_from("<IIIBBH", data, off + i)
        if info & 0xF == 2 and value:  # STT_FUNC
            out[value & ~1] = value & 1
    return out


def guess_mode(data, secs, addr):
    t = secs[".text"]
    o = t[4] + addr - t[3]
    w, = struct.unpack_from("<I", data, o)
    hw0 = w & 0xFFFF
    if (w >> 16) == 0xE92D:
        return 0
    if hw0 == 0xE92D or (hw0 & 0xFF00) == 0xB500 or (hw0 & 0xFE00) == 0xB400:
        return 1
    return None


LINE = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2,8}\s)+\s*(\S+)")


def disasm(objdump, lib, triple, lo, hi):
    out = subprocess.run(
        [objdump, "-d", f"--triple={triple}", f"--start-address={lo:#x}", f"--stop-address={hi:#x}",
         "-j", ".text", lib], capture_output=True, text=True, check=True).stdout
    res = {}
    for line in out.splitlines():
        m = LINE.match(line)
        if m:
            res[int(m.group(1), 16)] = m.group(2)
    return res


CONDS = "eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le"


def mnemonic_key(m):
    """Drops the .w/.n width suffixes; keeps the S suffix
    and NEON/VFP data types (vadd.f32), which matter to an interpreter."""
    return m.lower().replace(".w", "").replace(".n", "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lib")
    ap.add_argument("--objdump", default=subprocess.run(["xcrun", "-f", "llvm-objdump"], capture_output=True, text=True).stdout.strip() or "llvm-objdump")
    ap.add_argument("--top", type=int, default=80)
    a = ap.parse_args()
    data = open(a.lib, "rb").read()
    secs = sections(data)
    text = secs[".text"]
    lo, hi = text[3], text[3] + text[5]
    syms = dynsym_funcs(data, secs)
    starts = sorted(set(s for s in exidx_starts(data, secs) if lo <= s < hi)
                    | set(s for s in syms if lo <= s < hi))
    modes, prev = [], 0
    for s in starts:
        m = syms.get(s)
        if m is None:
            m = guess_mode(data, secs, s)
        if m is None:
            m = prev
        modes.append(m)
        prev = m
    arm = disasm(a.objdump, a.lib, "armv7a-linux-androideabi", lo, hi)
    thumb = disasm(a.objdump, a.lib, "thumbv7a-linux-androideabi", lo, hi)
    hist = {0: collections.Counter(), 1: collections.Counter()}
    bounds = starts + [hi]
    for i, s in enumerate(starts):
        src = thumb if modes[i] else arm
        for addr in range(s, bounds[i + 1], 2):
            m = src.get(addr)
            if m:
                hist[modes[i]][mnemonic_key(m)] += 1
    # Fold a condition suffix only where the rest is itself a mnemonic seen
    # here: "bls" is b+ls, but "movs" is not mo+vs.
    for mode in hist:
        seen = set(hist[mode])
        folded = collections.Counter()
        for k, n in hist[mode].items():
            mm = re.match(rf"^(.+?)({CONDS})(\..*)?$", k)
            if mm and (mm.group(1) + (mm.group(3) or "")) in seen:
                k = mm.group(1) + (mm.group(3) or "")
            folded[k] += n
        hist[mode] = folded
    total = {0: sum(hist[0].values()), 1: sum(hist[1].values())}
    print(f"# {a.lib}: {len(starts)} functions, {sum(1 for m in modes if not m)} ARM, "
          f"{sum(modes)} Thumb; {total[0]} ARM and {total[1]} Thumb instructions")
    for mode, name in ((0, "ARM"), (1, "Thumb")):
        if not total[mode]:
            continue
        print(f"## {name}")
        cum = 0
        for k, n in hist[mode].most_common(a.top):
            cum += n
            print(f"{k:16s} {n:8d} {100 * n / total[mode]:6.2f}% cum {100 * cum / total[mode]:6.2f}%")


if __name__ == "__main__":
    main()
