# F030MID: MIDI file player and Falcon MIDI synthesizer

F030MID is a single application with two modes: it plays Standard MIDI Files
(`.mid`) from disk, or turns the Atari Falcon into a live synthesizer receiving
MIDI through its MIDI IN port. Both use the DSP OPL2 engine, the included
General MIDI bank and nine simultaneous voices, with audio sent to the Falcon
DAC at approximately 49.17 kHz.

Download `OPL2.ZIP` from
[GitHub Releases](https://github.com/AnimaInCorpore/F030OPL2/releases) and use
the included `F030MID.TTP`. For a source build, run `make midi-tos` and rename
a copy of `release/f030mid.tos` to `F030MID.TTP`; the TTP extension lets the desktop ask
for parameters. Enter `SONG.MID` to play a file or `-l` for live input. The same
binary provides both modes. Successful physical-Falcon playback and actual
MIDI-port input remain unverified; see [verification](#verification).

```
F030MID.TTP [song.mid] [-l] [-t seconds] [-i bytes.bin] [-a | -n] [-d]
```

- With a file it plays the file plus about two seconds of release tail, then
  returns to the desktop. A key requests an early stop; the file path checks
  the keyboard every 64 periods (about one second).
- With no filename or live-mode argument, startup checks `MIDIIN.RAW` first,
  then `SONG.MID`, then falls back to physical MIDI IN if neither is available.
  `DEMO.MID` is not selected automatically; enter its name in the parameter box.
- `-l` selects the **live synthesizer**: MIDI IN sounds until a key is pressed.
  `-t seconds` approximately limits physical live input only; it does not limit
  file playback or raw-file input. Connect a keyboard/controller or another
  MIDI source to the Falcon's MIDI IN port. The Falcon's DSP produces the sound.
- The DSP renders ahead of the codec by default when playing a file, which gave
  the real songs 4-11 ms of slack instead of about half a millisecond; `-n`
  turns that off, `-a` turns it on for live input. It costs latency (a file does
  not mind), so live input defaults to off. An `AHEAD.FLG` or `NOAHEAD.FLG`
  in the working directory selects the mode when `-a`/`-n` is omitted.
  Explicit `-a`/`-n` overrides the flags; `NOAHEAD.FLG` wins if both exist. See
  [the render-ahead ring](speed-quality.md#render-ahead-ring).
- `-i` feeds a raw MIDI byte file through the live path at the port's rate
  (3,125 bytes a second), which is how the live path is gated; a `MIDIIN.RAW`
  beside the program is selected before `SONG.MID` when no file or live-mode
  argument is given.
- `-d`, or an empty `DIAG.FLG` beside the program, enables machine diagnostics
  and pauses at setup stages. Remove the flag for ordinary playback and gates.

## Listen

In the calibrated Hatari, at real speed with sound:

```sh
make midi-tos
make midi-hatari MIDI_FILE=song.mid
```

`midi-hatari.py` copies the program and the song into `build/midi-play` and starts
Hatari; the player returns to the desktop when the song ends. (It needs the
calibrated Hatari selected through `HATARI`; Hatari passes no
arguments to a program, so the file is staged as `SONG.MID`.) On a PC, without the
emulator, the same engine, decoder and reference chip are built for the host:

```sh
make midi-wav MIDI_FILE=song.mid WAV=song.wav
```

renders the practical host reference (16-bit stereo, 49,170 Hz) to a WAV through
`tools/opl/build/headless/opl-midi`, whose `--play`/`--data` options write
`PLAYDATA.BIN`/`OPLDATA.BIN` so that the older `f030opl2.tos` stream harness can
play the same file too.

### On a Falcon

`release/f030mid.tos` is an ordinary program that reads its parameters from the
command line. For desktop parameter entry rename a copy to `F030MID.TTP`
(TOS Takes Parameters): the desktop then asks for the file name on each start, for example
`FANFARE.MID`, or `-l` for the MIDI IN synthesizer. The file is the same; only
the extension differs.

Keep the player and songs together using the destination filesystem's supported
names (8.3 names for a conventional TOS disk). The program embeds the DSP image
and uploads its tables, so it needs neither `OPLDATA.BIN` nor `PLAYDATA.BIN`.
For disk-image installation, use the partition offset and logical sector size
verified for that image.

If it bombs, run with `-d` (for example `-d TOWNS.MID` in the TTP parameter
box), or create an empty `DIAG.FLG` beside it. Diagnostics print the TOS version
and date, cookie jar, basepage/TPA, text/data/BSS sizes, free ST-RAM and TT-RAM,
memory limits, trap/hook vectors and up to eight VBL queue entries. A vector's
`*` marks an address outside `$e00000..$efffff`, the assumed TOS 4.02 ROM range;
it is a diagnostic clue, not proof of a bad hook on every TOS version.

Setup pauses show the stack pointer before DSP boot, table upload, sound lock,
mode setup, DSP/crossbar connection and stream start. Once streaming, notes
print without pausing. Nothing printed means failure before `main`; the last
visible stage narrows down a later failure. These diagnostics do not establish
that playback works on hardware.

Tables are generated in user mode; only individual host-port transfer blocks
run under `Supexec`, avoiding large frames on TOS's supervisor stack. The player
checks DSP reservation before boot and unlocks the DSP on exit.

## How it works

```
MIDI IN / .MID --> engine (midi-opl.h) --OPL register writes--> decoder
                                                              --> period events --> DSP stream --> DAC
```

- **`midi-opl.h`**: channel state (program, volume, expression, bend and its
  range through RPN 0, modulation, sustain pedal), nine voices, the General MIDI
  instrument bank. Everything it does is a sequence of OPL2 register writes
  stamped with a codec-rate frame. Integer arithmetic only; no heap.
- **`midi-file.h`**: Standard MIDI Files (format 0 and 1, and format 2 of a
  single track, ticks per quarter note; a `MIDI` wrapper as in `.GMD` files is
  looked through) merged into one time-ordered stream, with tick-to-frame conversion done
  in exact integers (a frame is a ratio of integers, so a long file does not
  drift). `MidiStream` parses a live byte stream (running status, real-time
  bytes, system exclusive).
- **`period-stream.h`**: the engine's writes go through the practical decoder,
  the 68030's half of the OPL kernel, and come out as sample-stamped parameter
  events, grouped into the 768-frame periods the DSP stream takes.
- **`f030mid.cpp`**: the Falcon program. It boots the DSP kernel, uploads the
  tables with direct host-port writes, routes the SSI to the DAC, and sends one
  period per refill. The DSP's READY handshake paces the host, so a file plays at
  exactly the DAC's speed. Silent PCM flags keep the OPL as the whole song. The
  first period carries only the nine channel-routing words of the chip's reset:
  the uploaded tables hold the rest.

The 68030 build and the host build compile the same headers; the Falcon program
is C++ built with the MiNT cross compiler (`M68K_CXX` in `local.mk`). The
binary requires no FPU: it uses soft-float 68030 code linked against the
68000 soft-float runtime.

### Decisions worth knowing

- **Pitch mapping uses a one-octave f-number offset.** The included AdLib bank
  uses an f-number convention one octave low: 74 of its 128
  melodic patches use frequency multiplier 2 on the carrier. Applying the chip's
  own formula to the note an octave down, with the patch's multiplier, gives true
  pitch (a 440 Hz note measures 439 Hz in the rendered output). Percussion
  patches use the same convention.
- **Percussion is melodic.** Channel 10 plays the bank's 39 percussion patches as
  ordinary two-operator voices, each ended by its own duration, not the chip's
  rhythm mode. All nine voices stay available to the music.
- **Voice stealing**: a never-used voice, else the one released longest ago, else
  the oldest sounding note. Re-striking a held note re-triggers its own voice.
- **Levels** follow the instrument bank: velocity raises the instrument's own levels
  by each operator's sensitivity, channel volume and expression (CC7 x CC11)
  scale the carrier, and the modulator too when the patch is additive.

### Instrument bank

The patches and percussion map come from ScummVM's AdLib driver; see
[the bank attribution](provenance.md#midi-instrument-bank).

`gm-bank.h` includes 128 melodic and 39 percussion patches and the key map.
It is supplied with the project; playback and builds need no external bank
source. It carries its copyright and GPL-3.0-or-later notice. The engine,
allocator, pitch calculation, reader and player are included here.

## Verification

The published `v0.2.0` player passed host checks and packaged-player gates on
2026-10-10 (`v0.1.0` on 2026-10-08): the demo in both render-ahead modes and raw live input with it off.
Uploaded tables and DSP/host checksums matched, with no event overflow or late
period. See [validation records](releases.md#validation-records) for source
commits, exact results, earlier development checks and test limitations.

The following tables retain the earlier measurements for comparison.

`make check` runs the engine, file and stream-parser checks (`midi-test.cpp`:
pitch, voice allocation and stealing, the pedal, volume, bend, percussion
durations, tick-to-frame exactness, running status, damaged and unsupported
files).

`make midi-gate` and `make midi-live-gate` (`midi-gate.py`, under the
DSP-calibrated Hatari) play a song and a live byte stream through `F030MID.TOS`
and compare with the host tool's prediction. Results of 2026-10-05:

| input | periods | notes | parameter events | DSP checksum vs host | late periods |
| --- | ---: | ---: | ---: | --- | ---: |
| `a440.mid`, one second | 193 | 1 | 54 | equal (16,233,456) | 0 |
| `song.mid`, 13.5 s, five tracks, tempo change, GM reset, bend, pedal, drums | 990 | 138 | 2,884 | equal (6,797,716) | 0 |
| `live.bin` as MIDI IN (`-i`), GM reset, running status, clock bytes, bend, drums | 247 | 7 | 205 | equal (7,781,472) | 0 |

The checksum is the sum of every limited output word over the whole run,
modulo 2^24. A
match, together with the counters and upload checks, is an integration check
against the host reference; it does not prove that every individual word agrees.
The gate also checks that the player's uploaded tables are byte for byte the bench fixture's.
These dated results predate the later DSP reservation, user-mode table-upload
and diagnostic changes; rerun the gates to validate the current build.

After reset-burst removal, the recorded `song.mid` minimum slack is 225 frames
(4.6 ms) with render-ahead and 58 frames without. Real songs, which are not shipped here, play the same
way (see below).

### Songs to try

The release includes a generated synthetic `DEMO.MID`; its generator is in
`tools/opl/midi/make-test-midi.py`. No external song collection ships here.
Supply a collection of Standard MIDI Files or supported wrapped/XMIDI files from your own filesystem. The corpus
runner accepts explicit paths and does not require another project checkout.

- `tools/opl/midi/xmi2mid.py <xmi file or directory> <out dir>` converts XMIDI to
  SMF (notes carry lengths there, delays are sums of bytes, tempo is constant,
  and controllers 110-120 are the format's own and are dropped; 120 would read as
  "all sound off"). Loops are not unrolled.
- The reader accepts `.GMD`'s wrapper and format 2 files of a single track.
- `tools/opl/midi-corpus.py <paths...> --output DIR [--gate N]` converts what
  needs converting, measures every song on the host, plays the N densest (or
  `--gate-all`) through F030MID on the emulated Falcon, and prints a table.

Results of 2026-10-05, render-ahead on, every DSP checksum equal to the host's:
244 songs (Falcon 3, Underworld 1 and 2, Panzer General, Ultima 4) measured on
the host and the 20 densest played on the emulated Falcon, 16 minutes of music,
31,284 notes: no late period, tightest slack 139 frames (2.8 ms). The four TIE
Fighter songs: no late period, tightest 340 frames. These corpus runs checked aggregate
checksums, not individual output words. Earlier runs played Falcon 3
`A`, `B`, `C`, `D`, `E`, `F`, Underworld `AW05` and `UW05` and Ultima 4
`Castles` and `Combat` the same way. A DSP status word holds the
rendered-period count in twelve bits, so the gate compares it modulo 4,096 for
songs over about 64 s.

### Latency

A live note's delay is the wait for the host's next period (up to 15.6 ms), the
period's own length before the DSP renders it, and the ring: roughly 25 to 45
ms without render-ahead, which is why live input defaults to it off, and up to a
period or two more with it. **This has not been measured.** The ranked list in
[speed-quality](speed-quality.md#further-optimization-work-2026-10-05)
holds the remedy, a continuous cycle-stamped event stream, to build
if a keyboard is the intended input.

### Not established

- **No audio has been auditioned.** Pitch was measured (A4 renders at 439 Hz)
  but how a given file sounds, drums especially, is
  not judged here. Use `make midi-wav` and listen.
- **The `Bconin` read of the live port is not exercised**: this Hatari build has
  no MIDI input (PortMidi is not compiled in), so the live path is gated through
  `-i` with the same code after the byte is read.
- Successful physical-Falcon playback and timing under a loaded 68030 are not
  established by the recorded gates.
- **Under FreeMiNT it plays on an idle system; beside a busy process it has
  late periods (dropouts).** Each period `submitSuper` runs under `Supexec` and
  spins on the host port until the DSP answers `REPLY_READY`. That spin lasts at
  most one period, so the system keeps running: unlike F030SID's earlier player,
  which spun for the whole tune and left a busy neighbour 0.4% of its speed.
  F030MID locks the DSP (`Dsp_Lock`) and the sound system, and SIGINT, SIGTERM,
  SIGQUIT and SIGHUP stop it like a key, so both are released (before, a signal
  killed it with the DSP still streaming and both locks held).

  F030SID's remedy, waiting in user mode, was tried and dropped. The DSP
  buffers two periods (31 ms), shorter than a MiNT timeslice, so any wait MiNT
  can preempt lets a busy neighbour hold the CPU past the deadline: with
  `Syield` between polls the test song had 465 of 990 periods late and took
  22.3 s instead of 16.7; polling without `Syield` gave 433, and raising the
  priority with `Prenice` 494. (`Fselect` is no better: MiNT rounds it up to
  its 20 ms tick, longer than a period.) Playing well beside a busy process
  needs deeper buffering on the DSP, for example a queue of several staged
  periods of events, which a keyboard's latency must be kept out of.

  Measured 2026-10-10 in the DSP-calibrated Hatari (`--fpu 68882`), TOS 4.02,
  FreeMiNT 1.19 snapshot `648983e1` with memory protection off, started by a
  small `Pvfork`/`Pexec` program, not bash (see F030SID `docs/player.md`).
  `build/midi-test/song.mid`, 15.5 s, rendering ahead:

  | Situation | Before | Now |
  | --- | --- | --- |
  | Alone | 990 periods, none late, checksum equal | the same |
  | Beside a busy loop, whole song | 42 late; the loop kept 62% | 33 late; 63% |
  | Beside a busy loop, SIGINT after 10 s | killed, no cleanup | stopped in 0.25 s, locks released |

  The late counts vary from run to run by a few periods. Not tested: memory
  protection on, the XaAES desktop, live input under MiNT, a physical Falcon
  running MiNT.

### Limits

Nine voices, mono (the OPL2 has no panning). Not implemented: aftertouch, fine
tuning, SMPTE-timed files, format 2 files of several tracks, file looping (an XMIDI
song's loops are not unrolled either), and the chip's rhythm mode. The modulation
wheel switches vibrato on at values of 32 or greater. Percussion keys outside
GM's 35-81 are ignored.

The player writes integration counters to `RESULT.BIN` in its working directory
when it exits. This file is used by the gates; it is not an audio recording.
SMF playback reads at most 64 tracks and loads the file into RAM; minimum RAM
for real hardware has not been qualified.
