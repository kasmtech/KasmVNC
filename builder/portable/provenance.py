#!/usr/bin/env python3
"""Record pinned sources, installed builder RPMs and license notices.

This inventory is build provenance, not a claim to be a complete SBOM.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from openssl_boundary import read_sdk, sdk_path
from host_libraries import sonames, COMPILER_MAXIMUM, compiler_exceptions
from glibc_crt import MODE as GLIBC_STARTUP_MODE

doc = Path(sys.argv[1])
licenses = doc / "third-party-licenses"
licenses.mkdir(parents=True, exist_ok=True)
sources = []
musl = os.environ.get("KASMVNC_LIBC") == "musl"
def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()

for base in sorted({Path(os.environ.get("KASMVNC_RUNTIME_SOURCES", "/tmp/static-sources")),
             Path(os.environ.get("KASMVNC_CODEC_SOURCES", "/tmp/static-sources")),
             Path(os.environ.get("KASMVNC_DESKTOP_SOURCES", "/tmp/static-desktop-sources")),
             Path(os.environ.get("KASMVNC_GCC_RUNTIME_BUILD_DIR", "/tmp/kasm-gcc-runtime"))}):
    if not base.exists():
        continue
    for source in sorted(base.iterdir()):
        if source.is_file() and ".tar." in source.name:
            sources.append({"archive": source.name, "sha256": sha256(source)})
        elif source.is_dir():
            if (source / ".git").exists():
                sources.append({"repository": subprocess.check_output(["git", "-C", str(source), "remote", "get-url", "origin"], text=True).strip(),
                                "revision": subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()})
            for notice in source.iterdir():
                if notice.is_file() and notice.name.upper().startswith(("LICENSE", "COPYING", "COPYRIGHT", "NOTICE")):
                    dest = licenses / source.name
                    dest.mkdir(exist_ok=True)
                    shutil.copy2(notice, dest / notice.name)
# Retained distro archives carry distro security backports; preserve their exact
# package versions and installed license notices alongside source-built notices.
# Embedded RPM license notices are selected by supply-chain.py from link evidence.
# Keep builder inventory for diagnostics only; it is not the runtime SBOM.
if musl:
    shutil.copy2('/lib/apk/db/installed', doc / 'builder-apk-installed.txt')
else:
    rpms = subprocess.check_output(["rpm", "-qa", "--qf", "%{NAME}\t%{VERSION}-%{RELEASE}\t%{ARCH}\t%{LICENSE}\n"], text=True)
    (doc / "builder-rpms.tsv").write_text(rpms)
manifest = {"format": "kasmvnc-portable-build-provenance-v1", "revision": os.environ["KASMVNC_COMMIT_ID"],
            "version": os.environ["KASMVNC_PORTABLE_VERSION"], "glibc_abi_maximum": "2.34",
            "architecture": sys.argv[2], "sources": sources,
            "elf_audit": json.loads((doc / "elf-audit.json").read_text()),
            "runtime_boundary": "Host glibc, GPU loader/driver stack, timezone data, XKB tools/data and desktop remain external."}
manifest["dependency_builder"] = {
    "image": os.environ.get("KASMVNC_DEPENDENCY_IMAGE"),
    "recipe_sha256": os.environ.get("KASMVNC_DEPENDENCY_KEY"),
    "rpm_resolution": "Pinned base; RPM repositories resolved at image creation. Image digest identifies exact build environment; cold rebuild may resolve newer RPMs.",
}
manifest['ffmpeg_mode'] = os.environ.get('KASMVNC_FFMPEG_MODE', 'static')
manifest['openssl_mode'] = 'host-shared-3'
manifest['desktop_mode'] = 'host-shared'
manifest['compiler_runtime_mode'] = 'host-shared'
manifest['startup_mode'] = 'compiler-default' if musl else GLIBC_STARTUP_MODE
if not musl and any(elf.get('startup', {}).get('mode') != GLIBC_STARTUP_MODE
                    for elf in manifest['elf_audit']):
    raise RuntimeError('Portable glibc provenance requires audited first-party startup')
manifest['host_libraries'] = sorted({lib for elf in manifest['elf_audit']
                                    for lib in elf['needed']} & sonames('musl' if musl else 'glibc'))
manifest['compiler_symbol_maximum'] = COMPILER_MAXIMUM
manifest['compiler_symbol_exceptions'] = compiler_exceptions()
shutil.copy2(Path(__file__).with_name('host-libraries.json'), doc / 'host-libraries.json')
manifest['openssl_build_sdk'] = read_sdk()
shutil.copy2(sdk_path() / 'share/kasmvnc/openssl-sdk.json', doc / 'openssl-build-sdk.json')
manifest['sources_scope'] = 'Builder source inventory, including cached inputs not linked into this artifact; consult sources.json and sbom.cdx.json for shipped components.'
if manifest['ffmpeg_mode'] == 'adapters':
    header_root = Path(os.environ.get('KASMVNC_FFMPEG_HEADERS_ROOT', '/opt/kasm-ffmpeg-headers'))
    manifest['ffmpeg_adapter_headers'] = json.loads((header_root / 'manifest.json').read_text())
    shutil.copy2(header_root / 'manifest.json', doc / 'ffmpeg-adapter-headers.json')
if musl:
    manifest.pop("glibc_abi_maximum")
    manifest["musl_abi_minimum"] = "1.2.5"
    manifest["runtime_boundary"] = "Host musl, musl-compatible GPU drivers/loaders, timezone data, XKB tools/data and desktop remain external."
    manifest["dependency_builder"].pop("rpm_resolution")
    manifest["dependency_builder"]["apk_resolution"] = "Pinned Alpine base; APK repositories resolved at image creation; image digest and installed APK inventory identify the environment."
    shutil.copytree('/src/builder/musl/dependencies', doc / 'musl-dependency-recipes', dirs_exist_ok=True)
if manifest['ffmpeg_mode'] == 'adapters':
    families = ', '.join(str(item['avcodec_major']) for item in manifest['ffmpeg_adapter_headers']['families'])
    manifest['runtime_boundary'] += f' FFmpeg families {families} and their encoder libraries are host dependencies for video; no FFmpeg runtime is embedded.'
manifest['runtime_boundary'] += (' OpenSSL libssl.so.3 and libcrypto.so.3, providers and security configuration '
                                 'are supplied and patched by the host. The 3.0 SDK is a build input only; '
                                 'its runtime libraries are not shipped.')
manifest['runtime_boundary'] += (' Desktop C libraries and C++/exception runtimes are host-supplied. '
                                 'Their private dependency closure is owned by the distribution. '
                                 'Application zlib remains private/static for compression performance; '
                                 'host PNG/font libraries use their own distro zlib. '
                                 'Selected compiler compatibility/startup objects remain embedded; '
                                 'the link-input SBOM identifies them exactly.')
if not musl:
    manifest['runtime_boundary'] += (' First-party executable startup calls the host glibc 2.34 '
                                     'entry ABI; no glibc startup objects or nonshared archive '
                                     'members are embedded. Glibc headers remain build inputs.')
shutil.copytree('/src/builder/openssl3', doc / 'openssl-sdk-recipes', dirs_exist_ok=True)
shutil.copytree("/src/builder/scripts/portable-dependencies", doc / "dependency-recipes", dirs_exist_ok=True)
gcc_manifest = Path(os.environ.get("KASMVNC_GCC_RUNTIME_PREFIX", "/opt/kasm-gcc-runtime")) / "share/gcc-runtime"
manifest['gcc_runtime'] = {'delivery': 'host-shared', 'symbol_version_maximum': COMPILER_MAXIMUM,
                           'embedded_inputs': 'Only selected builtin/startup and toolset nonshared compatibility inputs; exact source and ownership recorded by supply-chain.py.'}
if not musl and gcc_manifest.exists():
    shutil.copytree(gcc_manifest, doc / "gcc-runtime", dirs_exist_ok=True)
    manifest["cached_gcc_runtime_build_input"] = json.loads((gcc_manifest / "manifest.json").read_text())
    manifest["cached_gcc_runtime_build_input"]["builder_image"] = os.environ.get("KASMVNC_GCC_RUNTIME_IMAGE")
    manifest["cached_gcc_runtime_build_input"]["recipe_sha256"] = os.environ.get("KASMVNC_GCC_RUNTIME_KEY")
    manifest["cached_gcc_runtime_build_input"]["scope"] = 'Cached build provenance only; not evidence that its static unwind runtime is linked.'
    shutil.copytree('/src/builder/scripts/portable-gcc-runtime', doc / 'gcc-runtime-recipes', dirs_exist_ok=True)
(doc / "provenance.json").write_text(json.dumps(manifest, indent=2) + "\n")
shutil.copytree("/src/builder/scripts", doc / "build-scripts", dirs_exist_ok=True)
for script in (doc / "build-scripts").rglob("*"):
    if script.is_file():
        script.chmod(0o644)
