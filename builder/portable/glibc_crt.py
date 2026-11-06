"""Build and audit the portable glibc executable startup boundary.

Only the glibc 2.34+ dynamic PIE profile opts in. GCC still supplies its own
crtbegin/crtend and normal implicit libraries, in the driver's usual order.
"""
import json
import os
from pathlib import Path
import re
import subprocess

MODE = 'owned-glibc-2.34'
OBJECTS = ('kasm-start.o', 'kasm-bootstrap.o', 'kasm-atexit.o', 'kasm-metadata.o')
GLIBC_OBJECTS = {'crt1.o', 'Scrt1.o', 'rcrt1.o', 'gcrt1.o', 'grcrt1.o', 'crti.o', 'crtn.o'}


def output(*args):
    return subprocess.check_output(list(args), text=True)


def prepare(compiler, arguments, binary, source_root):
    """Return driver flags plus first-party objects for one final executable."""
    if os.environ.get('KASMVNC_LIBC') == 'musl':
        raise RuntimeError('Owned glibc startup cannot be used with musl')
    unsupported = {'-static', '-static-pie', '-shared', '-no-pie', '-nostartfiles',
                   '-nostdlib', '-nodefaultlibs', '-pg', '-p', '-profile'}
    if unsupported.intersection(arguments) or any(a.startswith('-specs=') for a in arguments):
        raise RuntimeError('Owned glibc startup requires an ordinary dynamic PIE link')
    machine = output(*compiler, '-dumpmachine').strip()
    if machine.startswith('x86_64-'):
        arch, hardening = 'amd64', '-fcf-protection=full'
    elif machine.startswith('aarch64-'):
        arch, hardening = 'arm64', '-mbranch-protection=standard'
    else:
        raise RuntimeError('Unsupported owned glibc startup target: ' + machine)
    directory = Path(str(binary) + '.crt').resolve()
    directory.mkdir(parents=True, exist_ok=True)
    unexpected = {path.name for path in directory.iterdir()} - {*OBJECTS, 'startup.specs'}
    if unexpected:
        raise RuntimeError('Unexpected files in owned startup search directory: ' + ', '.join(sorted(unexpected)))
    source = Path(source_root) / 'builder/portable/crt'
    # Do not change the driver's library ordering with -nostdlib, nor put
    # crtendS before its implicit libstdc++ nonshared supplement. Override just
    # the distro startup files in the driver's own start/end specs, retaining
    # GCC's CRT, offload and other compiler support in their original positions.
    specs = output(*compiler, '-dumpspecs')
    replacements = []
    for name in ('startfile', 'endfile'):
        match = re.search(r'^\*' + name + r':\n([^\n]+)', specs, re.M)
        if not match:
            raise RuntimeError('GCC startup specs unavailable: ' + name)
        original = match[1]
        replacement, count = re.subn(r'(?<![\w])(?:[Sgr]?crt1|grcrt1|crti|crtn)\.o%s', '', original)
        expected = 'crtbeginS.o%s' if name == 'startfile' else 'crtendS.o%s'
        if not count or expected not in replacement:
            raise RuntimeError('Unrecognized GCC ' + name + ' specs')
        if name == 'startfile':
            # Preserve __data_start as the first .data contribution, before
            # GCC's __dso_handle. %s uses the private -B directory without
            # embedding or shell-quoting an absolute filename in GCC specs.
            replacement = 'kasm-metadata.o%s ' + replacement
        replacements.append('*' + name + ':\n' + replacement + '\n')
    specs_path = directory / 'startup.specs'
    specs_path.write_text('\n'.join(replacements) + '\n')
    flags = ['-O2', '-fPIE', '-fno-stack-protector', '-fno-sanitize=all',
             '-fno-lto', '-fvisibility=hidden', hardening]
    inputs = [source / ('start-' + arch + '.S'), source / 'bootstrap.c', source / 'atexit.c',
              source / 'metadata.S']
    for src, name in zip(inputs, OBJECTS):
        language = 'assembler-with-cpp' if src.suffix == '.S' else 'c'
        subprocess.check_call(compiler + flags + ['-x', language, '-c', str(src), '-o', str(directory / name)])
    # CMake's newer GNU linker dependency files do not escape spaces in
    # absolute object paths. Relative inputs also keep a build-directory name
    # containing spaces out of those generated prerequisites.
    objects = [os.path.relpath(directory / name) for name in OBJECTS if name != 'kasm-metadata.o']
    return ['-B' + os.path.relpath(directory) + '/', '-specs=' + str(specs_path),
            '-Wl,-z,relro,-z,now'], objects


def reject_glibc_inputs(text):
    """A linker script's unselected archive LOAD is not embedded code."""
    selected = text.split('Discarded input sections')[0]
    archives = re.findall(r'([^\s()]+\.a)\(', selected)
    objects = re.findall(r'^LOAD\s+(.+\.o)\s*$', text, re.M)
    forbidden = [p for p in archives if Path(p).name in {'libc.a', 'libc_nonshared.a'}]
    forbidden += [p for p in objects if Path(p).name in GLIBC_OBJECTS]
    if forbidden:
        raise RuntimeError('Embedded glibc startup/nonshared input: ' + ', '.join(forbidden))


def audit_link(binary):
    binary = Path(binary)
    metadata = json.loads(Path(str(binary) + '.static-link.json').read_text())
    if metadata.get('startup_mode') != MODE:
        raise RuntimeError('Missing owned glibc startup link policy: ' + str(binary))
    text = Path(str(binary) + '.link-map').read_text()
    reject_glibc_inputs(text)
    loaded = [Path(p).name for p in re.findall(r'^LOAD\s+(.+\.o)\s*$', text, re.M)]
    for name in (*OBJECTS, 'crtbeginS.o', 'crtendS.o'):
        if loaded.count(name) != 1:
            raise RuntimeError('Expected exactly one startup input ' + name + ': ' + str(binary))
    if loaded.index('crtbeginS.o') > loaded.index('crtendS.o'):
        raise RuntimeError('Reversed GCC startup inputs: ' + str(binary))
    if loaded.index('kasm-metadata.o') > loaded.index('crtbeginS.o'):
        raise RuntimeError('Startup data marker must precede GCC startup input: ' + str(binary))
    header = output('readelf', '-hW', str(binary))
    entry = re.search(r'Entry point address:\s+(0x[0-9a-fA-F]+)', header)
    symbols = output('readelf', '-sW', str(binary))
    starts = {int(value, 16) for value in re.findall(
        r'^\s*\d+:\s+([0-9a-fA-F]+)\s+\d+\s+FUNC\s+\S+\s+\S+\s+\S+\s+_start$', symbols, re.M)}
    if not entry or starts != {int(entry[1], 16)}:
        raise RuntimeError('Executable entry does not use owned _start: ' + str(binary))
    sections = output('readelf', '-SW', str(binary))
    data = re.search(r'\]\s+\.data\s+\S+\s+([0-9a-fA-F]+)\b', sections)
    markers = {int(value, 16) for value in re.findall(
        r'^\s*\d+:\s+([0-9a-fA-F]+)\s+\d+\s+OBJECT\s+\S+\s+\S+\s+\S+\s+__data_start$', symbols, re.M)}
    if not data or markers != {int(data[1], 16)}:
        raise RuntimeError('Startup data marker does not identify the start of .data: ' + str(binary))
    return audit_elf(binary)


def audit_elf(binary):
    """Works on stripped packaged executables as well as the original links."""
    header = output('readelf', '-hW', str(binary))
    segments = output('readelf', '-lW', str(binary))
    dynamic = output('readelf', '-dW', str(binary))
    sections = output('readelf', '-SW', str(binary))
    symbols = output('readelf', '-sDW', str(binary))
    if not re.search(r'Type:\s+DYN', header) or not re.search(r'\(FLAGS_1\).*\bPIE\b', dynamic):
        raise RuntimeError('Owned glibc startup requires PIE: ' + str(binary))
    if '[libc.so.6]' not in dynamic or not re.search(r'\bUND\s+__libc_start_main@GLIBC_2\.34\b', symbols):
        raise RuntimeError('Missing host glibc 2.34 startup ABI: ' + str(binary))
    if 'GLIBC_PRIVATE' in symbols:
        raise RuntimeError('Private glibc startup ABI: ' + str(binary))
    for line in sections.splitlines():
        match = re.search(r'\]\s+(\.init|\.fini)\s+\S+\s+[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+([0-9a-fA-F]+)', line)
        if match and int(match[2], 16):
            raise RuntimeError('Unsupported legacy ' + match[1] + ' section: ' + str(binary))
    if re.search(r'\((?:INIT|FINI)\)', dynamic):
        raise RuntimeError('Unsupported legacy initializer/finalizer: ' + str(binary))
    stack = next((line for line in segments.splitlines() if 'GNU_STACK' in line), '')
    if not stack or re.search(r'\bRWE\b', stack) or 'GNU_RELRO' not in segments:
        raise RuntimeError('Missing non-executable stack/RELRO: ' + str(binary))
    if not re.search(r'\((?:FLAGS|FLAGS_1)\).*\b(?:BIND_NOW|NOW)\b', dynamic):
        raise RuntimeError('Missing immediate binding for full RELRO: ' + str(binary))
    return {'mode': MODE, 'host_entry': '__libc_start_main@GLIBC_2.34',
            'legacy_init_fini': False, 'non_executable_stack': True, 'relro': True, 'bind_now': True}
