// Unit checks of the MIDI engine, the Standard MIDI File reader and the live
// byte-stream parser. Prints one JSON line; exits nonzero on any failure.
#include <cstdio>
#include <vector>

#include "midi-file.h"
#include "midi-opl.h"

using namespace OplMidi;

namespace {

int failures = 0, checks = 0;

void check(bool ok, const char *what, long a = 0, long b = 0) {
	++checks;
	if (!ok) {
		++failures;
		std::fprintf(stderr, "FAIL: %s (%ld, %ld)\n", what, a, b);
	}
}
#define CHECK(cond) check((cond), #cond)
#define CHECK_EQ(a, b) check((long)(a) == (long)(b), #a " == " #b, (long)(a), (long)(b))

struct Write {
	uint64_t frame;
	uint16_t reg;
	uint8_t value;
};

struct Recorder : RegisterSink {
	std::vector<Write> log;
	void write(uint64_t frame, uint16_t reg, uint8_t value) override { log.push_back(Write{frame, reg, value}); }
	void clear() { log.clear(); }
	// the last value written to a register, or -1
	int last(uint16_t reg) const {
		for (size_t i = log.size(); i-- > 0;)
			if (log[i].reg == reg)
				return log[i].value;
		return -1;
	}
	int count(uint16_t reg) const {
		int n = 0;
		for (size_t i = 0; i < log.size(); ++i)
			n += log[i].reg == reg;
		return n;
	}
};

void testPitch() {
	Recorder r;
	Engine e;
	e.reset(&r, 0);
	r.clear();
	e.send(0x90, 69, 100);          // A4 on program 0
	// The base note is an octave low: note 57 is octave 4, A, which is f-number 579 at block 3.
	CHECK_EQ(r.last(0xa0), 579 & 0xff);
	CHECK_EQ(r.last(0xb0), 0x20 | (3 << 2) | (579 >> 8));
	e.send(0x80, 69, 0);
	CHECK_EQ(r.last(0xb0), (3 << 2) | (579 >> 8));   // key bit clear, pitch kept

	// every semitone of a range is strictly higher than the one below it
	long previous = -1;
	for (int note = 24; note < 100; ++note) {
		Recorder q;
		Engine f;
		f.reset(&q, 0);
		q.clear();
		f.send(0x90, (uint8_t)note, 100);
		const long fnum = q.last(0xa0) | ((q.last(0xb0) & 3) << 8);
		const long block = (q.last(0xb0) >> 2) & 7;
		const long pitch = (fnum << block);   // monotonic in frequency
		CHECK(pitch > previous);
		previous = pitch;
	}
}

void testVoices() {
	Recorder r;
	Engine e;
	e.reset(&r, 0);
	r.clear();
	for (int i = 0; i < 9; ++i)
		e.send(0x90, (uint8_t)(48 + i), 100);
	CHECK_EQ(e.sounding(), 9);
	for (int v = 0; v < 9; ++v)
		CHECK(r.count(0xb0 + v) == 1);   // one key-on each, no key-off before a fresh voice
	r.clear();
	e.send(0x90, 60, 100);              // the tenth: takes the oldest note's voice
	CHECK_EQ(e.sounding(), 9);
	CHECK_EQ(r.count(0xb0), 2);         // key up, then key on again: an edge
	CHECK(r.log.front().reg == 0xb0 && !(r.log.front().value & 0x20));
	CHECK(r.last(0xb0) & 0x20);

	// the same note again retriggers its own voice, it does not spend another
	r.clear();
	e.send(0x90, 60, 100);
	CHECK_EQ(e.sounding(), 9);
	CHECK_EQ(r.count(0xb0) + r.count(0xb1) + r.count(0xb2) + r.count(0xb3) + r.count(0xb4) +
	         r.count(0xb5) + r.count(0xb6) + r.count(0xb7) + r.count(0xb8), 2);

	// a released voice is taken before a sounding one
	e.reset(&r, 0);
	r.clear();
	for (int i = 0; i < 9; ++i)
		e.send(0x90, (uint8_t)(48 + i), 100);
	e.send(0x80, 52, 0);                // voice 4 released
	r.clear();
	e.send(0x90, 80, 100);
	CHECK(r.count(0xa4) == 1);          // it went to voice 4
}

void testControllers() {
	Recorder r;
	Engine e;
	e.reset(&r, 0);
	e.send(0x90, 60, 100);
	r.clear();
	e.send(0xb0, 64, 127);              // pedal down
	e.send(0x80, 60, 0);                // key up under the pedal: nothing happens
	CHECK(r.log.empty());
	e.send(0xb0, 64, 0);                // pedal up: now it releases
	CHECK(r.last(0xb0) >= 0 && !(r.last(0xb0) & 0x20));

	// volume: a quieter channel writes a larger carrier total level
	e.reset(&r, 0);
	e.send(0xb0, 7, 127);
	e.send(0x90, 60, 100);
	const int loud = r.last(0x43) & 0x3f;   // voice 0's carrier is slot offset 3
	e.send(0xb0, 7, 30);
	const int soft = r.last(0x43) & 0x3f;
	CHECK(soft > loud);
	e.send(0xb0, 7, 0);
	CHECK_EQ(r.last(0x43) & 0x3f, 63);

	// pitch bend: full up is two semitones, centre is none
	e.reset(&r, 0);
	e.send(0x90, 69, 100);
	const int centre = r.last(0xa0) | ((r.last(0xb0) & 3) << 8);
	e.send(0xe0, 0x7f, 0x7f);
	const int up = r.last(0xa0) | ((r.last(0xb0) & 3) << 8);
	CHECK(up > centre);
	e.send(0xe0, 0x00, 0x40);           // 8192: centre
	CHECK_EQ(r.last(0xa0) | ((r.last(0xb0) & 3) << 8), centre);

	// all notes off
	e.reset(&r, 0);
	e.send(0x90, 60, 100);
	e.send(0x90, 64, 100);
	e.send(0xb0, 123, 0);
	CHECK_EQ(e.sounding(), 0);
}

void testPercussion() {
	Recorder r;
	Engine e;
	e.reset(&r, 0);
	r.clear();
	e.send(0x99, 36, 110);              // bass drum, a mapped key
	CHECK_EQ(e.sounding(), 1);
	CHECK(r.count(0xc0) == 1);
	const int before = r.count(0xb0);
	e.send(0x99, 20, 110);              // an unmapped key: ignored
	CHECK_EQ(r.count(0xb0), before);
	// the patch's duration ends the note by itself (19 ticks for the bass drum)
	e.advanceTo(40 * 197);
	CHECK_EQ(e.sounding(), 0);
}

void testClock() {
	Recorder r;
	Engine e;
	e.reset(&r, 0);
	e.send(0x99, 36, 110);
	uint64_t keyedAt = 0;
	for (size_t i = 0; i < r.log.size(); ++i)
		if (r.log[i].reg == 0xb0 && (r.log[i].value & 0x20))
			keyedAt = r.log[i].frame;
	e.advanceTo(50000);
	uint64_t offAt = 0;
	for (size_t i = 0; i < r.log.size(); ++i)
		if (r.log[i].reg == 0xb0 && !(r.log[i].value & 0x20) && r.log[i].frame > 0)
			offAt = r.log[i].frame;
	// the bass drum's duration of 5 is 5 * 63 / 17 = 19 ticks of 49,169.92 / 250 frames
	CHECK(offAt > keyedAt + 18 * 196 && offAt < keyedAt + 20 * 197);
}

// A minimal file: header, then one track given as raw bytes.
std::vector<uint8_t> smf(const std::vector<uint8_t> &track, unsigned division = 480, unsigned format = 0) {
	std::vector<uint8_t> f;
	const uint8_t head[] = { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, (uint8_t)format, 0, 1, (uint8_t)(division >> 8), (uint8_t)division };
	f.insert(f.end(), head, head + 14);
	const uint32_t n = (uint32_t)track.size();
	const uint8_t th[] = { 'M', 'T', 'r', 'k', (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
	f.insert(f.end(), th, th + 8);
	f.insert(f.end(), track.begin(), track.end());
	return f;
}

void testSmf() {
	// tick 0: note on; tick 480 (one quarter at 120/min): note off by running
	// status with velocity 0; tick 480 + 960: tempo to 250,000 us; tick 480+960+480: note on
	const uint8_t t[] = {
		0x00, 0x90, 60, 100,
		0x83, 0x60, 60, 0,                              // delta 480, running status
		0x87, 0x40, 0xff, 0x51, 3, 0x03, 0xd0, 0x90,    // delta 960, tempo 250,000
		0x83, 0x60, 0x90, 62, 100,                      // delta 480
		0x00, 0xff, 0x2f, 0x00
	};
	std::vector<uint8_t> file = smf(std::vector<uint8_t>(t, t + sizeof(t)));
	SmfPlayer p;
	CHECK(p.load(file.data(), (uint32_t)file.size()));
	Message m;
	CHECK(p.next(&m));
	CHECK_EQ(m.frame, 0);
	CHECK_EQ(m.status, 0x90);
	CHECK(p.next(&m));
	// 480 ticks, 500,000 us a quarter, 480 per quarter: half a second = 24,584.96 frames
	CHECK_EQ(m.frame, 24584);
	CHECK_EQ(m.status, 0x90);
	CHECK_EQ(m.data1, 60);
	CHECK_EQ(m.data2, 0);
	CHECK(p.next(&m));
	// 960 more ticks at 500,000 (to frame 24,584.96 + 49,169.92 = 73,754.88); then 480 at 250,000 (+12,292.48)
	CHECK_EQ(m.frame, 86047);
	CHECK_EQ(m.data1, 62);
	CHECK(!p.next(&m));

	// damaged and unsupported files are refused, not crashed on
	std::vector<uint8_t> bad(file.begin(), file.begin() + 10);
	CHECK(!p.load(bad.data(), (uint32_t)bad.size()));
	std::vector<uint8_t> smpte = smf(std::vector<uint8_t>(t, t + sizeof(t)), 0xe728);
	CHECK(!p.load(smpte.data(), (uint32_t)smpte.size()));
	std::vector<uint8_t> format3 = smf(std::vector<uint8_t>(t, t + sizeof(t)), 480, 3);
	CHECK(!p.load(format3.data(), (uint32_t)format3.size()));
	// a format 2 file of one track is a format 0 file; the "MIDI" wrapper of .GMD files is looked through
	std::vector<uint8_t> format2 = smf(std::vector<uint8_t>(t, t + sizeof(t)), 480, 2);
	CHECK(p.load(format2.data(), (uint32_t)format2.size()));
	std::vector<uint8_t> wrapped = { 'M', 'I', 'D', 'I', 0, 0, 0x51, 0x75, 'M', 'D', 'p', 'g', 0, 0, 0, 14, 'I', 'G', '9', '8',
	                                 0x3c, 0x0b, 0x2e, 0x2f, 0x3f, 0x30, 0x2d, 0x00, 0x58, 0x34 };
	wrapped.insert(wrapped.end(), format2.begin(), format2.end());
	CHECK(p.load(wrapped.data(), (uint32_t)wrapped.size()));
	CHECK(p.next(&m));
	CHECK_EQ(m.status, 0x90);

	// two tracks interleave by time
	std::vector<uint8_t> two;
	const uint8_t head[] = { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 2, 0x01, 0xe0 };
	two.insert(two.end(), head, head + 14);
	const uint8_t a[] = { 'M', 'T', 'r', 'k', 0, 0, 0, 12, 0x00, 0x90, 60, 100, 0x81, 0x40, 0x80, 60, 0, 0x00, 0xff, 0x2f, 0x00 };
	const uint8_t b[] = { 'M', 'T', 'r', 'k', 0, 0, 0, 12, 0x60, 0x91, 64, 100, 0x60, 0x81, 64, 0, 0x00, 0xff, 0x2f, 0x00 };
	// fix the lengths the literal arrays above did not match
	std::vector<uint8_t> ta(a + 8, a + sizeof(a)), tb(b + 8, b + sizeof(b));
	for (int k = 0; k < 2; ++k) {
		const std::vector<uint8_t> &body = k ? tb : ta;
		const uint8_t th[] = { 'M', 'T', 'r', 'k', 0, 0, 0, (uint8_t)body.size() };
		two.insert(two.end(), th, th + 8);
		two.insert(two.end(), body.begin(), body.end());
	}
	CHECK(p.load(two.data(), (uint32_t)two.size()));
	uint64_t last = 0;
	int n = 0;
	while (p.next(&m)) {
		CHECK(m.frame >= last);
		last = m.frame;
		++n;
	}
	CHECK_EQ(n, 4);
}

void testStream() {
	MidiStream s;
	Message m;
	// note on, then a running-status note on with a clock byte between its data bytes
	CHECK(!s.feed(0x90, &m));
	CHECK(!s.feed(60, &m));
	CHECK(s.feed(100, &m));
	CHECK_EQ(m.status, 0x90);
	CHECK_EQ(m.data1, 60);
	CHECK(!s.feed(64, &m));
	CHECK(!s.feed(0xf8, &m));
	CHECK(s.feed(90, &m));
	CHECK_EQ(m.data1, 64);
	CHECK_EQ(m.data2, 90);
	// program change has one data byte
	CHECK(!s.feed(0xc3, &m));
	CHECK(s.feed(41, &m));
	CHECK_EQ(m.status, 0xc3);
	CHECK_EQ(m.data1, 41);
	// system exclusive is delivered whole, and running status does not survive it
	CHECK(!s.feed(0xf0, &m));
	CHECK(!s.feed(0x7e, &m));
	CHECK(!s.feed(0x7f, &m));
	CHECK(!s.feed(0x09, &m));
	CHECK(!s.feed(0x01, &m));
	CHECK(s.feed(0xf7, &m));
	CHECK_EQ(m.status, 0xf0);
	CHECK_EQ(m.sysexSize, 4);
	CHECK(!s.feed(60, &m));   // no status to put it under
}

void testReset() {
	Recorder r;
	Engine e;
	e.reset(&r, 0);
	e.send(0xc0, 12, 0);
	e.send(0x90, 60, 100);
	const uint8_t gm[] = { 0x7e, 0x7f, 0x09, 0x01 };
	e.sysex(gm, sizeof(gm));
	CHECK_EQ(e.sounding(), 0);
}

} // namespace

int main() {
	testPitch();
	testVoices();
	testControllers();
	testPercussion();
	testClock();
	testSmf();
	testStream();
	testReset();
	std::printf("{\"midi_checks\": %d, \"failures\": %d}\n", checks, failures);
	return failures ? 1 : 0;
}
