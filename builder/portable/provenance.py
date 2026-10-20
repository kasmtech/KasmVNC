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
if musl:
    manifest.pop("glibc_abi_maximum")
    manifest["musl_abi_minimum"] = "1.2.5"
    manifest["runtime_boundary"] = "Host musl, musl-compatible GPU drivers/loaders, timezone data, XKB tools/data and desktop remain external."
    manifest["dependency_builder"].pop("rpm_resolution")
    manifest["dependency_builder"]["apk_resolution"] = "Pinned Alpine base; APK repositories resolved at image creation; image digest and installed APK inventory identify the environment."
    shutil.copytree('/src/builder/musl/dependencies', doc / 'musl-dependency-recipes', dirs_exist_ok=True)
shutil.copytree("/src/builder/scripts/portable-dependencies", doc / "dependency-recipes", dirs_exist_ok=True)
gcc_manifest = Path(os.environ.get("KASMVNC_GCC_RUNTIME_PREFIX", "/opt/kasm-gcc-runtime")) / "share/gcc-runtime"
if musl:
    manifest["gcc_runtime"] = "Native Alpine GCC archives; exact APK ownership, aports commit, patches and verified upstream sources recorded by supply-chain.py."
elif gcc_manifest.exists():
    shutil.copytree(gcc_manifest, doc / "gcc-runtime", dirs_exist_ok=True)
    manifest["gcc_runtime"] = json.loads((gcc_manifest / "manifest.json").read_text())
else:
    if os.environ.get("KASMVNC_PORTABLE_REQUIRE_PROVENANCE") == "1":
        raise SystemExit("Missing mandatory GCC runtime source manifest")
    manifest["gcc_runtime"] = "Prototype runtime: source manifest unavailable; not suitable for release."
(doc / "provenance.json").write_text(json.dumps(manifest, indent=2) + "\n")
shutil.copytree("/src/builder/scripts", doc / "build-scripts", dirs_exist_ok=True)
for script in (doc / "build-scripts").rglob("*"):
    if script.is_file():
        script.chmod(0o644)
