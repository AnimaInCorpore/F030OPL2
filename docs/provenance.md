# References and implementation basis

F030OPL2 targets the Yamaha YM3812 (OPL2), the two-operator FM chip used by
AdLib sound cards. Its production player uses a practical integer model built
for the Falcon DSP56001. The references below explain the implementation's
basis; they are not additional project checkouts required to build or play it.

## OPL reference models

| Component | Basis and role |
| --- | --- |
| `tools/opl/opl-kernel.h` | Exact two-operator host reference, originally checked sample by sample against Nuked-OPL3. Covers nine-channel OPL2 and eighteen-channel two-operator OPL3, including rhythm; hardware four-operator pairing is outside its scope. |
| `tools/opl/opl-practical.h` | Production decoder and practical host synthesis reference. Uses DSP-compatible arithmetic, codec-rate rendering and 32-frame envelope/LFO controls with sample-stamped events. This is the model the current DSP gates compare against. |
| `tools/opl/dsp/opl.asm` | DSP transliteration of the exact synthesis loop, retained for benchmarking rather than real-time playback. |
| `tools/opl/dsp/oplrt.asm` | DSP implementation of the practical model, with SSI streaming and optional render-ahead. |
| `tools/opl/generate-tables.py` | Computes the log-sine and exponential tables from mathematical formulas; derives practical envelope tables from the exact envelope machine and retimes them to the codec rate. |

**Nuked-OPL3 is the original chip-model comparison reference**, not the
production renderer. The original exact-kernel comparison operated at the
reference's native rate, approximately 49,716 Hz. The production DSP instead
runs at approximately 49,169.92 Hz and updates envelopes/LFOs in control
blocks, so equality against the practical model is not sample equality against
the chip model.

Nuked-OPL3 models OPL3, not every YM3812-specific behavior. In particular,
F030OPL2 models the OPL2 waveform-select enable (register `$01`, bit 5)
explicitly: while clear, operators use sine regardless of their waveform
registers. Original comparisons supplied the effective waveform to the OPL3
reference; they did not independently establish that enable bit's behavior on
physical OPL2 hardware.

The header comments and table generator retain the original comparison basis.
The external Nuked comparison harness and its historical detailed results are
not included here and are not part of `make check`. Current frame and stream
verification is documented in [the gate environment](hatari-timing.md) and
[current-validation.json](current-validation.json). A stream checksum match
is weaker than individual-frame comparison.

## Implementation origins

The host kernels, DSP kernels and register-stream harnesses originated in
ScummVM's Falcon OPL work under
`devtools/atari-falcon030/tools/foa-opl3`, at source snapshot
`53ba4e68ed4801844023de4bff54ac93529a4565`. F030OPL2 adapted them into a
standalone build with local fixtures, toolchain paths, DSP images and gates,
then added stream optimizations and the MIDI player.

The initial boot/toolchain scaffold came from F030SID snapshot
`ab1fa5b740022f4c853b41b6cd48fa9b9f3ca992`. Its performance approach informed
whole-stream measurement, parallel-move optimization and render-ahead buffering.
These are implementation origins; the current build uses the files and pinned
toolchain submodule in this repository. See [the source map](../README.md#source-map).

## MIDI instrument bank

`tools/opl/midi/gm-bank.h` contains 128 melodic and 39 percussion patches,
plus the percussion key map, extracted from ScummVM's `audio/adlib.cpp` by
`tools/opl/midi/extract-gm-bank.py`. Its encoding, velocity sensitivity and
volume conventions follow that AdLib driver. The generated header carries
its ScummVM attribution and GPL-3.0-or-later notice.

The committed bank is used directly; normal builds do not run the extractor
or require its source checkout. The MIDI engine, voice allocator, readers,
period pipeline and Falcon player are implemented within F030OPL2. See
[the MIDI player](midi-player.md) for behavior and limits.

## Hardware and licensing references

The DSP56000/DSP56001 User's Manual informs fractional arithmetic,
address-generation scheduling, hardware-loop restrictions and SSI setup.
The manual is not bundled. The Falcon host uses XBIOS DSP and sound calls;
[the implementation notes](dsp56001-notes.md) describe the current setup.
Emulator timing remains a model and does not establish physical playback.

Source files retain their original copyright and license notices. See
[COPYING](../COPYING), the instrument-bank header and the pinned toolchain's
own notices for the applicable terms. Historical reference comparisons are
not new results of the current checkout.
