#!/usr/bin/env python3
"""Emit bounded, timed progress even when the build produces no new output."""
import argparse
import codecs
import os
import selectors
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interval", type=float, default=60)
    args = parser.parse_args()
    if not 0 < args.interval < float("inf"):
        parser.error("--interval must be finite and positive")
    decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
    latest = "Waiting for build output"
    partial = ""
    started = received = time.monotonic()
    deadline = started + args.interval
    with selectors.DefaultSelector() as selector:
        selector.register(sys.stdin, selectors.EVENT_READ)
        while True:
            ready = selector.select(max(0, deadline - time.monotonic()))
            if ready:
                chunk = os.read(sys.stdin.fileno(), 65536)
                if not chunk:
                    return
                received = time.monotonic()
                # Curl uses CR rather than LF. Bound memory even for a producer
                # emitting an arbitrarily long line. Full output is kept by tee.
                for part in decoder.decode(chunk).replace("\r", "\n").splitlines(keepends=True):
                    partial = (partial + part.rstrip("\n"))[-180:]
                    if partial.strip():
                        latest = partial.strip()
                    if part.endswith("\n"):
                        partial = ""
            now = time.monotonic()
            if now >= deadline:
                print(f"Portable build progress [{now - started:.0f}s elapsed, "
                      f"{now - received:.0f}s since output]: {latest}", flush=True)
                deadline = now + args.interval


if __name__ == "__main__":
    main()
