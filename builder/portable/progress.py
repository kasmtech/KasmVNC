#!/usr/bin/env python3
"""Show bounded live build progress without buffering low-volume input to EOF."""
import argparse
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--interval", type=float, default=60)
args = parser.parse_args()
if args.interval <= 0:
    parser.error("--interval must be positive")
sys.stdin.reconfigure(errors="replace")
last = time.monotonic()
for line in sys.stdin:
    now = time.monotonic()
    if now - last >= args.interval:
        print("Portable build progress: " + line.rstrip()[:180], flush=True)
        last = now
