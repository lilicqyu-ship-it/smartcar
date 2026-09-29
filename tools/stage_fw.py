#!/usr/bin/env python3
"""Stage a C6 or TC275 firmware image into the S3 remote's flash.

doc/08-architecture-v2.md §3: each staging partition starts with a 4 KB SCFW
header (magic, target, size, CRC32, version, build time) followed by the raw
payload.  The S3 firmware-update page re-verifies the CRC and streams the
payload to the C6 (/ota/c6 or /ota/tc275).

  C6 : payload must be the signed C6FW bundle the C6 accepts on /ota/c6
  TC : payload is the raw TC275 image the C6 relays on /ota/tc275

Usage (ESP-IDF environment active, S3 on its UART bridge):
  tools/stage_fw.py --target c6    --image c6_bundle.bin --version 0.1.3 --port /dev/cu.usbserial-1130
  tools/stage_fw.py --target tc275 --image mycar.bin     --version 1.4.0 --port /dev/cu.usbserial-1130
  tools/stage_fw.py --target c6 --clear --port ...        # erase the header (region shows EMPTY)
  tools/stage_fw.py --target c6 --image x.bin --out hdr.bin   # only build the file, no flashing
"""
import argparse
import binascii
import datetime
import os
import struct
import subprocess
import sys
import tempfile

# must match partitions.csv
REGIONS = {
    "c6":    {"offset": 0x620000, "size": 0x310000, "code": 1},
    "tc275": {"offset": 0x930000, "size": 0x310000, "code": 2},
}
HDR_SIZE = 4096
MAX_PAYLOAD = 3 * 1024 * 1024


def build(target: str, image: bytes, version: str) -> bytes:
    reg = REGIONS[target]
    if not image:
        sys.exit("image is empty")
    if len(image) > MAX_PAYLOAD or len(image) > reg["size"] - HDR_SIZE:
        sys.exit(f"image too large: {len(image)} B (max {MAX_PAYLOAD})")
    if target == "c6" and image[:4] != b"C6FW":
        sys.exit("C6 payload must be a signed C6FW bundle (magic 'C6FW'); "
                 "a raw c6.bin is rejected by the C6")
    crc = binascii.crc32(image) & 0xFFFFFFFF
    built = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    hdr = struct.pack("<4sHBBII32s32s", b"SCFW", 1, reg["code"], 0, len(image), crc,
                      version.encode()[:31], built.encode()[:31])
    hdr += b"\xff" * (HDR_SIZE - len(hdr))
    print(f"{target}: {len(image)} B, crc32 {crc:08X}, version '{version}', built {built}")
    return hdr + image


def flash(port: str, offset: int, blob: bytes, baud: int) -> None:
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(blob)
        path = f.name
    try:
        cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", port, "-b", str(baud),
               "write-flash", hex(offset), path]
        print("$", " ".join(cmd))
        subprocess.run(cmd, check=True)
    finally:
        os.unlink(path)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", required=True, choices=REGIONS.keys())
    ap.add_argument("--image")
    ap.add_argument("--version", default="")
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--clear", action="store_true", help="erase the header sector")
    ap.add_argument("--out", help="write the staged blob to a file instead of flashing")
    a = ap.parse_args()
    reg = REGIONS[a.target]

    if a.clear:
        blob = b"\xff" * HDR_SIZE
    else:
        if not a.image:
            ap.error("--image is required (or --clear)")
        with open(a.image, "rb") as f:
            blob = build(a.target, f.read(), a.version or os.path.basename(a.image))

    if a.out:
        with open(a.out, "wb") as f:
            f.write(blob)
        print(f"wrote {a.out} ({len(blob)} B) -> flash at {reg['offset']:#x}")
        return
    if not a.port:
        ap.error("--port is required to flash")
    flash(a.port, reg["offset"], blob, a.baud)
    print("done - open Settings > FIRMWARE on the remote (or RESCAN) to verify")


if __name__ == "__main__":
    main()
