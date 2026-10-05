# Speed and quality informed by F030SID

SID's measured frame costs include work that average synthesis benchmarks can
miss: SSI interrupts, output writes, diagnostics and bursts of register events.
Its quality work also shows why reducing sample rate to save cycles is costly.

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

The current local ScummVM sources have improvements beyond their older prose:
sample-stamped parameter events split the fixed control blocks; the rhythm
noise advances by the chip's 36 slot clocks using lookup jumps, including exact
catch-up after silent spans. Unit checks cover noise state, every render split,
waveform-select enable, pitch, attack, feedback routing and reset behavior.

Keep the 49.17 kHz codec rate and 32-frame envelope/LFO cadence. This avoids the
extra aliasing of 32.78 kHz while retaining the established real-time kernel.
SID-style polyBLEP corrects discontinuous saw/pulse edges; OPL's modulated sine
path requires its own quality analysis, so it is not transplanted here.

Further quality work should compare the practical kernel against the exact
OPL reference before and after changing envelope cadence, native-rate rendering
or resampling. These are future improvements, not claims about this import.

## Rhythm loop optimization

The 36-clock noise jump now pairs a table read with an XOR and runs inline in
the hardware loop. Removing the single-use JSR/RTS and loop-tail NOP follows
SID's approach of keeping common work straight-line in internal program RAM.
On the one-second rhythm stress stream, late periods dropped from two to one,
with the same checksum (11,209,448).

## Noise stage operand ring (F030SID's parallel-move discipline)

SID's kernel loads constants on the parallel-move slot of an ALU instruction
instead of spending an instruction (two cycles for a long immediate) on them.
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
stream checksums are unchanged (stress 10,510,080). The layered 18-channel
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
