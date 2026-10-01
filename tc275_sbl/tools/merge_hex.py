#!/usr/bin/env python3
"""merge_hex.py - merge non-overlapping Intel-HEX images into one flash file.

Factory/whole-chip flashing of the OTA scheme needs the SBL (reset vector,
0x80000000..) and the App slot image (slot A at 0x80008000..) as ONE file:
the two images stay separate builds (each brings its own CStart/libc, so a
single ELF link is not an option), but their flash ranges are disjoint and
merge cleanly at the HEX level.

    python tools/merge_hex.py out.hex sbl.hex appA.hex [appB.hex ...]
    python tools/merge_hex.py out.hex a.hex b.hex --bin out.bin

Rules:
  - record types 00/01/04/05 (data, EOF, ext-linear, start-linear) handled;
    02/03 (segment addressing) rejected - TASKING emits type 04
  - any byte written twice must be identical, otherwise ERROR (overlap)
  - the output carries the lowest start-linear address as its entry
    (the SBL's 0x80000020), so flashers that honor it land in the SBL
  - --bin additionally writes a flat binary over [lowest..highest] address,
    gaps padded with --pad (default 0xFF, erased flash state)
"""
import argparse
import sys


class HexError(Exception):
    pass


def parse_hex(path, mem):
    base = 0
    lo, hi = None, None
    entry = None
    for lineno, raw in enumerate(open(path, 'rb').read().split(b'\n'), 1):
        line = raw.strip().decode('ascii', 'ignore')
        if not line:
            continue
        if not line.startswith(':'):
            raise HexError('%s:%d: not an Intel HEX line' % (path, lineno))
        try:
            b = bytes.fromhex(line[1:])
        except ValueError:
            raise HexError('%s:%d: bad hex digits' % (path, lineno))
        if len(b) < 5 or (len(b) - 5) != b[0]:
            raise HexError('%s:%d: length mismatch' % (path, lineno))
        cnt, addr, typ = b[0], (b[1] << 8) | b[2], b[3]
        data = b[4:4 + cnt]
        if (sum(b) & 0xFF) != 0:
            raise HexError('%s:%d: checksum error' % (path, lineno))
        if typ == 0:
            a = base + addr
            for i, v in enumerate(data):
                p = a + i
                if p in mem and mem[p] != v:
                    raise HexError(
                        'overlap: 0x%08X written 0x%02X and 0x%02X '
                        '(second file: %s)' % (p, mem[p], v, path))
                mem[p] = v
            lo = a if lo is None else min(lo, a)
            hi = a + cnt - 1 if hi is None else max(hi, a + cnt - 1)
        elif typ == 1:
            break
        elif typ == 4:
            base = ((data[0] << 8) | data[1]) << 16
        elif typ == 5:
            entry = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3]
        else:
            raise HexError('%s:%d: record type %d unsupported' %
                           (path, lineno, typ))
    return lo, hi, entry


def write_hex(path, mem, entry):
    addrs = sorted(mem)
    lo, hi = addrs[0], addrs[-1]
    cur_seg = None
    out = []
    row = 32

    def rec(cnt, addr, typ, data=b''):
        body = bytes([cnt, (addr >> 8) & 0xFF, addr & 0xFF, typ]) + data
        body += bytes([(-(sum(body))) & 0xFF])
        return ':' + body.hex().upper()

    i = 0
    while i < len(addrs):
        a = addrs[i]
        seg = a >> 16
        if seg != cur_seg:
            out.append(rec(2, 0, 4,
                           bytes([(seg >> 8) & 0xFF, seg & 0xFF])))
            cur_seg = seg
        chunk = addrs[i:i + row]
        # keep records contiguous within a 64 K segment window
        data = bytearray()
        start = a
        for p in chunk:
            if p != start + len(data) or ((start + len(data)) >> 16) != seg:
                break
            data.append(mem[p])
        out.append(rec(len(data), start & 0xFFFF, 0, bytes(data)))
        i += len(data)
    if entry is not None:
        out.append(rec(4, 0, 5, entry.to_bytes(4, 'big')))
    out.append(rec(0, 0, 1))
    open(path, 'w', newline='\n').write('\n'.join(out) + '\n')
    return lo, hi


def write_bin(path, mem, pad):
    addrs = sorted(mem)
    lo, hi = addrs[0], addrs[-1]
    buf = bytearray([pad]) * (hi - lo + 1)
    for p, v in mem.items():
        buf[p - lo] = v
    open(path, 'wb').write(bytes(buf))
    return lo, hi


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out', help='merged output .hex')
    ap.add_argument('inputs', nargs='+', help='input Intel HEX files')
    ap.add_argument('--bin', help='also write a flat binary (gaps padded)')
    ap.add_argument('--pad', type=lambda x: int(x, 0), default=0xFF)
    args = ap.parse_args()

    mem = {}
    entry = None
    for f in args.inputs:
        lo, hi, e = parse_hex(f, mem)
        print('%-40s 0x%08X..0x%08X%s' %
              (f, lo, hi, ('  entry 0x%08X' % e) if e is not None else ''))
        if e is not None:
            entry = e if entry is None else min(entry, e)

    lo, hi = write_hex(args.out, mem, entry)
    print('merged -> %s  0x%08X..0x%08X  (%d B)  entry 0x%08X' %
          (args.out, lo, hi, hi - lo + 1, entry or 0))
    if args.bin:
        blo, bhi = write_bin(args.bin, mem, args.pad)
        print('binary -> %s  0x%08X..0x%08X  (%d B, pad 0x%02X)' %
              (args.bin, blo, bhi, bhi - blo + 1, args.pad))
    return 0


if __name__ == '__main__':
    sys.exit(main())
