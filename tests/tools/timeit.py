#!/usr/bin/env python3
"""timeit.py -n N --warmup W -- CMD...   run CMD W+N times, print min/median/p95 wall time in ms (hyperfine stand-in)."""
import argparse, statistics, subprocess, sys, time

def main():
    a = argparse.ArgumentParser()
    a.add_argument("-n", type=int, default=10)
    a.add_argument("--warmup", type=int, default=1)
    a.add_argument("cmd", nargs=argparse.REMAINDER)
    a = a.parse_args()
    cmd = a.cmd[1:] if a.cmd[:1] == ["--"] else a.cmd
    if not cmd or a.n < 1:
        sys.exit("usage: timeit.py -n N --warmup W -- CMD...")
    t = []
    for i in range(a.warmup + a.n):
        s = time.perf_counter()
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if i >= a.warmup:
            t.append((time.perf_counter() - s) * 1000)
    t.sort()
    p95 = t[min(len(t) - 1, int(0.95 * len(t) + 0.5) - 1 if len(t) > 1 else 0)]
    print(f"min {t[0]:.3f} ms  median {statistics.median(t):.3f} ms  p95 {p95:.3f} ms  (n={len(t)})")

if __name__ == "__main__":
    main()
