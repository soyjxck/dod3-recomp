#!/usr/bin/env python3
"""stack_summary.py <sample.txt> -- one line per thread from a macOS `sample`:
the outermost and innermost guest/runtime frames, so a deadlock reads as a
list of who waits in what."""
import re
import sys

lines = open(sys.argv[1], errors="ignore").read().split("\n")
blocks, cur = [], None
for l in lines:
    m = re.match(r"^    (\d+) Thread_(\d+)(.*)$", l)
    if m:
        cur = [m.group(2), []]
        blocks.append(cur)
        continue
    if l.startswith("Total number") or l.startswith("Sort by"):
        cur = None
    if cur is not None and l.strip():
        cur[1].append(l)


def name(l):
    m = re.search(r"\d+ (.+?)  \(in ", l)
    return m.group(1).split("(")[0] if m else ""


KEEP = ("func_", "sys_", "spu_", "jc_", "cell", "WaitFor", "__psynch", "__semwait", "ppu_")
for tid, bl in blocks:
    # follow the heaviest path: the first child at each depth
    path = [name(l) for l in bl]
    g = [n for n in path if n.startswith(KEEP)]
    if not g:
        continue
    print(tid, " > ".join(g[:2]), " ... ", " > ".join(g[-6:]))
