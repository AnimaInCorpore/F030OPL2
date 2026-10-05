// MIDI to OPL2 register writes: channel state, voice allocation, patches.
//
// The engine knows nothing about time beyond the frame stamp the caller gives
// it, nor about the chip beyond its registers: every effect is a sequence of
// register writes through a RegisterSink, stamped with a codec-rate frame.
// The same header builds for the host tools and, with the m68k cross
// compiler, for the Falcon's 68030; it uses no floating point, no heap and no
// library beyond <stdint.h>.
//
// The instrument bank, the velocity and channel-volume arithmetic and the
// percussion mapping follow ScummVM's AdLib driver (audio/adlib.cpp, GPL-3.0
// or later); the voice allocator, the pitch calculation and the controller
// handling are this project's. Pitch is the chip's own formula (frequency =
// f-number * 49,716 Hz / 2^(20 - block)) applied one octave below the MIDI
// note: the bank's carriers mostly run at twice the base frequency (74 of the
// 128 melodic patches use frequency multiplier 2), which is how ScummVM's
// driver, whose f-number table sits an octave low, sounds true pitch.
#ifndef OPL_MIDI_OPL_H
#define OPL_MIDI_OPL_H

#include <stdint.h>

#include "gm-bank.h"

namespace OplMidi {

struct RegisterSink {
	virtual ~RegisterSink() {}
	virtual void write(uint64_t frame, uint16_t reg, uint8_t value) = 0;
};

class Engine {
public:
	enum { kVoices = 9, kChannels = 16, kPercussionChannel = 9 };
	// 250 Hz, the rate the AdLib drivers of the ScummVM games tick at; the
	// percussion patches' durations count in these ticks.
	enum { kTicksPerSecond = 250 };

	Engine() : sink_(0) { reset(0, 0); }

	// The chip's initial state, the controllers' defaults, every voice silent.
	void reset(RegisterSink *sink, uint64_t frame) {
		sink_ = sink;
		now_ = frame;
		tickCount_ = frame * 512ull / 100700ull;   // the ticks that have already fallen
		while (frameOfTick(tickCount_ + 1) <= frame)
			++tickCount_;
		age_ = 0;
		for (int c = 0; c < kChannels; ++c) {
			Channel &ch = channel_[c];
			ch.program = 0;
			ch.volume = 100;           // General MIDI's default
			ch.expression = 127;
			ch.bend = 0;
			ch.bendRange = 2;
			ch.modulation = 0;
			ch.pedal = false;
			ch.rpnMsb = ch.rpnLsb = 127;
		}
		for (int v = 0; v < kVoices; ++v) {
			Voice &vo = voice_[v];
			vo.channel = 0xff;
			vo.note = 0;
			vo.keyed = vo.held = false;
			vo.age = 0;
			vo.releasedAt = 0;
			vo.remaining = 0;
			vo.patch = 0;
			vo.vol1 = vo.vol2 = 0;
			vo.fnum = 0;
			vo.block = 0;
		}
		if (!sink_)
			return;
		put(0x01, 0x20);   // waveform select enable: the patches' waveforms count
		put(0x08, 0x40);   // keyboard split: note-select
		put(0xbd, 0x00);   // melodic mode, shallow tremolo and vibrato
		for (int v = 0; v < kVoices; ++v) {
			put(0xb0 + v, 0);
			for (int o = 0; o < 2; ++o)
				put(0x40 + opOffset(v, o), 0x3f);
		}
	}

	// A channel message. status is the full status byte (0x80-0xef).
	void send(uint8_t status, uint8_t d1, uint8_t d2) {
		const uint8_t ch = status & 0x0f;
		switch (status & 0xf0) {
		case 0x80: noteOff(ch, d1); break;
		case 0x90: if (d2) noteOn(ch, d1, d2); else noteOff(ch, d1); break;
		case 0xb0: control(ch, d1, d2); break;
		case 0xc0: channel_[ch].program = d1 & 0x7f; break;
		case 0xe0: pitchBend(ch, (int)(d1 & 0x7f) | ((int)(d2 & 0x7f) << 7)); break;
		default: break;   // key pressure and the like: not an OPL2 feature
		}
	}

	// A system-exclusive message's payload after F0 (without the F7): the
	// General MIDI reset is the one that matters.
	void sysex(const uint8_t *data, unsigned size) {
		if (size >= 4 && data[0] == 0x7e && data[2] == 0x09 && data[3] == 0x01)
			reset(sink_, now_);
	}

	// Sets the stamp of the writes the next calls make.
	void setTime(uint64_t frame) { now_ = frame; }
	uint64_t time() const { return now_; }

	// Moves the clock to a later frame, running the percussion durations'
	// 250 Hz ticks that fall between, each stamped at its own frame.
	void advanceTo(uint64_t frame) {
		while (now_ < frame) {
			const uint64_t nextTick = frameOfTick(tickCount_ + 1);
			if (nextTick > frame) {
				now_ = frame;
				return;
			}
			if (nextTick > now_)
				now_ = nextTick;
			++tickCount_;
			tick();
		}
	}

	int sounding() const {
		int n = 0;
		for (int v = 0; v < kVoices; ++v)
			n += voice_[v].keyed;
		return n;
	}

private:
	struct Channel {
		uint8_t program, volume, expression, modulation, bendRange, rpnMsb, rpnLsb;
		int bend;            // -8192..8191
		bool pedal;
	};
	struct Voice {
		uint8_t channel;     // 0xff: unused
		uint8_t note;
		bool keyed, held;    // held: key released while the sustain pedal was down
		uint32_t age;
		uint64_t releasedAt;
		int remaining;       // percussion: ticks of the patch's duration left, else 0
		const Patch *patch;
		uint8_t vol1, vol2;  // the instrument's own levels plus velocity, before channel volume
		uint16_t fnum;
		uint8_t block;
	};

	// ---- tables
	// The codec rate is 25,175,000 / 512 Hz exactly, so tick n of 250 per second
	// falls at frame n * 25,175,000 / (512 * 250) = n * 1007 / 5.12, rounded up.
	static uint64_t frameOfTick(uint64_t n) { return (n * 1007ull * 100ull + 511) / 512; }

	// Frequency to f-number: round(344.88 * 2^(r/12)), r = 0..12; the f-number
	// of semitone r above C at block (octave - 1).
	static uint16_t semitoneFnum(int r) {
		static const uint16_t t[13] = { 345, 365, 387, 410, 434, 460, 487, 516, 547, 579, 614, 650, 690 };
		return t[r];
	}
	// ScummVM's linear amplitude (0-63) to attenuation curve.
	static uint8_t volumeCurve(int v) {
		static const uint8_t t[64] = {
			0, 4, 7, 11, 13, 16, 18, 20, 22, 24, 26, 27, 29, 30, 31, 33,
			34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 44, 45, 46, 47, 47,
			48, 49, 49, 50, 51, 51, 52, 53, 53, 54, 54, 55, 55, 56, 56, 57,
			57, 58, 58, 59, 59, 60, 60, 60, 61, 61, 62, 62, 62, 63, 63, 63
		};
		return t[v];
	}
	// ScummVM's g_volumeLookupTable[a][b]: a scaled by (b + 1) / 32, zero at b = 0.
	static int scale(int a, int b) { return b == 0 ? 0 : (a * (b + 1)) >> 5; }

	// Operator register offsets of the nine channels.
	static int opOffset(int voice, int which) {
		static const uint8_t mod[9] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };
		return mod[voice] + 3 * which;
	}

	void put(int reg, int value) {
		if (sink_)
			sink_->write(now_, (uint16_t)reg, (uint8_t)value);
	}

	// ---- time
	void tick() {
		for (int v = 0; v < kVoices; ++v) {
			Voice &vo = voice_[v];
			if (vo.remaining > 0 && --vo.remaining == 0 && vo.keyed)
				keyOff(v, false);
		}
	}

	// ---- voices
	void noteOn(int ch, int note, int velocity) {
		const Patch *patch;
		if (ch == kPercussionChannel) {
			const uint8_t key = kGmPercussionMap[note & 0x7f];
			if (key == 0xff)
				return;
			patch = &kGmPercussion[key];
		} else {
			patch = &kGmMelodic[channel_[ch].program];
		}
		// A key already sounding the same note on this channel is retriggered
		// on its own voice.
		int v = -1;
		for (int i = 0; i < kVoices; ++i)
			if (voice_[i].channel == ch && voice_[i].note == note && voice_[i].keyed) {
				v = i;
				break;
			}
		if (v < 0)
			v = allocate();
		Voice &vo = voice_[v];
		if (vo.keyed || vo.channel != 0xff)
			put(0xb0 + v, vo.block << 2 | (vo.fnum >> 8));   // key up first: the key-on below is an edge
		vo.channel = (uint8_t)ch;
		vo.note = (uint8_t)note;
		vo.keyed = true;
		vo.held = false;
		vo.age = ++age_;
		vo.patch = patch;
		vo.remaining = (ch == kPercussionChannel && patch->duration) ? (patch->duration * 63 + 16) / 17 : 0;

		int v1 = (patch->modLevel & 0x3f) + scale(velocity >> 1, patch->modWaveform >> 2);
		int v2 = (patch->carLevel & 0x3f) + scale(velocity >> 1, patch->carWaveform >> 2);
		vo.vol1 = (uint8_t)(v1 > 63 ? 63 : v1);
		vo.vol2 = (uint8_t)(v2 > 63 ? 63 : v2);

		const int o1 = opOffset(v, 0), o2 = opOffset(v, 1);
		const int mod = channel_[ch].modulation >= 32 ? 0x40 : 0;
		put(0x20 + o1, patch->modCharacteristic | mod);
		put(0x40 + o1, levelRegister(patch->modLevel, modulatorVolume(vo, channel_[ch])));
		put(0x60 + o1, ~patch->modAttackDecay & 0xff);
		put(0x80 + o1, ~patch->modSustainRelease & 0xff);
		put(0xe0 + o1, patch->modWaveform);
		put(0x20 + o2, patch->carCharacteristic | mod);
		put(0x40 + o2, levelRegister(patch->carLevel, carrierVolume(vo, channel_[ch])));
		put(0x60 + o2, ~patch->carAttackDecay & 0xff);
		put(0x80 + o2, ~patch->carSustainRelease & 0xff);
		put(0xe0 + o2, patch->carWaveform);
		put(0xc0 + v, patch->feedback);
		pitch(v, true);
	}

	void noteOff(int ch, int note) {
		for (int v = 0; v < kVoices; ++v) {
			Voice &vo = voice_[v];
			if (vo.channel != ch || vo.note != note || !vo.keyed)
				continue;
			if (channel_[ch].pedal)
				vo.held = true;
			else
				keyOff(v, false);
		}
	}

	void keyOff(int v, bool) {
		Voice &vo = voice_[v];
		vo.keyed = false;
		vo.held = false;
		vo.releasedAt = now_;
		put(0xb0 + v, vo.block << 2 | (vo.fnum >> 8));
	}

	// The voice for a new note: one never used, else the one released longest
	// ago, else the oldest sounding note.
	int allocate() {
		int best = 0;
		uint64_t bestScore = ~0ull;
		for (int v = 0; v < kVoices; ++v) {
			const Voice &vo = voice_[v];
			uint64_t score;
			if (vo.channel == 0xff)
				score = 0;
			else if (!vo.keyed)
				score = 1 + vo.releasedAt;
			else
				score = (1ull << 62) + vo.age;
			if (score < bestScore) {
				bestScore = score;
				best = v;
			}
		}
		return best;
	}

	// ---- levels
	// The modulator is attenuated by channel volume only when it is a
	// carrier too (additive connection), as ScummVM's driver does.
	static int modulatorVolume(const Voice &vo, const Channel &ch) {
		if (!(vo.patch->feedback & 1))
			return vo.vol1;
		return volumeCurve(scale(vo.vol1, channelVolume(ch)));
	}
	static int carrierVolume(const Voice &vo, const Channel &ch) {
		return volumeCurve(scale(vo.vol2, channelVolume(ch)));
	}
	// 0-31: CC7 and CC11 combined
	static int channelVolume(const Channel &ch) {
		return ((ch.volume * ch.expression) / 127) >> 2;
	}
	static int levelRegister(int patchLevel, int volume) {
		return ((patchLevel | 0x3f) - volume) & 0xff;
	}

	void refreshLevels(int ch) {
		for (int v = 0; v < kVoices; ++v) {
			Voice &vo = voice_[v];
			if (vo.channel != ch || !vo.patch)
				continue;
			put(0x40 + opOffset(v, 0), levelRegister(vo.patch->modLevel, modulatorVolume(vo, channel_[ch])));
			put(0x40 + opOffset(v, 1), levelRegister(vo.patch->carLevel, carrierVolume(vo, channel_[ch])));
		}
	}

	// ---- pitch
	// f-number and block of a note plus an offset in 1/256 semitone. The base
	// frequency is an octave below the note's (see the file header).
	static void noteToFnum(int note, int offset256, uint16_t *fnum, uint8_t *block) {
		int pos = (note - 12) * 256 + offset256;
		if (pos < 0)
			pos = 0;
		int semitone = pos >> 8;
		const int frac = pos & 255;
		if (semitone > 127)
			semitone = 127;
		const int octave = semitone / 12, r = semitone % 12;
		int f = semitoneFnum(r) + (((semitoneFnum(r + 1) - semitoneFnum(r)) * frac) >> 8);
		int b = octave - 1;
		if (b < 0) {            // octave 0 runs at block 0 with half the f-number
			b = 0;
			f >>= 1;
			if (f < 1)
				f = 1;
		} else if (b > 7) {     // above block 7 the f-number doubles per octave, to its limit
			f <<= (b - 7);
			b = 7;
		}
		if (f > 1023)
			f = 1023;
		*fnum = (uint16_t)f;
		*block = (uint8_t)b;
	}

	void pitch(int v, bool keyOn) {
		Voice &vo = voice_[v];
		const Channel &ch = channel_[vo.channel];
		const int offset = (ch.bend * ch.bendRange * 256) / 8192;
		noteToFnum(vo.note, offset, &vo.fnum, &vo.block);
		put(0xa0 + v, vo.fnum & 0xff);
		put(0xb0 + v, (keyOn ? 0x20 : 0) | vo.block << 2 | (vo.fnum >> 8));
	}

	void pitchBend(int ch, int value14) {
		channel_[ch].bend = value14 - 8192;
		for (int v = 0; v < kVoices; ++v)
			if (voice_[v].channel == ch && voice_[v].keyed) {
				Voice &vo = voice_[v];
				const Channel &c = channel_[ch];
				noteToFnum(vo.note, (c.bend * c.bendRange * 256) / 8192, &vo.fnum, &vo.block);
				put(0xa0 + v, vo.fnum & 0xff);
				put(0xb0 + v, 0x20 | vo.block << 2 | (vo.fnum >> 8));
			}
	}

	// ---- controllers
	void control(int ch, int cc, int value) {
		Channel &c = channel_[ch];
		switch (cc) {
		case 1:
			c.modulation = (uint8_t)value;
			setVibrato(ch, value >= 32);
			break;
		case 6:     // data entry: the pitch bend range, in semitones, when RPN 0 is selected
			if (c.rpnMsb == 0 && c.rpnLsb == 0 && value > 0)
				c.bendRange = (uint8_t)(value > 24 ? 24 : value);
			break;
		case 7:
			c.volume = (uint8_t)value;
			refreshLevels(ch);
			break;
		case 11:
			c.expression = (uint8_t)value;
			refreshLevels(ch);
			break;
		case 64:
			c.pedal = value >= 64;
			if (!c.pedal)
				for (int v = 0; v < kVoices; ++v)
					if (voice_[v].channel == ch && voice_[v].held)
						keyOff(v, false);
			break;
		case 100: c.rpnLsb = (uint8_t)value; break;
		case 101: c.rpnMsb = (uint8_t)value; break;
		case 120:   // all sound off
		case 123:   // all notes off
			for (int v = 0; v < kVoices; ++v)
				if (voice_[v].channel == ch && voice_[v].keyed)
					keyOff(v, false);
			break;
		case 121:   // reset all controllers
			c.expression = 127;
			c.bend = 0;
			c.modulation = 0;
			c.pedal = false;
			refreshLevels(ch);
			setVibrato(ch, false);
			break;
		default:
			break;
		}
	}

	void setVibrato(int ch, bool on) {
		for (int v = 0; v < kVoices; ++v) {
			Voice &vo = voice_[v];
			if (vo.channel != ch || !vo.patch)
				continue;
			put(0x20 + opOffset(v, 0), vo.patch->modCharacteristic | (on ? 0x40 : 0));
			put(0x20 + opOffset(v, 1), vo.patch->carCharacteristic | (on ? 0x40 : 0));
		}
	}

	RegisterSink *sink_;
	uint64_t now_;
	uint64_t tickCount_;
	uint32_t age_;
	Channel channel_[kChannels];
	Voice voice_[kVoices];
};

} // namespace OplMidi

#endif
