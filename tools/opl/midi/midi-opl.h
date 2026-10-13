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
// or later); the voice allocator, the pitch calculation, the controller
// handling, the modulation wheel's vibrato and the stand-ins for the General
// MIDI drum keys the bank lacks are this project's. Pitch is the chip's own
// formula (frequency = f-number * 49,716 Hz / 2^(20 - block)) applied one
// octave below the MIDI note: the bank's carriers mostly run at twice the
// base frequency (74 of the 128 melodic patches use frequency multiplier 2),
// which is how ScummVM's driver, whose f-number table sits an octave low,
// sounds true pitch.
#ifndef OPL_MIDI_OPL_H
#define OPL_MIDI_OPL_H

#include <stdint.h>

#include "gm-bank-tuning.h"

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
	// The modulation wheel's vibrato: a triangle of about 5.5 Hz whose depth at
	// full modulation is half a semitone (128 / 256), General MIDI 2's default
	// modulation depth range. It moves every fourth tick (62.5 Hz, 5,768 / 65,536
	// of a cycle): a pitch write costs the decoder both operators' increment and
	// envelope, and nine voices of vibrato at the full 250 Hz made most periods
	// late on the emulated Falcon; at 125 Hz some still were without render-ahead.
	enum { kVibratoTicks = 4, kVibratoStep = 5768, kVibratoDepth = 128 };

	Engine() : sink_(0) { reset(0, 0); }

	// The chip's initial state, the controllers' defaults, every voice silent.
	void reset(RegisterSink *sink, uint64_t frame) {
		sink_ = sink;
		now_ = frame;
		tickCount_ = frame * 512ull / 100700ull;   // the ticks that have already fallen
		while (frameOfTick(tickCount_ + 1) <= frame)
			++tickCount_;
		nextTick_ = frameOfTick(tickCount_ + 1);
		age_ = 0;
		vibratoPhase_ = 0;
		vibratoTick_ = 0;
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
			ch.fineMsb = 64;
			ch.fineTune = 0;
			ch.coarseTune = 0;
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
			vo.trim = 0;
			vo.vol1 = vo.vol2 = 0;
			vo.fnum = 0;
			vo.block = 0;
			vo.vibrato = false;
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
		// Key and channel pressure: General MIDI leaves their effect to the
		// instrument, and General MIDI 2 gives them no effect by default.
		default: break;
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
			if (nextTick_ > frame) {
				now_ = frame;
				return;
			}
			if (nextTick_ > now_)
				now_ = nextTick_;
			++tickCount_;
			nextTick_ = frameOfTick(tickCount_ + 1);
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
		uint8_t fineMsb;     // RPN 1's data entry MSB, for the LSB that may follow
		int bend;            // -8192..8191
		int fineTune;        // RPN 1, in 1/256 semitone: -256..255
		int coarseTune;      // RPN 2, in semitones: -64..63
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
		int8_t trim;         // the program's level trim (gm-bank-tuning.h)
		uint8_t vol1, vol2;  // the instrument's own levels plus velocity, before channel volume
		uint16_t fnum;
		uint8_t block;
		bool vibrato;        // its pitch carries the modulation wheel's vibrato
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
		const bool vibratoStep = ++vibratoTick_ >= kVibratoTicks;
		if (vibratoStep) {
			vibratoTick_ = 0;
			vibratoPhase_ = (uint16_t)(vibratoPhase_ + kVibratoStep);
		}
		for (int v = 0; v < kVoices; ++v) {
			Voice &vo = voice_[v];
			if (vo.remaining > 0 && --vo.remaining == 0 && vo.keyed)
				keyOff(v, false);
			// a sounding note follows the vibrato, and returns to its pitch when the wheel does
			if (vibratoStep && vo.keyed && (vo.vibrato || channel_[vo.channel].modulation))
				repitch(v);
		}
	}

	// ---- voices
	void noteOn(int ch, int note, int velocity) {
		const Patch *patch;
		if (ch == kPercussionChannel) {
			note = percussionKey(note);
			const uint8_t key = kGmPercussionMap[note & 0x7f];
			if (key == 0xff)
				return;
			patch = &kGmPercussion[key];
		} else {
			patch = gmMelodicPatch(channel_[ch].program);
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
			v = allocate(ch);
		Voice &vo = voice_[v];
		if (vo.keyed || vo.channel != 0xff)
			put(0xb0 + v, vo.block << 2 | (vo.fnum >> 8));   // key up first: the key-on below is an edge
		vo.channel = (uint8_t)ch;
		vo.note = (uint8_t)note;
		vo.keyed = true;
		vo.held = false;
		vo.age = ++age_;
		vo.patch = patch;
		vo.trim = ch == kPercussionChannel ? 0 : kGmMelodicTrim[channel_[ch].program];
		vo.remaining = (ch == kPercussionChannel && patch->duration) ? (patch->duration * 63 + 16) / 17 : 0;

		int v1 = (patch->modLevel & 0x3f) + scale(velocity >> 1, patch->modWaveform >> 2);
		int v2 = (patch->carLevel & 0x3f) + scale(velocity >> 1, patch->carWaveform >> 2);
		vo.vol1 = (uint8_t)(v1 > 63 ? 63 : v1);
		vo.vol2 = (uint8_t)(v2 > 63 ? 63 : v2);

		const int o1 = opOffset(v, 0), o2 = opOffset(v, 1);
		put(0x20 + o1, patch->modCharacteristic);
		put(0x40 + o1, levelRegister(patch->modLevel, modulatorVolume(vo, channel_[ch]), modulatorTrim(vo)));
		put(0x60 + o1, ~patch->modAttackDecay & 0xff);
		put(0x80 + o1, ~patch->modSustainRelease & 0xff);
		put(0xe0 + o1, patch->modWaveform);
		put(0x20 + o2, patch->carCharacteristic);
		put(0x40 + o2, levelRegister(patch->carLevel, carrierVolume(vo, channel_[ch]), vo.trim));
		put(0x60 + o2, ~patch->carAttackDecay & 0xff);
		put(0x80 + o2, ~patch->carSustainRelease & 0xff);
		put(0xe0 + o2, patch->carWaveform);
		put(0xc0 + v, patch->feedback);
		pitch(v, true);
	}

	void noteOff(int ch, int note) {
		if (ch == kPercussionChannel)
			note = percussionKey(note);
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

	// The voice for a new note on channel ch. With nine voices for sixteen
	// channels a dense song steals constantly, and the oldest sounding note is
	// most often a long held one or a lead line: the steal goes where it is
	// least heard. In order, a voice never used; one released, longest ago
	// first; one only the sustain pedal holds; a drum; a note of the new
	// note's own channel or of a channel with several notes sounding (a chord
	// loses a tone); and only then a channel's single note. Within a class the
	// oldest goes.
	int allocate(int ch) {
		uint8_t sounding[kChannels] = { 0 };
		for (int v = 0; v < kVoices; ++v)
			if (voice_[v].keyed)
				++sounding[voice_[v].channel];
		// The class, then the key within it; 32-bit, as 64-bit compares are
		// slow on the 68030 and a dense chord allocates many voices at once.
		// (A frame of release wraps after 24 hours.)
		int best = 0;
		unsigned bestClass = ~0u;
		uint32_t bestKey = ~0u;
		for (int v = 0; v < kVoices; ++v) {
			const Voice &vo = voice_[v];
			unsigned cls;
			uint32_t key;
			if (vo.channel == 0xff) {
				cls = 0;
				key = 0;
			} else if (!vo.keyed) {
				cls = 1;
				key = (uint32_t)vo.releasedAt;
			} else {
				if (vo.held)
					cls = 2;
				else if (vo.channel == kPercussionChannel)
					cls = 3;
				else if (vo.channel == ch || sounding[vo.channel] > 1)
					cls = 4;
				else
					cls = 5;
				key = vo.age;
			}
			if (cls < bestClass || (cls == bestClass && key < bestKey)) {
				bestClass = cls;
				bestKey = key;
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
	// The key scale bits, and the attenuation the volume leaves plus the
	// program's trim, held to the register's range.
	static int levelRegister(int patchLevel, int volume, int trim) {
		int attenuation = 0x3f - volume + trim;
		if (attenuation < 0)
			attenuation = 0;
		else if (attenuation > 0x3f)
			attenuation = 0x3f;
		return (patchLevel & 0xc0) | attenuation;
	}
	// The trim reaches the modulator only where it is heard: the additive connection.
	static int modulatorTrim(const Voice &vo) {
		return (vo.patch->feedback & 1) ? vo.trim : 0;
	}

	void refreshLevels(int ch) {
		for (int v = 0; v < kVoices; ++v) {
			Voice &vo = voice_[v];
			if (vo.channel != ch || !vo.patch)
				continue;
			put(0x40 + opOffset(v, 0), levelRegister(vo.patch->modLevel, modulatorVolume(vo, channel_[ch]), modulatorTrim(vo)));
			put(0x40 + opOffset(v, 1), levelRegister(vo.patch->carLevel, carrierVolume(vo, channel_[ch]), vo.trim));
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

	// The vibrato's offset now, in 1/256 semitone, for a modulation of 0-127.
	int vibratoOffset(int modulation) const {
		const int triangle = vibratoPhase_ < 32768 ? vibratoPhase_ : 65535 - vibratoPhase_;   // 0..32767
		return ((triangle * 2 - 32767) * (modulation * kVibratoDepth / 127)) / 32768;
	}

	// The voice's offset from its note: bend, tuning and vibrato.
	int pitchOffset(Voice &vo) {
		const Channel &ch = channel_[vo.channel];
		vo.vibrato = ch.modulation != 0;
		return (ch.bend * ch.bendRange * 256) / 8192 + ch.coarseTune * 256 + ch.fineTune +
		       (vo.vibrato ? vibratoOffset(ch.modulation) : 0);
	}

	void pitch(int v, bool keyOn) {
		Voice &vo = voice_[v];
		noteToFnum(vo.note, pitchOffset(vo), &vo.fnum, &vo.block);
		put(0xa0 + v, vo.fnum & 0xff);
		put(0xb0 + v, (keyOn ? 0x20 : 0) | vo.block << 2 | (vo.fnum >> 8));
	}

	// A sounding voice to its pitch now; only the registers that change are written.
	void repitch(int v) {
		Voice &vo = voice_[v];
		uint16_t fnum;
		uint8_t block;
		noteToFnum(vo.note, pitchOffset(vo), &fnum, &block);
		const bool low = (fnum & 0xff) != (vo.fnum & 0xff);
		const bool high = block != vo.block || (fnum >> 8) != (vo.fnum >> 8);
		vo.fnum = fnum;
		vo.block = block;
		if (low)
			put(0xa0 + v, fnum & 0xff);
		if (high)
			put(0xb0 + v, 0x20 | block << 2 | (fnum >> 8));
	}

	// Every sounding voice of the channel to its pitch, both registers written.
	void retune(int ch) {
		for (int v = 0; v < kVoices; ++v)
			if (voice_[v].channel == ch && voice_[v].keyed)
				pitch(v, true);
	}

	void pitchBend(int ch, int value14) {
		channel_[ch].bend = value14 - 8192;
		retune(ch);
	}

	// RPN 1: 8,192 is in tune, each step 1/8,192 of 100 cents either way.
	void setFineTune(int ch, int value14) {
		channel_[ch].fineTune = ((value14 - 8192) * 256) / 8192;
		retune(ch);
	}

	// The General MIDI drum keys the bank has no patch for, played by the
	// nearest patch it has: ride cymbal 2 by ride cymbal 1, the shaken and
	// scraped ones (vibraslap, maracas, guiros) by the cabasa, the cuicas by the
	// high congas. The whistles stay silent; no drum stands in for a pitch.
	static int percussionKey(int note) {
		switch (note) {
		case 58: return 69;   // vibraslap
		case 59: return 51;   // ride cymbal 2
		case 70: return 69;   // maracas
		case 73: return 69;   // short guiro
		case 74: return 69;   // long guiro
		case 78: return 62;   // mute cuica
		case 79: return 63;   // open cuica
		default: return note;
		}
	}

	// ---- controllers
	void control(int ch, int cc, int value) {
		Channel &c = channel_[ch];
		switch (cc) {
		case 1:     // the vibrato follows from the next tick
			c.modulation = (uint8_t)value;
			break;
		case 6:     // data entry MSB, for the selected RPN
			if (c.rpnMsb != 0)
				break;
			if (c.rpnLsb == 0 && value > 0)          // pitch bend range, in semitones
				c.bendRange = (uint8_t)(value > 24 ? 24 : value);
			else if (c.rpnLsb == 1) {                // fine tuning
				c.fineMsb = (uint8_t)value;
				setFineTune(ch, value << 7);
			} else if (c.rpnLsb == 2) {              // coarse tuning, in semitones
				c.coarseTune = value - 64;
				retune(ch);
			}
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
		case 38:    // data entry LSB: only fine tuning's counts
			if (c.rpnMsb == 0 && c.rpnLsb == 1)
				setFineTune(ch, c.fineMsb << 7 | value);
			break;
		case 98:    // an NRPN: the data entry that follows is not for an RPN
		case 99:
			c.rpnMsb = c.rpnLsb = 127;
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
			retune(ch);
			break;
		default:
			break;
		}
	}

	RegisterSink *sink_;
	uint64_t now_;
	uint64_t tickCount_;
	uint64_t nextTick_;    // frameOfTick(tickCount_ + 1), kept: it is 64-bit arithmetic on the 68030
	uint32_t age_;
	uint16_t vibratoPhase_;
	uint8_t vibratoTick_;
	Channel channel_[kChannels];
	Voice voice_[kVoices];
};

} // namespace OplMidi

#endif
