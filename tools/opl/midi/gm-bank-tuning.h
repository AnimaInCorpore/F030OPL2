// This project's adjustments to the General MIDI bank of gm-bank.h, which
// stays as ScummVM's driver has it.
//
// kGmMelodicTrim attenuates each program's audible operators by whole steps
// of the chip's total level (0.75 dB each; negative values raise the level as
// far as the patch has room): a level trim measured by make-gm-tuning.py, so
// that the instruments sound about equally loud at the same velocity.
// kGmMelodicOverrides replaces whole patches whose envelopes do not suit their
// General MIDI role, such as sound effects that die away while held.
#ifndef OPL_GM_BANK_TUNING_H
#define OPL_GM_BANK_TUNING_H

#include <stdint.h>

#include "gm-bank.h"

namespace OplMidi {

static const int8_t kGmMelodicTrim[128] = {
	  3,  -1,  -5,   1,   2,   2,   0,  -4,   0,   0,   0,   0,   0,   0, -12,  -6,
	 -1,   0,   0,   0,   0,   0,  -5,   0,   0,  -2,   2,   0,  -8,  -3,   0,   0,
	  0,   3,   3,   4,   2,   0,   2,   0,   0,  -4,  -4,   0,   0,  -6,   0,   0,
	 -4,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  -4,   0,   0,  -3,   0,
	 -1,   0,   0,   0,   0,   0,   0,   0,   0,   1,   0,   3,   0,   2,   0,   8,
	  0,   0,   5,   5,   0,   6,   0,   6,   4,   0,   0,   0,   0,  -6,  -7,  -5,
	  2,   0,   0,  -1,  -1,   0,   0,   2,  -4,   0,  -8,  -1,   1,   0,  -4,  -4,
	  0,  -8,   0,  -9,   0,   0,   0,  -1,   0,  -3,   0,  -1,   2,   2,  10,   0,
};

struct PatchOverride {
	uint8_t program;   // 0xff ends the list
	Patch patch;
};

static const PatchOverride kGmMelodicOverrides[] = {
	// 29. Overdrive guitar: the carrier holds at its sustain level (-18 dB) while the key is down, as a held electric guitar does, instead of fading out over about eight seconds.
	{ 29, { 0xc2, 0x2e, 0x4f, 0x77, 0x00, 0xe4, 0x08, 0x0e, 0x98, 0x59, 0x0a, 0x00 } },
	// 30. Distortion guitar: the carrier holds at its sustain level (-12 dB) while the key is down, instead of fading out over about five seconds.
	{ 30, { 0xc2, 0x30, 0x4f, 0xca, 0x01, 0xe4, 0x0d, 0x0e, 0xb8, 0x7f, 0x08, 0x00 } },
	// 121. Breath noise: the carrier holds at -3 dB while the key is down (was -36 dB, and then released).
	{ 121, { 0x0f, 0x00, 0x90, 0xfa, 0x68, 0x26, 0x00, 0xa7, 0xe9, 0x54, 0x0e, 0x06 } },
	// 122. Seashore: the carrier holds at full level after its slow swell while the key is down, instead of fading out again.
	{ 122, { 0xc9, 0x15, 0xdd, 0xff, 0x7c, 0x20, 0x00, 0xe7, 0xfc, 0x6c, 0x0e, 0x38 } },
	// 125. Helicopter: both operators hold while the key is down, the carrier at full level with the chip's tremolo, the self-modulating noise at -6 dB (the patch was a blip of about a quarter of a second).
	{ 125, { 0x20, 0x21, 0x66, 0xd0, 0x03, 0xa0, 0x3f, 0x47, 0xf0, 0x00, 0x0e, 0x02 } },
	// 126. Applause: both operators hold while the key is down, the noise at -3 dB (the patch was a blip of about a quarter of a second).
	{ 126, { 0x28, 0x00, 0x0b, 0xec, 0x7c, 0x28, 0x3f, 0x06, 0xf3, 0x00, 0x0e, 0x02 } },
	{ 0xff, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
};

// The patch a program plays: an override, or the bank's own.
static inline const Patch *gmMelodicPatch(int program) {
	for (const PatchOverride *o = kGmMelodicOverrides; o->program != 0xff; ++o)
		if (o->program == program)
			return &o->patch;
	return &kGmMelodic[program];
}

} // namespace OplMidi

#endif
