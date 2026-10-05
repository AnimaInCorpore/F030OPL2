#!/usr/bin/env python3
"""Run a collection of MIDI songs through the MIDI player and report how each fares.

usage: midi-corpus.py <file or directory>... --output DIR [--gate N | --gate-all] [--ahead on|off|default]

Standard MIDI Files are used as they are (any file that starts with MThd, so
the suffixes games give their arrangements, .adl .mdi and the like, count); Miles
XMIDI (.xmi) files are converted first by xmi2mid.py. Every song is played through
the host tool (opl-midi), which is quick and predicts the DSP's output
checksum; the N songs with the densest register traffic (or all of them) are
then played on the emulated Falcon through F030MID.TOS by midi-gate.py, which
requires the DSP's checksum to equal the host's and counts late periods.

The songs are not part of this repository: point it at whatever you have, for
example the music folders of the game projects beside it. A song the reader
refuses is listed and skipped. Writes DIR/corpus.json and prints a table.
"""
import argparse
from datetime import date
import json
from pathlib import Path
import subprocess
import sys

from gate_env import program

HERE = Path(__file__).resolve().parent


def is_smf(path):
    """A Standard MIDI File, bare or inside a RIFF-style MIDI wrapper (.GMD)."""
    try:
        with path.open("rb") as f:
            head = f.read(64)
    except OSError:
        return False
    return head[:4] == b"MThd" or (head[:4] == b"MIDI" and b"MThd" in head[8:])


def gather(paths, work):
    songs = []
    for path in paths:
        files = sorted(p for p in path.rglob("*") if p.is_file()) if path.is_dir() else [path]
        for file in files:
            suffix = file.suffix.lower()
            if suffix == ".xmi":
                target = work / "xmi" / file.parent.name / file.stem
                subprocess.run([sys.executable, str(HERE / "midi/xmi2mid.py"), str(file), str(target)],
                               check=True, capture_output=True)
                songs.extend(sorted(target.glob("*.mid")))
            elif is_smf(file):
                songs.append(file)
    return songs


def label(song):
    """A name that tells apart A.MID and A.ADL."""
    return song.name.replace(".", "_")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--gate", type=int, default=6, metavar="N",
                        help="play the N densest songs on the emulated Falcon (default 6)")
    parser.add_argument("--gate-all", action="store_true")
    parser.add_argument("--ahead", choices=("default", "on", "off"), default="default")
    parser.add_argument("--vbls", type=int, default=120000)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    tool = program(HERE / "build/headless/opl-midi")

    songs = gather(args.paths, args.output)
    measured, refused = [], []
    for song in songs:
        run = subprocess.run([str(tool), str(song), "--json"], capture_output=True, text=True)
        if run.returncode != 0:
            refused.append(str(song))
            continue
        facts = json.loads(run.stdout)
        facts["file"] = str(song)
        facts["events_per_second"] = round(facts["parameter_events"] / max(facts["song_seconds"], 1e-9), 1)
        measured.append(facts)
    measured.sort(key=lambda f: -f["events_per_second"])
    chosen = measured if args.gate_all else measured[:args.gate]

    for facts in chosen:
        case = args.output / "gate" / label(Path(facts["file"]))
        run = subprocess.run([sys.executable, str(HERE / "midi-gate.py"), facts["file"], "--output", str(case),
                              "--ahead", args.ahead, "--vbls", str(args.vbls)],
                             capture_output=True, text=True)
        result = case / "results.json"
        if result.is_file():
            r = json.loads(result.read_text())
            facts["gate"] = {"checksum_equal": r["checksum_equal"], "late_periods": r["late_periods"],
                             "min_slack_frames": r["min_slack_frames"], "passed": run.returncode == 0}
        else:
            facts["gate"] = {"passed": False, "error": run.stderr.strip().splitlines()[-1:] or ["no result"]}

    summary = {"date": date.today().isoformat(), "songs": len(songs), "refused": refused,
               "render_ahead": args.ahead, "results": measured}
    (args.output / "corpus.json").write_text(json.dumps(summary, indent=1) + "\n")

    print(f"{len(songs)} songs, {len(refused)} refused, {len(chosen)} gated on the emulated Falcon "
          f"(render-ahead {args.ahead})")
    print(f"{'song':34} {'sec':>6} {'notes':>6} {'ev/s':>7} {'peak':>5}  gate")
    for f in measured[:max(len(chosen), 12)]:
        g = f.get("gate")
        verdict = "" if g is None else ("ok" if g.get("passed") else "FAIL") + (
            f"  late {g['late_periods']}, slack {g['min_slack_frames']} frames" if "late_periods" in g else "")
        print(f"{label(Path(f['file']))[:34]:34} {f['song_seconds']:6.1f} {f['note_ons']:6d} "
              f"{f['events_per_second']:7.1f} {f['peak_events_per_period']:5d}  {verdict}")
    if refused:
        print("refused:", ", ".join(Path(r).name for r in refused))
    failed = [f for f in chosen if not f.get("gate", {}).get("passed")]
    if failed:
        raise SystemExit(f"{len(failed)} gated songs failed")


if __name__ == "__main__":
    main()
