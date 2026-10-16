#!/usr/bin/env python3
"""Diff two DCP save payloads (SAVEn.DAT, 0x20eb18 B) region by region.

  savediff.py A.DAT B.DAT                 whole file, per region (header / slot0..2 / tail)
  savediff.py A.DAT B.DAT --slot 0:1      slot 0 of A vs slot 1 of B (offsets slot-relative)
  options: --gap N  merge ranges closer than N bytes (default 8); --max N  ranges printed per region (default 60)

Offsets are payload offsets (or slot-relative with --slot). Layout from notes/map/input_save.md 3.2.
"""
import argparse

SIZE, SLOT0, SLOT = 0x20EB18, 0x50, 0xAF8D4


def regions():
    yield "header", 0, SLOT0
    for i in range(3):
        yield f"slot{i}", SLOT0 + i * SLOT, SLOT0 + (i + 1) * SLOT
    yield "tail", SLOT0 + 3 * SLOT, SIZE


def ranges(a, b, gap):
    out, start, last = [], None, None
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            if start is None: start = i
            elif i - last > gap:
                out.append((start, last + 1)); start = i
            last = i
    if start is not None: out.append((start, last + 1))
    return out


def show(name, a, b, base, gap, mx):
    rs = ranges(a, b, gap)
    nbytes = sum(1 for x, y in zip(a, b) if x != y)
    print(f"== {name}: {nbytes} bytes differ in {len(rs)} ranges")
    for s, e in rs[:mx]:
        n = min(e - s, 16)
        print(f"  +{base + s:07x} len {e - s:5d}  {a[s:s + n].hex(' ')}  ->  {b[s:s + n].hex(' ')}")
    if len(rs) > mx: print(f"  ... {len(rs) - mx} more")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a"); ap.add_argument("b")
    ap.add_argument("--slot"); ap.add_argument("--gap", type=int, default=8); ap.add_argument("--max", type=int, default=60)
    o = ap.parse_args()
    A, B = open(o.a, "rb").read(), open(o.b, "rb").read()
    if o.slot:
        i, j = (int(x) for x in o.slot.split(":"))
        sa, sb = SLOT0 + i * SLOT, SLOT0 + j * SLOT
        show(f"slot{i} vs slot{j}", A[sa:sa + SLOT], B[sb:sb + SLOT], 0, o.gap, o.max)
        return
    for name, s, e in regions():
        show(name, A[s:e], B[s:e], s, o.gap, o.max)


if __name__ == "__main__":
    main()
