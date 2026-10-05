#!/usr/bin/env python3
"""Write deterministic Standard MIDI Files for the MIDI gates.

usage: make-test-midi.py <output directory>

song.mid    a format 1 file: tempo change, a GM reset, piano chords, a bass line,
            a violin with pitch bend and the sustain pedal, and a drum pattern.
a440.mid    a single A4 on program 0 held for one second, for a pitch check.
live.bin    a raw MIDI byte stream as a keyboard would send it: a GM reset, running
            status, real-time clock bytes between data bytes, active-sensing padding
            that stands for the time between events at 3,125 bytes a second, a chord,
            a bass note, a bend and drums.
"""
import struct
import sys
from pathlib import Path


def vlq(value):
    out = [value & 0x7F]
    value >>= 7
    while value:
        out.append(0x80 | (value & 0x7F))
        value >>= 7
    return bytes(reversed(out))


def track(events):
    """events: (tick, bytes) pairs, in any order; returns an MTrk chunk."""
    events = sorted(events, key=lambda e: e[0])
    data, last = b"", 0
    for tick, message in events:
        data += vlq(tick - last) + message
        last = tick
    data += vlq(0) + b"\xff\x2f\x00"
    return b"MTrk" + struct.pack(">I", len(data)) + data


def note(tick, length, channel, key, velocity=90):
    return [(tick, bytes([0x90 | channel, key, velocity])), (tick + length, bytes([0x80 | channel, key, 0]))]


def smf(tracks, division=480):
    return b"MThd" + struct.pack(">IHHH", 6, 1, len(tracks), division) + b"".join(tracks)


def song():
    q = 480
    tempo = [(0, b"\xff\x51\x03" + (500000).to_bytes(3, "big")),
             (0, b"\xf0\x05\x7e\x7f\x09\x01\xf7"),                      # GM reset
             (8 * q, b"\xff\x51\x03" + (400000).to_bytes(3, "big"))]    # faster for the second half
    piano = [(0, bytes([0xc0, 0])), (0, bytes([0xb0, 7, 110]))]
    chords = [(60, 64, 67), (65, 69, 72), (67, 71, 74), (60, 64, 67)]
    for bar in range(8):
        for key in chords[bar % 4]:
            piano += note(bar * 2 * q, 2 * q - 20, 0, key, 80)
    bass = [(0, bytes([0xc1, 33]))]
    for beat in range(16):
        bass += note(beat * q, q - 30, 1, (36, 36, 43, 41)[beat % 4] + (0 if beat < 8 else 2), 100)
    violin = [(0, bytes([0xc2, 40])), (4 * q, bytes([0xb2, 64, 127])), (10 * q, bytes([0xb2, 64, 0]))]
    violin += note(4 * q, 3 * q, 2, 72, 85) + note(8 * q, 4 * q, 2, 76, 85)
    for step in range(0, 16):   # a bend up and back over the held E
        value = 8192 + int(4000 * (1 - abs(step - 8) / 8))
        violin.append((8 * q + step * 30, bytes([0xe2, value & 0x7F, value >> 7])))
    drums = []
    for bar in range(8):
        for beat in range(4):
            t = (bar * 4 + beat) * q
            drums += note(t, 60, 9, 42, 70) + note(t + q // 2, 60, 9, 42, 50)
            if beat % 2 == 0:
                drums += note(t, 90, 9, 36, 110)
            else:
                drums += note(t, 90, 9, 38, 100)
    return smf([track(tempo), track(piano), track(bass), track(violin), track(drums)])


def a440():
    q = 480
    return smf([track([(0, b"\xff\x51\x03" + (500000).to_bytes(3, "big"))] +
                      [(0, bytes([0xc0, 0]))] + note(0, 2 * q, 0, 69, 127))])


def live():
    def wait(seconds):
        return bytes([0xFE]) * int(seconds * 3125)
    data = bytes([0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7, 0xC0, 0x04, 0xC1, 0x21, 0xB0, 0x07, 100])
    data += wait(0.1)
    # a chord by running status, with timing clock bytes inside it
    data += bytes([0x90, 60, 100, 64, 0xF8, 100, 67, 100, 0xF8])
    data += wait(0.4)
    data += bytes([0x91, 36, 110])                       # bass on channel 1
    data += wait(0.3)
    data += bytes([0xE0, 0x00, 0x60, 0xE0, 0x00, 0x70, 0xE0, 0x7F, 0x7F])   # bend up
    data += wait(0.3)
    data += bytes([0x99, 36, 120, 42, 80, 0xF8, 38, 110])   # drums on channel 10
    data += wait(0.3)
    data += bytes([0x80, 60, 0, 64, 0, 67, 0, 0x81, 36, 0])  # releases
    data += wait(0.2)
    data += bytes([0xB0, 123, 0])                          # all notes off
    data += wait(0.2)
    return data


out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
(out / "song.mid").write_bytes(song())
(out / "a440.mid").write_bytes(a440())
(out / "live.bin").write_bytes(live())
print(f"wrote {out}/song.mid, {out}/a440.mid and {out}/live.bin")
