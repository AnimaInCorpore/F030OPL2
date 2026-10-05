// Standard MIDI Files and live MIDI byte streams, as the OPL engine's input.
//
// SmfPlayer merges a file's tracks into one time-ordered stream and converts
// ticks to codec-rate frames exactly: a tick is tempo / division microseconds
// and a frame 512 / 25,175,000 s, so the frame of an event is a ratio of
// integers kept in a 64-bit numerator, not a running float. No heap, no
// floating point, no library beyond <stdint.h>.
#ifndef OPL_MIDI_FILE_H
#define OPL_MIDI_FILE_H

#include <stdint.h>

namespace OplMidi {

struct Message {
	uint64_t frame;
	uint8_t status, data1, data2;   // status 0xf0: system exclusive, data in sysex/sysexSize
	const uint8_t *sysex;
	unsigned sysexSize;
};

class SmfPlayer {
public:
	enum { kMaxTracks = 64 };

	SmfPlayer() : tracks_(0) {}

	// The file must stay in memory while it plays. False for anything but a
	// format 0 or 1 file (or a format 2 file of a single track, which is the same
	// thing) with ticks-per-quarter-note timing. A RIFF-style "MIDI" wrapper, as
	// LucasArts' .GMD files have, is looked through to the MThd inside it.
	bool load(const uint8_t *data, uint32_t size) {
		tracks_ = 0;
		if (size >= 12 && tag(data, "MIDI")) {
			uint32_t at = 8;
			while (at + 4 <= size && at < 64 && !tag(data + at, "MThd"))
				++at;
			if (at + 4 > size || at >= 64)
				return false;
			data += at;
			size -= at;
		}
		if (size < 14 || !tag(data, "MThd") || be32(data + 4) < 6)
			return false;
		const unsigned format = be16(data + 8);
		const unsigned count = be16(data + 10);
		division_ = be16(data + 12);
		if (format > 2 || (format == 2 && count != 1) || (division_ & 0x8000) || division_ == 0)
			return false;
		uint32_t at = 8 + be32(data + 4);
		while (at + 8 <= size && tracks_ < kMaxTracks && tracks_ < count) {
			const uint32_t length = be32(data + at + 4);
			if (!tag(data + at, "MTrk")) {
				at += 8 + length;
				continue;
			}
			if (at + 8 + length > size)
				return false;
			Track &t = track_[tracks_++];
			t.pos = data + at + 8;
			t.end = t.pos + length;
			t.status = 0;
			t.done = false;
			t.tick = 0;
			readDelta(t);
			at += 8 + length;
		}
		tempo_ = 500000;      // 120 beats a minute until the file says otherwise
		lastTick_ = 0;
		numerator_ = 0;
		return tracks_ > 0;
	}

	// The next channel or system-exclusive message in time order. False at the
	// end of every track. Tempo changes and the other meta events are consumed.
	bool next(Message *out) {
		for (;;) {
			int best = -1;
			for (unsigned i = 0; i < tracks_; ++i)
				if (!track_[i].done && (best < 0 || track_[i].tick < track_[best].tick))
					best = (int)i;
			if (best < 0)
				return false;
			Track &t = track_[best];
			advanceClock(t.tick);
			if (event(t, out))
				return true;
		}
	}

	// The length of the whole file in frames (a pass over a copy of the cursors).
	uint64_t totalFrames() {
		SmfPlayer copy = *this;
		Message m;
		uint64_t last = 0;
		while (copy.next(&m))
			last = m.frame;
		return last;
	}

	unsigned trackCount() const { return tracks_; }

private:
	struct Track {
		const uint8_t *pos, *end;
		uint8_t status;
		bool done;
		uint64_t tick;
	};

	static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
	static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
	static bool tag(const uint8_t *p, const char *t) { return p[0] == (uint8_t)t[0] && p[1] == (uint8_t)t[1] && p[2] == (uint8_t)t[2] && p[3] == (uint8_t)t[3]; }

	static void readDelta(Track &t) {
		uint32_t delta = 0;
		for (int i = 0; i < 4 && t.pos < t.end; ++i) {
			const uint8_t b = *t.pos++;
			delta = delta << 7 | (b & 0x7f);
			if (!(b & 0x80))
				break;
		}
		t.tick += delta;
		if (t.pos >= t.end)
			t.done = true;
	}

	// Frames per tick are tempo * 1007 / (division * 20480): tempo microseconds
	// a quarter note, 25,175,000 / 512 frames a second. The numerator is kept
	// whole; the frame is its floor.
	void advanceClock(uint64_t tick) {
		numerator_ += (tick - lastTick_) * (uint64_t)tempo_ * 1007ull;
		lastTick_ = tick;
		frame_ = numerator_ / ((uint64_t)division_ * 20480ull);
	}

	// Reads the event at the track's cursor; true if it is one to deliver.
	bool event(Track &t, Message *out) {
		const uint8_t *p = t.pos;
		if (p >= t.end) {
			t.done = true;
			return false;
		}
		uint8_t status = *p;
		if (status & 0x80)
			++p;
		else
			status = t.status;   // running status
		if (!(status & 0x80)) {  // data with no status to put it under: the track is damaged
			t.done = true;
			return false;
		}
		bool deliver = false;
		out->frame = frame_;
		out->sysex = 0;
		out->sysexSize = 0;
		if (status >= 0xf0) {
			if (status == 0xff) {
				const uint8_t type = p < t.end ? *p++ : 0;
				const uint32_t length = varLength(p, t.end);
				if (type == 0x51 && length == 3 && p + 3 <= t.end)
					tempo_ = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
				if (type == 0x2f) {
					t.done = true;
					return false;
				}
				p += length;
			} else if (status == 0xf0 || status == 0xf7) {
				const uint32_t length = varLength(p, t.end);
				out->status = 0xf0;
				out->sysex = p;
				out->sysexSize = length;
				p += length;
				deliver = status == 0xf0;
			}
			// a sysex or meta event ends running status
			t.status = 0;
		} else {
			const unsigned need = (status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0 ? 1 : 2;
			if (p + need > t.end) {
				t.done = true;
				return false;
			}
			out->status = status;
			out->data1 = p[0];
			out->data2 = need == 2 ? p[1] : 0;
			p += need;
			t.status = status;
			deliver = true;
		}
		t.pos = p > t.end ? t.end : p;
		if (t.pos >= t.end)
			t.done = true;
		else
			readDelta(t);
		return deliver;
	}

	static uint32_t varLength(const uint8_t *&p, const uint8_t *end) {
		uint32_t v = 0;
		for (int i = 0; i < 4 && p < end; ++i) {
			const uint8_t b = *p++;
			v = v << 7 | (b & 0x7f);
			if (!(b & 0x80))
				break;
		}
		return v;
	}

	Track track_[kMaxTracks];
	unsigned tracks_;
	unsigned division_;
	uint32_t tempo_;
	uint64_t lastTick_, numerator_, frame_;
};

// A live MIDI byte stream: running status, real-time bytes skipped, system
// exclusive collected. feed() returns true when a message completed.
class MidiStream {
public:
	enum { kSysexMax = 64 };

	MidiStream() : status_(0), need_(0), have_(0), inSysex_(false), sysexSize_(0) {}

	bool feed(uint8_t byte, Message *out) {
		if (byte >= 0xf8)
			return false;   // clock, active sensing and the rest
		if (byte & 0x80) {
			if (inSysex_) {
				inSysex_ = false;
				if (byte == 0xf7) {
					out->status = 0xf0;
					out->sysex = sysex_;
					out->sysexSize = sysexSize_;
					return true;
				}
			}
			if (byte == 0xf0) {
				inSysex_ = true;
				sysexSize_ = 0;
				status_ = 0;
				return false;
			}
			if (byte >= 0xf0) {   // other system common: no running status, no data we use
				status_ = 0;
				return false;
			}
			status_ = byte;
			need_ = (byte & 0xf0) == 0xc0 || (byte & 0xf0) == 0xd0 ? 1 : 2;
			have_ = 0;
			return false;
		}
		if (inSysex_) {
			if (sysexSize_ < kSysexMax)
				sysex_[sysexSize_++] = byte;
			return false;
		}
		if (!status_)
			return false;
		data_[have_++] = byte;
		if (have_ < need_)
			return false;
		have_ = 0;
		out->status = status_;
		out->data1 = data_[0];
		out->data2 = need_ == 2 ? data_[1] : 0;
		out->sysex = 0;
		out->sysexSize = 0;
		return true;
	}

private:
	uint8_t status_, need_, have_, data_[2];
	bool inSysex_;
	uint8_t sysex_[kSysexMax];
	unsigned sysexSize_;
};

} // namespace OplMidi

#endif
