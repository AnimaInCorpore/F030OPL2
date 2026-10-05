#!/usr/bin/env python3
"""Play a MIDI file through F030MID.TOS in the DSP-calibrated Hatari, with sound.

usage: midi-hatari.py song.mid [--live]

Stages the program and the file in build/midi-play and starts Hatari at real
speed with its audio on. The player returns to the desktop when the song ends;
close the window or press a key to stop it earlier. With --live no file is
staged and the player waits for MIDI IN (Hatari needs PortMidi for that, which
the calibrated build here lacks), so it is only useful on a build that has it.
Not a gate: nothing is compared, nothing is recorded.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

from gate_env import HATARI, TOS402 as TOS

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("midi", type=Path, nargs="?")
    parser.add_argument("--live", action="store_true")
    parser.add_argument("--rate", type=int, default=48000, help="Hatari's audio output rate")
    args = parser.parse_args()
    if not args.midi and not args.live:
        parser.error("give a MIDI file, or --live")
    program = ROOT / "build/midi-tos/F030MID.TOS"
    if not program.is_file():
        parser.error(f"{program} is not built (make midi-tos)")
    if not HATARI.is_file():
        parser.error(f"the DSP-calibrated Hatari is not at {HATARI}")
    case = ROOT / "build/midi-play"
    case.mkdir(parents=True, exist_ok=True)
    for stale in ("SONG.MID", "MIDIIN.RAW", "RESULT.BIN"):
        (case / stale).unlink(missing_ok=True)
    shutil.copy(program, case / "F030MID.TOS")
    if args.midi:
        shutil.copy(args.midi, case / "SONG.MID")
    subprocess.run([str(HATARI), "--machine", "falcon", "--dsp", "emu", "--memsize", "14",
                    "--tos", str(TOS), "--patch-tos", "true", "--fast-boot", "true",
                    "--sound", str(args.rate), "--confirm-quit", "false",
                    "--log-file", str((case / "hatari.log").resolve()), "F030MID.TOS"],
                   cwd=case, check=True)


if __name__ == "__main__":
    main()
