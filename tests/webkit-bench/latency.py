#!/usr/bin/env python3
"""latency.py TRACE... : how soon wheel notches reach the screen, from BENCH_TRACE files.

For each wheel notch, the time until the first window update below the
toolbar (the page area, y >= --top); and the gaps between page updates
while scrolling (the frame pacing the eye sees). Prints a line per file and
a JSON summary with --json OUT.
"""
import argparse, json, statistics


def pct(xs, p):
    xs = sorted(xs)
    if not xs:
        return None
    k = (len(xs) - 1) * p / 100
    lo, hi = int(k), min(int(k) + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def analyze(path, top):
    wheels, flushes = [], []
    for line in open(path):
        p = line.split()
        if p and p[0] == "w":
            wheels.append(int(p[1]))
        elif p and p[0] == "f" and len(p) == 6:
            t, y, h = int(p[1]), int(p[3]), int(p[5])
            if y + h > top:
                flushes.append(t)
    if not wheels:
        return None
    lat, j = [], 0
    for w in wheels:
        while j < len(flushes) and flushes[j] < w:
            j += 1
        if j < len(flushes):
            lat.append((flushes[j] - w) / 1000)
    start, end = wheels[0], wheels[-1] + 100000
    during = [t for t in flushes if start <= t <= end]
    gaps = [(b - a) / 1000 for a, b in zip(during, during[1:])]
    return {"notches": len(wheels), "latency_ms_median": statistics.median(lat) if lat else None,
            "latency_ms_p95": pct(lat, 95), "updates": len(during),
            "gap_ms_median": statistics.median(gaps) if gaps else None, "gap_ms_p95": pct(gaps, 95),
            "gaps_over_50ms": sum(1 for g in gaps if g > 50)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("traces", nargs="+")
    ap.add_argument("--top", type=int, default=80, help="where the page area starts (window y)")
    ap.add_argument("--json")
    a = ap.parse_args()
    out = {}
    for t in a.traces:
        r = analyze(t, a.top)
        out[t] = r
        if r:
            print(f"{t}: {r['notches']} notches, latency median {r['latency_ms_median']:.1f} ms, p95 {r['latency_ms_p95']:.1f} ms; "
                  f"{r['updates']} updates, gap median {r['gap_ms_median']:.1f} ms, p95 {r['gap_ms_p95']:.1f} ms, "
                  f"{r['gaps_over_50ms']} gaps over 50 ms")
    if a.json:
        json.dump(out, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
