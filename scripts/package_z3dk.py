#!/usr/bin/env python3
"""Package locally built Z3DK binaries; never downloads executables."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True, help='source tree used to build')
parser.add_argument('--build', type=Path, required=True, help='CMake build directory')
parser.add_argument('--version', required=True, help='release identifier (letters, digits, . _ -)')
parser.add_argument('--revision', required=True, help='full Git commit used for the source')
parser.add_argument('--platform', choices=('macos-arm64', 'macos-x86_64', 'linux-x86_64', 'linux-arm64', 'windows-x64'), required=True)
parser.add_argument('--output', type=Path, required=True, help='new ZIP file; existing files are not overwritten')
args = parser.parse_args()
if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', args.version):
    parser.error('invalid version identifier')
if not re.fullmatch(r'[0-9a-f]{40}', args.revision):
    parser.error('revision must be a full lowercase Git SHA-1')
if args.platform == 'windows-x64':
    binaries = {
        'bin/z3asm.exe': args.build / 'src/z3asm/bin/Release/z3asm.exe',
        'bin/z3disasm.exe': args.build / 'bin/Release/z3disasm.exe',
    }
else:
    binaries = {
        'bin/z3asm': args.build / 'src/z3asm/bin/z3asm',
        'bin/z3disasm': args.build / 'bin/z3disasm',
        'bin/z3lsp': args.build / 'bin/z3lsp',
    }
files = {
    **binaries,
    'LICENSE': args.source / 'LICENSE',
    'licenses/license-gpl.txt': args.source / 'docs/legacy/license-gpl.txt',
    'licenses/license-lgpl.txt': args.source / 'docs/legacy/license-lgpl.txt',
    'licenses/license-wtfpl.txt': args.source / 'docs/legacy/license-wtfpl.txt',
}
missing = [str(path) for path in files.values() if not path.is_file()]
if missing:
    parser.error('required files missing: ' + ', '.join(missing))
contents = {name: path.read_bytes() for name, path in files.items()}
manifest = {'version': args.version, 'platform': args.platform, 'source_revision_claimed': args.revision,
            'sha256': {name: hashlib.sha256(data).hexdigest() for name, data in contents.items()}}
contents['manifest.json'] = (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode()
with zipfile.ZipFile(args.output, 'x', compression=zipfile.ZIP_DEFLATED) as archive:
    for name, data in contents.items():
        info = zipfile.ZipInfo(name)
        info.create_system = 3
        info.external_attr = ((0o100755 if name.startswith('bin/') else 0o100644) << 16)
        info.compress_type = zipfile.ZIP_DEFLATED
        archive.writestr(info, data)
print(f'Created {args.output}')
