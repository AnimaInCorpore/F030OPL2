# A 68k host for programs that make OPL2 writes themselves

`tools/opl/m68k/opldec.s` and `tools/opl/m68k/oplhost.s` let an assembly
program - a game's own AdLib music driver - play through the practical DSP
kernel without the C++ MIDI pipeline. First user: the F030Comanche port
(`OPL2=1` 68060 build), whose MS3 driver hands its OPL2 register writes to
`opl_host_write`.

## The pieces

- **`opldec.s`** is `OplPractical::Decoder` (`opl-practical.h`) for an OPL2:
  nine channels, eighteen operators, register bank 0. Same register image,
  same dirty marks, same derived words in the same order, same shadow
  suppression; `opl_dec_begin` is `OplMidi::Pipeline::begin`'s fresh-machine
  reset and `opl_dec_take` is `takePeriod`. The increment's 64-bit product is
  done in two 32-bit halves with the same floor, since the 68060 has no 64-bit
  multiply.
- **`oplhost.s`** boots the kernel (`oplrt_image.i`), uploads an image made by
  `tools/opl/opl-upload-image` (the blocks `OplMidi::uploadTables` sends: the
  upload half of `OPLDATA.BIN`), routes the DSP to the DAC at 49.17 kHz, and
  serves the stream's refills from a periodic interrupt as a state machine -
  REFILL, READY, payload, acknowledgement, one step a call, never waiting on
  the DSP. Each payload carries the decoder's events of the next period and,
  through `opl_host_pcm_hook`, 192 host samples (signed 8-bit, sent as
  `sample << 14`), which the kernel expands and adds to the FM mix.
- Writes are stamped in the period the next refill carries, at its start plus
  the time `opl_host_clock` has counted since the last refill (5 ms steps at
  a 200 Hz Timer C), never past its last frame and never into a period
  already sent.

## Verification

`tools/opl/opldec-gate.py` runs register streams through `opldec-ref.cpp`
(the C++ Decoder as the Pipeline drives it) and through `opldec.s` in a TOS
program under Hatari, and compares every period's wire words and the overflow
flag byte for byte. Development run, 2026-10-15, F030Arcade's Hatari 2.6.1-devel
(2026-09-22 build), EmuTOS: 10 generated streams (every OPL2 register class,
rhythm section included, up to 20,000 writes) and 22 register streams of a
game's music (F030Comanche's captures, not in this repository) - **32 of 32
identical on the 68030 and on the 68060.**

`oplhost.s` was run inside the game on Hatari's emulated DSP: kernel booted
and uploaded, 2,041 periods served in 40 s of attract, no protocol error, no
late period (the kernel's own counter), and a recording of the output plays
the music and the mixed PCM. Not audited against the kernel's checksum for
these payloads, not auditioned by the author, not run on a Falcon.

## Limits

- OPL2 only (bank 0, nine channels); `opl_dec_write` ignores bank 1.
- The PCM level (`sample << 14`, half the kernel's PCM range) is a choice, not
  a measured balance against the FM.
- The refill runs with interrupts masked for its payload (as `oplplay.s`
  does); a dense period's payload is a few hundred host-port words.
