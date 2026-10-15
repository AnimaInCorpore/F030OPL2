// The DSP kernel's upload image for a 68k host (tools/opl/m68k/oplhost.s):
// the tables and records OplMidi::uploadTables sends, as one file.
//
// Format (big-endian 32-bit, the upload half of OPLDATA.BIN): 'OPLR', block
// count, then per block: space (0 X, 1 Y), address, word count, the words.
//
// usage: opl-upload-image OPLDATA.BIN [--opl3]
#include <stdio.h>
#include <string.h>
#include <vector>

#include "midi/opl-upload.h"

namespace {
struct Collect {
	std::vector<uint32_t> words;
	uint32_t blocks = 0;
	void block(int space, uint16_t address, const uint32_t *w, unsigned count) {
		words.push_back((uint32_t)space);
		words.push_back(address);
		words.push_back(count);
		for (unsigned i = 0; i < count; ++i)
			words.push_back(w[i] & 0xffffff);
		++blocks;
	}
};
void put(FILE *f, uint32_t v) {
	fputc((int)(v >> 24) & 255, f); fputc((int)(v >> 16) & 255, f);
	fputc((int)(v >> 8) & 255, f); fputc((int)v & 255, f);
}
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: opl-upload-image OPLDATA.BIN [--opl3]\n");
		return 2;
	}
	const bool opl3 = argc > 2 && !strcmp(argv[2], "--opl3");
	Collect c;
	OplMidi::uploadTables(c, 9, opl3);
	FILE *f = fopen(argv[1], "wb");
	if (!f)
		return 1;
	put(f, 0x4f504c52);
	put(f, c.blocks);
	for (uint32_t w : c.words)
		put(f, w);
	fclose(f);
	return 0;
}
