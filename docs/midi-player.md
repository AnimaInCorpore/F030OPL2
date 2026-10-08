# F030MID: MIDI file player and Falcon MIDI synthesizer

F030MID is a single application with two modes: it plays Standard MIDI Files
(`.mid`) from disk, or turns the Atari Falcon into a live synthesizer receiving
MIDI through its MIDI IN port. Both use the DSP OPL2 engine, the included
General MIDI bank and nine simultaneous voices, with audio sent to the Falcon
DAC at approximately 49.17 kHz.

Build with `make midi-tos`. For desktop use, rename a copy of
`release/f030mid.tos` to `F030MID.TTP`; the TTP extension lets the desktop ask
for parameters. Enter `SONG.MID` to play a file or `-l` for live input. The same
binary provides both modes. Successful physical-Falcon playback and actual
MIDI-port input remain unverified; see [verification](#verification).

```
F030MID.TOS [song.mid] [-l] [-t seconds] [-i bytes.bin] [-a | -n] [-d]
```

- With a file it plays the file and returns to the desktop at the end (or on a
  key press). With no argument it plays `SONG.MID` if one is beside the program.
- With `-l`, or with no argument and no `SONG.MID`, it is a **live synthesizer**:
  bytes arriving on MIDI IN sound until a key is pressed (`-t` limits it).
  An ST, a PC interface or any keyboard on the Falcon's MIDI IN port will do;
  the Falcon itself is the synthesizer. A plain ST cannot be the synthesizer,
  because the OPL2 runs on the Falcon's DSP.
- The DSP renders ahead of the codec by default when playing a file, which gave
  the real songs 4-11 ms of slack instead of about half a millisecond; `-n`
  turns that off, `-a` turns it on for live input. It costs latency (a file does
  not mind), so live input defaults to off. An `AHEAD.FLG` or `NOAHEAD.FLG`
  beside the program selects the mode when the launcher passes no arguments.
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

writes the DSP's output (16-bit stereo, 49,170 Hz) as a WAV through
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
Falcon has no FPU, so the code is soft-float 68030 linked against the 68000
(soft-float) runtime.

### Decisions worth knowing

- **Pitch is one octave below the note.** The included AdLib bank
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

Current-build verification (2026-10-08, source commit `8961ed6`) is recorded in
[current-validation.json](current-validation.json). All 158 MIDI host checks
passed. `song.mid` passed in render-ahead on and off modes, and `live.bin` passed
through the raw-input live path: matching DSP/host checksums, matching uploaded
tables, no event overflow and no late period. The external song corpus and
physical MIDI port were not rerun; no audio was auditioned or hardware tested.

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

The checksum is the sum of every limited output word over the whole run. A
match, together with the counters and upload checks, is an integration check
against the host reference; it does not prove that every individual word agrees.
The gate also checks that the player's uploaded tables are byte for byte the bench fixture's.
These dated results predate the later DSP reservation, user-mode table-upload
and diagnostic changes; rerun the gates to validate the current build.

The tightest period of `song.mid` leaves 225 frames (4.6 ms) of slack with
render-ahead (58 without). Real songs, which are not shipped here, play the same
way (see below).

### Songs to try

Songs do not ship with this repository. Supply a collection of Standard MIDI
Files or supported wrapped/XMIDI files from your own filesystem. The corpus
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

### Limits

Nine voices, mono (the OPL2 has no panning). Not implemented: aftertouch, fine
tuning, SMPTE-timed files, format 2 files of several tracks, file looping (an XMIDI
song's loops are not unrolled either), and the chip's rhythm mode. The modulation wheel switches vibrato on above 32. Percussion keys outside
GM's 35-81 are ignored.
