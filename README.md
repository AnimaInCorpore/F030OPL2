# F030OPL2

Standalone AdLib / YM3812-style sound emulation on the Atari Falcon DSP56001.
The 68030 decodes register writes into DSP parameters; the DSP synthesizes FM
and streams stereo audio through SSI to the Falcon DAC at approximately
49.17 kHz. The project includes host references, DSP kernels, a two-stage boot
loader, standalone stream harnesses and a MIDI player.

The main application is **F030MID**, one program with two uses:

- **MIDI file player:** download `F030MID.TTP` (or build and rename a copy
  of `release/f030mid.tos`), and enter a `.mid` filename in the desktop's
  parameter box.
- **Live MIDI synthesizer:** start `F030MID.TTP` with `-l` to receive notes
  through the Falcon's MIDI IN port and play them through its audio output.

Both modes use the Falcon DSP's OPL2 engine and the included General MIDI
instrument bank, with nine simultaneous voices. See
[the player and synthesizer guide](docs/midi-player.md) for startup options,
installation and verification. Emulator gates pass; audio audition, successful
physical-Falcon playback and actual MIDI-port input remain unestablished.

## Download for a Falcon

Download [OPL2.ZIP](https://github.com/AnimaInCorpore/F030OPL2/releases/download/v0.2.0/OPL2.ZIP)
from [GitHub Releases](https://github.com/AnimaInCorpore/F030OPL2/releases).
Extract it and transfer `F030MID.TTP` and `DEMO.MID` to the Falcon.
Double-click the player and enter `DEMO.MID`, your own `.mid` filename, or `-l`
for live MIDI IN synthesis.
The ZIP includes a `.TOS` copy, 40-column instructions, source notices and build identity;
the player files are also available as separate downloads. No compilation or
separate DSP image is needed on the Falcon. `v0.2.0` is a prerelease for
hardware testing; physical playback and MIDI-port input are not formally
verified, though testers report both working on a Falcon under TOS. Under
FreeMiNT it plays on an idle system and drops out beside busy programs.

## Build and verify

Use an MSYS2 login shell on Windows with `/ucrt64/bin` on PATH. Dependencies:
make, C/C++ compiler, Python 3, DOSBox Staging; calibrated Hatari for DSP gates.
Set `DOSBOX` and `PYTHON` in ignored `local.mk` if needed. The MIDI player's
68030 program also needs the MiNT cross compiler (`m68k-atari-mintelf-g++`,
with headers providing `mint/falcon.h`); put it on PATH or set `M68K_CXX`
in `local.mk`. Set the `HATARI` environment variable to the calibrated emulator
binary for gates; see [the gate environment](docs/releases.md#gate-environment). Gates that
drive the emulator need the shell to find the `/ucrt64/bin` DLLs and a `HOME`.

```sh
git submodule update --init third_party/f030dsp3d
make check
make dsp-gate
make stream-gate
make rhythm-gate
make midi-tos midi-gate midi-live-gate   # the MIDI player: needs the m68k-atari-mintelf cross compiler
```

The MIDI player's other targets: `make midi-host` (the host tool `opl-midi` and its
unit test, part of `make all` and `make check`), `make midi-wav MIDI_FILE=song.mid
WAV=song.wav` (render a file to a WAV on the PC), and `make midi-hatari
MIDI_FILE=song.mid` (play it in the calibrated Hatari with sound). The stream
gate takes `--render-ahead`; the MIDI player takes `-a`/`-n` (its gate uses
`--ahead on|off`). See
[the render-ahead ring](docs/speed-quality.md#render-ahead-ring).

`release/f030opl2.tos` is the standalone stream harness. It reads
`OPLDATA.BIN` (tables/state) and `PLAYDATA.BIN` (period events) beside the
program. Generate a synthetic nine-channel score with:

```sh
tools/opl/build/headless/opl-rt-fixture stress release/OPLDATA.BIN release/EXPECT.BIN --seconds 10 --play release/PLAYDATA.BIN
```

For a captured register trace, use `trace` as the scenario and pass the trace
option after the two output paths:

```sh
tools/opl/build/headless/opl-rt-fixture trace release/OPLDATA.BIN release/EXPECT.BIN --trace path/to/opl-writes.ev --seconds 10 --play release/PLAYDATA.BIN
```

This is a register-stream player and integration foundation; it does not load IMF,
DRO or game music formats directly (MIDI is `f030mid.tos`'s job). `release/oplrt.tos`
is the frame comparison harness; `oplbench.tos` measures the slower exact kernel.

## Scope and limits

The OPL2 path has nine two-operator channels, four waveforms, FM/additive
connections, feedback, envelopes, tremolo, vibrato, key scaling and rhythm
percussion. The core retains OPL3 extensions for regression
compatibility; F030OPL2 does not require those extensions for AdLib playback.

The practical DSP kernel uses 32-frame control blocks (about 0.65 ms), with sample-stamped writes splitting the render span. Envelopes and LFOs
remain block-rate, so synthesis is not chip exact. It is tested against a matching integer host
reference. The exact reference is retained separately; the exact DSP kernel
is a benchmark and is not the real-time playback path. Historical results are
not proof of this checkout. No physical Falcon validation is claimed. Results
recorded during development (the rhythm stream's late period fixed, the DSP's
host-port receive moved out of the render, a render-ahead ring that banks idle time,
real songs from six games with equal aggregate checksums) are in
[speed-quality](docs/speed-quality.md); `docs/validation.json` is the initial validation
record. `rt-stream-gate.py --profile` and `tools/opl/profile-labels.py` show where a
stream's cycles go. The rhythm worst case still uses about 94% of the DSP budget
and the layered 18-channel configuration remains experimental.

## References and origins

The OPL host reference was originally checked against **Nuked-OPL3**; the
production DSP uses the project's approximate, block-rate practical model.
The kernels originated in ScummVM's Falcon OPL work, the initial boot scaffold
in F030SID, and the General MIDI bank in ScummVM's AdLib driver. See
[references and implementation basis](docs/provenance.md) for source snapshots,
model differences, table generation, licensing and verification boundaries.

## Source map

- `tools/opl/dsp/oplrt.asm`: production DSP synthesis and SSI transport.
- `tools/opl/m68k/oplplay.s`: standalone 68030 stream host.
- `tools/opl/opl-practical.h`: register decoder and practical host oracle.
- `tools/opl/opl-kernel.h`: exact host reference.
- `tools/opl/midi/`: the MIDI player: engine (`midi-opl.h`), SMF and live-stream
  readers (`midi-file.h`), the period pipeline, the Falcon program
  (`f030mid.cpp`), the host tool `opl-midi`, the XMIDI converter `xmi2mid.py` and
  the GM bank (`gm-bank.h`, GPL-3.0-or-later). `tools/opl/midi-corpus.py`
  plays a collection of songs through it on the emulated Falcon.
- `src/`: two-stage boot loader, XBIOS definitions and hardware probes.
- [Player guide](docs/midi-player.md): startup, MIDI behavior and limitations.
- [DSP implementation and references](docs/provenance.md): models, layout,
  transport, boot and attribution.
- [Speed and quality](docs/speed-quality.md): optimization measurements and
  remaining work.
- [Releases and validation](docs/releases.md): downloads, gate setup,
  historical records and release preparation.

`make all` builds DSP harnesses and host tools; `make midi-tos` separately
builds the Falcon MIDI player. `src/` also retains hardware-probe
sources, which have no build or gate target in the current Makefile.

The build uses the pinned toolchain submodule under `third_party/f030dsp3d`.
Initialize it before building. Build output and image-header generation stay
inside this project. Source files retain their copyright and license notices;
see `COPYING`.

See [speed and quality work](docs/speed-quality.md) for the measured
optimizations, the profiling method and the ranked list of what is left, and
[the MIDI player](docs/midi-player.md) for the player, its decisions and its
verification.

The published `v0.2.0` player passed its packaged demo in both render-ahead
modes and raw live-input gates. See [validation records](docs/releases.md#validation-records)
for the tested commits, earlier baselines and limits.

To reproduce the published package, use a clean checkout of tag `v0.2.0`
with the build dependencies configured:

```sh
git switch --detach v0.2.0
make midi-release VERSION=v0.2.0
```

This creates `release/v0.2.0/OPL2.ZIP`, direct player downloads, the matching
`F030OPL2-SOURCE.tar.gz` archive and `SHA256.TXT`. Run
`make package-gate VERSION=v0.2.0` to test the packaged demo and player. See [release packaging](docs/releases.md) for validation and publishing.
