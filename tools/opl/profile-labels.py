#!/usr/bin/env python3
"""Cycles per output frame by DSP label, from a Hatari profile a bench gate saved.

usage: profile-labels.py <case directory> [--top N]

The case directory (build/<gate>/<scenario>) holds profile.txt and results come
from the listing at dsp/OPLRT.LST. Each executed address is charged to the last
label at or before it, so a label that heads a loop collects the loop's cost.
Cycles are Hatari's instruction cycles divided by two clocks, as rt-bench-gate.
"""
import argparse
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

HERE = Path(__file__).resolve().parent
LABEL_RE = re.compile(r"^\s*\d+\s+([A-Za-z_][A-Za-z0-9_]*):\s*(;.*)?$")
ADDRESS_RE = re.compile(r"^\s*\d+\s+P:([0-9A-F]+)\b")
DATA_RE = re.compile(r"^\s*\d+\s+[XYL]:[0-9A-F]+\b")
PROFILE_RE =re.compile(r"^p:([0-9a-f]+).*?\s[0-9]+[.,][0-9]+% \((\d+), (\d+), (\d+)\)$")


def symbols(listing):
    out, pending = {}, []
    for line in listing.read_text(errors="replace").splitlines():
        label = LABEL_RE.match(line)
        if label:
            pending.append(label.group(1))
            continue
        if DATA_RE.match(line):
            pending.clear()          # a data label: the next code address is not its
            continue
        address = ADDRESS_RE.match(line)
        if address and pending:
            for name in pending:
                out[name] = int(address.group(1), 16)
            pending.clear()
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case", type=Path)
    parser.add_argument("--frames", type=int, required=True, help="frames the profiled window rendered")
    parser.add_argument("--top", type=int, default=40)
    args = parser.parse_args()
    syms = sorted(((a, n) for n, a in symbols(HERE / "dsp/OPLRT.LST").items()))
    addresses = [a for a, _ in syms]
    import bisect
    total, by_label = 0.0, {}
    for line in (args.case / "profile.txt").read_text(errors="replace").splitlines():
        match = PROFILE_RE.match(line.strip())
        if not match:
            continue
        pc, cycles = int(match.group(1), 16), int(match.group(3))
        i = bisect.bisect_right(addresses, pc) - 1
        name = syms[i][1] if i >= 0 else "?"
        by_label[name] = by_label.get(name, 0) + cycles
    for name, cycles in sorted(by_label.items(), key=lambda kv: -kv[1])[:args.top]:
        print(f"{cycles / 2 / args.frames:9.2f}  {name}")
    print(f"{sum(by_label.values()) / 2 / args.frames:9.2f}  total (includes host waits)")


if __name__ == "__main__":
    main()
