# Speed and quality

Whole-stream frame costs include SSI interrupts, output writes, diagnostics
and bursts of register events. Quality checks must account for aliasing when
considering a lower sample rate.

Contents, in the order the work was done, with where it stands:

- the output loop and rhythm-noise optimizations, and the noise stage's operand
  ring (done, bit exact);
- where the remaining cost is, with a ranked list of further work (items 1 and 3 done; 2, 4 and 5 open);
- the render-ahead ring, which banks the idle time quiet periods leave (done,
  off by default, on when F030MID plays a file);
- the removal of the reset-event burst at start (done).

The figures below are retained historical F030OPL2 measurements from the
initial validation and 2026-10-05 optimization work, not fresh runs of the current checkout.
They use the calibrated Hatari model, not hardware. DSP bench cases compare
individual frames against the practical host reference; stream and MIDI runs
compare aggregate checksums and counters.

## Implemented output-loop optimization

The nine-channel output loop previously wrote the same limited accumulator to
the stereo ring twice, copied it to X1, then separately added X1 twice to the
checksum. It now captures the limited sample in X1 once and pairs each checksum
add with its ring write using DSP parallel moves. Both PCM and silent-PCM paths
save two instructions per frame (about 98,340 instruction dispatches per second).
No oscillator, gain, envelope or sample values change. The DSP program shrank
from 2,078 to 2,069 words after both optimizations.

Under the calibrated Hatari, the one-second nine-channel stress stream's
minimum slack rose from 0.112 ms to 0.214 ms; the emitted checksum was unchanged
at 16,639,700. This is one measured workload, not a general speedup percentage.
The margin remains thin and needs physical-hardware validation.

## Quality features preserved and verified

The kernel retains these quality features:
sample-stamped parameter events split the fixed control blocks; the rhythm
noise advances by the chip's 36 slot clocks using lookup jumps, including exact
catch-up after silent spans. Unit checks cover noise state, every render split,
waveform-select enable, pitch, attack, feedback routing and reset behavior.

Keep the 49.17 kHz codec rate and 32-frame envelope/LFO cadence. This avoids the
extra aliasing of 32.78 kHz while retaining the established real-time kernel.
Saw/pulse polyBLEP corrects discontinuous saw/pulse edges; OPL's modulated sine
path requires its own quality analysis, so it is not transplanted here.

Further quality work should compare the practical kernel against the exact
OPL reference before and after changing envelope cadence, native-rate rendering
or resampling. These are future improvements, not claims about this implementation.

## Rhythm loop optimization

The 36-clock noise jump now pairs a table read with an XOR and runs inline in
the hardware loop. Removing the single-use JSR/RTS and loop-tail NOP follows
the practice of keeping common work straight-line in internal program RAM.
On the one-second rhythm stress stream, late periods dropped from two to one,
with the same checksum (11,209,448).

## Noise stage operand ring

Constants can load in the parallel-move slot of an ALU instruction instead
of spending an instruction (two cycles for a long immediate) on them.
`stage_drum_noise` still spent twelve of its 33 cycles per frame on six
`move #>constant` loads. Its five operands (>> 6, the two-bit mask, the drum
table address, >> 8, >> 16) now sit in a modulo-5 ring in Y internal at `$0070`,
written once at start; `y:(r3)+,y0` rides on the ALU instructions and y1 holds
the byte mask. The row store rides on an `AND`. The loop is 20 cycles per frame.
The dead operand set-up in `mode_drums` went too.

Measured under the calibrated Hatari (frame cycles are the bench gate's, one
second windows; the stream results are `make rhythm-gate` and `stream-gate`):

| case | before | after |
| --- | ---: | ---: |
| bench rhythm, cycles/frame | 263.87 | 254.41 |
| bench phase, cycles/frame | 265.46 | 258.51 |
| bench stress, timing, paths | 286.87, 295.46, 231.64 | unchanged |
| rhythm period 51 (all drums, bass FM, six FM channels), cycles/frame | 317.2 | 306.3 |
| rhythm stream, late periods (64) | 1 | 0 |
| stress stream, late periods / min slack | 0 / 0.214 ms | 0 / 0.214 ms |

Every bench case stays word exact against the practical host reference, and the
stream checksums were unchanged (historical stress window: 10,510,080).
Checksums from different fixtures or stream durations are not comparable. The layered 18-channel
bench is exact and its stream still counts 93 late periods; that
configuration remains experimental. The rhythm worst case is still at 94% of
the 326-cycle budget, so its margin is thin, and nothing has run on hardware.

The cause of the old rhythm miss was found with the new profiling aids, not
guessed: `rt-stream-gate.py --profile` profiles a whole stream including the SSI
interrupts and tracking, and `profile-labels.py` charges the profile to every
label. A period-by-period profile showed the late period was a steady
97% load once the bass drum and tom joined six feedback channels and the other
drums, not an event burst.

## Where the remaining cost is

Per frame, with nine feedback-FM channels (stress, 287 cycles): the two
per-frame stages 184 (the modulator loop is 11 cycles, the serial carrier 9; an
indexed sine read costs two), the block boundary pass about 42, the channel
render loop and operator loaders about 30, SSI and transport about 15. The
boundary pass costs about 74 cycles per operator, dominated by the three-cycle
conditional bit tests. Candidates, not yet done: precomputed per-channel
descriptors to replace the five pointer counters of `render_channels`, carrier
pointers by address arithmetic in `load_carrier*`, and skipping steady
operators in music without tremolo or vibrato.

## Further optimization work (2026-10-05)

(Items 1 and 3 of the ranking below have since been implemented; their
sections follow the list.)

Real music exposed a limit the synthetic gates did not: Falcon 3's `C.MID`
played with one late period and `F.MID` with four, though the aggregate output
checksums matched the reference. A period-by-period profile (a one-shot Hatari DSP
profile of period N, taken with `:<count>` breakpoints on `rp_go` and
`rp_counted`) showed the cause. The late period, 1535 of 2304, cost 336
cycles per frame against its neighbours' 285-300 and the 326-cycle budget.
Only about 7 of the extra 51 cycles were synthesis; about 36 were
`track_halves` and `wait_rx`, the DSP standing in the host-port receive of the
next period's 59-140 events, word by word at the 68030's pace, in the middle
of the render. Receiving the parked refill in the boundary wait avoids this
cost during rendering; the kernel previously received it between every block.

**Applied.** `render_period` no longer calls `early_receive` between blocks;
the refill is taken in the waits after the render, where the DSP has nothing
else to do. Measured under the calibrated Hatari, DSP checksums unchanged:

| song | late periods before | after | tightest slack after |
| --- | ---: | ---: | ---: |
| Falcon 3 `C.MID`, 34 s | 1 | 0 | 20 frames |
| Falcon 3 `F.MID`, 56 s | 4 | 0 | 10 frames |
| stress and rhythm streams, bench, `song.mid`, live input | 0 | 0 | unchanged |

The margin in the densest periods is still a fraction of a millisecond.
`--starve` reports 61 late periods against an expected 62.7 with this change
and without it, so that check's tolerance, not the change, is the issue.

**Ranked work (completed items marked).**

1. *Render-ahead ring at block granularity*: **done, see
   below.**
2. *Cycle-stamped events with a horizon, pushed continuously*. Removes the per-period READY handshake and cuts live-MIDI
   latency from about two periods (31 ms) to a few blocks, since an event no
   longer waits for its period to be assembled.
3. *No reset burst at start*: **done, see the end of this file.**
4. *Steady-state envelope handlers*.
   An operator in sustain without tremolo or vibrato need not rerun the
   boundary pass: it would cut the pass's 42 cycles per frame in music, not in
   the stress case.
5. *Parallel-move constants in the remaining hot code*, as done for the noise
   stage: the five stride counters of `render_channels` and the carrier
   pointers of `load_carrier*`, about 5-6 cycles per frame.
6. *Not worth it here:* a Timer-A producer queue (this host does a few
   hundred cycles of work per period) and saw/pulse polyBLEP (AGENTS.md).

## Render-ahead ring

The stream rendered a half of the SSI ring only once the transmitter had left
it, so a period had exactly one period of time and idle time could not be
banked: a period that cost more was late however quiet the ones before it were.
The render-ahead mode renders whenever the ring has room. With a host-set flag
(`X:$0096`, `OplPractical::SC_RENDER_AHEAD`) it waits at block granularity: before each 32-frame block `slot_wait` returns once the
transmitter has played that block's 64 ring words (immediately in the half the
transmitter is not in, once it is past the slot in the half it is in), and until
then the DSP does the host's work and watches the transmitter. The period is
still the unit of the events and the lateness count; a half is judged late by
a `caught_a`/`caught_b` flag the transmitter-tracking code sets when the
transmitter enters it stale (with render-ahead the transmitter may sit in the
half all along, so its being there says nothing). The slack the kernel reports
is now the ring words before the transmitter reaches the half just rendered,
without the old clamp, so it runs up to two periods.

The render-ahead change produced a 2,148-word DSP program (2,069 after the
initial output-loop work); the internal-program
limit and the check that keeps the kernel clear of the OPL3 Y tables still hold.

Off by default for the stream gates and for live MIDI, on by default when
F030MID plays a file (`-a`, `-n`, `AHEAD.FLG`, `NOAHEAD.FLG`). It costs
latency: the host runs further ahead of the audio (up to about three periods,
47 ms, instead of two), which a file does not mind and a keyboard does.

Measured under the calibrated Hatari, with every DSP checksum equal to the host
reference's and no late period:

| song | tightest slack, off | tightest slack, render-ahead |
| --- | ---: | ---: |
| Falcon 3 `C.MID`, 34 s | 24 frames (0.5 ms) | 428 frames (8.7 ms) |
| Falcon 3 `F.MID`, 56 s | 35 frames (0.7 ms) | 261 frames (5.3 ms) |
| `song.mid`, Falcon 3 `A`, `B`, `D`, `E`, Ultima 4 `Castles`, `Combat` | | 191-538 frames (3.9-10.9 ms) |

The opening of `song.mid`, whose reset burst was its tightest period (58
frames), now leaves 191. The stress and rhythm streams pass with the flag
(`rt-stream-gate.py --render-ahead`; their minimum slack is set by the opening
and does not show the benefit). The stall check, whose expectation drops by one
period in this mode (the transmitter plays the extra banked period before any
replay), counts 59 of 60.7 against 61 of 62.7 without it: the same 1.7-period
shortfall as before this change, which remains unexplained.

## No reset burst at start

The uploaded tables already hold the chip's reset state, yet the decoder's
reset sent 397 events in a song's first period, which was its tightest. For a
freshly loaded DSP (`Pipeline::begin`'s default) only the nine channel output
routing words are sent: the upload leaves them at zero, where the reset state
is "the mix", and zero would mute the channel. Everything else the reset sends
equals what the upload holds. A440's first period now carries 52 events, not
397, `song.mid`'s busiest period 177, not 497, and the host reference's
checksums are unchanged (16,233,456 and 6,797,716). On the DSP every checksum
still equals the host's (`a440`, `song`, Falcon 3 `C`, `F`, Ultima 4
`Combat`, live input), and with render-ahead the tightest slack rose by about
35 frames on each song: `C.MID` 428 to 463, `F.MID` 261 to 296, `song.mid`
191 to 225, `Combat` 538 to 573.

## From F030SID (2026-10-09): host transport and SSI input

Found in F030SID; details in its `docs/dsp-kernel.md` ("Host transport", "SSI
receive route") and `docs/performance.md` ("Pacing and the write queue").
Cycle figures are from the DSP56001 User's Manual (`F030SID/docs/DSP56001_um.pdf`
and a copy in F030MXDRV), in oscillator clocks; one instruction cycle (Icyc) is
two clocks.

- **`Dsp_BlkUnpacked` writes blind after its first word** (TOS 4.02). The uses
  here are safe: the stage-two load into a tight loader loop, and one-word
  transfers. A multi-word block to a kernel that reads between frames loses words.
  `Dsp_BlkHandShake` tests the port before every word, three bytes a word.
- **A host-port word costs the DSP at least 5 Icyc** when it is already waiting:
  `jclr #0,x:m_hsr,*` 6 clocks per test, `movep x:m_hrx,x:(r)+` 4 clocks.
  Everything beyond that is waiting for the 68030, which is the `wait_rx`
  cost found above.
- **SSI receive from DMA playback.** A two-`movep` fast receive interrupt is
  8 clocks (4 Icyc), one stereo track 98,340 interrupts a second, 2.5% of the
  DSP. On an overrun the hardware keeps the older word, sets ROE and uses
  the exception vector `P:$000E` (manual §11.3.2.3.6). Hatari does the opposite:
  it keeps the newer word, sets no flag and always raises `P:$000C`. Hatari
  also loses words in free-running mode whenever a 68030 instruction outlasts
  a slot. ScummVM's `ssi-dma-c2p` (branch `ssi-dma-c2p-test`) has the working
  free-running and handshake kernels and `SSIMIX` for sound beside data.
- **`REP` is not interruptible**, nor are consecutive `REP`s: an SSI interrupt
  waits for the repeat. A 49,170 Hz frame is 325.4 Icyc; with 8 slots a slot
  is 40.7 Icyc.
- **Pass rate matters more than lead for a busy DSP.** On F030SID's heaviest
  tune, small pushes every 5 ms kept the original's result (4-5 overtakes in
  120 s), while one push every 40 ms gave 113. The DSP renders nothing while it
  takes a push at the 68030's pace.

The published-player and earlier development records are indexed in
[releases and validation](releases.md#validation-records). They remain separate
from the historical optimization measurements above.
