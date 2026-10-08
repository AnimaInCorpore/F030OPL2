# Releases, gate environment and validation

[GitHub Releases](https://github.com/AnimaInCorpore/F030OPL2/releases/tag/v0.1.0)
currently provides the `v0.1.0` prerelease from source commit `9dd60bd`. Assets
are `OPL2.ZIP`, `F030MID.TTP`, `F030MID.TOS`, `F030OPL2-SOURCE.tar.gz` and
`SHA256.TXT`. The ZIP uses flat uppercase 8.3 filenames and includes
a generated `DEMO.MID`, 40-column instructions, license text, source notices
and the source commit. The two player files are
identical; the `.TTP` extension lets the TOS desktop ask for parameters.
Commercial songs, toolchain binaries and ROMs are not included.

## Validation records

| Record | Source and scope |
| --- | --- |
| [validation.json](validation.json) | Initial 2026-10-05 baseline, before later stream optimizations; historical measurements are preserved. |
| [current-validation.json](current-validation.json) | Development snapshot of `8961ed6` on 2026-10-08: host checks, five DSP bench cases, one-second stress/rhythm streams in both modes, and MIDI file/raw-live gates. The filename is retained for existing links; this is not the latest packaged release record. |
| [release-validation.json](release-validation.json) | Published `v0.1.0` at `9dd60bd`, validated on 2026-10-08: packaged player, generated demo in both render-ahead modes, raw-live input, host checks and uploaded-asset digests. |
| [speed-quality.md](speed-quality.md) and [midi-player.md](midi-player.md#verification) | Dated optimization baselines and external-song corpus results, retained separately from publication tests. |

The release demo rendered 990 periods with checksum 6,797,716 in each mode.
Raw live input rendered 247 periods with checksum 7,781,472. Every uploaded
table matched the fixture, no event overflow occurred and no period was late.
All 158 MIDI host checks passed; practical-model host checks had zero failures.

DSP benches compare individual frame words. Stream and MIDI gates compare
aggregate checksums, upload images and counters; checksum equality alone is
not proof of every individual sample. The practical integer host reference
is approximate and distinct from the original Nuked-OPL3 comparison reference.

The source hashes in the development and release snapshots identify the
implementation tested. Documentation and release packaging changed between
those commits, while the synthesis, MIDI engine and transport sources remained
the same. Historical JSON measurements have not been rewritten as new runs.

Release verification used TOS 4.02 and 14 MB emulated ST-RAM. Minimum hardware
RAM, other TOS versions, physical MIDI input, loaded-host timing and audio
quality remain unqualified. External song corpus, starvation injection and the
layered experiment were not rerun for the release. The one-second development
rhythm streams had no late period but reported zero minimum slack, so they
establish no positive margin for that workload. No endurance result is implied.

## Published instruction clarifications

The README inside the fixed `v0.1.0` ZIP describes `-t` too broadly. In this
binary it limits only physical live MIDI input, not `.mid` playback or raw
input. File keyboard-stop requests are polled about once a second. With no
explicit input the player checks `MIDIIN.RAW`, then `SONG.MID`, then physical
MIDI IN; it does not automatically select `DEMO.MID`. Enter `DEMO.MID` in the
TTP parameter box. These clarifications are included in the current
[instructions template](falcon-release.txt) for subsequent packages.

## Gate environment

F030OPL2's DSP and MIDI gates require a DSP-calibrated Hatari binary.
The calibration must address DSP clock accounting and CPU-side host-port wait
states. Stock-emulator timing is not a substitute for these gates. Emulated
deadlines remain model results, not physical Falcon measurements.

### Selecting the environment

Set `HATARI` to the calibrated emulator's executable path. Every current
emulator script reads this environment variable through `tools/opl/gate_env.py`.
Explicit configuration makes the gates independent of checkout discovery.
Overrides are not checked for calibration; use a known calibrated build for
timing claims.

```sh
export HATARI=/path/to/calibrated/hatari
make stream-gate
make midi-gate
```

The assembler, vasm/vlink, two-stage loader and TOS 4.02 ROM come from this
project and its pinned toolchain submodule under `third_party/f030dsp3d`.
`DOSBOX` controls the DSP assembler runner; `M68K_CXX` selects the MIDI player's
MiNT cross compiler. `local.mk` can set these Makefile variables. Export
`HATARI` in the shell so the Python gates inherit it.

On Windows run from an MSYS2 login shell with `/ucrt64/bin` on PATH and a
`HOME`. Scripts that require Hatari's control FIFO cannot use a Windows build
without Unix-domain-socket support. Directory junctions used by gates must be
unlinked with `gate_env.unlink_directory`, never recursively removed.

### What the gates measure

The synthesis cadence is 32 frames, the stream period is 768 frames, and the
Falcon codec clock is 25.175 MHz / 512, approximately 49.17 kHz. One DSP
instruction cycle budget is approximately 326 cycles per codec frame at the
Falcon's 16 MIPS throughput.

- `make check`: build and host unit checks; it does not launch Hatari.
- `make dsp-gate`: individual rendered-frame equality against the practical
  integer host reference, plus profiled synthesis cost.
- `make stream-gate rhythm-gate`: whole-stream transport through the 68030,
  DSP host port and SSI; aggregate output checksum equality and no late period.
- `make midi-gate midi-live-gate`: shared MIDI pipeline on the 68030 against
  host predictions, upload equality, counters and aggregate checksum. The file
  target runs render-ahead both on and off; live input defaults to off.

Name render-ahead explicitly in timing records. To measure both stream modes
and retain their results separately:

```sh
make stream-gate rhythm-gate
make stream-gate GATE_ARGS='--render-ahead --output build/stream-gate-ahead'
make rhythm-gate GATE_ARGS='--render-ahead --output build/rhythm-gate-ahead'
```

`rt-stream-gate.py --profile` captures whole-stream DSP work including
transport and SSI interrupts; `profile-labels.py` attributes it to labels.
Retain baseline and changed results rather than overwriting the baseline.
See [speed and quality](speed-quality.md) for dated project measurements and
known starvation-count discrepancies. An aggregate checksum does not establish
individual sample equality or audible quality. Audio audition, physical MIDI
port input and playback on a physical Falcon remain unestablished here.

## Build and verify

Use the build dependencies in [the README](../README.md#build-and-verify).
Run `make check`, then `make midi-gate midi-live-gate` with calibrated Hatari
and the MiNT cross compiler configured. File gates exercise render-ahead on
and off; the live gate supplies raw MIDI bytes rather than a physical port.
Keep the JSON results and logs. Run affected DSP/SSI gates if those sources
changed, including both stream modes for transport changes.

For a new release, commit the source and packaging changes and choose an
unused version. To reproduce the published `v0.1.0` package, start from a clean
checkout of its tag:

```sh
git switch --detach v0.1.0
make midi-release VERSION=v0.1.0
```

The packager checks the TOS executable header, requires a clean working tree
(including non-ignored untracked files), records the source commit and binary
SHA-256, and verifies the ZIP contents. Output stays in the ignored `release/v0.1.0` directory. The
player includes its DSP image, upload tables and MIDI instrument bank.

Gate the packaged executable and demo in both modes before publishing:

```sh
make package-gate VERSION=v0.1.0
```

The equivalent direct commands are:

```sh
python3 tools/opl/midi-gate.py release/v0.1.0/DEMO.MID --tos release/v0.1.0/F030MID.TTP --ahead on --output build/release-midi-on
python3 tools/opl/midi-gate.py release/v0.1.0/DEMO.MID --tos release/v0.1.0/F030MID.TTP --ahead off --output build/release-midi-off
python3 tools/opl/midi-gate.py build/midi-test/live.bin --raw --tos release/v0.1.0/F030MID.TTP --output build/release-midi-live
```

## Publish

Push the release commit. Create a versioned GitHub release targeting that exact
commit and upload the ZIP, both player files, matching source archive and
checksum file. Use a prerelease
while physical-Falcon compatibility is unqualified. Include the source version,
what was gated, and what was not auditioned or run on hardware in the notes.
The source asset archives the exact commit and includes `SOURCE-REVISION.TXT`
with the project and toolchain revisions. It excludes the toolchain binaries
and ROMs. For a Git checkout, initialize the submodule as documented. For an
extracted source archive, clone the toolchain and select its recorded commit:

```sh
git clone https://github.com/AnimaInCorpore/f030dsp3d.git third_party/f030dsp3d
git -C third_party/f030dsp3d checkout <recorded-toolchain-commit>
make check midi-tos
```

Keep published tags and assets fixed; use a new version for subsequent changes.
