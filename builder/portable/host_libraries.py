"""The required host-library boundary shared by packaging and ELF audits.

Unversioned C APIs are qualified by building against the oldest supported native
SDK and executing the same packaged binaries across the mandatory runtime
matrix. SONAMEs or compiler symbol versions alone do not prove that boundary.
"""
import fnmatch
import json
import os
from pathlib import Path
import re
import subprocess

CATALOG = json.loads(Path(__file__).with_name('host-libraries.json').read_text())
COMPILER_MAXIMUM = CATALOG['compiler_symbol_maximum']


def compiler_exceptions():
    mode = os.environ.get('KASMVNC_FFMPEG_MODE', 'static')
    return [item for item in CATALOG.get('compiler_symbol_exceptions', [])
            if mode in item['ffmpeg_modes']]


CONVERTED_ARCHIVES = {
    'png', 'png16', 'freetype', 'pixman-1', 'atspi', 'glib-2.0', 'gobject-2.0',
    'gio-2.0', 'gmodule-2.0', 'gthread-2.0', 'dbus-1', 'X11', 'X11-xcb', 'Xau',
    'xcb', 'xcb-*', 'Xcursor', 'Xdmcp', 'Xext', 'Xfixes', 'Xfont2', 'Xrandr',
    'Xrender', 'Xtst', 'Xi', 'xshmfence', 'fontenc', 'unwind', 'unwind-*',
    'crypt', 'systemd', 'systemd-*', 'bsd', 'md', 'intl', 'ffi', 'pcre', 'pcre2-*',
    'expat', 'cap', 'mount', 'blkid', 'uuid', 'lzma', 'lz4', 'zstd', 'bz2',
    'brotli*', 'drm', 'drm_*', 'va', 'va-*', 'sysprof-capture-*',
    'stdc++', 'gcc_eh',
}


def components(profile):
    mode = os.environ.get('KASMVNC_FFMPEG_MODE', 'static')
    return [item for item in CATALOG['components'] if profile in item['profiles']
            and mode in item.get('ffmpeg_modes', ['static', 'adapters'])]


def sonames(profile):
    return {name for item in components(profile) for name in item['sonames']}


def package_dependencies(kind):
    profile = 'musl' if kind == 'apk' else 'glibc'
    return (', ' if kind == 'deb' else ' ').join(dict.fromkeys(
        item[kind] for item in components(profile)))


def reject_static_host_library(path):
    name = re.sub(r'^lib|\.a$', '', Path(path).name)
    if Path(path).suffix == '.a' and any(fnmatch.fnmatch(name, pattern) for pattern in CONVERTED_ARCHIVES):
        raise RuntimeError('Portable artifact unexpectedly embeds a host library: ' + str(path))


def symbol_imports(path):
    text = subprocess.check_output(['readelf', '--dyn-syms', '--wide', str(path)], text=True)
    result = []
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 8 or not re.fullmatch(r'\d+:', fields[0]) or fields[6] != 'UND':
            continue
        name, _, version = fields[7].partition('@')
        result.append({'symbol': name, 'version': version.lstrip('@') or None,
                       'binding': fields[4]})
    return result


def check_compiler_versions(imports):
    for item in imports:
        version = item['version'] or ''
        for prefix, maximum in COMPILER_MAXIMUM.items():
            if not version.startswith(prefix + '_'):
                continue
            suffix = version[len(prefix) + 1:]
            if not re.fullmatch(r'\d+(?:\.\d+)+', suffix) or tuple(map(int, suffix.split('.'))) > tuple(map(int, maximum.split('.'))):
                if any(item['symbol'] == exception['symbol'] and version == exception['version']
                       for exception in compiler_exceptions()):
                    continue
                raise RuntimeError('Compiler runtime import exceeds portable ABI: ' + item['symbol'] + '@' + version)


def audit_host_libraries(path, needed, profile):
    imports = symbol_imports(path)
    check_compiler_versions(imports)
    required = set()
    if path.name in ('Xvnc', 'Xkasmvnc'):
        required = {'libpng16.so.16', 'libfreetype.so.6', 'libpixman-1.so.0',
                    'libatspi.so.0', 'libdbus-1.so.3', 'libglib-2.0.so.0',
                    'libgobject-2.0.so.0', 'libunwind.so.8', 'libXfont2.so.2',
                    'libX11.so.6', 'libstdc++.so.6', 'libgcc_s.so.1'}
        required |= {'libcrypt.so.1', 'libsystemd.so.0'} if profile == 'glibc' else {'libbsd.so.0'}
    elif path.name in ('vncserver-native', 'kasmvncserver', 'vncconfig', 'kasmvncconfig'):
        required = {'libstdc++.so.6'}
    elif path.name == 'kasmxproxy':
        required = {'libX11.so.6', 'libXext.so.6', 'libXtst.so.6', 'libXrandr.so.2',
                    'libXcursor.so.1', 'libXfixes.so.3'}
    missing = required - set(needed)
    if missing:
        raise RuntimeError('Missing required host libraries in ' + str(path) + ': ' + ', '.join(sorted(missing)))
    if 'libz.so.1' in needed:
        raise RuntimeError('Application zlib must remain private/static: ' + str(path))
    # Private zlib must not interpose on distro libpng/FreeType's own zlib.
    defined = subprocess.check_output(['nm', '-D', '--defined-only', '--format=posix', str(path)], text=True)
    zlib_api = re.compile(r'^(?:zlibVersion|zlibCompileFlags|zError|adler32.*|crc32.*|deflate.*|inflate.*|compress(?:2)?(?:_z)?|compressBound(?:_z)?|uncompress(?:2)?(?:_z)?|gz.*|get_crc_table)$')
    imported = [item['symbol'] for item in imports if zlib_api.fullmatch(item['symbol'])]
    if imported:
        raise RuntimeError('Application imports host zlib APIs: ' + str(path) + ': ' + ', '.join(imported))
    exposed = [line.split()[0].split('@')[0] for line in defined.splitlines() if line.split()
               and zlib_api.fullmatch(line.split()[0].split('@')[0])]
    if exposed:
        raise RuntimeError('Private zlib symbols exported by ' + str(path) + ': ' + ', '.join(exposed))
    return {'sonames': sorted(set(needed) & sonames(profile)),
            'compiler_symbol_maximum': COMPILER_MAXIMUM,
            'compiler_symbol_exceptions': compiler_exceptions(),
            'imports': sorted(imports, key=lambda item: (item['symbol'], item['version'] or '')),
            'qualification': 'Native baseline build plus mandatory distribution runtime matrix; imports include libc/OpenSSL/GPU APIs.'}


if __name__ == '__main__':
    import sys
    print(package_dependencies(sys.argv[1]))
