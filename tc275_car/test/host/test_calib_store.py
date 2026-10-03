#!/usr/bin/env python3
"""Run the production persistence scheduler with mocked flash/xcore/time.

The included C comes verbatim from calib_store.c's scheduler and live-record
sections. Only the iLLD flash section is excluded; its boundary is mocked by
test_calib_store.c. This does not compile or validate the TriCore firmware.
"""
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[2]
    source = root / "mw/calib/calib_store.c"
    text = source.read_text()
    start = text.index("/* Deferred write only")
    hardware = text.index("/* ---- low-level flash")
    live = text.index("/* ---- live-record plumbing")
    with tempfile.TemporaryDirectory(prefix="calib-store-test-") as directory:
        work = Path(directory)
        include = work / "calib_store_host.inc"
        include.write_text(
            f'#line {text[:start].count(chr(10)) + 1} "{source.as_posix()}"\n'
            + text[start:hardware]
            + f'#line {text[:live].count(chr(10)) + 1} "{source.as_posix()}"\n'
            + text[live:]
        )
        binary = work / "test_calib_store"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra",
            "-Werror", "-O2", "-I", str(root / "test/host/stub"),
            "-I", str(root), "-I", str(work),
            str(root / "test/host/test_calib_store.c"),
            str(root / "mw/calib/calib_record.c"),
            str(root / "mw/sf/sf_frame.c"), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
