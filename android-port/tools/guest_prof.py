#!/usr/bin/env python3
"""Guest-level profile report for the OpenCrown Mobile runtime.

Input: the guest_prof.txt written by the runtime with DCP_GUEST_PROF=1 (src/guest/guest_prof.h): per guest thread,
samples of "which JIT block was entered last" (guest address, bit 0 = Thumb) or "inside import n", each marked busy
(guest code, or the thread running inside the import) or idle (blocked in the import). Percentages are of busy
samples, i.e. roughly of the thread's CPU time.

Blocks are rolled up to guest functions with a heuristic over the user's own eboot ELF (never shipped, read at run
time; default ../../Work/vita/update109.elf = update 1.09, text at 0x81000000): a function starts at the nearest
preceding Thumb `push {.., lr}` / `push.w {.., lr}` or BL / BLX target (called from >= 2 sites). Good enough to
find hot spots; check the asm (Work/vita/dis_thumb.py) before trusting a boundary.

Optional --simpleperf <report.txt>: the text of `simpleperf report --sort comm,dso,symbol` for the same run, for
the host-side split (our HLE / renderer by symbol vs JIT code, which simpleperf only sees as anonymous memory).

  python guest_prof.py guest_prof.txt [--elf update109.elf] [--top 30] [--thread "main thread"] [--simpleperf r.txt]
"""
import argparse
import bisect
import collections
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ELF = os.path.normpath(os.path.join(HERE, "..", "..", "Work", "vita", "update109.elf"))


def load_profile(path):
    meta = {}
    threads = {}  # name -> dict(cpu_ms, samples, busy, guest{pc: [busy, idle]}, imports{desc: [busy, idle]})
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            p = line.split("\t")
            if p[0] == "T":
                threads[p[1]] = {"cpu_ms": float(p[2]), "samples": int(p[3]), "busy": int(p[4]),
                                 "guest": {}, "imports": {}}
            elif p[0] == "G":
                threads[p[1]]["guest"][int(p[2], 16)] = [int(p[3]), int(p[4])]
            elif p[0] == "I":
                desc = p[5] if len(p) > 5 and p[5] else "import #" + p[2]
                desc = desc.split(" ")[-1] if " " in desc else desc
                threads[p[1]]["imports"][desc] = [int(p[3]), int(p[4])]
            elif len(p) == 2:
                meta[p[0]] = p[1]
    return meta, threads


class Functions:
    """Guest function starts from the eboot ELF (Thumb heuristics, see the module doc)."""

    def __init__(self, path):
        self.starts = []
        self.text = (0, 0)
        if not path or not os.path.exists(path):
            print(f"[warn] no ELF at {path}: blocks are not rolled up to functions", file=sys.stderr)
            return
        d = open(path, "rb").read()
        if d[:4] != b"\x7fELF":
            print(f"[warn] {path} is not an ELF", file=sys.stderr)
            return
        phoff = struct.unpack_from("<I", d, 0x1C)[0]
        phnum = struct.unpack_from("<H", d, 0x2C)[0]
        seg = None
        for i in range(phnum):
            t, off, va, pa, fs, ms, fl, al = struct.unpack_from("<8I", d, phoff + 32 * i)
            if t == 1 and fl & 1:  # PT_LOAD, executable
                seg = (off, va, fs)
                break
        if not seg:
            return
        off, base, size = seg
        self.text = (base, base + size)
        code = d[off:off + size - (size & 1)]
        hw = struct.unpack(f"<{len(code) // 2}H", code)
        push = set()
        calls = collections.Counter()
        n = len(hw)
        for i in range(n - 1):
            h = hw[i]
            if (h & 0xFF00) == 0xB500:  # push {.., lr} (T1, M bit)
                push.add(base + 2 * i)
            elif h == 0xE92D:  # push.w / stmdb sp!, {.., lr}
                if hw[i + 1] & 0x4000 and not hw[i + 1] & 0xA000:
                    push.add(base + 2 * i)
            elif (h & 0xF800) == 0xF000:
                h2 = hw[i + 1]
                if (h2 & 0xD000) in (0xD000, 0xC000):  # BL (T1) / BLX (T2)
                    s = (h >> 10) & 1
                    j1 = (h2 >> 13) & 1
                    j2 = (h2 >> 11) & 1
                    i1 = 1 ^ j1 ^ s
                    i2 = 1 ^ j2 ^ s
                    imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((h & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
                    if s:
                        imm -= 1 << 25
                    pc = base + 2 * i + 4
                    t = pc + imm
                    if (h2 & 0xD000) == 0xC000:
                        t &= ~3
                    if base <= t < base + size:
                        calls[t] += 1
        starts = push | {t for t, k in calls.items() if k >= 2}
        self.starts = sorted(starts)

    def of(self, pc):
        a = pc & ~1
        if not (self.text[0] <= a < self.text[1]):
            return None
        i = bisect.bisect_right(self.starts, a) - 1
        return self.starts[i] if i >= 0 else None


def fn_name(start, pc):
    if start is None:
        return f"[outside eboot 0x{pc & ~1:08x}]"
    return f"fn_{start:08x}"


def pct(a, b):
    return 100.0 * a / b if b else 0.0


def report_thread(name, t, fns, top):
    busy = t["busy"]
    gb = sum(v[0] for v in t["guest"].values())
    ib = sum(v[0] for v in t["imports"].values())
    print(f"\n=== {name}: cpu {t['cpu_ms']:.0f} ms, {t['samples']} samples, {busy} busy "
          f"(guest {pct(gb, busy):.1f}%, imports {pct(ib, busy):.1f}% of busy)")
    funcs = collections.defaultdict(lambda: [0, 0, collections.Counter()])
    for pc, (b, i) in t["guest"].items():
        key = fn_name(fns.of(pc), pc)
        funcs[key][0] += b
        funcs[key][1] += i
        funcs[key][2][pc] += b + i
    rows = sorted(funcs.items(), key=lambda kv: -kv[1][0])
    if rows:
        print(f"  top guest functions (% of the thread's busy samples):")
        print(f"  {'busy%':>6} {'busy':>7} {'idle':>6}  function        hottest blocks")
        for key, (b, i, blocks) in rows[:top]:
            if not b and not i:
                continue
            hot = ", ".join(f"{pc & ~1:08x}" for pc, _ in blocks.most_common(3))
            print(f"  {pct(b, busy):6.2f} {b:7d} {i:6d}  {key:<15} {hot}")
    imps = sorted(t["imports"].items(), key=lambda kv: -kv[1][0])
    if imps:
        print(f"  imports (busy = host code on CPU; idle = blocked in it):")
        for desc, (b, i) in imps[:min(top, 15)]:
            print(f"  {pct(b, busy):6.2f} {b:7d} {i:6d}  {desc}")
    return funcs


def report_overall(threads, per_thread, top):
    total = sum(t["busy"] for t in threads.values())
    agg = collections.defaultdict(lambda: [0, collections.Counter()])
    for name, funcs in per_thread.items():
        for key, (b, i, _) in funcs.items():
            agg[key][0] += b
            agg[key][1][name] += b
    rows = sorted(agg.items(), key=lambda kv: -kv[1][0])
    print(f"\n=== all threads: top {top} guest functions (% of all busy samples, {total}) with thread split")
    for key, (b, split) in rows[:top]:
        if not b:
            continue
        s = ", ".join(f"{n} {pct(k, b):.0f}%" for n, k in split.most_common(3))
        print(f"  {pct(b, total):6.2f} {b:7d}  {key:<15} {s}")


def report_simpleperf(path, top):
    """Aggregates a `simpleperf report --sort comm,dso,symbol` text by dso and by symbol."""
    by_dso = collections.Counter()
    by_sym = collections.Counter()
    by_comm = collections.Counter()
    jit = collections.Counter()
    header = None
    rx = re.compile(r"^\s*([\d.]+)%\s+(?:([\d.]+)%\s+)?(?:(\d+)\s+)?(.*)$")
    for line in open(path, encoding="utf-8", errors="replace"):
        if header is None:
            if "Overhead" in line and ("Shared Object" in line or "Symbol" in line):
                header = line
            continue
        m = rx.match(line)
        if not m:
            continue
        overhead = float(m.group(1))
        rest = re.split(r"\s{2,}", m.group(4).strip())
        if len(rest) < 3:
            continue
        comm, dso, sym = rest[0], rest[1], "  ".join(rest[2:])
        anon = dso.startswith("[anon") or dso.startswith("//anon") or "jit" in dso.lower() or dso == "[unknown]"
        by_comm[comm] += overhead
        by_dso[dso] += overhead
        if anon:
            jit[comm] += overhead
        else:
            by_sym[(os.path.basename(dso), sym)] += overhead
    if header is None:
        print(f"\n[warn] {path}: no simpleperf report header found")
        return
    print(f"\n=== simpleperf ({path}): host split")
    print("  by shared object:")
    for dso, v in by_dso.most_common(12):
        print(f"  {v:6.2f}%  {dso}")
    print("  JIT code (anonymous memory) by thread:")
    for comm, v in jit.most_common(8):
        print(f"  {v:6.2f}%  {comm}  (of all samples; thread total {by_comm[comm]:.2f}%)")
    print(f"  top {top} host symbols:")
    for (dso, sym), v in by_sym.most_common(top):
        print(f"  {v:6.2f}%  {dso}  {sym}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile", help="guest_prof.txt from DCP_GUEST_PROF=1")
    ap.add_argument("--elf", default=DEFAULT_ELF, help="the eboot as ELF (default: Work/vita/update109.elf)")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--thread", action="append", help="only these threads (repeatable); default: every thread "
                    "with >= 2%% of all busy samples")
    ap.add_argument("--simpleperf", help="simpleperf report --sort comm,dso,symbol output")
    a = ap.parse_args()

    meta, threads = load_profile(a.profile)
    fns = Functions(a.elf)
    total = sum(t["busy"] for t in threads.values())
    print(f"{a.profile}: {meta.get('seconds', '?')} s at {meta.get('hz', '?')} Hz, {len(threads)} threads, "
          f"{len(fns.starts)} function starts from {os.path.basename(a.elf)}")
    print(f"  {'thread':<32} {'cpu ms':>8} {'busy':>7} {'busy%':>6}")
    order = sorted(threads.items(), key=lambda kv: -kv[1]["busy"])
    for name, t in order:
        print(f"  {name:<32} {t['cpu_ms']:8.0f} {t['busy']:7d} {pct(t['busy'], total):6.2f}")
    want = a.thread or [n for n, t in order if pct(t["busy"], total) >= 2.0]
    per_thread = {}
    for name in want:
        if name not in threads:
            print(f"\n[warn] no thread named {name!r}")
            continue
        per_thread[name] = report_thread(name, threads[name], fns, a.top)
    if not a.thread:
        for name, t in threads.items():
            if name not in per_thread:
                funcs = collections.defaultdict(lambda: [0, 0, collections.Counter()])
                for pc, (b, i) in t["guest"].items():
                    key = fn_name(fns.of(pc), pc)
                    funcs[key][0] += b
                    funcs[key][1] += i
                per_thread[name] = funcs
    report_overall({n: threads[n] for n in per_thread}, per_thread, a.top)
    if a.simpleperf:
        report_simpleperf(a.simpleperf, a.top)


if __name__ == "__main__":
    main()
