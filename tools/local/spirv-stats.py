#!/usr/bin/env python3
"""Driver statistics (registers, instructions, spills) of dumped compute SPIR-V modules.

    spirv-stats.py MODULE.spv [MODULE.spv ...]

Writes each module's set-0 descriptor layout from `spirv-cross --reflect` next to it
(MODULE.layout.txt) and runs _Build/windows-tools/spirv-stats.exe (tools/local/windows/
spirv-stats.cpp) on them. Dump modules with the emulator's KYTY_DUMP_HASHES=<hash,...>.
"""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TYPES = {'ssbos': 7, 'ubos': 6, 'images': 3, 'separate_images': 2, 'separate_samplers': 0,
         'textures': 1, 'storage_texel_buffers': 5, 'uniform_texel_buffers': 4}


def layout(module):
    reflect = json.loads(subprocess.run(['spirv-cross', str(module), '--reflect'], capture_output=True,
                                        text=True, check=True).stdout)
    lines = []
    for kind, descriptor_type in TYPES.items():
        for resource in reflect.get(kind, []):
            if resource.get('set', 0) != 0:
                continue
            count = resource.get('array', [1])
            lines.append(f"binding {resource['binding']} {descriptor_type} {count[0] if count and count[0] else 1}")
    path = module.with_suffix('.layout.txt')
    path.write_text('\n'.join(lines) + '\n')
    return path


def main():
    modules = [Path(argument) for argument in sys.argv[1:]]
    if not modules:
        sys.exit(__doc__)
    args = []
    for module in modules:
        args += [str(module), str(layout(module))]
    subprocess.run([str(ROOT / '_Build/windows-tools/spirv-stats.exe')] + args, check=True)


if __name__ == '__main__':
    main()
