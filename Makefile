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
.PHONY: all tools dsp ref check dsp-gate stream-gate rhythm-gate
all: dsp ref

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

# Synthetic cases need neither ScummVM nor game data.
dsp-gate: all
	$(PYTHON) $(OPL)/rt-bench-gate.py --scenario synthetic --output build/dsp-gate $(GATE_ARGS)
stream-gate: all
	$(PYTHON) $(OPL)/rt-stream-gate.py --scenario stress --output build/stream-gate $(GATE_ARGS)
rhythm-gate: all
	$(PYTHON) $(OPL)/rt-stream-gate.py --scenario rhythm --output build/rhythm-gate $(GATE_ARGS)
