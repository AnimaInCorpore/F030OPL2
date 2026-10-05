# F030OPL2

Standalone AdLib / YM3812-style sound emulation on the Atari Falcon DSP56001.
The 68030 decodes register writes into DSP parameters; the DSP synthesizes FM
and streams stereo audio through SSI to the Falcon DAC at approximately
49.17 kHz. The initial implementation is imported from the mature local
ScummVM Falcon OPL work, with the F030SID toolchain and two-stage boot scaffold.

`release/f030mid.tos` is a real-time MIDI player: it plays Standard MIDI Files
and acts as a synthesizer for the Falcon's MIDI IN port, through the OPL2 on the
DSP. See [the MIDI player](docs/midi-player.md).

## Build and verify

Use an MSYS2 login shell on Windows with `/ucrt64/bin` on PATH. Dependencies:
make, C/C++ compiler, Python 3, DOSBox Staging; calibrated Hatari for DSP gates.
Set `DOSBOX` and `PYTHON` in ignored `local.mk` if needed.

```sh
git submodule update --init third_party/f030dsp3d
make check
make dsp-gate
make stream-gate
make rhythm-gate
make midi-tos midi-gate midi-live-gate   # the MIDI player: needs the m68k-atari-mintelf cross compiler
```

`release/f030opl2.tos` is the inherited standalone stream harness. It reads
`OPLDATA.BIN` (tables/state) and `PLAYDATA.BIN` (period events) beside the
program. Generate a synthetic nine-channel score with:

```sh
tools/opl/build/headless/opl-rt-fixture stress release/OPLDATA.BIN release/EXPECT.BIN --seconds 10 --play release/PLAYDATA.BIN
```

For a captured register trace replace `stress` with `trace --trace path/to/opl-writes.ev`.
This is a register-stream player and integration foundation; it does not yet
load IMF, DRO, MIDI or game music formats directly. `release/oplrt.tos` is the
frame comparison harness; `oplbench.tos` measures the slower exact kernel.

## Scope and limits

The OPL2 path has nine two-operator channels, four waveforms, FM/additive
connections, feedback, envelopes, tremolo, vibrato, key scaling and rhythm
percussion. The imported core retains upstream OPL3 extensions for regression
compatibility; F030OPL2 does not require those extensions for AdLib playback.

The practical DSP kernel uses 32-frame control blocks (about 0.65 ms), with sample-stamped writes splitting the render span. Envelopes and LFOs
remain block-rate, so synthesis is not chip exact. It is tested against a matching integer host
reference. The exact reference is retained separately; the exact DSP kernel
is a benchmark and is not the real-time playback path. Inherited results are
not proof of this checkout. No physical Falcon validation is claimed. The rhythm
stress stream, formerly one late period, now renders on time under the calibrated
Hatari with its worst period at about 94% of the DSP budget; see
[speed-quality](docs/speed-quality.md). `rt-stream-gate.py --profile` and
`tools/opl/profile-labels.py` show where a stream's cycles go.

## Source map and provenance

- `tools/opl/dsp/oplrt.asm`: production DSP synthesis and SSI transport.
- `tools/opl/m68k/oplplay.s`: standalone 68030 stream host.
- `tools/opl/opl-practical.h`: register decoder and practical host oracle.
- `tools/opl/opl-kernel.h`: exact host reference.
- `tools/opl/midi/`: the MIDI player: engine (`midi-opl.h`), SMF and live-stream
  readers (`midi-file.h`), the period pipeline, the Falcon program
  (`f030mid.cpp`), the host tool `opl-midi` and the GM bank (`gm-bank.h`, from
  ScummVM, GPL-3.0-or-later).
- `src/`: SID-derived boot loader, XBIOS definitions and hardware probes.
- `docs/import-manifest.json`: source revisions and LF-normalized import hashes.
- `tools/opl/README.md`: historical ScummVM investigation and measurements;
  its game/capture instructions refer to the original ScummVM checkout.

The fixture now uses standard fixed-width types instead of ScummVM headers.
Build output and image-header generation stay inside this project. DSP gates
use this project's loader/toolchain and discover the sibling F030Arcade
calibrated Hatari. The toolchain is the same pinned `f030dsp3d` Git submodule used by SID.
Initialize it before building; its tool binaries, archives and ROM retain
their upstream terms. ScummVM-derived
sources retain their original notices; see `COPYING`.

See [SID-derived speed and quality work](docs/speed-quality.md) for the measured
output-loop optimization and the quality features retained from the current core.
