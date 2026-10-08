# F030OPL2 development

Keep changes confined to this project. Use `make check`, then the affected
DSP/SSI gates with a DSP-calibrated Hatari binary selected through `HATARI`.
Do not report historical validation results as new results.

Performance discipline: preserve bit-exact output against the
practical host reference, measure whole-stream timing including transport,
and retain baseline and changed results. The practical model is not the exact
chip oracle. Do not lower the 49.17 kHz rate to gain speed without measuring
aliasing and listening to bright patches. Preserve sample-stamped events,
36-slot rhythm noise, and zero-wait-state DSP BCR setup. Avoid applying
saw/pulse polyBLEP to the OPL sine waveform path without an OPL-specific model.

MIDI player: the engine, SMF reader and decoder pipeline in tools/opl/midi are
shared by the host tool and the 68030 program, so a change to them is gated
twice: `make check` (host unit checks) and `make midi-gate midi-live-gate`
(the 68030 build on the emulated Falcon against the host reference, DSP checksum
equal, no late period). External song collections are not in this repository;
the release ships a generated synthetic demo. `tools/opl/midi-corpus.py` runs
whatever collection is at hand. Render-ahead is a DSP mode a gate must name
(`--render-ahead`, `--ahead`); a change to the stream transport is measured in
both modes. State what was not auditioned or run on hardware.

The Makefile uses the DSP layout under tools/opl. The toolchain is a pinned
submodule under third_party/f030dsp3d. Build and release directories
are ignored. Never recursively remove a directory junction.

Published release tags and assets stay fixed. Use a new version for changed
binaries or packaged instructions. Keep development and release validation
records separate and identify the source commit tested.
