# F030OPL2 development

Keep changes confined to this project; SID and ScummVM are source references.
Use `make check`, then the affected DSP/SSI gates with calibrated sibling
F030Arcade Hatari. Do not report historical ScummVM results as new results.

Follow SID's performance discipline: preserve bit-exact output against the
practical host reference, measure whole-stream timing including transport,
and retain baseline and changed results. The practical model is not the exact
chip oracle. Do not lower the 49.17 kHz rate to gain speed without measuring
aliasing and listening to bright patches. Preserve sample-stamped events,
36-slot rhythm noise, and zero-wait-state DSP BCR setup. Avoid applying SID's
saw/pulse polyBLEP to the OPL sine waveform path without an OPL-specific model.

MIDI player: the engine, SMF reader and decoder pipeline in tools/opl/midi are
shared by the host tool and the 68030 program, so a change to them is gated
twice: `make check` (host unit checks) and `make midi-gate midi-live-gate`
(the 68030 build on the emulated Falcon against the host reference, DSP checksum
equal, no late period). Songs are not in this repository; `tools/opl/midi-corpus.py`
runs whatever collection is at hand. Render-ahead is a DSP mode a gate must name
(`--render-ahead`, `--ahead`); a change to the stream transport is measured in
both modes. State what was not auditioned or run on hardware.

The Makefile uses the upstream DSP layout under tools/opl. The toolchain is a pinned
f030dsp3d submodule matching SID. Build and release directories
are ignored. Never recursively remove a directory junction.
