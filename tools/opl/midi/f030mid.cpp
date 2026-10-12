// F030MID: a real-time MIDI player for the Falcon's emulated AdLib.
//
//   F030MID.TOS [song.mid] [-l] [-t seconds] [-i bytes.bin] [-a | -n] [-d]
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
// Under plain TOS a file's periods are built ahead of the DSP, up to 32 of
// them (half a second), in the time the 68030 would otherwise spend waiting
// for READY: a dense chord can take it longer to build than the DSP has
// rendered ahead, and the queue absorbs that. Under MiNT, and for live input,
// each period is built and then exchanged in one Supexec, as before.
//
// The DSP renders ahead of the codec, block by block as the ring frees, by
// default when playing a file (-a forces it, -n forbids it): a dense passage
// can then borrow the time the quiet ones before it left over, at the price of
// latency, which a file does not mind and a keyboard does. An AHEAD.FLG or
// NOAHEAD.FLG beside the program stands in for -a and -n (Hatari passes no
// arguments).
//
// -d (or a DIAG.FLG beside the program) is for a machine that bombs: it prints
// what the machine looks like (TOS, cookies, TPA, memory) and waits for a key at
// each stage, so the screen shows how far the program got.
//
// Under FreeMiNT it locks the DSP and the sound system, and SIGINT, SIGTERM,
// SIGQUIT and SIGHUP stop the song like a key, so both are handed back. Under
// plain TOS, Control-C is read as a key like any other (the BIOS, not GEMDOS,
// reads the keyboard), and whatever else ends the program hands both back
// through the terminate vector. Each
// period's exchange still waits for READY inside one Supexec, which MiNT cannot
// preempt: with only two periods of audio buffered on the DSP, a wait that MiNT
// can preempt loses to any busy process beside it (docs/midi-player.md).
//
// RESULT.BIN, written when a file ends, carries the DSP's counters for
// midi-gate.py: status (late periods << 12 | periods rendered), the output
// checksum, the least slack in ring words, periods, parameter events,
// note-ons and an overflow flag, as big-endian 32-bit words.
#include <mint/basepage.h>
#include <mint/falcon.h>
#include <mint/osbind.h>
#include <mint/sysbind.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp-opl-image.h"
#include "midi-file.h"
#include "midi-opl.h"
#include "opl-upload.h"
#include "period-stream.h"

using namespace OplMidi;

#ifndef F030MID_VERSION
#define F030MID_VERSION "unknown"
#endif

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

// ---- stopping

// SIGINT, SIGTERM, SIGQUIT and SIGHUP stop the song the way a key does, so the
// DSP and the sound system are always handed back.
volatile sig_atomic_t g_stop = 0;
void onSignal(int) {
	g_stop = 1;
}

// A key at the console, read through the BIOS: GEMDOS's console calls would
// end the program on Control-C before it could hand anything back.
bool keyPressed() {
	if (!Bconstat(2))
		return false;
	Bconin(2);
	return true;
}

// ---- -d: say what the machine looks like and stop at each stage

// For a machine that bombs: the screen then shows how far the program got, and
// the state that differs between a bare boot and a desktop with accessories.
bool g_diag = false;

unsigned long stackPointer() {
	unsigned long sp;
	asm volatile("move.l %%sp,%0" : "=r"(sp));
	return sp;
}

// A stage waits for a key; a note does not (the DSP is streaming by then).
void note(const char *what) {
	if (!g_diag)
		return;
	printf("[%s] sp %08lx\n", what, stackPointer());
	fflush(stdout);
}

void stage(const char *what) {
	if (!g_diag)
		return;
	printf("[%s] sp %08lx  key\n", what, stackPointer());
	fflush(stdout);
#ifndef DIAG_NOWAIT
	Cconin();
#endif
}

struct Cookie {
	uint32_t tag, value;
};
Cookie g_cookies[32];
unsigned g_cookie_count;
uint16_t g_tos_version;
uint32_t g_tos_date;
// vectors a resident program may have taken over: a * marks one outside the TOS 4.02 ROM ($e00000..$efffff)
uint32_t g_vec[11];
uint32_t g_vbl[8];
uint32_t g_nvbls;
uint32_t g_mem[3];

long readSystemSuper() {
	const uint8_t *os = (const uint8_t *)*(volatile uint32_t *)0x4f2;   // _sysbase
	g_tos_version = (uint16_t)((os[2] << 8) | os[3]);
	g_tos_date = ((uint32_t)os[0x18] << 24) | ((uint32_t)os[0x19] << 16) | ((uint32_t)os[0x1a] << 8) | os[0x1b];
	static const uint32_t kVectors[11] = {0x08, 0x84, 0x88, 0xb4, 0xb8, 0x70, 0x400, 0x404, 0x408, 0x472, 0x476};
	for (unsigned i = 0; i < 11; ++i)
		g_vec[i] = *(volatile uint32_t *)kVectors[i];
	g_nvbls = *(volatile uint16_t *)0x454;
	const uint32_t *queue = (const uint32_t *)*(volatile uint32_t *)0x456;
	for (unsigned i = 0; i < 8; ++i)
		g_vbl[i] = queue && i < g_nvbls ? queue[i] : 0;
	g_mem[0] = *(volatile uint32_t *)0x432;   // _membot
	g_mem[1] = *(volatile uint32_t *)0x436;   // _memtop
	g_mem[2] = *(volatile uint32_t *)0x42e;   // phystop
	g_cookie_count = 0;
	const uint32_t *jar = (const uint32_t *)*(volatile uint32_t *)0x5a0;   // _p_cookies
	while (jar && jar[0] && g_cookie_count < 32) {
		g_cookies[g_cookie_count].tag = jar[0];
		g_cookies[g_cookie_count].value = jar[1];
		++g_cookie_count;
		jar += 2;
	}
	return 0;
}

void describeMachine() {
	Supexec(readSystemSuper);
	printf("TOS %x.%02x, %08lx\n", g_tos_version >> 8, g_tos_version & 0xff, (unsigned long)g_tos_date);
	printf("cookies:\n");
	for (unsigned i = 0; i < g_cookie_count; ++i) {
		const uint32_t t = g_cookies[i].tag;
		printf(" %c%c%c%c=%lx%s", (int)(t >> 24) & 0x7f, (int)(t >> 16) & 0x7f, (int)(t >> 8) & 0x7f, (int)t & 0x7f,
		       (unsigned long)g_cookies[i].value, i % 4 == 3 || i + 1 == g_cookie_count ? "\n" : "");
	}
	static const char *const kNames[11] = {"bus", "trap1", "trap2", "trap13", "trap14", "vbl", "etv_timer", "etv_critic",
	                                       "etv_term", "hdv_bpb", "hdv_rw"};
	for (unsigned i = 0; i < 11; ++i)
		printf("%s %08lx%s%s", kNames[i], (unsigned long)g_vec[i], g_vec[i] >= 0xe00000 && g_vec[i] < 0xf00000 ? "" : " *",
		       i % 2 ? "\n" : "  ");
	printf("\nVBL queue (%lu):", (unsigned long)g_nvbls);
	for (unsigned i = 0; i < 8 && i < g_nvbls; ++i)
		if (g_vbl[i])
			printf(" %08lx", (unsigned long)g_vbl[i]);
	printf("\nmembot %08lx memtop %08lx phystop %08lx\n", (unsigned long)g_mem[0], (unsigned long)g_mem[1],
	       (unsigned long)g_mem[2]);
	printf("basepage %08lx\nTPA %08lx..%08lx (%lu KB)\n", (unsigned long)_base, (unsigned long)_base->p_lowtpa,
	       (unsigned long)_base->p_hitpa, (unsigned long)((_base->p_hitpa - _base->p_lowtpa) >> 10));
	printf("text %lu data %lu bss %lu\n", (unsigned long)_base->p_tlen, (unsigned long)_base->p_dlen,
	       (unsigned long)_base->p_blen);
	printf("free ST-RAM %ld KB, TT-RAM %ld KB\n", (long)Mxalloc(-1L, 0) >> 10, (long)Mxalloc(-1L, 1) >> 10);
	printf("command line: %ld bytes\n", (long)(unsigned char)_base->p_cmdlin[0]);
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

// File playback splits the exchange so the 68030 can build periods ahead
// while the DSP renders: the request goes out at once, the READY is looked
// for between periods (the kernel keeps rendering until the payload's first
// word arrives), and only a full queue waits for it.
long requestSuper() {
	portSend(CMD_REFILL);
	return 0;
}

bool g_ready;
long pollReadySuper() {
	g_ready = false;
	if (!(*kHostIsr & 1))
		return 0;
	if (portReceive() != REPLY_READY)
		g_port_failed = true;
	else
		g_ready = true;
	return 0;
}

long waitReadySuper() {
	if (portReceive() != REPLY_READY)
		g_port_failed = true;
	return 0;
}

long payloadSuper() {
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

// The DSP and the sound system go back to the next program however we leave.
bool g_dsp_held = false;
bool g_sound_held = false;
bool g_streaming = false;

bool bootDsp() {
	if (Dsp_Lock() != 0) {
		printf("the DSP is in use by another program\n");
		return false;
	}
	g_dsp_held = true;
	if (Dsp_Reserve(16, 16) < 0) {
		printf("the DSP cannot reserve its memory\n");
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

void releaseDsp() {
	if (g_dsp_held)
		Dsp_Unlock();
	g_dsp_held = false;
}

bool startAudio() {
	stage("before Locksnd");
	if (Locksnd() != 1) {
		printf("the sound system is locked\n");
		return false;
	}
	g_sound_held = true;
	stage("locked, before Buffoper..Setmontracks");
	Buffoper(0);
	Sndstatus(1);
	Soundcmd(4, 2);          // ADDERIN, matrix input
	Setmode(1);              // 16-bit stereo
	Settracks(0, 0);
	Setmontracks(0);
	stage("mode set, before Dsptristate/Devconnect");
	Dsptristate(1, 0);
	// DSP transmit to the DAC, 25 MHz clock at 49.17 kHz, no handshake
	Devconnect(1, 8, 0, 1, 1);
	stage("connected, before the stream starts");
	command(CMD_STREAM_START);
	g_streaming = true;
	note("stream started");
	return true;
}

void stopAudio() {
	if (g_streaming)
		command(CMD_STREAM_STOP);
	g_streaming = false;
	if (g_sound_held)
		Unlocksnd();
	g_sound_held = false;
	releaseDsp();
}

// GEMDOS calls the terminate vector as any process ends: after main's own
// cleanup it finds nothing left; after Control-C in a GEMDOS console call, or
// any other end, it finds the stream, the sound system and the DSP still held.
// TOS calls it in supervisor mode; the stop goes through Supexec all the same,
// which works from either mode. The vector goes back to what it was, and that
// is called in turn.
typedef void (*TerminateVector)();
TerminateVector g_old_terminate;

void onTerminate() {
	if (g_streaming && !g_port_failed)
		command(CMD_STREAM_STOP);
	g_streaming = false;
	if (g_sound_held)
		Unlocksnd();
	g_sound_held = false;
	releaseDsp();
	Setexc(0x102, (long)g_old_terminate);
	if (g_old_terminate)
		g_old_terminate();
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

// The periods built ahead of the DSP: a ring of up to kQueuePeriods, their
// events in one pool. A dense chord can take the 68030 longer to build than
// the two periods the DSP holds rendered; built ahead in the time the
// waits for READY used to take, it is ready when the DSP asks.
enum { kQueuePeriods = 32, kPoolEvents = 16384 };
struct Queued {
	unsigned first, count;
};
Queued g_queue[kQueuePeriods];
unsigned g_queue_head, g_queue_size;
WireEvent *g_pool;
unsigned g_pool_head, g_pool_tail;

// Where the next period's events may go (room for a full DSP table), or false.
bool poolRoom(unsigned *at) {
	enum { kNeed = Pipeline::kMaxEventsPerPeriod };
	if (!g_queue_size)
		g_pool_head = g_pool_tail = 0;
	if (g_queue_size == kQueuePeriods)
		return false;
	if (!g_queue_size || g_pool_tail >= g_pool_head) {
		if (kPoolEvents - g_pool_tail >= kNeed) {
			*at = g_pool_tail;
			return true;
		}
		if (g_pool_head > kNeed) {   // wrap: the start of the pool is free up to the oldest period
			*at = 0;
			return true;
		}
		return false;
	}
	if (g_pool_head - g_pool_tail > kNeed) {
		*at = g_pool_tail;
		return true;
	}
	return false;
}

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
	// MiNT semantics (EINVFN under TOS). Under MiNT a file is built and sent
	// one period at a time, each exchange in one Supexec: building ahead
	// returns to user mode between periods, where MiNT gives a busy neighbour
	// whole time slices, and beside a busy loop the demo then had 332 late
	// periods instead of 45 (and 345 with the queue held to one period, its
	// exchange split over three Supexecs).
	const bool mint = Pdomain(1) >= 0;
	g_old_terminate = (TerminateVector)Setexc(0x102, (long)onTerminate);
	signal(SIGINT, onSignal);
	signal(SIGTERM, onSignal);
	signal(SIGQUIT, onSignal);
	signal(SIGHUP, onSignal);
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "-l"))
			live = true;
		else if (!strcmp(argv[i], "-d"))
			g_diag = true;
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
	if (!g_diag) {
		FILE *flag = fopen("DIAG.FLG", "rb");
		if (flag) {
			fclose(flag);
			g_diag = true;
		}
	}
	if (g_diag) {
		describeMachine();
		stage("start");
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
	printf("F030MID " F030MID_VERSION ": %s%s\n", live ? "live MIDI IN synthesizer, any key quits" : "playing the file",
	       g_render_ahead ? ", rendering ahead" : "");

	stage("file loaded, before the DSP");
	if (!bootDsp()) {
		releaseDsp();
		return 1;
	}
	stage("DSP booted, before the table upload");
	uploadAll();
	stage("tables uploaded");
	if (g_port_failed) {
		printf("the DSP stopped answering during the table upload\n");
		releaseDsp();
		return 1;
	}
	if (!startAudio()) {
		releaseDsp();
		return 1;
	}
	note("audio running, playing");

	Totals totals;
	memset(&totals, 0, sizeof(totals));
	g_pipeline.begin(0);

	if (!live) {
		// The song's length is known once its last message is read, while it
		// plays: a pass over the whole file before the first period would keep
		// a long, dense one silent for seconds. The last message sounds in the
		// period that reads it, so the count is set before the loop reaches it.
		uint32_t periods = 0xffffffffu;
		uint64_t lastFrame = 0;
		Message m;
		bool have = smf.next(&m);
		if (!have)
			periods = (uint32_t)((kTailFrames + kPeriodFrames - 1) / kPeriodFrames);
		for (uint32_t p = 0; mint && p < periods && !g_port_failed && !g_stop; ++p) {
			const uint64_t start = (uint64_t)p * kPeriodFrames, end = start + kPeriodFrames;
			while (have && m.frame < end) {
				if (m.status != 0xf0 && (m.status & 0xf0) == 0x90 && m.data2)
					totals.note_ons++;
				lastFrame = m.frame;
				g_pipeline.message(m);
				have = smf.next(&m);
				if (!have)
					periods = (uint32_t)((lastFrame + kTailFrames + kPeriodFrames - 1) / kPeriodFrames);
			}
			const unsigned n = g_pipeline.takePeriod(start, g_wire);
			if (!submit(n, totals))
				break;
			if (g_diag && (p < 3 || (p & 1023) == 0)) {
				printf("period %lu\n", (unsigned long)p);
				fflush(stdout);
			}
			if ((p & 63) == 0 && keyPressed())
				break;
		}
		g_pool = mint ? 0 : (WireEvent *)malloc(kPoolEvents * sizeof(WireEvent));
		if (!mint && !g_pool) {
			printf("not enough memory for the period queue\n");
			stopAudio();
			return 1;
		}
		uint32_t built = 0, sent = 0;
		bool requested = false;
		// A stop is taken only between a payload and the next request: one
		// outstanding would read the closing status command as its count.
		while (!mint && sent < periods && !g_port_failed) {
			if (!requested) {
				Supexec(requestSuper);
				requested = true;
			}
			// Build ahead until the DSP has answered or the queue is full;
			// with nothing built, build the period it will ask for now.
			unsigned at;
			if (built < periods && (built == sent || !g_ready) && poolRoom(&at)) {
				if (built != sent)
					Supexec(pollReadySuper);
				if (built == sent || !g_ready) {
					const uint64_t start = (uint64_t)built * kPeriodFrames, end = start + kPeriodFrames;
					while (have && m.frame < end) {
						if (m.status != 0xf0 && (m.status & 0xf0) == 0x90 && m.data2)
							totals.note_ons++;
						lastFrame = m.frame;
						g_pipeline.message(m);
						have = smf.next(&m);
						if (!have)
							periods = (uint32_t)((lastFrame + kTailFrames + kPeriodFrames - 1) / kPeriodFrames);
					}
					Queued &q = g_queue[(g_queue_head + g_queue_size) % kQueuePeriods];
					q.first = at;
					q.count = g_pipeline.takePeriod(start, g_pool + at);
					g_pool_tail = at + q.count;
					++g_queue_size;
					++built;
					continue;
				}
			}
			if (!g_ready)
				Supexec(waitReadySuper);
			g_ready = false;
			if (g_port_failed)
				break;
			const Queued &q = g_queue[g_queue_head];
			g_events = g_pool + q.first;
			g_event_count = q.count;
			Supexec(payloadSuper);
			g_queue_head = (g_queue_head + 1) % kQueuePeriods;
			if (--g_queue_size)
				g_pool_head = g_queue[g_queue_head].first;
			requested = false;
			totals.periods++;
			totals.events += g_event_count;
			totals.late = g_ack >> 12;
			if (g_diag && (sent < 3 || (sent & 1023) == 0)) {
				printf("period %lu\n", (unsigned long)sent);
				fflush(stdout);
			}
			if (g_stop || ((sent & 63) == 0 && keyPressed()))
				break;
			++sent;
		}
		free(g_pool);
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
		while (p < limit && !g_port_failed && !g_stop) {
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
			if (!raw && keyPressed())
				break;
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
	printf("F030MID " F030MID_VERSION ": periods %lu, events %lu, notes %lu, late periods %lu%s\n", (unsigned long)totals.periods,
	       (unsigned long)totals.events, (unsigned long)totals.note_ons, (unsigned long)(status >> 12),
	       g_pipeline.overflowed() ? ", EVENT OVERFLOW" : "");
	stopAudio();
	free(data);
	return g_port_failed ? 1 : 0;
}
