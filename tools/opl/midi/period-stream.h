// MIDI messages to the DSP's period events: the engine's register writes go
// through the practical decoder (the 68030's job on a Falcon) and come out as
// the sample-stamped parameter events the stream protocol carries.
//
// The host tools and the Falcon player both drive this class, so the events a
// song produces on the Falcon are by construction the ones the host reference
// renders; midi-gate.py checks that against the DSP's output checksum.
#ifndef OPL_PERIOD_STREAM_H
#define OPL_PERIOD_STREAM_H

#include <stdint.h>
#include <string.h>

#include "../opl-kernel.h"
#include "../opl-practical.h"
#include "midi-file.h"
#include "midi-opl.h"

namespace OplMidi {

// One DSP stream period is 24 blocks of 32 frames.
enum { kPeriodFrames = OPL_PRACTICAL_PERIOD_BLOCKS * OPL_PRACTICAL_BLOCK_FRAMES };

// Live MIDI arrives at 31,250 baud, ten bits a byte: 3,125 bytes a second. The
// bytes that have arrived by the start of a period are the ones its events
// include; this schedule (3,125 * 768 / 49,169.921875 bytes a period, as an
// exact ratio) is what the raw-byte test input of the Falcon player and the
// host tool both use, so a live run is as reproducible as a file's.
static inline uint32_t liveBytesBy(uint32_t period) {
	return (uint32_t)((uint64_t)period * 49152ull / 1007ull);
}

// An event as the protocol sends it: two words, the second masked to 24 bits.
struct WireEvent {
	uint32_t address;   // frame within the period << 12 | the DSP's X address
	uint32_t value;
};

class Pipeline : public RegisterSink, private OplPractical::Sink {
public:
	// The DSP holds two event tables of 2,048 events each.
	enum { kMaxEventsPerPeriod = 2048, kPendingCapacity = 3072 };

	Pipeline() : count_(0), overflowed_(false) {}

	// The chip and the engine start from reset at the given frame. A fresh
	// machine (the default: the tables were just uploaded) already holds the
	// reset state in every word the decoder's reset would send except the
	// channels' output routing, which the upload leaves at zero where the reset
	// state is "the mix" (and zero would mute): only those nine words go out,
	// not the 397 the full reset sends, which were the tightest period of a song.
	void begin(uint64_t frame, bool freshMachine = true) {
		count_ = 0;
		overflowed_ = false;
		if (freshMachine) {
			FreshMachine filter(this);
			decoder_.reset(&filter, 9, (uint32_t)frame);
			decoder_.sink = this;   // the reset is the only thing the filter is for
		} else {
			decoder_.reset(this, 9, (uint32_t)frame);
		}
		engine_.reset(this, frame);
	}

	Engine &engine() { return engine_; }
	bool overflowed() const { return overflowed_; }

	// A message at its own frame (a file's), or at the engine's current time
	// (live input: set it first with engine().setTime()).
	void message(const Message &m) {
		if (m.status == 0xf0) {
			engine_.sysex(m.sysex, m.sysexSize);
			return;
		}
		if (m.frame > engine_.time())
			engine_.advanceTo(m.frame);
		engine_.send(m.status, m.data1, m.data2);
	}

	// Finishes the period starting at periodStart and moves its events, with
	// frames made relative to it, into out (at most kMaxEventsPerPeriod).
	// Returns how many. Events of later frames stay for the next period.
	unsigned takePeriod(uint64_t periodStart, WireEvent *out) {
		const uint64_t periodEnd = periodStart + kPeriodFrames;
		if (engine_.time() < periodEnd)
			engine_.advanceTo(periodEnd);
		decoder_.flush();
		unsigned taken = 0, kept = 0;
		for (unsigned i = 0; i < count_; ++i) {
			const OplPractical::Event &e = pending_[i];
			if (e.frame < periodEnd && taken < kMaxEventsPerPeriod) {
				out[taken].address = OplPractical::packEvent((uint32_t)(e.frame - periodStart), e.address);
				out[taken].value = (uint32_t)e.value & 0xffffff;
				++taken;
			} else {
				if (e.frame < periodEnd)
					overflowed_ = true;   // more events than one DSP table holds: dropped
				pending_[kept++] = e;
			}
		}
		count_ = kept;
		return taken;
	}

private:
	// Passes on only the words a freshly uploaded machine does not already hold.
	struct FreshMachine : OplPractical::Sink {
		explicit FreshMachine(Pipeline *p) : owner(p) {}
		void write(uint32_t frame, uint16_t address, int32_t value) override {
			namespace P = OplPractical;
			if (address >= P::kChannelBase && address < P::kChannelBase + P::kChannels * P::kChannelStride &&
			    (address - P::kChannelBase) % P::kChannelStride == P::CH_ROUTE)
				owner->push(frame, address, value);
		}
		Pipeline *owner;
	};

	// RegisterSink: the engine's register writes
	void write(uint64_t frame, uint16_t reg, uint8_t value) override {
		decoder_.write((uint32_t)frame, reg, value);
	}
	// OplPractical::Sink: the decoder's parameter events
	void write(uint32_t frame, uint16_t address, int32_t value) override { push(frame, address, value); }
	void push(uint32_t frame, uint16_t address, int32_t value) {
		if (count_ >= kPendingCapacity) {
			overflowed_ = true;
			return;
		}
		OplPractical::Event &e = pending_[count_++];
		e.frame = frame;
		e.address = address;
		e.value = value;
	}

	Engine engine_;
	OplPractical::Decoder decoder_;
	OplPractical::Event pending_[kPendingCapacity];
	unsigned count_;
	bool overflowed_;
};

} // namespace OplMidi

#endif
