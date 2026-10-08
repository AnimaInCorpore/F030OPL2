# Falcon player releases

GitHub Releases distribute `OPL2.ZIP`, `F030MID.TTP`, `F030MID.TOS` and
`F030OPL2-SOURCE.tar.gz` and `SHA256.TXT`. The ZIP uses flat uppercase 8.3 filenames and includes
a generated `DEMO.MID`, 40-column instructions, license text, source notices
and the source commit. The two player files are
identical; the `.TTP` extension lets the TOS desktop ask for parameters.
Commercial songs, toolchain binaries and ROMs are not included.

## Build and verify

Use the build dependencies in [the README](../README.md#build-and-verify).
Run `make check`, then `make midi-gate midi-live-gate` with calibrated Hatari
and the MiNT cross compiler configured. File gates exercise render-ahead on
and off; the live gate supplies raw MIDI bytes rather than a physical port.
Keep the JSON results and logs. Run affected DSP/SSI gates if those sources
changed, including both stream modes for transport changes.

Commit the source and packaging changes before packaging:

```sh
make midi-release VERSION=v0.1.0
```

The packager checks the TOS executable header, requires a clean tracked
working tree, records the source commit and binary SHA-256, and verifies the
ZIP contents. Output stays in the ignored `release/v0.1.0` directory. The
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
commit and upload the ZIP, both player files, matching source archive and checksum file. Use a prerelease
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

The first release targets an Atari Falcon030 with DSP56001. Verification uses
TOS 4.02 and 14 MB emulated ST-RAM. Minimum memory, other TOS versions, physical
MIDI input and physical audio playback have not been qualified.
