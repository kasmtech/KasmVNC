#!/usr/bin/env python3
"""Audit every ELF in the staged package, not just Xvnc."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys

root = Path(sys.argv[1])
arch = sys.argv[2]
allowed = {"libc.so.6", "libm.so.6", "libmvec.so.1", "libresolv.so.2", "libdl.so.2",
           "libpthread.so.0", "librt.so.1", "libGL.so.1", "libgbm.so.1",
           "ld-linux-x86-64.so.2", "ld-linux-aarch64.so.1"}
records = []
for path in sorted(root.rglob("*")):
    if not path.is_file() or path.is_symlink():
        continue
    with path.open("rb") as stream:
        if stream.read(4) != b"\x7fELF":
            continue
    header = subprocess.check_output(["readelf", "-hW", str(path)], text=True)
    machine = "Advanced Micro Devices X86-64" if arch == "amd64" else "AArch64"
    if machine not in header or not re.search(r"Type:\s+DYN", header):
        raise SystemExit(f"Expected native PIE executable: {path}")
    dynamic = subprocess.check_output(["readelf", "-dW", str(path)], text=True)
    needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic)
    if set(needed) - allowed:
        raise SystemExit(f"Unexpected runtime libraries in {path}: {set(needed) - allowed}")
    if "TEXTREL" in dynamic or "RPATH" in dynamic or "RUNPATH" in dynamic:
        raise SystemExit(f"Nonportable relocations or library search path in {path}")
    subprocess.run(["/src/builder/scripts/check-glibc-floor", "--maximum", "2.34", str(path)], check=True)
    records.append({"path": "/" + str(path.relative_to(root)), "needed": needed,
                    "glibc_maximum": "2.34", "pie": True})
if len(records) != 4:
    raise SystemExit(f"Expected all four packaged executables, found {len(records)}")
Path(sys.argv[3]).write_text(json.dumps(records, indent=2) + "\n")
