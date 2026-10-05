VASM_ARCHIVE := third_party/f030dsp3d/tools/vasm.tar.gz
VLINK_ARCHIVE := third_party/f030dsp3d/tools/vlink.tar.gz
DSP_TOOL_SOURCE := third_party/f030dsp3d/tools/asm56k
RESID_SOURCE := third_party/resid

TOOLS_DIR := build/tools
VASM_DIR := $(TOOLS_DIR)/vasm
VLINK_DIR := $(TOOLS_DIR)/vlink
VASM := $(VASM_DIR)/vasmm68k_mot
VLINK := $(VLINK_DIR)/vlink

# vlink's vendored dir.c calls chmod() from its _WIN32 branch without a
# declaration; GCC 14+ rejects that. gnu99 plus a forced io.h supplies it.
# Only needed on Windows hosts.
HOST_UNAME := $(shell uname -s)
ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(HOST_UNAME)))
VLINK_MAKE_ARGS := COPTS="-std=gnu99 -O2 -fomit-frame-pointer -c -include io.h"
endif


.DEFAULT_GOAL := all
-include local.mk
PYTHON ?= python3
HOST_CXX ?= g++
DOSBOX ?= dosbox-staging
OPL := tools/opl
EXE := $(if $(filter MINGW% MSYS% CYGWIN%,$(HOST_UNAME)),.exe,)
# The 68030 MIDI player is built with the MiNT cross compiler (GCC for
# m68k-atari-mintelf): put it on PATH or point M68K_CXX at it in local.mk. The
# Falcon has no FPU, so the objects are soft-float 68030 code linked with the
# 68000 (soft-float) runtime.
M68K_CXX ?= m68k-atari-mintelf-g++
MIDI := $(OPL)/midi
MIDI_HEADERS := $(MIDI)/midi-opl.h $(MIDI)/midi-file.h $(MIDI)/period-stream.h $(MIDI)/opl-upload.h $(MIDI)/gm-bank.h 	$(OPL)/opl-practical.h $(OPL)/opl-kernel.h $(OPL)/opl-tables.h $(OPL)/opl-practical-tables.h
.PHONY: all tools dsp ref check dsp-gate stream-gate rhythm-gate midi-host midi-tos midi-gate midi-live-gate midi-wav midi-hatari
all: dsp ref midi-host

tools: $(VASM) $(VLINK)

$(TOOLS_DIR)/.vasm-unpacked: $(VASM_ARCHIVE)
	@mkdir -p $(TOOLS_DIR)
	tar -xf $< -C $(TOOLS_DIR)
	@touch $@

$(VASM): $(TOOLS_DIR)/.vasm-unpacked
	$(MAKE) -C $(VASM_DIR) CPU=m68k SYNTAX=mot

$(TOOLS_DIR)/.vlink-unpacked: $(VLINK_ARCHIVE)
	@mkdir -p $(TOOLS_DIR)
	tar -xf $< -C $(TOOLS_DIR)
	@touch $@

$(VLINK): $(TOOLS_DIR)/.vlink-unpacked
	$(MAKE) -C $(VLINK_DIR) $(VLINK_MAKE_ARGS)


# Keep the upstream DSP/gate layout so inherited harnesses share one image.
dsp: tools
	DOSBOX="$(DOSBOX)" sh $(OPL)/build-dsp.sh
	mkdir -p release
	cp $(OPL)/build/OPLPLAY.TOS release/f030opl2.tos
	cp $(OPL)/build/OPLRT.TOS release/oplrt.tos
	cp $(OPL)/build/OPLBENCH.TOS release/oplbench.tos
	cp $(OPL)/dsp/OPLRT.LOD release/opl2.lod

ref: $(OPL)/build/headless/opl-practical-unit-test$(EXE) $(OPL)/build/headless/opl-rt-fixture$(EXE)

$(OPL)/build/headless/opl-practical-unit-test$(EXE): $(OPL)/practical-unit-test.cpp $(OPL)/opl-practical.h $(OPL)/opl-kernel.h $(OPL)/opl-tables.h $(OPL)/opl-practical-tables.h
	mkdir -p $(OPL)/build/headless
	$(HOST_CXX) -O2 -std=c++11 -Wall -Wextra $< -o $@

$(OPL)/build/headless/opl-rt-fixture$(EXE): $(OPL)/rt-fixture.cpp $(OPL)/opl-practical.h $(OPL)/opl-kernel.h $(OPL)/opl-tables.h $(OPL)/opl-practical-tables.h
	mkdir -p $(OPL)/build/headless
	$(HOST_CXX) -O2 -std=c++11 -Wall -Wextra $< -o $@

check: all
	$(OPL)/build/headless/opl-practical-unit-test$(EXE)
	$(OPL)/build/headless/opl-midi-test$(EXE)

# Synthetic cases need neither ScummVM nor game data.
dsp-gate: all
	$(PYTHON) $(OPL)/rt-bench-gate.py --scenario synthetic --output build/dsp-gate $(GATE_ARGS)
stream-gate: all
	$(PYTHON) $(OPL)/rt-stream-gate.py --scenario stress --output build/stream-gate $(GATE_ARGS)
rhythm-gate: all
	$(PYTHON) $(OPL)/rt-stream-gate.py --scenario rhythm --output build/rhythm-gate $(GATE_ARGS)

# ---- the MIDI player
midi-host: $(OPL)/build/headless/opl-midi$(EXE) $(OPL)/build/headless/opl-midi-test$(EXE)

$(OPL)/build/headless/opl-midi$(EXE): $(MIDI)/opl-midi.cpp $(MIDI_HEADERS)
	mkdir -p $(OPL)/build/headless
	$(HOST_CXX) -O2 -std=c++11 -Wall -Wextra $< -o $@

$(OPL)/build/headless/opl-midi-test$(EXE): $(MIDI)/midi-test.cpp $(MIDI_HEADERS)
	mkdir -p $(OPL)/build/headless
	$(HOST_CXX) -O2 -std=c++11 -Wall -Wextra $< -o $@

# F030MID.TOS embeds the DSP image the `dsp` target generates.
midi-tos: dsp
	mkdir -p build/midi-tos release
	$(M68K_CXX) -m68030 -msoft-float -O2 -std=gnu++17 -fno-exceptions -fno-rtti -Wall -Wextra 		-I $(OPL)/build -I $(MIDI) -c $(MIDI)/f030mid.cpp -o build/midi-tos/f030mid.o
	$(M68K_CXX) -m68000 build/midi-tos/f030mid.o -o build/midi-tos/F030MID.TOS
	cp build/midi-tos/F030MID.TOS release/f030mid.tos

# The test songs the MIDI gates play, written by a deterministic script.
build/midi-test/song.mid: $(MIDI)/make-test-midi.py
	$(PYTHON) $< build/midi-test

midi-gate: midi-host midi-tos build/midi-test/song.mid
	$(PYTHON) $(OPL)/midi-gate.py build/midi-test/song.mid --output build/midi-gate $(GATE_ARGS)
	$(PYTHON) $(OPL)/midi-gate.py build/midi-test/song.mid --ahead off --output build/midi-gate-off $(GATE_ARGS)
midi-live-gate: midi-host midi-tos build/midi-test/song.mid
	$(PYTHON) $(OPL)/midi-gate.py build/midi-test/live.bin --raw --output build/midi-live-gate $(GATE_ARGS)

# Audition a file on the PC: make midi-wav MIDI_FILE=song.mid WAV=song.wav
midi-wav: midi-host
	$(OPL)/build/headless/opl-midi$(EXE) $(MIDI_FILE) --wav $(WAV)

# Listen in the calibrated Hatari: make midi-hatari MIDI_FILE=song.mid
midi-hatari: midi-tos
	$(PYTHON) $(OPL)/midi-hatari.py $(MIDI_FILE)
