#!/usr/bin/env python3
"""Convert Miles XMIDI (.XMI) files to Standard MIDI Files.

usage: xmi2mid.py <file.xmi | directory> <output directory>

Many DOS games (Ultima Underworld, Panzer General and others) keep their music
as XMIDI. An XMIDI file holds one or more sequences; each becomes a format 0
file named <stem>_<n>.mid (just <stem>.mid when there is only one) at 60 ticks
per quarter note and a fixed 500,000 us tempo, which is XMIDI's 120 ticks a
second. What it does:

- a note carries its length instead of a note-off, so the note-off is placed
  at the end of the length (a length of 0 plays as 1, as Miles' driver and
  ScummVM do);
- XMIDI's delays are sums of bytes below 0x80, not variable-length numbers;
- tempo events are dropped (XMIDI plays at the constant tempo);
- controllers 110 to 120 (0x6e-0x78: channel lock, voice and timbre
  protection, bank change, loop and branch markers) are XMIDI's own and are
  dropped; 120 would otherwise read as General MIDI's "all sound off". A
  song's loops are not unrolled: it plays once.

The TIMB chunk (the timbres a sequence uses, which Miles loads into the sound
device from its own bank) is not needed by a General MIDI player and is ignored.
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


def chunks(data, start, end):
    """(tag, offset, length) of the IFF chunks in data[start:end], big-endian sizes."""
    at = start
    while at + 8 <= end:
        tag = data[at:at + 4]
        length = struct.unpack(">I", data[at + 4:at + 8])[0]
        yield tag, at + 8, length
        at += 8 + length + (length & 1)


def sequences(data):
    """The EVNT chunk contents of every sequence in an XMIDI file."""
    if data[:4] != b"FORM":
        raise ValueError("not an IFF file")
    out = []

    def walk(start, end):
        for tag, at, length in chunks(data, start, end):
            if tag not in (b"FORM", b"CAT "):
                continue
            if tag == b"FORM" and data[at:at + 4] == b"XMID":
                for t, a, n in chunks(data, at + 4, at + length):
                    if t == b"EVNT":
                        out.append(data[a:a + n])
            else:                        # XDIR, or the CAT that holds the sequences
                walk(at + 4, at + length)

    walk(0, len(data))
    return out


def read_vlq(buf, pos):
    value = 0
    for _ in range(4):
        b = buf[pos]
        pos += 1
        value = value << 7 | (b & 0x7F)
        if not b & 0x80:
            break
    return value, pos


def convert(evnt):
    """One EVNT chunk to SMF track events: (tick, order, bytes)."""
    events = []
    pos, tick, order = 0, 0, 0
    end = len(evnt)
    while pos < end:
        while pos < end and not evnt[pos] & 0x80:   # delays add up
            tick += evnt[pos]
            pos += 1
        if pos >= end:
            break
        status = evnt[pos]
        pos += 1
        kind = status >> 4
        channel = status & 0x0F
        if kind == 0x9:
            note, velocity = evnt[pos], evnt[pos + 1]
            pos += 2
            length, pos = read_vlq(evnt, pos)
            if velocity == 0:
                events.append((tick, 1, bytes([0x80 | channel, note, 0])))
            else:
                events.append((tick, 2, bytes([status, note, velocity])))
                events.append((tick + max(length, 1), 1, bytes([0x80 | channel, note, 0])))
        elif kind in (0xC, 0xD):
            events.append((tick, 2, bytes([status, evnt[pos]])))
            pos += 1
        elif kind in (0x8, 0xA, 0xE):
            events.append((tick, 2, bytes([status, evnt[pos], evnt[pos + 1]])))
            pos += 2
        elif kind == 0xB:
            controller, value = evnt[pos], evnt[pos + 1]
            pos += 2
            if not 0x6E <= controller <= 0x78:
                events.append((tick, 2, bytes([status, controller, value])))
        elif status == 0xFF:
            kind_byte = evnt[pos]
            length, pos = read_vlq(evnt, pos + 1)
            pos += length
            if kind_byte == 0x2F:
                break
        elif status == 0xF0:
            length, pos = read_vlq(evnt, pos)
            events.append((tick, 2, b"\xf0" + vlq(length) + evnt[pos:pos + length]))
            pos += length
        elif status in (0xF2,):
            pos += 2
        elif status in (0xF3,):
            pos += 1
        # the remaining system common and real-time bytes carry nothing
    return events


def smf(events):
    events = sorted(events, key=lambda e: (e[0], e[1]))
    last_tick = events[-1][0] if events else 0
    track = vlq(0) + b"\xff\x51\x03" + (500000).to_bytes(3, "big")
    previous = 0
    for tick, _, message in events:
        track += vlq(tick - previous) + message
        previous = tick
    track += vlq(0) + b"\xff\x2f\x00"
    return (b"MThd" + struct.pack(">IHHH", 6, 0, 1, 60) +
            b"MTrk" + struct.pack(">I", len(track)) + track), last_tick


def convert_file(path, out_dir):
    data = path.read_bytes()
    seqs = sequences(data)
    written = []
    for index, evnt in enumerate(seqs):
        events = convert(evnt)
        if not any(m[0] & 0xF0 == 0x90 for _, _, m in events if m[:1] != b"\xf0"):
            continue   # no notes: not music
        name = path.stem if len(seqs) == 1 else f"{path.stem}_{index}"
        target = out_dir / f"{name}.mid"
        target.write_bytes(smf(events)[0])
        written.append(target)
    return written


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    source, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    files = sorted(source.rglob("*.[xX][mM][iI]")) if source.is_dir() else [source]
    total = 0
    for path in files:
        try:
            total += len(convert_file(path, out_dir))
        except (ValueError, IndexError, struct.error) as error:
            print(f"{path}: {error}", file=sys.stderr)
    print(f"{len(files)} XMIDI files, {total} sequences written to {out_dir}")


if __name__ == "__main__":
    main()
