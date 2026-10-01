#!/usr/bin/env python3
"""flash.py - Python wrapper around the AURIX Flasher CLI (ADS bundle).

AURIXFlasher.exe is GUI-first but ships a full command line (AURIXFlasher
--help), which makes it scriptable from Python via subprocess - this module
is that thin wrapper plus two conveniences for the OTA layout:

  devices                     list supported devices (-l)
  flash <file.hex|elf>        program one image
                              (defaults: -erase on = only the logical sectors
                              the image touches, so flashing SBL/App never
                              wipes the other slot; -ver on; -start on =
                              reset and run afterwards)
  factory [app.hex]           merge SBL hex + App slot-A hex (merge_hex.py)
                              and flash the combined image in one go

Examples:
  python tools/flash.py devices
  python tools/flash.py flash   # 默认取 SCons 产物 build/tasking-*/ 下最新 hex
  python tools/flash.py flash "TriCore Debug (TASKING)/tc275_car.hex" --id 0
  python tools/flash.py factory "../tc275_car/TriCore Debug (TASKING)/tc275_car.hex"

Raw CLI (what this passes through), notable options:
  -hex <f>|-elf <f>  image to program          -id <n>     DAS port index
  -erase on|all|off  erase policy (default on) -prog on|off
  -ver on|off        post verify               -connect 0|6 (hot attach | reset&halt)
  -start on|off      reset-and-run at the end  -log <xml>  detailed log
  -ucb on|off        BMHD into UCB (TC3xx only, DANGEROUS - off by default)
  -script <txt>      TAS script (INIT/COPY/SET/WAIT/RESET/LOAD/VERIFY/FLASH/ERASE)
"""
import argparse
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# SCons 产物（带版本名，取 mtime 最新的 hex；退回 ADS 的 Debug/）
SBL_HEX_CANDIDATES = [
    os.path.join(REPO, 'build', 'tasking-*', 'tc275_sbl_v*.hex'),
    os.path.join(REPO, 'build', 'tasking-*', 'tc275_sbl.hex'),
    os.path.join(REPO, 'Debug', 'tc275_sbl.hex'),
]


def default_sbl_hex():
    for pat in SBL_HEX_CANDIDATES:
        hits = sorted(glob.glob(pat), key=os.path.getmtime)
        if hits:
            return hits[-1]
    return SBL_HEX_CANDIDATES[-1]


DEFAULT_EXE_SEARCH = [
    r'C:\Infineon\AURIX-Studio-1.10.40\tools\AurixFlasherSoftwareTool_v3.0.18\AURIXFlasher.exe',
    r'C:\Infineon\AURIX-Studio-1.10.36\tools\AurixFlasherSoftwareTool_v3.0.18\AURIXFlasher.exe',
]


def find_exe(override):
    if override:
        if not os.path.isfile(override):
            sys.exit('AURIXFlasher not found: %s' % override)
        return override
    for p in DEFAULT_EXE_SEARCH:
        if os.path.isfile(p):
            return p
    sys.exit('AURIXFlasher.exe not found in the known ADS locations; '
             'pass --exe <path>')


def run_flasher(exe, args, timeout_s=600):
    """Run AURIXFlasher with CLI args, stream output, return exit code."""
    cmd = [exe] + args
    print('>> %s' % ' '.join('"%s"' % c if ' ' in c else c for c in cmd))
    try:
        proc = subprocess.run(cmd, timeout=timeout_s,
                              stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT)
    except subprocess.TimeoutExpired:
        print('TIMEOUT after %ss' % timeout_s)
        return 1
    out = proc.stdout.decode('mbcs', errors='replace')
    if out.strip():
        print(out.rstrip())
    return proc.returncode


def cmd_devices(exe, a):
    return run_flasher(exe, ['-l'])


def cmd_flash(exe, a):
    ext = os.path.splitext(a.image)[1].lower()
    if ext == '.elf':
        img = ['-elf', a.image]
    else:
        img = ['-hex', a.image]
    args = img + ['-id', str(a.id),
                  '-erase', a.erase,
                  '-prog', 'on' if not a.no_prog else 'off',
                  '-ver', 'on' if a.verify else 'off',
                  '-start', a.start]
    if a.connect is not None:
        args += ['-connect', str(a.connect)]
    if a.log:
        args += ['-log', a.log]
    if a.script:
        args += ['-script', a.script]
    return run_flasher(exe, args)


def cmd_factory(exe, a):
    sbl = a.sbl if a.sbl else default_sbl_hex()
    if not os.path.isfile(sbl):
        sys.exit('SBL hex not found: %s (build the SBL first)' % sbl)
    if not os.path.isfile(a.app):
        sys.exit('App hex not found: %s' % a.app)
    merged = a.out
    r = subprocess.run(
        [sys.executable, os.path.join(HERE, 'merge_hex.py'),
         merged, sbl, a.app]).returncode
    if r != 0:
        return r
    return run_flasher(exe, ['-hex', merged, '-id', str(a.id),
                             '-erase', a.erase, '-prog', 'on',
                             '-ver', 'on' if a.verify else 'off',
                             '-start', a.start] +
                            (['-connect', str(a.connect)] if a.connect is not None else []) +
                            (['-log', a.log] if a.log else []))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exe', help='path to AURIXFlasher.exe')
    ap.add_argument('--id', type=int, default=0, help='DAS port index (default 0)')
    ap.add_argument('--erase', default='on', choices=['on', 'all', 'off'],
                    help='on = only sectors the image uses (default, keeps '
                         'the other OTA slot), all = whole flash, off')
    ap.add_argument('--verify', action='store_true', default=True,
                    help='post-flash verify (default on)')
    ap.add_argument('--start', default='on', choices=['on', 'off'],
                    help='reset and run after flashing (default on)')
    ap.add_argument('--connect', type=int, choices=[0, 6], default=None,
                    help='0 = hot attach, 6 = reset&halt (tool default 6)')
    ap.add_argument('--log', help='write the tool XML log to this file')
    sub = ap.add_subparsers(dest='cmd', required=True)

    sub.add_parser('devices', help='list supported devices (-l)')

    p_flash = sub.add_parser('flash', help='program one hex/elf image')
    p_flash.add_argument('image')
    p_flash.add_argument('--no-prog', action='store_true',
                         help='load/verify only, do not program')
    p_flash.add_argument('--script', help='extra TAS script to run afterwards')

    p_fact = sub.add_parser('factory', help='merge SBL+App hex and flash it')
    p_fact.add_argument('app', help='App slot-A hex (e.g. tc275_car build output)')
    p_fact.add_argument('--sbl', help='SBL hex (default: newest build/tasking-*/tc275_sbl_v*.hex)')
    p_fact.add_argument('--out', default=os.path.join(REPO, 'build', 'tasking-debug', 'factory_full.hex'),
                        help='merged file to flash')

    a = ap.parse_args()
    exe = find_exe(a.exe)

    if a.cmd == 'devices':
        sys.exit(cmd_devices(exe, a))
    if a.cmd == 'flash':
        sys.exit(cmd_flash(exe, a))
    if a.cmd == 'factory':
        sys.exit(cmd_factory(exe, a))


if __name__ == '__main__':
    main()
