# Hatari timing and gate environment

F030OPL2's DSP and MIDI gates require a DSP-calibrated Hatari binary.
The calibration must address DSP clock accounting and CPU-side host-port wait
states. Stock-emulator timing is not a substitute for these gates. Emulated
deadlines remain model results, not physical Falcon measurements.

## Selecting the environment

Set `HATARI` to the calibrated emulator's executable path. Every current
emulator script reads this environment variable through `tools/opl/gate_env.py`.
Explicit configuration makes the gates independent of checkout discovery.
Overrides are not checked for calibration; use a known calibrated build for
timing claims.

```sh
export HATARI=/path/to/calibrated/hatari
make stream-gate
make midi-gate
```

The assembler, vasm/vlink, two-stage loader and TOS 4.02 ROM come from this
project and its pinned toolchain submodule under `third_party/f030dsp3d`.
`DOSBOX` controls the DSP assembler runner; `M68K_CXX` selects the MIDI player's
MiNT cross compiler. `local.mk` can set these Makefile variables. Export
`HATARI` in the shell so the Python gates inherit it.

On Windows run from an MSYS2 login shell with `/ucrt64/bin` on PATH and a
`HOME`. Scripts that require Hatari's control FIFO cannot use a Windows build
without Unix-domain-socket support. Directory junctions used by gates must be
unlinked with `gate_env.unlink_directory`, never recursively removed.

## What the gates measure

The synthesis cadence is 32 frames, the stream period is 768 frames, and the
Falcon codec clock is 25.175 MHz / 512, approximately 49.17 kHz. One DSP
instruction cycle budget is approximately 326 cycles per codec frame at the
Falcon's 16 MIPS throughput.

- `make check`: build and host unit checks; it does not launch Hatari.
- `make dsp-gate`: individual rendered-frame equality against the practical
  integer host reference, plus profiled synthesis cost.
- `make stream-gate rhythm-gate`: whole-stream transport through the 68030,
  DSP host port and SSI; aggregate output checksum equality and no late period.
- `make midi-gate midi-live-gate`: shared MIDI pipeline on the 68030 against
  host predictions, upload equality, counters and aggregate checksum. The file
  target runs render-ahead both on and off; live input defaults to off.

Name render-ahead explicitly in timing records. To measure both stream modes
and retain their results separately:

```sh
make stream-gate rhythm-gate
make stream-gate GATE_ARGS='--render-ahead --output build/stream-gate-ahead'
make rhythm-gate GATE_ARGS='--render-ahead --output build/rhythm-gate-ahead'
```

`rt-stream-gate.py --profile` captures whole-stream DSP work including
transport and SSI interrupts; `profile-labels.py` attributes it to labels.
Retain baseline and changed results rather than overwriting the baseline.
See [speed and quality](speed-quality.md) for dated project measurements and
known starvation-count discrepancies. An aggregate checksum does not establish
individual sample equality or audible quality. Audio audition, physical MIDI
port input and playback on a physical Falcon remain unestablished here.

Fresh verification of source commit `8961ed6` on 2026-10-08 is recorded in
[the current validation snapshot](current-validation.json). Host checks, five DSP bench
cases, one-second stress/rhythm streams in both render-ahead modes, and MIDI
file/live gates passed. The external corpus, starvation and layered gates,
audio audition and hardware playback were not rerun.
