#!/usr/bin/env python3
"""One line of numbers per compat.log from an app built with legacy_perf_probe
(--dart-define=LEGACY_PERF_PROBE=true) and/or run with the shim's compat-log-fps.

  tools/perf-summary.py [--from MARK] app.log [more.log ...]

--from MARK: only count lines after the first line containing MARK (e.g. a
line your test script appends to compat.log when the scenario starts).
Key delays over 200 ms are left out of the key figures: in practice they are
keys that changed nothing (e.g. Down on the last row), so no frame follows.
"""
import re
import statistics as st
import sys


def summary(path, mark=None):
    text = open(path, errors="replace").read()
    head, body = "", text
    if mark and mark in text:
        head, body = text.split(mark, 1)
    out = []
    ff = re.search(r"first frame (\d+) ms", head + body)
    if ff:
        out.append(f"first frame {ff.group(1)} ms")
    sums = re.findall(r"\[perf\] summary (\d+) frames: build avg/p90/max ([\d.]+)/[\d.]+/[\d.]+  "
                      r"raster ([\d.]+)/([\d.]+)/([\d.]+)", body)
    if sums:
        n = sum(int(s[0]) for s in sums)
        w = lambda i: sum(int(s[0]) * float(s[i]) for s in sums) / n
        out.append(f"build {w(1):.1f} ms, raster {w(2):.1f} ms (p90 {w(3):.1f}, max {max(float(s[4]) for s in sums):.0f}) over {n} frames")
        slow = re.findall(r"\[perf\] slow frame .*?build ([\d.]+) raster ([\d.]+)", body)
        out.append(f"slow frames {len(slow)} ({100 * len(slow) / n:.1f}%), over 50 ms {sum(float(b) + float(r) > 50 for b, r in slow)}")
    keys = sorted(float(x) for x in re.findall(r"\[perf\] key .*?: ([\d.]+) ms to a drawn frame", body))
    keys = [k for k in keys if k < 200]
    if keys:
        out.append(f"key to frame median {keys[len(keys) // 2]:.0f} ms, p90 {keys[len(keys) * 9 // 10]:.0f}, max {keys[-1]:.0f} ({len(keys)} keys)")
    imgs = re.findall(r"\[perf\] image .*?: (\w+) \d+ KB, fetch (\d+) ms, decode (\d+) ms", head + body)
    if imgs:
        src = {s: sum(1 for i in imgs if i[0] == s) for s in sorted({i[0] for i in imgs})}
        out.append("images " + ", ".join(f"{k} {v}" for k, v in src.items())
                   + f"; decode median {st.median(int(i[2]) for i in imgs):.0f} ms")
    fps = re.findall(r"\[compat\] fps ([\d.]+) .*?frames >20ms (\d+), >33ms (\d+) of (\d+)", body)
    if fps:
        n = sum(int(f[3]) for f in fps)
        seconds = sum(int(f[3]) / float(f[0]) for f in fps)
        out.append(f"presents {n / seconds:.1f} fps, "
                   f">20 ms {100 * sum(int(f[1]) for f in fps) / n:.1f}%")
    return "; ".join(out) or "no [perf] or fps lines"


def main(argv):
    mark = None
    if len(argv) > 2 and argv[1] == "--from":
        mark, argv = argv[2], [argv[0]] + argv[3:]
    if len(argv) < 2:
        sys.exit(__doc__)
    for p in argv[1:]:
        print(f"{p}: {summary(p, mark)}")


if __name__ == "__main__":
    main(sys.argv)
