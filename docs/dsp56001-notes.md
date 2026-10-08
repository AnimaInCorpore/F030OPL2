# DSP56001 implementation notes

These notes describe F030OPL2's current practical kernel in
`tools/opl/dsp/oplrt.asm`. The exact kernel in `dsp/opl.asm` is retained as a
benchmark; it is not the real-time player.

The [reference-model notes](provenance.md#opl-reference-models) identify
Nuked-OPL3 and distinguish the exact host model from the practical renderer.

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

## Verification

Run `make check`, then the affected DSP/SSI gates using the
[calibrated environment](hatari-timing.md). Kernel changes require frame equality
against the practical reference and whole-stream timing including transport.
Transport changes require both render-ahead modes; shared MIDI pipeline changes
also require `make midi-gate midi-live-gate`. Preserve dated baseline and changed
records. The practical model is not an exact YM3812 oracle. No current audio
audition or physical-Falcon validation is claimed by these notes.

`src/dsp/dspprobe.asm`, `src/dsp/ratetest.asm` and their 68030 hosts are retained
hardware-probe sources from the scaffold. The current Makefile does not build
or gate them; their presence is not evidence that OPL playback ran on hardware.
