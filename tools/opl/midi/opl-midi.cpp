// Host MIDI-to-OPL2 tool: plays a Standard MIDI File through the engine and the
// practical reference chip, and writes what the Falcon would hear.
//
//   opl-midi song.mid [--wav out.wav] [--tail SECONDS] [--seconds LIMIT]
//   opl-midi --raw live.bin [...]      a raw MIDI byte stream, as live input
//                     [--play PLAYDATA.BIN --data OPLDATA.BIN] [--json]
//
// --wav      16-bit stereo at the codec's 49,170 Hz, from the reference chip
//            (the DSP's output word for word, by rt-bench-gate.py); a quick
//            way to audition a file on any PC.
// --play     the song as stream periods, and --data the DSP's table image, in
//            the formats oplplay.s reads: the existing f030opl2.tos then plays
//            the file from OPLDATA.BIN and PLAYDATA.BIN.
// --json     one line of facts, including the output checksum the DSP must
//            reproduce (the sum of the limited left and right words, mod 2^24,
//            over every period), which midi-gate.py compares with the DSP's.
// --tail     seconds of silence-or-ring-out after the last event (default 2).
// --raw      instead of a file, a raw MIDI byte stream played as live input at
//            3,125 bytes a second (liveBytesBy): the bytes that have arrived
//            by a period's start sound at that start, as F030MID's -i does.
#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "midi-file.h"
#include "midi-opl.h"
#include "opl-upload.h"
#include "period-stream.h"

namespace P = OplPractical;
using namespace OplMidi;

namespace {

void fail(const char *what) {
	std::fprintf(stderr, "opl-midi: %s\n", what);
	std::exit(1);
}

std::vector<uint8_t> readFile(const char *path) {
	std::vector<uint8_t> bytes;
	FILE *f = std::fopen(path, "rb");
	if (!f)
		fail("cannot open the MIDI file");
	uint8_t buffer[4096];
	size_t n;
	while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
		bytes.insert(bytes.end(), buffer, buffer + n);
	std::fclose(f);
	return bytes;
}

struct BigEndianWriter {
	FILE *file;
	void word(uint32_t v) {
		for (int shift = 24; shift >= 0; shift -= 8)
			std::fputc((int)((v >> shift) & 0xff), file);
	}
	void block(int space, uint16_t address, const uint32_t *words, unsigned count) {
		word((uint32_t)space);
		word(address);
		word(count);
		for (unsigned i = 0; i < count; ++i)
			word(words[i]);
	}
};

void writeLe16(FILE *f, unsigned v) { std::fputc(v & 0xff, f); std::fputc((v >> 8) & 0xff, f); }
void writeLe32(FILE *f, uint32_t v) { writeLe16(f, v & 0xffff); writeLe16(f, v >> 16); }

// The same event split as the DSP: events at or before a frame apply before it renders.
void renderTimed(P::Chip *chip, const std::vector<P::Event> &events, size_t &at, uint32_t first,
                 int32_t *left, int32_t *right) {
	const uint32_t limit = first + P::kBlockFrames;
	for (uint32_t frame = first; frame < limit;) {
		while (at < events.size() && events[at].frame <= frame) {
			P::poke(chip, events[at].address, events[at].value);
			++at;
		}
		const uint32_t until = at < events.size() && events[at].frame < limit ? events[at].frame : limit;
		P::renderSpanStereo(chip, nullptr, left + frame - first, right + frame - first, until - frame, frame == first);
		frame = until;
	}
}

} // namespace

int main(int argc, char **argv) {
	const char *midiPath = nullptr, *wavPath = nullptr, *playPath = nullptr, *dataPath = nullptr, *rawPath = nullptr;
	double tail = 2.0, limit = -1.0;
	bool json = false;
	for (int i = 1; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--wav") && i + 1 < argc) wavPath = argv[++i];
		else if (!std::strcmp(argv[i], "--play") && i + 1 < argc) playPath = argv[++i];
		else if (!std::strcmp(argv[i], "--data") && i + 1 < argc) dataPath = argv[++i];
		else if (!std::strcmp(argv[i], "--tail") && i + 1 < argc) tail = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) limit = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--json")) json = true;
		else if (!std::strcmp(argv[i], "--raw") && i + 1 < argc) rawPath = argv[++i];
		else if (argv[i][0] != '-' && !midiPath) midiPath = argv[i];
		else fail("usage: opl-midi song.mid [--wav f] [--play f --data f] [--tail s] [--seconds s] [--json]");
	}
	if (!midiPath && !rawPath)
		fail("no MIDI file given");
	std::vector<uint8_t> file, raw;
	SmfPlayer smf;
	uint64_t songFrames = 0;
	const double rate = OPL_PRACTICAL_CODEC_RATE;
	if (rawPath) {
		raw = readFile(rawPath);
		midiPath = rawPath;
		// the period in which the last byte has arrived ends the "song"
		uint32_t last = 0;
		while (liveBytesBy(last) < raw.size())
			++last;
		songFrames = (uint64_t)(last + 1) * kPeriodFrames;
	} else {
		file = readFile(midiPath);
		if (!smf.load(file.data(), (uint32_t)file.size()))
			fail("not a format 0 or 1 Standard MIDI File with ticks-per-quarter-note timing");
		songFrames = smf.totalFrames();
	}
	uint64_t totalFrames = songFrames + (uint64_t)(tail * rate);
	if (limit >= 0 && (uint64_t)(limit * rate) < totalFrames)
		totalFrames = (uint64_t)(limit * rate);
	const uint32_t periods = (uint32_t)((totalFrames + kPeriodFrames - 1) / kPeriodFrames);

	if (dataPath) {
		BigEndianWriter out;
		out.file = std::fopen(dataPath, "wb");
		if (!out.file)
			fail("cannot create the data image");
		out.word(0x4F504C52);
		out.word(kUploadBlocks);
		uploadTables(out, 9, true);
		std::fclose(out.file);
	}
	BigEndianWriter play;
	play.file = nullptr;
	if (playPath) {
		play.file = std::fopen(playPath, "wb");
		if (!play.file)
			fail("cannot create the play data");
		play.word(0x4F504C50);
		play.word(periods);
	}
	FILE *wav = nullptr;
	if (wavPath) {
		wav = std::fopen(wavPath, "wb");
		if (!wav)
			fail("cannot create the WAV");
		const uint32_t bytes = periods * kPeriodFrames * 4;
		std::fwrite("RIFF", 1, 4, wav); writeLe32(wav, 36 + bytes); std::fwrite("WAVEfmt ", 1, 8, wav);
		writeLe32(wav, 16); writeLe16(wav, 1); writeLe16(wav, 2);
		writeLe32(wav, 49170); writeLe32(wav, 49170 * 4); writeLe16(wav, 4); writeLe16(wav, 16);
		std::fwrite("data", 1, 4, wav); writeLe32(wav, bytes);
	}

	static Pipeline pipeline;
	pipeline.begin(0);
	P::Chip chip;
	P::reset(&chip, 9);
	static WireEvent wire[Pipeline::kMaxEventsPerPeriod];
	std::vector<P::Event> events;
	uint32_t checksum = 0;
	uint64_t totalEvents = 0, noteOns = 0;
	unsigned peakEvents = 0;
	Message m;
	bool have = rawPath ? false : smf.next(&m);
	MidiStream stream;
	uint32_t rawUsed = 0;
	int32_t left[P::kBlockFrames], right[P::kBlockFrames];

	for (uint32_t period = 0; period < periods; ++period) {
		const uint64_t start = (uint64_t)period * kPeriodFrames, end = start + kPeriodFrames;
		if (rawPath) {
			// what arrived since the last period sounds at this one's start
			const uint32_t arrived = std::min<uint32_t>(liveBytesBy(period), (uint32_t)raw.size());
			for (; rawUsed < arrived; ++rawUsed)
				if (stream.feed(raw[rawUsed], &m)) {
					m.frame = start;
					if (m.status != 0xf0 && (m.status & 0xf0) == 0x90 && m.data2)
						++noteOns;
					pipeline.message(m);
				}
		}
		while (have && m.frame < end) {
			if (m.status != 0xf0 && (m.status & 0xf0) == 0x90 && m.data2)
				++noteOns;
			pipeline.message(m);
			have = smf.next(&m);
		}
		const unsigned n = pipeline.takePeriod(start, wire);
		if (pipeline.overflowed())
			fail("a period carries more events than the DSP's table holds");
		totalEvents += n;
		peakEvents = std::max(peakEvents, n);
		if (play.file) {
			play.word(n);
			for (unsigned i = 0; i < n; ++i) {
				play.word(wire[i].address);
				play.word(wire[i].value);
			}
			play.word(0);   // silent PCM
		}
		events.clear();
		for (unsigned i = 0; i < n; ++i) {
			P::Event e;
			e.frame = wire[i].address >> 12;
			e.address = (uint16_t)(wire[i].address & 0xfff);
			// the 24-bit word sign-extended, as the DSP's X memory holds it
			e.value = (int32_t)(wire[i].value << 8) >> 8;
			events.push_back(e);
		}
		size_t at = 0;
		for (uint32_t b = 0; b < OPL_PRACTICAL_PERIOD_BLOCKS; ++b) {
			renderTimed(&chip, events, at, b * P::kBlockFrames, left, right);
			for (int i = 0; i < P::kBlockFrames; ++i) {
				checksum = (checksum + ((uint32_t)left[i] & 0xffffff) + ((uint32_t)right[i] & 0xffffff)) & 0xffffff;
				if (wav) {
					// the SSI sends the top sixteen bits of each 24-bit word
					const int l = (int)((uint32_t)left[i] << 8) >> 16, r = (int)((uint32_t)right[i] << 8) >> 16;
					writeLe16(wav, (unsigned)l & 0xffff);
					writeLe16(wav, (unsigned)r & 0xffff);
				}
			}
		}
	}
	if (play.file)
		std::fclose(play.file);
	if (wav)
		std::fclose(wav);
	if (json)
		std::printf("{\"midi\": \"%s\", \"tracks\": %u, \"song_frames\": %llu, \"song_seconds\": %.3f,"
		            " \"periods\": %u, \"note_ons\": %llu, \"parameter_events\": %llu,"
		            " \"peak_events_per_period\": %u, \"period_checksum\": %u}\n",
		            midiPath, smf.trackCount(), (unsigned long long)songFrames, songFrames / rate, periods,
		            (unsigned long long)noteOns, (unsigned long long)totalEvents, peakEvents, checksum);
	return 0;
}
