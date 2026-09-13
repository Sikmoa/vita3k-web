#!/usr/bin/env python3
"""Build isolated loader tests using an existing native-interpreter Ninja build.

No project CMake files are modified. Production libraries must already be built;
compile flags and link dependencies come from that build's real runtime target.
The current loader and relocation sources are compiled explicitly, so tests never
silently use a stale load_self.cpp from the kernel archive.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess


def run(command, cwd):
    print('+', shlex.join(map(str, command)), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, default=Path('/opt/vitasdk/vitasdk'))
    parser.add_argument('--sanitize', action='store_true', help='ASan+UBSan for tests, loader and relocation engine (not prebuilt dependencies)')
    parser.add_argument('--ndebug', action='store_true', help='also test with assertions disabled')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    build = args.build_dir.resolve()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    commands = json.loads((build / 'compile_commands.json').read_text())
    entry = next(c for c in commands if c['file'].endswith('/kernel/src/load_self.cpp'))
    template = shlex.split(entry['command'])
    # The compile database omits dependency-file flags for Ninja builds.
    flags = []
    skip = False
    for token in template:
        if skip:
            skip = False
        elif token in ('-o', '-c'):
            skip = True
        else:
            flags.append(token)
    flags += ['-g1', '-Wno-deprecated-declarations']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    if args.ndebug:
        flags += ['-DNDEBUG']
    objects = []
    for source in [root / 'vita3k/kernel/tests/sized_loader_tests.cpp',
                   root / 'vita3k/kernel/src/load_self.cpp',
                   root / 'vita3k/kernel/src/relocation.cpp']:
        obj = output / (source.stem + '.o')
        run(flags + ['-c', str(source), '-o', str(obj)], entry['directory'])
        objects.append(str(obj))
    runtime_commands = subprocess.check_output(
        ['ninja', '-C', str(build), '-t', 'commands', 'vita3k_interpreter_runtime_tests'], text=True)
    link = shlex.split(runtime_commands.strip().splitlines()[-1])
    # Ninja's CMake link rule is ': && <compiler> ... && :'.
    link = link[link.index('&&') + 1:]
    link = link[:link.index('&&')]
    link = [token for token in link if not token.startswith('-Wl,--dependency-file=')]
    old_object = next(t for t in link if t.endswith('interpreter_runtime_tests.cpp.o'))
    index = link.index(old_object)
    link[index:index + 1] = objects
    executable = output / 'sized_loader_tests'
    link[link.index('-o') + 1] = str(executable)
    if args.sanitize:
        link += ['-fsanitize=address,undefined']
    run(link, build)
    fixture = root / 'browser/tests/vita_homebrew_fixture'
    compressed = output / 'compressed-eboot.bin'
    run([str(args.sdk / 'bin/vita-make-fself'), '-c', str(fixture / 'fixture.velf'), str(compressed)], root)
    run([str(executable), str(fixture / 'fixture.velf'), str(fixture / 'eboot.bin'), str(compressed)], root)


if __name__ == '__main__':
    main()
