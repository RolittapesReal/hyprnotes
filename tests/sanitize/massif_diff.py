#!/usr/bin/env python3
"""massif_diff.py massif.out TOTAL_CYCLES MAX_KIB_PER_CYCLE: compare the allocation trees of two detailed snapshots (default: lowest-heap snapshot of the first
third after the start vs lowest-heap snapshot of the last third: both are quiescent points) and list the call chains whose live bytes grew."""
import re, sys, collections
snaps = []   # (index, time, heap, {chain: bytes})
cur = None
stack = []
for line in open(sys.argv[1]):
    if line.startswith("snapshot="):
        cur = {"i": int(line.split("=")[1]), "tree": collections.Counter()}; snaps.append(cur); stack = []; continue
    if cur is None: continue
    if line.startswith("time="): cur["time"] = int(line.split("=")[1])
    elif line.startswith("mem_heap_B="): cur["heap"] = int(line.split("=")[1])
    m = re.match(r"( *)n(\d+): (\d+) (.*)", line)
    if m:
        depth = len(m.group(1)); n = int(m.group(2)); b = int(m.group(3)); loc = m.group(4)
        loc = re.sub(r"^0x[0-9A-F]+: ", "", loc)
        loc = re.sub(r" \(in [^)]*\)$", "", loc)
        stack = stack[:depth] + [loc]
        if n == 0 and depth > 0: cur["tree"][" < ".join(reversed(stack[-8:]))] += b   # leaf: allocation site, innermost first
n = len(snaps)
det = [s for s in snaps if s["tree"]]
print(f"{n} snapshots, {len(det)} detailed")
third = max(1, len(det) // 3)
total, limit = int(sys.argv[2]), float(sys.argv[3])
a = min(det[third:2 * third], key=lambda s: s["heap"]); b = min(det[-third:], key=lambda s: s["heap"])
print(f"A: snapshot {a['i']} t={a['time']} heap={a['heap']/1048576:.3f} MiB   B: snapshot {b['i']} t={b['time']} heap={b['heap']/1048576:.3f} MiB   delta={(b['heap']-a['heap'])/1024:.0f} KiB")
d = collections.Counter()
for k in set(a["tree"]) | set(b["tree"]): d[k] = b["tree"][k] - a["tree"][k]
for k, v in sorted(d.items(), key=lambda kv: -kv[1])[:12]:
    print(f"{v/1024:+9.1f} KiB  {k[:900]}")

# The run is almost all lifecycle cycles at a steady rate, so the time between the two snapshots tells how many cycles lie between them.
t0, t1 = det[0]["time"], det[-1]["time"]
between = max(1.0, total * (b["time"] - a["time"]) / max(1, t1 - t0))
per = (b["heap"] - a["heap"]) / 1024.0 / between
print(f"live heap grew {(b['heap'] - a['heap']) / 1024:.0f} KiB over about {between:.0f} cycles = {per:.3f} KiB/cycle (limit {limit} KiB/cycle)")
sys.exit(0 if per < limit else 1)
