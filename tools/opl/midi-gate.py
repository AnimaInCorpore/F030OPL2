#!/usr/bin/env python3
"""Play a Standard MIDI File through F030MID.TOS on the emulated Falcon and
check it against the host engine, period for period.

The host tool opl-midi runs the same engine, decoder and practical reference
chip and predicts the output checksum (the sum of the limited left and right
words mod 2^24 over every period) and the number of periods. The Falcon
program, built with the m68k cross compiler from the same headers, plays the
file in real time under the DSP-calibrated Hatari; the DSP counts its own
checksum. The two must be equal, no period may be late, and the note count the
68030 saw must be the file's.

What this establishes: the 68030 build of the engine and the SMF reader make
the same register writes as the host build, the decoder events reach the DSP
intact through the stream protocol, and the DSP renders them exactly as the
reference does, in real time. What it does not: no audio was auditioned, the
live MIDI IN path is not covered here (see midi-live-gate.py), and nothing
ran on a physical Falcon.
"""
import argparse
from datetime import date
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

from gate_env import HATARI, TOS402 as TOS, program, source_sha256

HERE = Path(__file__).resolve().parent
LABEL_RE = re.compile(r"^\s*\d+\s+([A-Za-z_][A-Za-z0-9_]*):\s*(;.*)?$")
ADDRESS_RE = re.compile(r"^\s*\d+\s+P:([0-9A-F]+)\b")
DATA_RE = re.compile(r"^\s*\d+\s+[XYL]:[0-9A-F]+\b")


def listing_symbol(listing, wanted):
    pending = []
    for line in listing.read_text(errors="replace").splitlines():
        label = LABEL_RE.match(line)
        if label:
            pending.append(label.group(1))
            continue
        if DATA_RE.match(line):
            pending.clear()
            continue
        address = ADDRESS_RE.match(line)
        if address and pending:
            if wanted in pending:
                return int(address.group(1), 16)
            pending.clear()
    raise SystemExit(f"{wanted} is missing from the DSP listing")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("midi", type=Path, help="a Standard MIDI File, or with --raw a raw MIDI byte stream")
    parser.add_argument("--raw", action="store_true",
                        help="play the file as live MIDI input (F030MID -i): a reproducible stand-in for the "
                             "port, at 3,125 bytes a second")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ahead", choices=("default", "on", "off"), default="default",
                        help="the DSP's render-ahead mode: the player's default (on for a file), or forced")
    parser.add_argument("--tos", type=Path, default=HERE.parent.parent / "build/midi-tos/F030MID.TOS")
    parser.add_argument("--vbls", type=int, default=6000)
    args = parser.parse_args()
    if not HATARI.is_file():
        parser.error(f"the DSP-calibrated Hatari is not at {HATARI}")
    if not args.tos.is_file():
        parser.error(f"{args.tos} is not built (make midi-tos)")
    args.output.mkdir(parents=True, exist_ok=True)
    case = args.output
    # a run leaves no stale answer behind to be mistaken for this one's
    for stale in ("RESULT.BIN", "results.json"):
        (case / stale).unlink(missing_ok=True)

    host = subprocess.run([str(program(HERE / "build/headless/opl-midi"))]
                          + (["--raw"] if args.raw else []) + [str(args.midi), "--json"],
                          capture_output=True, text=True, check=True)
    expected = json.loads(host.stdout)

    # The tables the player uploads are the ones the DSP gates' fixture writes:
    # the host tool's data image must be that file's block section, byte for byte.
    host_image, fixture_image = case / "HOST-OPLDATA.BIN", case / "FIXTURE-OPLDATA.BIN"
    subprocess.run([str(program(HERE / "build/headless/opl-midi"))] + (["--raw"] if args.raw else [])
                   + [str(args.midi), "--data", str(host_image)], check=True, capture_output=True)
    subprocess.run([str(program(HERE / "build/headless/opl-rt-fixture")), "stress", str(fixture_image),
                    str(case / "FIXTURE-EXPECT.BIN"), "--seconds", "0.1"], check=True, capture_output=True)
    tables_match = fixture_image.read_bytes().startswith(host_image.read_bytes())
    shutil.copy(args.tos, case / "F030MID.TOS")
    shutil.copy(args.midi, case / ("MIDIIN.RAW" if args.raw else "SONG.MID"))
    for flag in ("AHEAD.FLG", "NOAHEAD.FLG"):
        (case / flag).unlink(missing_ok=True)
    if args.ahead != "default":
        (case / ("AHEAD.FLG" if args.ahead == "on" else "NOAHEAD.FLG")).write_bytes(b"1")

    stop = listing_symbol(HERE / "dsp/OPLRT.LST", "stream_stopped")
    (case / "start.ini").write_text(f"db pc = ${stop:04x} :once :trace :file {(case / 'end.ini').resolve()}\n")
    (case / "end.ini").write_text("quit 0\n")
    with (case / "debug.log").open("w") as log:
        subprocess.run([str(HATARI), "--machine", "falcon", "--dsp", "emu", "--memsize", "14",
                        "--conout", "2", "--tos", str(TOS), "--patch-tos", "true",
                        "--fast-boot", "true", "--fast-forward", "true", "--sound", "off",
                        "--confirm-quit", "false", "--run-vbls", str(args.vbls),
                        "--parse", str((case / "start.ini").resolve()),
                        "--log-file", str((case / "hatari.log").resolve()), "F030MID.TOS"],
                       cwd=case, stdout=log, stderr=subprocess.STDOUT, check=True)

    result_file = case / "RESULT.BIN"
    if not result_file.is_file():
        raise SystemExit(f"the run produced no result; see {case}/debug.log")
    status, checksum, slack, periods, events, note_ons, overflow = struct.unpack(">7I", result_file.read_bytes()[:28])
    rendered, late = status & 0xfff, status >> 12
    result = {
        "date": date.today().isoformat(),
        "gate": "F030MID.TOS on the emulated Falcon against the host MIDI engine and reference chip",
        "source_sha256": {name: source_sha256(HERE / name) for name in (
            "midi/f030mid.cpp", "midi/midi-opl.h", "midi/midi-file.h", "midi/period-stream.h",
            "midi/opl-upload.h", "midi/gm-bank.h", "dsp/oplrt.asm")},
        "midi": args.midi.name,
        "tables_match_fixture": tables_match,
        "render_ahead": args.ahead,
        "song_seconds": expected["song_seconds"],
        "periods_expected": expected["periods"],
        "periods_submitted": periods,
        "periods_rendered_mod_4096": rendered,
        "late_periods": late,
        "note_ons_expected": expected["note_ons"],
        "note_ons_seen": note_ons,
        "parameter_events_expected": expected["parameter_events"],
        "parameter_events_sent": events,
        "event_overflow": bool(overflow),
        "checksum_expected": expected["period_checksum"],
        "checksum_dsp": checksum,
        "checksum_equal": checksum == expected["period_checksum"],
        "min_slack_frames": slack // 2,
        "not_established": [
            "No audio captured or auditioned from the emulator; a checksum match does not prove individual words",
            "No hardware run",
        ],
    }
    (case / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    problems = []
    if not tables_match:
        problems.append("the player's table image differs from the bench fixture's")
    # The DSP's status word keeps the rendered-period count in twelve bits, so a song of
    # more than 4,095 periods (about 64 s) reads back modulo 4,096.
    if periods != expected["periods"] or rendered != expected["periods"] % 4096:
        problems.append(f"{periods} periods submitted and {rendered} rendered (mod 4096), "
                        f"expected {expected['periods']}")
    if checksum != expected["period_checksum"]:
        problems.append("the DSP's output checksum differs from the host reference's")
    if events != expected["parameter_events"]:
        problems.append("the 68030 sent a different number of parameter events than the host engine")
    if note_ons != expected["note_ons"]:
        problems.append("the 68030 counted a different number of note-ons than the file holds")
    if overflow:
        problems.append("a period overflowed the DSP's event table")
    if late:
        problems.append(f"{late} periods were late")
    if problems:
        raise SystemExit("; ".join(problems))


if __name__ == "__main__":
    main()
