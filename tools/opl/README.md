# F030OPL2 tools and kernels

This directory contains the standalone host reference, DSP implementation,
68030 stream harness and shared MIDI pipeline. Build from the repository root;
see [build instructions](../../README.md), [MIDI playback](../../docs/midi-player.md),
[DSP implementation and references](../../docs/provenance.md) and
[releases and gate environment](../../docs/releases.md#gate-environment).

## Source map

| Files | Purpose |
| --- | --- |
| `opl-kernel.h`, `opl-tables.h` | Exact host reference and tables |
| `opl-practical.h`, `opl-practical-tables.h` | Practical register decoder and matching integer host synthesis |
| `dsp/oplrt.asm`, `dsp/oplrttab.inc` | Production block-rate DSP kernel, rhythm synthesis and SSI transport |
| `dsp/opl.asm`, `m68k/oplbench.s` | Exact DSP benchmark, outside the real-time path |
| `m68k/oplrt.s`, `rt-bench-gate.py` | Rendered-frame comparison and synthesis profiling |
| `m68k/oplplay.s`, `rt-stream-gate.py` | Standalone period stream and whole-stream timing/checksum gate |
| `rt-fixture.cpp`, `practical-unit-test.cpp` | Host fixtures and practical-model unit checks |
| `midi/` | Shared engine, SMF/live readers, period pipeline, GM bank, host tool and Falcon player |
| `midi-gate.py`, `midi-corpus.py`, `midi-hatari.py` | MIDI validation, external song corpus runs and audible emulator playback |
| `build-dsp.sh`, `generate-tables.py`, `generate-image-header.py` | Local assembly, table generation and embedded image generation |
| `gate_env.py`, `profile-labels.py` | Local toolchain/calibrated Hatari discovery and profile attribution |

## Build and run

```sh
make check
make dsp-gate stream-gate rhythm-gate
make midi-tos midi-gate midi-live-gate
make midi-wav MIDI_FILE=song.mid WAV=song.wav
make midi-hatari MIDI_FILE=song.mid
```

`make all` builds DSP harnesses and host tools; the MiNT cross-compiled MIDI
player is built by `make midi-tos`. Build output is in `tools/opl/build`,
`build` and `release`, all ignored. No game assets or song collection ships here.
The synthetic gates generate their inputs locally; the downloadable package
includes the generated score as `DEMO.MID`. `make midi-release` packages the
player and source, and `make package-gate` tests the packaged player. See
[release preparation](../../docs/releases.md).

The practical kernel renders at approximately 49.17 kHz with 32-frame controls
and sample-stamped writes. Stream periods are 768 frames. OPL2 playback uses
nine channels; retained OPL3/layered extensions are regression/experimental
paths, not a claim of real-time OPL3 capability. Keep render-ahead mode explicit
when comparing transport timing (`--render-ahead` for the stream gate,
`--ahead on|off` for the MIDI gate).

## Validation and licensing

See [references and implementation basis](../../docs/provenance.md) for the
Nuked-OPL3 comparison reference, kernel origins and MIDI-bank attribution.

The kernels, references, fixtures and MIDI bank are included in this project.
[validation.json](../../docs/validation.json) retains the initial validation
baseline. Source files and the GM bank retain their copyright and license
notices; see [COPYING](../../COPYING).

Dated F030OPL2 measurements and optimization baselines are retained in
[speed-quality](../../docs/speed-quality.md) and
[MIDI verification](../../docs/midi-player.md#verification). DSP benches compare
individual frames; stream and MIDI gates compare aggregate checksums and
counters. A checksum match does not prove equality of every output word.
The practical reference is not a chip oracle. Historical results are not new
runs, and neither audible quality nor physical-Falcon playback is established
by emulator gates.

See [validation records](../../docs/releases.md#validation-records) for the
published player, the earlier development snapshot and their distinct scopes.
