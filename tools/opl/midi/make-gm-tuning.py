#!/usr/bin/env python3
"""Measure how loud each General MIDI program plays, and level the outliers.

usage: make-gm-tuning.py [--write] [--report FILE]

Each melodic program plays C3, C4 and C5 at velocity 100, held, through the
host tool (tools/opl/build/headless/opl-midi, which must be built from the
current gm-bank-tuning.h). Its loudness is the loudest 400 ms window of the
first two seconds, K-weighted as ITU-R BS.1770 measures programme loudness,
averaged over the three notes. A program more than 3 dB from the median is
moved to the edge of that band by its trim in kGmMelodicTrim, in the chip's
0.75 dB total-level steps; the rest keep theirs. The trims found are added to
the ones the header holds, so a second run on the result changes nothing
beyond what clamping at a patch's headroom leaves.

--write rewrites the table in gm-bank-tuning.h; without it the script only
reports. Loudness is a model of hearing, not hearing: listen to the result.
"""
import argparse
import array
import math
import re
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / "build/headless/opl-midi"
HEADER = HERE / "gm-bank-tuning.h"
BAND_DB = 3.0
STEP_DB = 0.75
NOTES = (48, 60, 72)


def vlq(value):
    out = [value & 0x7F]
    value >>= 7
    while value:
        out.append(0x80 | (value & 0x7F))
        value >>= 7
    return bytes(reversed(out))


def held_note(program, key, seconds=4):
    ticks = 480 * 2 * seconds   # 120 beats a minute
    data = vlq(0) + bytes([0xC0, program]) + vlq(0) + bytes([0x90, key, 100])
    data += vlq(ticks) + bytes([0x80, key, 0]) + vlq(0) + b"\xff\x2f\x00"
    return b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + b"MTrk" + struct.pack(">I", len(data)) + data


def biquad(x, b, a):
    y = [0.0] * len(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(x):
        o = b[0] * v + b[1] * x1 + b[2] * x2 - a[1] * y1 - a[2] * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y[i] = o
    return y


def loudness(samples, rate):
    """K-weighted loudness of the loudest 400 ms window in the first 2 s."""
    y = biquad([v / 32768.0 for v in samples],
               [1.53512485958697, -2.69169618940638, 1.19839281085285],
               [1, -1.69065929318241, 0.73248077421585])
    y = biquad(y, [1.0, -2.0, 1.0], [1, -1.99004745483398, 0.99007225036621])
    squares = [v * v for v in y]
    window, hop = int(0.4 * rate), int(0.1 * rate)
    best = max(sum(squares[s:s + window]) / window
               for s in range(0, max(1, min(len(squares), 2 * rate) - window), hop))
    return -0.691 + 10 * math.log10(best) if best > 1e-12 else -99.0


def measure(program, work):
    values = []
    for key in NOTES:
        midi, wav = work / "note.mid", work / "note.wav"
        midi.write_bytes(held_note(program, key))
        subprocess.run([str(TOOL), str(midi), "--wav", str(wav), "--tail", "0.1"], check=True, capture_output=True)
        with wave.open(str(wav)) as w:
            frames = array.array("h", w.readframes(w.getnframes()))
            values.append(loudness(frames[0::2], w.getframerate()))
    return sum(values) / len(values)


def read_trims(text):
    body = text[text.index("kGmMelodicTrim[128]"):]
    body = body[body.index("{") + 1:body.index("};")]
    return [int(v) for v in re.findall(r"-?\d+", body)]


def write_trims(text, trims):
    rows = "\n".join("\t" + " ".join("%3d," % t for t in trims[i:i + 16]) for i in range(0, 128, 16))
    start = text.index("kGmMelodicTrim[128]")
    open_brace = text.index("{", start) + 1
    close = text.index("};", open_brace)
    return text[:open_brace] + "\n" + rows + "\n" + text[close:]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--write", action="store_true", help="rewrite the trim table in gm-bank-tuning.h")
    parser.add_argument("--report", type=Path, help="write each program's loudness and trim here")
    args = parser.parse_args()
    if not TOOL.is_file():
        sys.exit(f"{TOOL} is not built (make midi-host)")
    text = HEADER.read_text()
    trims = read_trims(text)
    with tempfile.TemporaryDirectory() as tmp:
        levels = [measure(p, Path(tmp)) for p in range(128)]
    median = sorted(levels)[len(levels) // 2]
    new = list(trims)
    for p, level in enumerate(levels):
        excess = level - median
        if abs(excess) > BAND_DB:
            steps = round((abs(excess) - BAND_DB) / STEP_DB)
            new[p] = max(-63, min(63, trims[p] + (steps if excess > 0 else -steps)))
    lines = ["program loudness_LU_vs_median trim_before trim_after"]
    lines += ["%d %+.1f %d %d" % (p, levels[p] - median, trims[p], new[p]) for p in range(128)]
    report = "\n".join(lines) + "\n"
    if args.report:
        args.report.write_text(report)
    changed = sum(a != b for a, b in zip(trims, new))
    inside = sum(abs(level - median) <= BAND_DB for level in levels)
    print(f"median {median:.1f}; {inside} of 128 within {BAND_DB:g} dB; {changed} trims changed")
    if args.write and changed:
        HEADER.write_text(write_trims(text, new))
        print(f"wrote {HEADER}")


if __name__ == "__main__":
    main()
