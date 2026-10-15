// The practical decoder's period events for a register stream: the host
// reference opldec-gate.py holds tools/opl/m68k/opldec.s against.
//
// Input (big-endian): 'OPLR', record count, then per record frame.l, reg.w,
// value.w. The records run through OplPractical::Decoder exactly as
// OplMidi::Pipeline drives it - a fresh-machine reset at frame 0 - and each
// period of OPL_PRACTICAL_PERIOD_BLOCKS * BLOCK_FRAMES frames is taken as
// Pipeline::takePeriod takes it, once the stream has passed its end.
// Output: per period, the event count and its wire words (two per event).
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <vector>

#include "opl-practical.h"

namespace P = OplPractical;
enum { kPeriodFrames = OPL_PRACTICAL_PERIOD_BLOCKS * OPL_PRACTICAL_BLOCK_FRAMES, kMax = 2048, kPending = 3072 };

struct Pending : P::Sink {
	std::vector<P::Event> e;
	bool overflow = false, fresh = false;
	void write(uint32_t frame, uint16_t address, int32_t value) override {
		if (fresh && !(address >= P::kChannelBase && address < P::kChannelBase + 9 * P::kChannelStride &&
		               (address - P::kChannelBase) % P::kChannelStride == P::CH_ROUTE))
			return;
		if (e.size() >= kPending) { overflow = true; return; }
		e.push_back(P::Event{frame, address, value});
	}
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put(FILE *f, uint32_t v) { fputc(v >> 24, f); fputc(v >> 16 & 255, f); fputc(v >> 8 & 255, f); fputc(v & 255, f); }

int main(int argc, char **argv) {
	if (argc != 3) { fprintf(stderr, "usage: opldec-ref REGS.BIN EVENTS.BIN\n"); return 2; }
	FILE *in = fopen(argv[1], "rb");
	if (!in) return 1;
	std::vector<uint8_t> b;
	for (int c; (c = fgetc(in)) != EOF;) b.push_back((uint8_t)c);
	fclose(in);
	if (b.size() < 8 || be32(&b[0]) != 0x4f504c52) { fprintf(stderr, "not a register stream\n"); return 1; }
	const uint32_t n = be32(&b[4]);
	static P::Decoder d;
	Pending pend;
	pend.fresh = true;
	d.reset(&pend, 9, 0);
	pend.fresh = false;
	FILE *out = fopen(argv[2], "wb");
	uint32_t period = 0;
	auto take = [&]() {
		d.flush();
		const uint32_t start = period * kPeriodFrames, end = start + kPeriodFrames;
		std::vector<P::Event> kept;
		std::vector<uint32_t> wire;
		for (const P::Event &e : pend.e) {
			const bool due = (int32_t)(e.frame - end) < 0;
			if (due && wire.size() / 2 < kMax) {
				wire.push_back(P::packEvent(e.frame - start, e.address));
				wire.push_back((uint32_t)e.value & 0xffffff);
			} else {
				if (due) pend.overflow = true;
				kept.push_back(e);
			}
		}
		pend.e = kept;
		put(out, (uint32_t)wire.size() / 2);
		for (uint32_t w : wire) put(out, w);
		++period;
	};
	for (uint32_t i = 0; i < n; ++i) {
		const uint8_t *r = &b[8 + 8 * i];
		const uint32_t frame = be32(r);
		while (frame >= (period + 1) * kPeriodFrames) take();
		d.write(frame, (uint16_t)(r[4] << 8 | r[5]), r[7]);
	}
	take();
	put(out, pend.overflow ? 0xffffffffu : 0);
	fclose(out);
	return 0;
}
