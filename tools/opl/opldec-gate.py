#!/usr/bin/env python3
"""Gate: the 68k register decoder (tools/opl/m68k/opldec.s) against the
practical model's C++ Decoder, event for event.

Each stream - generated here, or given as REGS.BIN files - runs through
build/opldec/opldec-ref (tools/opl/opldec-ref.cpp, the C++ Decoder as
OplMidi::Pipeline drives it) and through the 68k decoder in a stand-alone TOS
program under Hatari. The two outputs - every period's wire events, then the
overflow flag - must be byte-identical.

The generated streams are random OPL2 register writes at random frames: every
register class (0x01, 0x08, 0x20-0xf5, 0xa0-0xc8, 0xbd with the rhythm
section), dense and sparse, so the derived words, the dirty marks, the
same-frame coalescing and the period split are all exercised.

    python3 tools/opl/opldec-gate.py [--cpu 68030|68060] [--streams N] [REGS.BIN ...]

Environment: HATARI (the Falcon emulator), TOS (the ROM; EmuTOS by default),
VASM, VLINK (default: build/tools).
"""
import argparse
import os
import pathlib
import random
import shutil
import struct
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
OPL = ROOT / "tools" / "opl"
OUT = ROOT / "build" / "opldec"
PERIOD = 24 * 32


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.exit(f"{' '.join(map(str, cmd))} failed:\n{r.stdout}\n{r.stderr}")
    return r.stdout


def stream_bytes(records):
    return b"OPLR" + struct.pack(">I", len(records)) + b"".join(
        struct.pack(">IHH", f, r, v) for f, r, v in records)


def random_stream(rng, n):
    regs = [0x01, 0x08, 0xbd] + [base + off for base in (0x20, 0x40, 0x60, 0x80, 0xe0)
                                 for off in (0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 16, 17, 18, 19, 20, 21, 6, 7, 14, 22)] \
        + [0xa0 + c for c in range(10)] + [0xb0 + c for c in range(10)] + [0xc0 + c for c in range(10)]
    frame, out = 0, []
    for _ in range(n):
        frame += rng.choice((0, 0, 0, 1, 3, 32, 97, 400, 768, 1500))
        reg = rng.choice(regs)
        if reg == 0x01:
            value = rng.choice((0x00, 0x20))
        elif reg == 0xbd:
            value = rng.randrange(256)
        else:
            value = rng.randrange(256)
        out.append((frame, reg, value))
    return out


def build_ref():
    OUT.mkdir(parents=True, exist_ok=True)
    cxx = os.environ.get("HOST_CXX", "c++")
    run([cxx, "-std=c++17", "-O1", "-I", OPL, "-o", OUT / "opldec-ref", OPL / "opldec-ref.cpp"])


def build_tos(cpu):
    vasm = os.environ.get("VASM", ROOT / "build" / "tools" / "vasm" / "vasmm68k_mot")
    vlink = os.environ.get("VLINK", ROOT / "build" / "tools" / "vlink" / "vlink")
    objs = []
    for src in ("opldectest.s", "opldec.s"):
        obj = OUT / (src[:-2] + ".o")
        run([vasm, OPL / "m68k" / src, "-quiet", "-Felf", f"-m{cpu}", "-o", obj])
        objs.append(obj)
    run([vlink, *objs, "-q", "-b", "ataritos", "-e", "_main", "-o", OUT / "OPLDEC.TOS"])


def hatari(drive, cpu, timeout=600):
    exe = os.environ.get("HATARI") or shutil.which("hatari")
    if not exe:
        sys.exit("set HATARI to the Falcon emulator")
    tos = os.environ.get("TOS")
    cpuopt = (["--cpulevel", "6", "--cpuclock", "32", "--fpu", "internal"] if cpu == "68060"
              else ["--cpulevel", "3", "--cpuclock", "16", "--fpu", "none"])
    cmd = [exe, "--machine", "falcon", "--memsize", "14", *cpuopt, "--fast-forward", "true",
           "--fast-boot", "true", "--sound", "off", "--disable-video", "on", "--dsp", "off",
           "--confirm-quit", "off", "--harddrive", str(drive), "--auto", "C:\\OPLDEC.TOS",
           "--alert-level", "fatal", "--log-file", str(drive / "hatari.log")]
    if tos:
        cmd[1:1] = ["--tos", tos]
    (drive / "DONE.TXT").unlink(missing_ok=True)
    p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    t0 = time.time()
    try:
        while time.time() - t0 < timeout and not (drive / "DONE.TXT").exists() and p.poll() is None:
            time.sleep(0.3)
        time.sleep(0.3)
    finally:
        p.terminate()
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            p.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpu", choices=("68030", "68060"), default="68030")
    ap.add_argument("--streams", type=int, default=6)
    ap.add_argument("--seed", type=int, default=20261015)
    ap.add_argument("files", nargs="*")
    args = ap.parse_args()
    build_ref()
    build_tos(args.cpu)
    rng = random.Random(args.seed)
    cases = [(f"random-{i}", stream_bytes(random_stream(rng, rng.choice((200, 2000, 20000)))))
             for i in range(args.streams)]
    cases += [(pathlib.Path(f).name, pathlib.Path(f).read_bytes()) for f in args.files]
    failures = 0
    for name, data in cases:
        drive = OUT / "drive"
        if drive.exists():
            shutil.rmtree(drive)
        drive.mkdir()
        shutil.copy(OUT / "OPLDEC.TOS", drive)
        (drive / "REGS.BIN").write_bytes(data)
        run([OUT / "opldec-ref", drive / "REGS.BIN", drive / "EXPECT.BIN"])
        hatari(drive, args.cpu)
        got = (drive / "EVENTS.BIN").read_bytes() if (drive / "EVENTS.BIN").exists() else b""
        want = (drive / "EXPECT.BIN").read_bytes()
        records = struct.unpack_from(">I", data, 4)[0]
        if got == want:
            print(f"ok   {name}: {records} writes, {len(want) // 4} words identical")
        else:
            failures += 1
            at = next((i for i in range(0, min(len(got), len(want)), 4) if got[i:i + 4] != want[i:i + 4]),
                      min(len(got), len(want)))
            print(f"FAIL {name}: {len(got)} bytes against {len(want)}; first difference at word {at // 4}: "
                  f"68k {got[at:at + 16].hex()} C++ {want[at:at + 16].hex()}")
    print("RESULT:", "identical" if not failures else f"{failures} stream(s) differ")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
