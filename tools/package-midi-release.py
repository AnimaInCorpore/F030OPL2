#!/usr/bin/env python3
"""Package the built Falcon MIDI player for transfer to a TOS filesystem."""
import argparse
import gzip
import hashlib
import io
from pathlib import Path
import re
import subprocess
import tarfile
import textwrap
import time
import zipfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True, help='release tag, for example v0.2.0')
    args = parser.parse_args()
    if not re.fullmatch(r'v[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?', args.version):
        parser.error('version must be a v-prefixed semantic version')
    binary = (ROOT / 'release/f030mid.tos').read_bytes()
    if len(binary) < 28 or binary[:2] != b'\x60\x1a':
        parser.error('release/f030mid.tos is not an Atari TOS executable (run make midi-tos)')
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    dirty = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True).strip())
    if dirty:
        parser.error('commit the source changes before packaging a release')
    toolchain_path = 'third_party/f030dsp3d'
    toolchain_commit = subprocess.check_output(
        ['git', 'rev-parse', 'HEAD:' + toolchain_path], cwd=ROOT, text=True).strip()
    actual = subprocess.check_output(
        ['git', 'rev-parse', 'HEAD'], cwd=ROOT / toolchain_path, text=True).strip()
    if actual != toolchain_commit:
        parser.error('toolchain submodule is not at the committed revision')
    digest = hashlib.sha256(binary).hexdigest()
    build = (f'F030OPL2 {args.version}\r\nSource commit: {commit}\r\n'
             f'Player SHA-256: {digest}\r\n'
             f'Source: https://github.com/AnimaInCorpore/F030OPL2/tree/{args.version}\r\n')
    # Flat, uppercase 8.3 filenames and CRLF instructions work on TOS disks.
    readme = (ROOT / 'docs/falcon-release.txt').read_text().replace('@VERSION@', args.version)
    readme = '\n'.join('\n'.join(textwrap.wrap(line, width=40,
                        break_long_words=False, break_on_hyphens=False))
                        for line in readme.splitlines()) + '\n'
    source_note = (f'F030OPL2 {args.version}\n'
                   'Matching source archive:\nF030OPL2-SOURCE.tar.gz\n'
                   'Source and notices are also at:\n'
                   'github.com/AnimaInCorpore/F030OPL2\n'
                   'OPL basis: Nuked-OPL3 comparison.\n'
                   'Kernels: ScummVM Falcon OPL work.\n'
                   'GM bank: ScummVM audio/adlib.cpp,\nGPL-3.0-or-later.\n'
                   'See docs/provenance.md in source.\n')
    files = {
        'F030MID.TTP': binary,
        'F030MID.TOS': binary,
        'README.TXT': readme.replace('\n', '\r\n').encode('ascii'),
        'COPYING.TXT': (ROOT / 'COPYING').read_text().replace('\n', '\r\n').encode('ascii'),
        'SOURCE.TXT': source_note.replace('\n', '\r\n').encode('ascii'),
        'DEMO.MID': (ROOT / 'build/midi-test/song.mid').read_bytes(),
        'BUILD.TXT': build.encode('ascii'),
    }
    output = ROOT / 'release' / args.version
    output.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        (output / name).write_bytes(data)
    # Every entry carries the source commit's time (UTC): the archive stays
    # reproducible, and the files do not show up dated 1980 on the Falcon.
    epoch = int(subprocess.check_output(['git', 'show', '-s', '--format=%ct', commit], cwd=ROOT))
    stamp = time.gmtime(epoch)[:6]
    archive = output / 'OPL2.ZIP'
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        for name, data in files.items():
            entry = zipfile.ZipInfo(name, date_time=stamp)
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            z.writestr(entry, data)
    # Verify the actual archive contents against the files just packaged.
    with zipfile.ZipFile(archive) as z:
        if z.testzip() is not None or any(z.read(n) != data for n, data in files.items()):
            raise SystemExit('release archive verification failed')
    # Archive the exact project commit; the build toolchain is separately pinned.
    source = output / 'F030OPL2-SOURCE.tar.gz'
    prefix = 'F030OPL2-SOURCE/'
    data = subprocess.check_output(['git', 'archive', '--format=tar',
                                   '--prefix=' + prefix, commit], cwd=ROOT)
    with source.open('wb') as raw:
        with gzip.GzipFile(fileobj=raw, filename='', mode='wb', mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode='w|') as merged:
                with tarfile.open(fileobj=io.BytesIO(data), mode='r:') as source_tar:
                    for member in source_tar:
                        merged.addfile(member, source_tar.extractfile(member) if member.isfile() else None)
                content = (f'F030OPL2 {commit}\n{toolchain_path} {toolchain_commit}\n').encode('ascii')
                member = tarfile.TarInfo(prefix + 'SOURCE-REVISION.TXT')
                member.size, member.mtime, member.mode = len(content), epoch, 0o644
                merged.addfile(member, io.BytesIO(content))
    checksums = ''.join(f'{hashlib.sha256((output / n).read_bytes()).hexdigest()}  {n}\n'
                        for n in ('OPL2.ZIP', 'F030MID.TTP', 'F030MID.TOS', 'F030OPL2-SOURCE.tar.gz'))
    (output / 'SHA256.TXT').write_text(checksums)
    print(f'Packaged {archive}: player SHA-256 {digest}, source {commit}')


if __name__ == '__main__':
    main()
