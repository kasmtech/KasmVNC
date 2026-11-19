#!/usr/bin/env python3
# Copyright (c) 2026 Kasm. SPDX-License-Identifier: GPL-2.0-or-later
"""Run inside a dedicated GPU test container; do not attach to a user session."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("server", type=Path)
parser.add_argument("render_node")
parser.add_argument("--build-dir", type=Path, default=Path("/tmp/dri3-lifetime"))
parser.add_argument("--display", default=":72")
args = parser.parse_args()
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
source = Path(__file__).resolve().parent
for name, flags in (("lifetime", ["-lgbm", "-lxcb", "-lxcb-dri3"]),
                    ("lifetime-audit", ["-shared", "-fPIC", "-ldl"])):
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-O0", "-g",
                    str(source / (name + ".c")), "-o",
                    str(build / (name + (".so" if "audit" in name else ""))),
                    *flags], check=True)
env = dict(os.environ, DISPLAY=args.display)
for cycle in range(3):
    log_path = build / f"server-{cycle}.log"
    with tempfile.TemporaryDirectory(prefix="dri3-home-") as home, log_path.open("w") as log:
        server = subprocess.Popen([
            str(args.server.resolve()), args.display, "-geometry", "128x128",
            "-depth", "24", "-hw3d", "-drinode", args.render_node,
            "-SecurityTypes", "None", "-disableBasicAuth", "-sslOnly", "0",
            "-websocketPort", "6972", "-rfbport", "5972", "-interface", "127.0.0.1",
            "-ac", "-noreset"], stdout=log, stderr=subprocess.STDOUT,
            env=dict(env, HOME=home, LD_PRELOAD=str(build / "lifetime-audit.so")))
        try:
            for attempt in range(100):
                if server.poll() is not None:
                    raise RuntimeError(log_path.read_text())
                if subprocess.run(["xdpyinfo"], env=env, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL).returncode == 0:
                    break
                time.sleep(.1)
            else:
                raise RuntimeError("X server did not become ready")
            subprocess.run([str(build / "lifetime"), args.render_node], env=env,
                           check=True, timeout=35)
            # Only the exported root pixmap should remain before shutdown.
            events = re.findall(r"GBM lifetime: created=(\d+) destroyed=(\d+) live=(\d+)",
                                log_path.read_text())
            assert events and int(events[-1][0]) >= 201, events[-1:]
            assert int(events[-1][2]) == 1, f"Buffers leaked before shutdown: {events[-1]}"
        finally:
            if server.poll() is None:
                server.terminate()
            try:
                code = server.wait(timeout=15)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
                raise
    text = log_path.read_text()
    assert code == 0, text
    assert "live_at_device_destroy=0" in text, text
    print(f"Cycle {cycle + 1}: imports, exports, re-exports and retained root cleanup PASS", flush=True)
