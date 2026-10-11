// Unit checks of the MIDI engine, the Standard MIDI File reader and the live
// byte-stream parser. Prints one JSON line; exits nonzero on any failure.
#include <cstdio>
#include <cstdlib>
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

// f-number and block of voice v as last written, as one comparable pitch
long voicePitch(const Recorder &r, int v) {
	const long fnum = r.last((uint16_t)(0xa0 + v)) | ((r.last((uint16_t)(0xb0 + v)) & 3) << 8);
	return fnum << ((r.last((uint16_t)(0xb0 + v)) >> 2) & 7);
}

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

	// a channel's only note outlives the chords around it: a lead held on
	// channel 1 since before eight chord notes on channel 0 keeps its voice
	// when channel 2 needs one, which comes from the oldest chord note instead
	e.reset(&r, 0);
	e.send(0x91, 72, 100);              // voice 0, the oldest
	for (int i = 0; i < 8; ++i)
		e.send(0x90, (uint8_t)(48 + i), 100);
	r.clear();
	e.send(0x92, 40, 100);
	CHECK_EQ(r.count(0xa0), 0);
	CHECK_EQ(r.count(0xa1), 1);
	// the new note's own channel gives up its oldest note even when it is the
	// channel's only one: a line moving on
	r.clear();
	e.send(0x91, 74, 100);
	CHECK_EQ(r.count(0xa0), 1);

	// a drum goes before a melodic note, and a note only the pedal holds
	// before a drum
	e.reset(&r, 0);
	e.send(0x90, 60, 100);              // voice 0: the oldest, channel 0's only note
	e.send(0x99, 36, 100);              // voice 1: bass drum, sounding
	for (int i = 1; i < 8; ++i)
		e.send((uint8_t)(0x90 + i), (uint8_t)(60 + i), 100);   // one note on each of seven more channels
	r.clear();
	e.send(0x9b, 40, 100);
	CHECK_EQ(r.count(0xa0), 0);
	CHECK_EQ(r.count(0xa1), 1);
	e.reset(&r, 0);
	e.send(0x99, 36, 100);              // voice 0: a drum
	e.send(0xb1, 64, 127);
	e.send(0x91, 50, 100);              // voice 1: channel 1, then its key goes up under the pedal
	e.send(0x81, 50, 0);
	for (int i = 0; i < 7; ++i)
		e.send((uint8_t)(0x92 + i), (uint8_t)(60 + i), 100);
	CHECK_EQ(e.sounding(), 9);
	r.clear();
	e.send(0x9b, 40, 100);
	CHECK_EQ(r.count(0xa1), 1);
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

	// an NRPN's data entry does not reach the RPN selected before it
	e.reset(&r, 0);
	e.send(0xb0, 101, 0);
	e.send(0xb0, 100, 0);               // RPN 0: bend range
	e.send(0xb0, 6, 12);
	e.send(0xb0, 99, 1);
	e.send(0xb0, 98, 8);                // NRPN 1/8 (a GS vibrato rate)
	e.send(0xb0, 6, 100);
	e.send(0x90, 57, 100);
	e.send(0xe0, 0x7f, 0x7f);           // full bend: twelve semitones (less one step), not 24
	const long bent = voicePitch(r, 0);
	e.send(0xe0, 0x00, 0x40);
	e.send(0x90, 69, 100);              // the note an octave up, unbent, on voice 1
	CHECK(std::labs(bent - voicePitch(r, 1)) * 100 < voicePitch(r, 1));

	// all notes off
	e.reset(&r, 0);
	e.send(0x90, 60, 100);
	e.send(0x90, 64, 100);
	e.send(0xb0, 123, 0);
	CHECK_EQ(e.sounding(), 0);
}

void testTuning() {
	Recorder r;
	Engine e;
	e.reset(&r, 0);
	e.send(0x90, 69, 100);
	const long inTune = voicePitch(r, 0);
	// RPN 2: coarse tuning a semitone up sounds the note above, on the note already held
	e.send(0xb0, 101, 0);
	e.send(0xb0, 100, 2);
	e.send(0xb0, 6, 65);
	const long coarse = voicePitch(r, 0);
	e.send(0xb0, 6, 64);
	CHECK_EQ(voicePitch(r, 0), inTune);
	{
		Recorder q;
		Engine f;
		f.reset(&q, 0);
		f.send(0x90, 70, 100);
		CHECK_EQ(coarse, voicePitch(q, 0));
	}
	// RPN 1: fine tuning, its MSB and then its LSB; 8,192 is in tune
	e.send(0xb0, 100, 1);
	e.send(0xb0, 6, 96);                // +50 cents
	const long fineUp = voicePitch(r, 0);
	CHECK(fineUp > inTune && fineUp < coarse);
	e.send(0xb0, 38, 127);              // the LSB raises it a little more
	CHECK(voicePitch(r, 0) >= fineUp);
	e.send(0xb0, 6, 32);                // -50 cents
	CHECK(voicePitch(r, 0) < inTune);
	e.send(0xb0, 6, 64);
	e.send(0xb0, 38, 0);
	CHECK_EQ(voicePitch(r, 0), inTune);
}

void testVibrato() {
	Recorder r;
	Engine e;
	e.reset(&r, 0);
	e.send(0x90, 69, 100);
	const long centre = voicePitch(r, 0);
	// no modulation: the ticks leave the pitch alone
	const size_t quiet = r.log.size();
	e.advanceTo(49170);                 // a second
	CHECK_EQ(r.log.size(), quiet);
	// full modulation: the pitch swings both ways, about half a semitone
	e.send(0xb0, 1, 127);
	long lowest = centre, highest = centre;
	for (uint64_t f = 49170; f < 2 * 49170; f += 197) {
		e.advanceTo(f);
		const long p = voicePitch(r, 0);
		lowest = p < lowest ? p : lowest;
		highest = p > highest ? p : highest;
	}
	CHECK(highest > centre && lowest < centre);
	CHECK(highest * 1000 < centre * 1035 && lowest * 1000 > centre * 965);   // within about 60 cents
	CHECK(r.last(0xb0) & 0x20);         // the key stays down throughout
	// the wheel back to zero: the next tick returns the note to its pitch, then nothing more
	e.send(0xb0, 1, 0);
	e.advanceTo(2 * 49170 + 400);
	CHECK_EQ(voicePitch(r, 0), centre);
	const size_t settled = r.log.size();
	e.advanceTo(3 * 49170);
	CHECK_EQ(r.log.size(), settled);
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
	e.send(0x99, 71, 110);              // a whistle: no drum stands in for it
	CHECK_EQ(r.count(0xb0), before);
	// ride cymbal 2, which the bank lacks, is played as ride cymbal 1
	{
		Recorder a, b;
		Engine x, y;
		x.reset(&a, 0);
		y.reset(&b, 0);
		x.send(0x99, 59, 100);
		y.send(0x99, 51, 100);
		CHECK_EQ(x.sounding(), 1);
		CHECK_EQ(voicePitch(a, 0), voicePitch(b, 0));
		CHECK_EQ(a.last(0xc0), b.last(0xc0));
		x.send(0x89, 59, 0);            // and its key-off finds it
		CHECK(!(a.last(0xb0) & 0x20));
	}
	// every General MIDI drum key but the two whistles sounds
	for (int key = 35; key <= 81; ++key) {
		Engine z;
		z.reset(0, 0);
		z.send(0x99, (uint8_t)key, 100);
		CHECK(z.sounding() == (key == 71 || key == 72 ? 0 : 1));
	}
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

	// a player loaded again starts its clock again
	CHECK(p.load(file.data(), (uint32_t)file.size()));
	CHECK(p.next(&m));
	CHECK_EQ(m.frame, 0);
	CHECK(p.next(&m));
	CHECK_EQ(m.frame, 24584);
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
	testTuning();
	testVibrato();
	testPercussion();
	testClock();
	testSmf();
	testStream();
	testReset();
	std::printf("{\"midi_checks\": %d, \"failures\": %d}\n", checks, failures);
	return failures ? 1 : 0;
}
