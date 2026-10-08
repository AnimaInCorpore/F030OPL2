# DSP implementation and references

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
verification and its limits are documented in
[releases and validation](releases.md#validation-records). A stream checksum match
is weaker than individual-frame comparison.

## Arithmetic and address scheduling

The DSP data paths are 24-bit with 56-bit accumulators. Fractional `MPY`/`MAC`,
accumulator limiting and parallel moves must preserve the practical host
reference's integer result. A checksum match alone is weaker than the DSP
bench's individual-frame comparison.

Allow an independent instruction between writing an address register and using
it indirectly. Indexed addressing pairs `Rn` with the same-numbered `Nn`, and
address arithmetic obeys that pointer's `Mn` modifier. Restore temporary
modifiers before returning to a linear walker. The noise stage intentionally
uses a modulo-5 operand ring in internal Y memory at `$0070`.

A zero hardware-loop count represents 65,536 iterations, so guard variable
`DO`/`REP` counts. Follow the DSP56000/DSP56001 User's Manual's restrictions on
instructions near loop ends. The manual is not bundled in this repository.
Assembly success alone does not establish correct scheduling or state updates.

## Kernel and memory layout

The production OPL2 configuration has nine two-operator channels. The decoder
supplies sample-stamped parameter events: rendering splits at those events,
while envelopes and LFOs retain the 32-frame control cadence. Rhythm noise
advances by 36 slot clocks per frame, including catch-up after silent spans.
The 18-channel layered extension is experimental and misses the real-time
budget at full occupancy.

The reset vector is at `P:$0000`, SSI vectors at `P:$0010/$0012`, hot code
starts at `P:$0080`, and cold code starts at `P:$2000`. External P and Y alias
on the Falcon; code placement must remain clear of Y tables. Consult the
assembly's memory map and the image generator's bounds checks before moving
code or tables. `BCR` is explicitly cleared at kernel start: reset's fifteen
wait states must not be left active.

## SSI and stream transport

The host routes DSP transmit to the DAC with no handshake, using the
25.175 MHz clock divided by 512 (49,169.921875 frames/s). DSP setup uses
`CRA=$4100`, `CRB=$5a00`, `PCC=$1f8` and `IPR=$3000`. Port C must actually
select the SSI pins; emulator output alone cannot establish physical routing.

The ring at `X:$1000` holds 3,072 interleaved stereo words: two 768-frame
periods. `r6/m6` belong to the SSI transmitter. The normal fast interrupt
writes one prepared word; the exception path reads SSI status before writing
TX. Synthesis must not disturb the transmitter's registers.

The current stream commands are `$09` start, `$0a` refill, `$0b` stop,
`$0c` status, `$0d` checksum and `$0e` minimum margin. A refill transfers ordered
parameter events and the PCM flag/payload after a `$524459` READY reply. MIDI
sends no PCM. The standalone harness can supply 192 mono PCM points expanded
to the codec period. The status packs late periods above a twelve-bit rendered
period count, which long-song gates compare modulo 4,096.

The next refill is received during waits after rendering, rather than between
render blocks. Optional render-ahead (`X:$0096`) waits for space block by block
and banks idle time in the ring. File MIDI playback defaults to on; live input
and standalone stream gates default to off. See
[the render-ahead design](speed-quality.md#render-ahead-ring).

## Two-stage boot and table upload

`src/dsp/stage2_loader.asm` provides a small `Dsp_ExecBoot` image. The final
program reserves `P:$0040-$007f` for the loader while it receives sparse
address/count/data sections. The stream magic is `$4d584c`; acknowledgement
is `$4c4f41`, followed by entry through the replaced reset vector.
`tools/generate_dsp_stage2.py` validates section overlap, loader-gap overlap,
P-memory bounds and bootstrap size. Generated images stay in ignored build
paths; `make dsp` regenerates them before embedding them in the 68030 hosts.

F030MID builds tables in user mode because that work needs several KB of
stack. Only each direct host-port transfer block runs under `Supexec`, keeping
large table-generation frames off TOS's small supervisor stack. It checks
`Dsp_Reserve(16, 16)` before boot and releases the DSP on exit. Its `-d`
diagnostics pause at sound setup stages; see [the MIDI player](midi-player.md).

## Development checks and probes

Run `make check`, then the affected DSP/SSI gates in the
[calibrated environment](releases.md#gate-environment). Kernel changes require
individual-frame comparison and whole-stream timing including transport;
transport changes require both render-ahead modes. Shared MIDI pipeline changes
also require `make midi-gate midi-live-gate`. Retain baseline and changed results,
and identify what was not auditioned or run on hardware.

`src/dsp/dspprobe.asm`, `src/dsp/ratetest.asm` and their 68030 hosts are retained
hardware-probe sources. The current Makefile does not build or gate them;
their presence is not evidence that OPL playback ran on hardware.

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
[the SSI and stream transport notes](#ssi-and-stream-transport) describe the
current setup.
Emulator timing remains a model and does not establish physical playback.

Source files retain their original copyright and license notices. See
[COPYING](../COPYING), the instrument-bank header and the pinned toolchain's
own notices for the applicable terms. Historical reference comparisons are
not new results of the current checkout.
