#!/usr/bin/env python3
"""ESP32-C6 flashing helper - click CLI, pure-Python twin of flash.bat.

Commands:
  build   compile the firmware (idf.py build)
  full    flash bootloader + partition table + otadata + firmware (default)
  assets  control page only (repacks assets_src, firmware untouched)
  all     assets first, then full firmware
  mon     serial monitor only (Ctrl+C to exit)

The COM port is auto-detected (Espressif / bridge-chip USB VID); override
with -p COMx.  Works on Windows and macOS/Linux: the IDF environment
(venv python, IDF path, activation script) is discovered from the EIM
metadata of the respective install layout.

Examples:
  python flash.py                 # same as: full
  python flash.py build           # compile only
  python flash.py all -b -m       # build + assets + firmware + monitor

Runs under any Python 3.8+; if `click` is missing the script relaunches
itself under the ESP-IDF venv interpreter (discovered from EIM metadata),
which always has click (idf.py depends on it).
"""
import json
import os
import re
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parent
BUILD = PROJECT / "build"
BAUD = 460800
CHIP = "esp32c6"  # this helper is esp32c6_car-specific

# Port auto-detection: the board's built-in USB-Serial-JTAG reports
# Espressif's VID; external USB-UART bridges get a second-chance match.
# An explicit -p always wins.  Note VID_058B (Infineon DAS, the TC275 side)
# must never match.
ESPRESSIF_VID = 0x303A
BRIDGE_VIDS = frozenset((0x10C4, 0x1A86, 0x0403))  # CP210x, CH34x, FTDI


def eim_json_candidates():
    """EIM writes its install manifest at a per-OS default location."""
    if sys.platform == "win32":
        return [Path(r"C:\Espressif\tools\eim_idf.json")]
    home = Path.home()
    return sorted(home.glob(".espressif/tools/eim_idf.json"))


def _ver_token(path):
    """The version tag inside a path: the deepest part that carries one, so
    python/v6.1/venv/..., esp/v6.1/esp-idf and Microsoft.v6.1.ps1 all compare."""
    hits = [p for p in Path(path).parts if re.search(r"\d+\.\d+", p)]
    return hits[-1] if hits else Path(path).name


def _ver_of(path):
    """Compare/sort key for an install path: ((6, 1), 1, '') for the v6.1
    release, ((6, 1), 0, 'beta1') for a prerelease of it - the release must
    outrank the betas it was cut from, and the numeric series must compare as
    numbers (v6.10 > v6.9).  Paths with no version sort first."""
    token = _ver_token(path)
    # A prerelease tag carries digits (beta1, rc2), so the trailing
    # PowerShell_profile.ps1 of an EIM script name is not read as one.
    m = re.search(r"(\d+(?:\.\d+)+)(?:[-_.]([A-Za-z]*\d+[A-Za-z0-9]*))?", token)
    if not m:
        return ((0,), 0, token.lower())
    pre = (m.group(2) or "").lower()
    return (tuple(int(x) for x in m.group(1).split(".")), 0 if pre else 1, pre)


def _newest(paths):
    """Highest ESP-IDF version of `paths`, empty Path when there are none."""
    paths = list(paths)
    return max(paths, key=_ver_of) if paths else Path()


def _for_version(paths, want):
    """The candidate whose version equals `want`'s, else the same numeric
    series (a beta when only its prerelease is installed), else the newest.
    Keeps the python/IDF/activation triple consistent on a mixed machine."""
    w = _ver_of(want)
    paths = list(paths)
    for cands in ([p for p in paths if _ver_of(p) == w],
                  [p for p in paths if _ver_of(p)[0] == w[0]]):
        if cands:
            return _newest(cands)
    return _newest(paths)


def _win_tools():
    return Path(os.environ.get("IDF_TOOLS_PATH", r"C:\Espressif\tools"))


def _posix_idf_fallbacks():
    """(venv python, idf, activation script) from ~/.espressif EIM layout,
    newest version wins.  Empty Paths when nothing matches."""
    home = Path.home()
    idf = _newest(home.glob(".espressif/v*/esp-idf"))
    if not idf.parts:
        return (Path(), Path(), Path())
    return (_for_version(home.glob(".espressif/tools/python/*/venv/bin/python"), idf),
            idf,
            _for_version(home.glob(".espressif/tools/activate_idf_v*.sh"), idf))


def _win_idf_fallbacks():
    """Same for the Windows EIM layout, covering both install shapes:
    C:\\esp\\v<ver>\\esp-idf and C:\\Espressif\\frameworks\\esp-idf-v<ver>."""
    tools = _win_tools()
    idf = _newest(list(Path(r"C:\esp").glob("v*/esp-idf"))
                  + list(Path(r"C:\Espressif\frameworks").glob("esp-idf-v*")))
    if not idf.parts:
        return (Path(), Path(), Path())
    return (_for_version(tools.glob("python/*/venv/Scripts/python.exe"), idf),
            idf,
            _act_for(idf))


def _act_for(idf):
    """Windows activation script for an IDF install, from the EIM tools dir."""
    return _for_version(_win_tools().glob("Microsoft.v*.PowerShell_profile.ps1"), idf)


def _eim_install(meta):
    """(venv python, IDF_PATH, activation script) of the install EIM selected,
    else the first listed that is still on disk - an upgrade can leave the
    manifest naming a version whose directory was already replaced."""
    try:
        doc = json.loads(meta.read_text(encoding="utf-8-sig"))
        insts = doc.get("idfInstalled") or []
    except (OSError, ValueError, AttributeError):
        return None
    selected = [i for i in insts if i.get("id") == doc.get("idfSelectedId")]
    for inst in selected + insts:
        py, idf = Path(inst.get("python") or ""), Path(inst.get("path") or "")
        if py.is_file() and idf.is_dir():
            act = Path(inst.get("activationScript") or "")
            # A stale manifest script name still points at the right version
            # dir; fall back to the on-disk profile when EIM moved it.
            if os.name == "nt" and not act.is_file():
                act = _act_for(idf)
            return (py, idf, act)
    return None


def find_idf_env():
    """(venv python, IDF_PATH, activation script) from EIM metadata, env vars,
    then a newest-installed-wins scan of the per-OS EIM layout."""
    for meta in eim_json_candidates():
        if meta.is_file():
            hit = _eim_install(meta)
            if hit:
                return hit
            break
    venv, idf = os.environ.get("IDF_PYTHON_ENV_PATH"), os.environ.get("IDF_PATH")
    if venv and idf:
        py = Path(venv) / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
        # POSIX needs no script (the env is already active); Windows gets the
        # toolchain PATH from it, so pick the one matching this IDF version.
        return (py, Path(idf), _act_for(Path(idf)) if os.name == "nt" else Path())
    if os.name == "nt":
        return _win_idf_fallbacks()
    return _posix_idf_fallbacks()


try:
    import click
except ImportError:
    if os.environ.get("C6_FLASH_REEXEC") != "1":
        py, _, _ = find_idf_env()
        if py.exists():
            print(f"[c6] click missing - relaunching under IDF venv: {py}")
            env = dict(os.environ, C6_FLASH_REEXEC="1")
            os.execve(str(py), [str(py), __file__, *sys.argv[1:]], env)
    sys.exit("click is required: pip install click (or use the IDF venv python)")


def run(cmd, **kw):
    print("[c6]", " ".join(str(c) for c in cmd))
    return subprocess.run([str(c) for c in cmd], **kw)


def list_serial_ports():
    """[(device, vid, pid, desc)] - pyserial when available, else a WMI query
    on Windows, else /dev globbing."""
    try:
        from serial.tools import list_ports
    except ImportError:
        pass
    else:
        return [(p.device, p.vid, p.pid, p.description)
                for p in list_ports.comports()]
    if os.name == "nt":
        # Force UTF-8 on the PowerShell side too: its default pipe encoding
        # follows the console codepage (GBK on zh-CN boxes) and text=True
        # would otherwise raise UnicodeDecodeError.
        ps = ("[Console]::OutputEncoding = [Text.Encoding]::UTF8; "
              "Get-CimInstance Win32_PnPEntity -Filter \"Name LIKE '%COM%'\" | "
              "ForEach-Object { $_.Name + '|' + $_.PNPDeviceID }")
        try:
            r = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                               capture_output=True, text=True, encoding="utf-8",
                               errors="replace", timeout=30)
        except (OSError, subprocess.TimeoutExpired):
            return []
        out = []
        for line in r.stdout.splitlines():
            # FTDI-based drivers join VID/PID with '+' (FTDIBUS\...), the
            # rest with '&' (USB\VID_...) - accept both.
            m = re.search(r"\((COM\d+)\)\s*\|.*?VID_([0-9A-Fa-f]{4})"
                          r"[&+]PID_([0-9A-Fa-f]{4})", line)
            if m:
                desc = line.split("(", 1)[0].strip()
                out.append((m.group(1), int(m.group(2), 16),
                            int(m.group(3), 16), desc))
        return out
    import glob
    devs = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")
                  + glob.glob("/dev/cu.usb*"))
    return [(d, None, None, "") for d in devs]


def detect_port():
    """(device, desc) of the board's port or None.  Espressif VID wins over
    bridge chips; ties break by lowest COM number."""
    ports = list_serial_ports()

    def com_no(p):
        return int(re.sub(r"\D", "", p[0]) or 0)

    for vids in ((ESPRESSIF_VID,), BRIDGE_VIDS):
        hits = sorted((p for p in ports if p[1] in vids), key=com_no)
        if hits:
            return hits[0][0], hits[0][3]
    if len(ports) == 1:
        return ports[0][0], ports[0][3]
    return None


def resolve_port(port):
    """The explicit -p value, else auto-detect the board."""
    if port:
        return port
    hit = detect_port()
    if hit:
        dev, desc = hit
        # localized Windows descriptions ("USB 串行设备") garble across
        # codepages - keep them only when plain ASCII
        print(f"[c6] port {dev} ({desc if desc.isascii() else 'usb serial'})")
        return dev
    seen = ", ".join(sorted(p[0] for p in list_serial_ports())) or "none"
    hint = "-p COMx" if os.name == "nt" else "-p /dev/cu.usbmodemXXXX"
    sys.exit(f"[c6] board port not found (ports: {seen}) - plug in the board "
             f"(USB cable must be a data cable) or pass {hint}")


def ensure_port_free(port):
    """Fail early with a readable message when a monitor / log-capture
    process holds the port - esptool's own error is cryptic."""
    try:
        import serial
    except ImportError:
        return
    try:
        serial.Serial(port).close()
    except serial.SerialException as e:
        msg = str(e)
        if "PermissionError" in msg or "denied" in msg.lower():
            sys.exit(f"[c6] {port} is held by another process (serial "
                     "monitor / log capture) - close it and retry")
        # missing device etc.: let esptool report the details


def run_idf(*args):
    """Run one idf.py command in the discovered IDF environment and return its
    exit code.  The toolchain PATH lives in the EIM activation script on both
    platforms: Windows runs the command inside an activated PowerShell; on
    macOS/Linux that script is sourced in sh only for its exports - it also
    defines idf.py as a shell *function*, which /bin/sh (POSIX mode) rejects
    because of the dot in the name, so idf.py is called explicitly there."""
    py, idf, act = find_idf_env()
    if not (idf / "tools" / "idf.py").is_file():
        sys.exit(f"IDF not found at {idf}")
    joined = " ".join(str(a) for a in args)
    if os.name == "nt" and act.is_file():
        ps = (f"Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue; "
              f". '{act}'; Set-Location '{PROJECT}'; idf.py {joined}; "
              f"exit $LASTEXITCODE")
        return run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                    "-Command", ps]).returncode
    if act.is_file():
        return run(["sh", "-c", f". '{act}' >/dev/null 2>&1; cd '{PROJECT}' && "
                               f"'{py}' '{idf}/tools/idf.py' {joined}"]).returncode
    env = dict(os.environ)
    env.pop("MSYSTEM", None)   # idf.py refuses to run under MSys
    print("[c6] EIM activation script not found - relying on IDF_PATH/PATH")
    return run([py, idf / "tools" / "idf.py", *args],
               cwd=PROJECT, env=env).returncode


def stale_build_idf(idf):
    """The IDF install build/config.env says the build dir was configured
    with, or None when there is no build dir or it already matches `idf`.
    An ESP-IDF upgrade leaves build/ pinned to the old install, and idf.py
    then refuses every command until it is reconfigured."""
    try:
        recorded = Path(json.loads(
            (BUILD / "config.env").read_text(encoding="utf-8-sig")
        )["IDF_PATH"])
    except (OSError, ValueError, KeyError):
        return None
    return None if os.path.normcase(str(recorded)) == os.path.normcase(str(idf)) \
        else recorded


def run_build():
    """Compile via idf.py, reconfiguring first when an IDF upgrade made the
    existing build dir unusable (stale_build_idf)."""
    old = stale_build_idf(find_idf_env()[1])
    if old is not None:
        print(f"[c6] build dir was configured with {old} - running idf.py "
              "fullclean before the build")
        if run_idf("fullclean"):
            sys.exit("fullclean failed")
    if run_idf("build"):
        sys.exit("build failed")


def flash_assets(port):
    """Repack assets_src/ and write the assets partition (parttool)."""
    py, idf, _ = find_idf_env()
    r = run([py, PROJECT / "tools" / "build_assets.py", "assets_src", BUILD / "assets.bin"],
            cwd=PROJECT)
    if r.returncode != 0:
        sys.exit("assets packing failed")
    parttool = idf / "components" / "partition_table" / "parttool.py"
    ensure_port_free(port)
    r = run([py, parttool, "-p", port, "write_partition",
             "--partition-name=assets", "--input", BUILD / "assets.bin"], cwd=BUILD)
    if r.returncode != 0:
        sys.exit("parttool write failed")


def flash_full(port):
    """Write the four flashing images exactly like `idf.py flash` does.

    build/flash_args carries the flash mode/freq/size line plus the
    offset/image pairs with build-relative paths, hence cwd=BUILD.
    """
    py, _, _ = find_idf_env()
    if not (BUILD / "esp32c6_car.bin").exists():
        sys.exit("build/esp32c6_car.bin missing - run `python flash.py build` first "
                 "or pass -b")
    ensure_port_free(port)
    run([py, "-m", "esptool", "--chip", CHIP, "-p", port, "-b", BAUD,
         "--before=default-reset", "--after=hard-reset",
         "write-flash", "@flash_args"], cwd=BUILD)


def do_monitor(port):
    try:
        import serial
    except ImportError:
        print("pyserial not available for monitor - run inside the IDF venv "
              "or use `idf.py -p %s monitor`" % port)
        return
    try:
        ser = serial.Serial(port, 115200, timeout=0.5)
    except serial.SerialException as e:
        sys.exit(f"[c6] cannot open {port}: {e}")
    with ser:
        print(f"--- monitor {port} (Ctrl+C to exit) ---")
        try:
            while True:
                data = ser.read(4096)
                if data:
                    sys.stdout.write(data.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
        except KeyboardInterrupt:
            pass


def common_opts(f):
    f = click.option("-p", "--port", default=None,
                     help="Serial port (auto-detected when omitted).")(f)
    f = click.option("-m", "--monitor", is_flag=True,
                     help="Open the serial monitor after flashing.")(f)
    f = click.option("-b", "--build", is_flag=True,
                     help="Compile (idf.py build) before flashing.")(f)
    return f


@click.group(invoke_without_command=True,
             context_settings={"help_option_names": ["-h", "--help"]})
@click.pass_context
def cli(ctx):
    """ESP32-C6 flashing helper (esp32c6_car).  No BOOT button needed - the board
    auto-resets into download mode and again after flashing."""
    if ctx.invoked_subcommand is None:
        ctx.invoke(full)


@cli.command()
def build():
    """Compile the firmware (idf.py build)."""
    print("[c6] mode=build")
    run_build()


@cli.command()
@common_opts
def full(port, monitor, build):
    """Flash bootloader + partition table + otadata + firmware."""
    port = resolve_port(port)
    print(f"[c6] mode=full port={port}")
    if build or not (BUILD / "esp32c6_car.bin").exists():
        run_build()
    flash_full(port)
    if monitor:
        do_monitor(port)


@cli.command()
@common_opts
def assets(port, monitor, build):
    """Repack assets_src/ and flash the control page only (firmware untouched)."""
    port = resolve_port(port)
    print(f"[c6] mode=assets port={port}")
    flash_assets(port)
    if monitor:
        do_monitor(port)


@cli.command(name="all")
@common_opts
def all_cmd(port, monitor, build):
    """Flash assets first, then full firmware (one power cycle for the user)."""
    port = resolve_port(port)
    print(f"[c6] mode=all port={port}")
    flash_assets(port)
    if build or not (BUILD / "esp32c6_car.bin").exists():
        run_build()
    flash_full(port)
    if monitor:
        do_monitor(port)


@cli.command()
@click.argument("port", required=False)
def mon(port):
    """Serial monitor only (115200, Ctrl+C to exit)."""
    do_monitor(resolve_port(port))


if __name__ == "__main__":
    cli()
