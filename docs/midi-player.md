# F030MID: a real-time MIDI player for the emulated AdLib

`F030MID.TOS` turns the Falcon into an OPL2 synthesizer. It plays Standard MIDI
Files, or acts as a synthesizer for the Falcon's own MIDI IN port, and the DSP
renders the OPL2 at 49.17 kHz into the DAC.

```
F030MID.TOS [song.mid] [-l] [-t seconds] [-i bytes.bin]
```

- With a file it plays the file and returns to the desktop at the end (or on a
  key press). With no argument it plays `SONG.MID` if one is beside the program.
- With `-l`, or with no argument and no `SONG.MID`, it is a **live synthesizer**:
  bytes arriving on MIDI IN sound until a key is pressed (`-t` limits it).
  An ST, a PC interface or any keyboard on the Falcon's MIDI IN port will do;
  the Falcon itself is the synthesizer. A plain ST cannot be the synthesizer,
  because the OPL2 runs on the Falcon's DSP.
- `-i` feeds a raw MIDI byte file through the live path at the port's rate
  (3,125 bytes a second), which is how the live path is gated; a `MIDIIN.RAW`
  beside the program is taken as that file.

## Audition on a PC

The same engine, decoder and reference chip are built for the host:

```sh
make midi-host
tools/opl/build/headless/opl-midi song.mid --wav song.wav
```

writes the DSP's output (16-bit stereo, 49,170 Hz) as a WAV. `--play`/`--data`
write `PLAYDATA.BIN`/`OPLDATA.BIN` so that the older `f030opl2.tos` stream
harness can play the same file too.

## How it works

```
MIDI IN / .MID --> engine (midi-opl.h) --OPL register writes--> decoder
                                                              --> period events --> DSP stream --> DAC
```

- **`midi-opl.h`**: channel state (program, volume, expression, bend and its
  range through RPN 0, modulation, sustain pedal), nine voices, the General MIDI
  instrument bank. Everything it does is a sequence of OPL2 register writes
  stamped with a codec-rate frame. Integer arithmetic only; no heap.
- **`midi-file.h`**: Standard MIDI Files (format 0 and 1, ticks per quarter
  note) merged into one time-ordered stream, with tick-to-frame conversion done
  in exact integers (a frame is a ratio of integers, so a long file does not
  drift). `MidiStream` parses a live byte stream (running status, real-time
  bytes, system exclusive).
- **`period-stream.h`**: the engine's writes go through the practical decoder,
  the 68030's half of the OPL kernel, and come out as sample-stamped parameter
  events, grouped into the 768-frame periods the DSP stream takes.
- **`f030mid.cpp`**: the Falcon program. It boots the DSP kernel, uploads the
  tables with direct host-port writes, routes the SSI to the DAC, and sends one
  period per refill. The DSP's READY handshake paces the host, so a file plays at
  exactly the DAC's speed and a live note is heard within about two periods
  (31 ms) of its byte arriving. Silent PCM flags keep the OPL as the whole song.

The 68030 build and the host build compile the same headers; the Falcon program
is C++ built with the MiNT cross compiler (`M68K_CXX` in `local.mk`). The
Falcon has no FPU, so the code is soft-float 68030 linked against the 68000
(soft-float) runtime.

### Decisions worth knowing

- **Pitch is one octave below the note.** The instrument bank comes from
  ScummVM's AdLib driver, whose f-number table sits an octave low: 74 of its 128
  melodic patches use frequency multiplier 2 on the carrier. Applying the chip's
  own formula to the note an octave down, with the patch's multiplier, gives true
  pitch (a 440 Hz note measures 439 Hz in the rendered output). Percussion
  patches use the same convention.
- **Percussion is melodic.** Channel 10 plays the bank's 39 percussion patches as
  ordinary two-operator voices, each ended by its own duration, not the chip's
  rhythm mode. All nine voices stay available to the music.
- **Voice stealing**: a never-used voice, else the one released longest ago, else
  the oldest sounding note. Re-striking a held note re-triggers its own voice.
- **Levels** follow ScummVM's driver: velocity raises the instrument's own levels
  by each operator's sensitivity, channel volume and expression (CC7 x CC11)
  scale the carrier, and the modulator too when the patch is additive.

### Provenance

`gm-bank.h` (128 melodic and 39 percussion patches and the key map) is generated
by `extract-gm-bank.py` from ScummVM's `audio/adlib.cpp`, GPL-3.0-or-later like
this project; the file carries that notice. The velocity and volume arithmetic
follows the same driver. The engine, the allocator, the pitch calculation, the
reader and the player are this project's.

## Verification

`make check` runs 154 engine, file and stream-parser checks (`midi-test.cpp`:
pitch, voice allocation and stealing, the pedal, volume, bend, percussion
durations, tick-to-frame exactness, running status, damaged and unsupported
files).

`make midi-gate` and `make midi-live-gate` (`midi-gate.py`, under the
DSP-calibrated Hatari) play a song and a live byte stream through `F030MID.TOS`
and compare with the host tool's prediction. Results of 2026-10-05:

| input | periods | notes | parameter events | DSP checksum vs host | late periods |
| --- | ---: | ---: | ---: | --- | ---: |
| `a440.mid`, one second | 193 | 1 | 399 | equal (16,233,456) | 0 |
| `song.mid`, 13.5 s, five tracks, tempo change, GM reset, bend, pedal, drums | 990 | 138 | 3,229 | equal (6,797,716) | 0 |
| `live.bin` as MIDI IN (`-i`), GM reset, running status, clock bytes, bend, drums | 247 | 7 | 550 | equal (7,781,472) | 0 |

The checksum is the sum of every limited output word over the whole run, so a
match means the 68030's register writes, the decoder events, the stream
protocol and the DSP render all agree with the host reference. The gate also
checks that the player's uploaded tables are byte for byte the bench fixture's.
The tightest period of `song.mid` leaves 58 frames (1.2 ms) of slack, at the
start where the reset's 397 events arrive in one period.

### Not established

- **No audio has been auditioned.** Pitch was measured (A4 renders at 439 Hz)
  and the bank is ScummVM's, but how a given file sounds, drums especially, is
  not judged here. Use `make midi-wav` and listen.
- **The `Bconin` read of the live port is not exercised**: this Hatari build has
  no MIDI input (PortMidi is not compiled in), so the live path is gated through
  `-i` with the same code after the byte is read.
- No physical Falcon, no timing under a loaded 68030.

### Limits

Nine voices, mono (the OPL2 has no panning). Not implemented: aftertouch, fine
tuning, SMPTE-timed files, format 2 files, file looping, and the chip's rhythm
mode. The modulation wheel switches vibrato on above 32. Percussion keys outside
GM's 35-81 are ignored.
