// F030MID: a real-time MIDI player for the Falcon's emulated AdLib.
//
//   F030MID.TOS [song.mid] [-l] [-t seconds] [-i bytes.bin] [-a | -n]
//
// With a file it plays it; with -l, or with no file and no SONG.MID beside the
// program, it is a synthesizer for the Falcon's MIDI IN port: whatever arrives
// is played through the OPL2 on the DSP, until a key is pressed (or -t seconds).
//
// The 68030 runs the same code the host tools run (midi-opl.h, midi-file.h,
// period-stream.h and the practical decoder): MIDI becomes OPL register
// writes, the decoder turns them into sample-stamped parameter events, and one
// 768-frame period of them goes to the DSP per refill. The DSP owns the codec
// and renders the OPL2 at 49.17 kHz; the host is paced by its READY handshake,
// so a file plays at exactly the speed of the DAC and a live note is heard
// within about two periods (31 ms) of the byte arriving. -i feeds a raw MIDI
// byte file through the same live path at the port's 3,125 bytes a second, for
// a reproducible test (Hatari's MIDI input needs PortMidi): midi-gate.py. A
// MIDIIN.RAW beside the program, when none is named, is taken as that file.
//
// The DSP renders ahead of the codec, block by block as the ring frees, by
// default when playing a file (-a forces it, -n forbids it): a dense passage
// can then borrow the time the quiet ones before it left over, at the price of
// latency, which a file does not mind and a keyboard does. An AHEAD.FLG or
// NOAHEAD.FLG beside the program stands in for -a and -n (Hatari passes no
// arguments).
//
// RESULT.BIN, written when a file ends, carries the DSP's counters for
// midi-gate.py: status (late periods << 12 | periods rendered), the output
// checksum, the least slack in ring words, periods, parameter events,
// note-ons and an overflow flag, as big-endian 32-bit words.
#include <mint/falcon.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp-opl-image.h"
#include "midi-file.h"
#include "midi-opl.h"
#include "opl-upload.h"
#include "period-stream.h"

using namespace OplMidi;

namespace {

enum {
	CMD_PING = 0x010000, CMD_WRITE_X = 0x020000, CMD_WRITE_Y = 0x030000, CMD_STREAM_START = 0x090000,
	CMD_REFILL = 0x0A0000, CMD_STREAM_STOP = 0x0B0000, CMD_STATUS = 0x0C0000, CMD_CHECKSUM = 0x0D0000,
	CMD_MARGIN = 0x0E0000,
	REPLY_PING = 0x4F5052, REPLY_READY = 0x524459
};

// The song's tail after the last event: two seconds, as opl-midi's default.
const uint64_t kTailFrames = 98339;

volatile uint8_t *const kHostIsr = (volatile uint8_t *)0xffffa202;
volatile uint32_t *const kHostData = (volatile uint32_t *)0xffffa204;

// ---- the DSP's host port (supervisor mode only)

// A bound on every wait, so a dead DSP ends the program rather than freezing it.
const long kSpinLimit = 40000000;
bool g_port_failed = false;

inline bool portSend(uint32_t word) {
	for (long i = 0; !(*kHostIsr & 2); ++i)
		if (i > kSpinLimit) {
			g_port_failed = true;
			return false;
		}
	*kHostData = word;
	return true;
}

inline uint32_t portReceive() {
	for (long i = 0; !(*kHostIsr & 1); ++i)
		if (i > kSpinLimit) {
			g_port_failed = true;
			return 0;
		}
	// the low data byte last: reading it clears RXDF
	volatile uint8_t *const b = (volatile uint8_t *)kHostData;
	uint32_t w = (uint32_t)b[1] << 16;
	w |= (uint32_t)b[2] << 8;
	w |= b[3];
	return w;
}

inline uint32_t portExchange(uint32_t word) {
	portSend(word);
	return portReceive();
}

// ---- table upload

// Only the port transfer runs in supervisor mode, one block at a time. The table
// generator (uploadTables) builds the chip image and needs several KB of stack;
// under Supexec that is TOS's small supervisor stack, which a desktop with
// accessories and resident programs may not leave room for.
int g_block_space;
uint16_t g_block_address;
const uint32_t *g_block_words;
unsigned g_block_count;

long blockSuper() {
	portExchange(g_block_space ? CMD_WRITE_Y : CMD_WRITE_X);
	portExchange(g_block_address);
	portExchange(g_block_count);
	for (unsigned i = 0; i < g_block_count && !g_port_failed; ++i)
		portExchange(g_block_words[i] & 0xffffff);
	return 0;
}

struct Uploader {
	void block(int space, uint16_t address, const uint32_t *words, unsigned count) {
		g_block_space = space;
		g_block_address = address;
		g_block_words = words;
		g_block_count = count;
		Supexec(blockSuper);
	}
};

bool g_render_ahead = false;

void uploadAll() {
	Uploader u;
	// An OPL2 never selects the OPL3 waveforms: leave them out.
	uploadTables(u, 9, false);
	if (g_render_ahead) {
		static const uint32_t on = 1;
		u.block(0, OplPractical::SC_RENDER_AHEAD, &on, 1);
	}
}

// ---- one period

const WireEvent *g_events;
unsigned g_event_count;
uint32_t g_ack;

long submitSuper() {
	if (!portSend(CMD_REFILL))
		return 0;
	if (portReceive() != REPLY_READY) {
		g_port_failed = true;
		return 0;
	}
	portSend(g_event_count);
	for (unsigned i = 0; i < g_event_count; ++i) {
		portSend(g_events[i].address);
		portSend(g_events[i].value);
	}
	portSend(0);   // no PCM: the OPL is the whole song
	g_ack = portReceive();
	return 0;
}

uint32_t g_command_word, g_command_reply;
long commandSuper() {
	g_command_reply = portExchange(g_command_word);
	return 0;
}

uint32_t command(uint32_t word) {
	g_command_word = word;
	Supexec(commandSuper);
	return g_command_reply;
}

// ---- big-endian result words

void putWord(FILE *f, uint32_t v) {
	fputc((int)(v >> 24) & 0xff, f);
	fputc((int)(v >> 16) & 0xff, f);
	fputc((int)(v >> 8) & 0xff, f);
	fputc((int)v & 0xff, f);
}

bool bootDsp() {
	if (Dsp_Reserve(16, 16) < 0) {
		printf("the DSP is in use by another program\n");
		return false;
	}
	Dsp_ExecBoot(kAtariDspOplBoot, ATARI_DSP_OPL_BOOT_WORDS, 3);
	static unsigned long reply;
	reply = 0;
	Dsp_BlkUnpacked(kAtariDspOplStream, ATARI_DSP_OPL_STREAM_WORDS, &reply, 1);
	if (reply != ATARI_DSP_OPL_STREAM_REPLY_OK) {
		printf("the DSP loader did not acknowledge (%06lx)\n", reply);
		return false;
	}
	if (command(CMD_PING) != REPLY_PING) {
		printf("the DSP kernel did not answer\n");
		return false;
	}
	return true;
}

// The DSP and the sound system go back to the next program however we leave.
bool g_dsp_held = false;
void releaseDsp() {
	if (g_dsp_held)
		Dsp_Unlock();
	g_dsp_held = false;
}

bool startAudio() {
	if (Locksnd() != 1) {
		printf("the sound system is locked\n");
		return false;
	}
	Buffoper(0);
	Sndstatus(1);
	Soundcmd(4, 2);          // ADDERIN, matrix input
	Setmode(1);              // 16-bit stereo
	Settracks(0, 0);
	Setmontracks(0);
	Dsptristate(1, 0);
	// DSP transmit to the DAC, 25 MHz clock at 49.17 kHz, no handshake
	Devconnect(1, 8, 0, 1, 1);
	command(CMD_STREAM_START);
	return true;
}

void stopAudio() {
	command(CMD_STREAM_STOP);
	Unlocksnd();
	releaseDsp();
}

uint8_t *loadFile(const char *path, uint32_t *size) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *data = n > 0 ? (uint8_t *)malloc((size_t)n) : 0;
	if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
		free(data);
		data = 0;
	}
	fclose(f);
	*size = data ? (uint32_t)n : 0;
	return data;
}

Pipeline g_pipeline;
WireEvent g_wire[Pipeline::kMaxEventsPerPeriod];

struct Totals {
	uint32_t periods, events, note_ons;
	uint32_t late;
};

// Submits the period; returns false if the port died.
bool submit(uint32_t n, Totals &t) {
	g_events = g_wire;
	g_event_count = n;
	Supexec(submitSuper);
	t.periods++;
	t.events += n;
	t.late = g_ack >> 12;
	return !g_port_failed;
}

} // namespace

int main(int argc, char **argv) {
	const char *file = 0;
	bool live = false;
	long seconds = -1;
	int ahead = -1;            // -1: the default for the mode; 0 or 1: asked for
	const char *rawFile = 0;
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "-l"))
			live = true;
		else if (!strcmp(argv[i], "-a"))
			ahead = 1;
		else if (!strcmp(argv[i], "-n"))
			ahead = 0;
		else if (!strcmp(argv[i], "-i") && i + 1 < argc) {
			rawFile = argv[++i];
			live = true;
		}
		else if (!strcmp(argv[i], "-t") && i + 1 < argc)
			seconds = atol(argv[++i]);
		else if (argv[i][0] != '-')
			file = argv[i];
	}
	uint8_t *data = 0;
	uint32_t size = 0;
	SmfPlayer smf;
	if (!file && !rawFile && !live) {
		// no arguments, as when started from the desktop (or by Hatari): a
		// MIDIIN.RAW beside the program is the live-input test stream
		FILE *probe = fopen("MIDIIN.RAW", "rb");
		if (probe) {
			fclose(probe);
			rawFile = "MIDIIN.RAW";
			live = true;
		}
	}
	if (!live) {
		data = loadFile(file ? file : "SONG.MID", &size);
		if (!data && file) {
			printf("cannot read %s\n", file);
			return 1;
		}
		if (!data)
			live = true;       // nothing to play: be a synthesizer
		else if (!smf.load(data, size)) {
			printf("not a format 0 or 1 Standard MIDI File with ticks-per-quarter-note timing\n");
			return 1;
		}
	}
	if (ahead < 0) {
		FILE *flag = fopen("AHEAD.FLG", "rb");
		if (flag) {
			fclose(flag);
			ahead = 1;
		}
		flag = fopen("NOAHEAD.FLG", "rb");
		if (flag) {
			fclose(flag);
			ahead = 0;
		}
	}
	g_render_ahead = ahead < 0 ? !live : ahead != 0;
	printf("F030MID: %s%s\n", live ? "live MIDI IN synthesizer, any key quits" : "playing the file",
	       g_render_ahead ? ", rendering ahead" : "");

	g_dsp_held = true;
	if (!bootDsp()) {
		releaseDsp();
		return 1;
	}
	uploadAll();
	if (g_port_failed) {
		printf("the DSP stopped answering during the table upload\n");
		releaseDsp();
		return 1;
	}
	if (!startAudio()) {
		releaseDsp();
		return 1;
	}

	Totals totals;
	memset(&totals, 0, sizeof(totals));
	g_pipeline.begin(0);

	if (!live) {
		const uint64_t songFrames = smf.totalFrames();
		const uint32_t periods = (uint32_t)((songFrames + kTailFrames + kPeriodFrames - 1) / kPeriodFrames);
		Message m;
		bool have = smf.next(&m);
		for (uint32_t p = 0; p < periods && !g_port_failed; ++p) {
			const uint64_t start = (uint64_t)p * kPeriodFrames, end = start + kPeriodFrames;
			while (have && m.frame < end) {
				if (m.status != 0xf0 && (m.status & 0xf0) == 0x90 && m.data2)
					totals.note_ons++;
				g_pipeline.message(m);
				have = smf.next(&m);
			}
			const unsigned n = g_pipeline.takePeriod(start, g_wire);
			if (!submit(n, totals))
				break;
			if ((p & 63) == 0 && Cconis()) {
				Cnecin();
				break;
			}
		}
	} else {
		MidiStream stream;
		Message m;
		uint32_t p = 0;
		uint32_t rawSize = 0, rawUsed = 0, rawEnd = 0xffffffffu;
		uint8_t *raw = 0;
		if (rawFile) {
			raw = loadFile(rawFile, &rawSize);
			if (!raw) {
				printf("cannot read %s\n", rawFile);
				stopAudio();
				return 1;
			}
			// the period in which the last byte has arrived, and the usual tail after it
			uint32_t last = 0;
			while (liveBytesBy(last) < rawSize)
				++last;
			rawEnd = last + 1 + (uint32_t)((kTailFrames + kPeriodFrames - 1) / kPeriodFrames);
		}
		const uint32_t limit = rawFile ? rawEnd : seconds > 0 ? (uint32_t)(seconds * 64) : 0xffffffffu;   // ~64 periods a second
		while (p < limit && !g_port_failed) {
			const uint64_t start = (uint64_t)p * kPeriodFrames;
			// everything that arrived since the last period sounds at its start
			for (int guard = 0; guard < 512; ++guard) {
				uint8_t byte;
				if (raw) {
					const uint32_t arrived = liveBytesBy(p) < rawSize ? liveBytesBy(p) : rawSize;
					if (rawUsed >= arrived)
						break;
					byte = raw[rawUsed++];
				} else {
					if (!Bconstat(3))
						break;
					byte = (uint8_t)Bconin(3);
				}
				if (stream.feed(byte, &m)) {
					m.frame = start;
					if (m.status != 0xf0 && (m.status & 0xf0) == 0x90 && m.data2)
						totals.note_ons++;
					g_pipeline.message(m);
				}
			}
			const unsigned n = g_pipeline.takePeriod(start, g_wire);
			if (!submit(n, totals))
				break;
			if (!raw && Cconis()) {
				Cnecin();
				break;
			}
			++p;
		}
	}

	// the counters, before the drain: the kernel answers once the last period is rendered
	const uint32_t status = command(CMD_STATUS);
	const uint32_t slack = command(CMD_MARGIN);
	for (int i = 0; i < 7; ++i)
		Vsync();   // let the last rendered periods play out
	const uint32_t checksum = command(CMD_CHECKSUM);

	FILE *out = fopen("RESULT.BIN", "wb");
	if (out) {
		putWord(out, status);
		putWord(out, checksum);
		putWord(out, slack);
		putWord(out, totals.periods);
		putWord(out, totals.events);
		putWord(out, totals.note_ons);
		putWord(out, g_pipeline.overflowed() ? 1 : 0);
		fclose(out);
	}
	printf("periods %lu, events %lu, notes %lu, late periods %lu%s\n", (unsigned long)totals.periods,
	       (unsigned long)totals.events, (unsigned long)totals.note_ons, (unsigned long)(status >> 12),
	       g_pipeline.overflowed() ? ", EVENT OVERFLOW" : "");
	stopAudio();
	free(data);
	return g_port_failed ? 1 : 0;
}
