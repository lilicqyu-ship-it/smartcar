#!/usr/bin/env python3
"""fw.py - smartcar 四工程固件统一管理：编译 / 烧录 / 归档（纯标准库）。

本脚本是 meta 仓库的固件总入口，不重复实现各工程的构建烧录，只做发现、
委托与归档：

  esp32c6_car     build/flash 委托 esp32c6_car/flash.py（EIM 环境自动发现）
  smartcar_remote 自行发现 EIM 环境后调用 idf.py build / -p PORT flash
  tc275_sbl       python -m SCons（解析 .cproject 与 ADS 同源；产物名带版本）、
                  tools/flash.py（AURIXFlasher CLI 烧录）
  tc275_car       python -m SCons（同上）；烧录走 tc275_sbl/tools/flash.py

用法（或 just fw-* 配方）:
  python firmware/fw.py list                      各工程构建产物 / 归档状态一览
  python firmware/fw.py build <project>           编译一个工程（成功即自动归档 dist）
  python firmware/fw.py flash <project> [args…]   烧录（余参透传给各工程入口）
  python firmware/fw.py collect [project]         补充归档到 firmware/dist/<工程>/
  python firmware/fw.py factory [--flash]         SBL+App 出厂整包（合成，可选烧录）
  python firmware/fw.py clean [--yes]             清空 firmware/dist/

  project ∈ esp32c6_car | smartcar_remote | tc275_car | tc275_sbl
  别名: c6 / remote / app / sbl / myCarSbl

环境覆盖（自动探测失败时）: TASKING_TRICORE_HOME（SCons 工具链发现）/ FW_IDF_PROFILE
"""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "firmware" / "dist"
PROJECTS = ("esp32c6_car", "smartcar_remote", "tc275_car", "tc275_sbl")
ALIASES = {"c6": "esp32c6_car", "remote": "smartcar_remote",
           "app": "tc275_car", "sbl": "tc275_sbl", "myCarSbl": "tc275_sbl"}

# TC275 侧固定约定：命令行构建 = SCons（build/tasking-<cfg>/，产物名带版本）；
# ADS IDE 构建目录作为产物发现的回退（与各仓库 README 一致）。
ADS_APP_BUILD = "TriCore Debug (TASKING)"
ADS_SBL_BUILD = "Debug"


def die(msg, code=1):
    print(f"[fw] ERROR: {msg}", file=sys.stderr)
    sys.exit(code)


def info(msg):
    print(f"[fw] {msg}")


def run(cmd, cwd=None, env=None, shell=False):
    """打印并执行一条命令，返回 returncode（不抛异常）。"""
    if not shell and env and env.get("PATH"):
        # Windows 的 CreateProcess 按父进程 PATH 解析可执行文件，不看传入
        # env 的 PATH —— 先用目标环境的 PATH 把命令名解析成绝对路径。
        exe = shutil.which(str(cmd[0]), path=env["PATH"])
        if exe:
            cmd = [exe, *cmd[1:]]
    shown = cmd if isinstance(cmd, str) else " ".join(f'"{c}"' if " " in str(c) else str(c) for c in cmd)
    print(f">> {shown}")
    return subprocess.run(cmd if not isinstance(cmd, str) else cmd, cwd=cwd, env=env,
                          shell=shell).returncode


# ---------------------------------------------------------------- 工程定位

def sbl_dir():
    """SBL 子模块的挂载目录：当前叫 myCarSbl，改名 tc275_sbl 后自动跟上。"""
    for d in ("tc275_sbl", "myCarSbl"):
        if (ROOT / d).exists():
            return ROOT / d
    return ROOT / "myCarSbl"


def repo_dir(p):
    return sbl_dir() if p == "tc275_sbl" else ROOT / p


def require_project(p):
    p = ALIASES.get(p, p)
    if p not in PROJECTS:
        die(f"未知工程 {p!r}，可选: {' | '.join(PROJECTS)}")
    return p


# ---------------------------------------------------------------- 工具发现

# TASKING 工具链发现由 SCons 的 aurix_tasking.find_tasking() 负责：
# TASKING_TRICORE_HOME / TASKING_HOME 环境变量，或 Program Files 常见位置。


def git_info(repo):
    """(短哈希, 分支, 是否脏)；子仓库缺失时返回占位。"""
    def g(*a):
        return subprocess.run(["git", "-C", str(repo), *a], capture_output=True,
                              text=True, encoding="utf-8", errors="replace").stdout.strip()
    h = g("rev-parse", "--short", "HEAD")
    if not h:
        return ("-", "(n/a)", False)
    return (h, g("branch", "--show-current") or "(detached)",
            bool(g("status", "--porcelain")))


# ---------------------------------------------------------------- 产物发现

def esp_artifacts(repo):
    """ESP-IDF build/ 里值得归档的产物：以 flasher_args.json 为真源。"""
    b = repo / "build"
    out = []
    fa = b / "flasher_args.json"
    if fa.exists():
        try:
            j = json.loads(fa.read_text(encoding="utf-8"))
            for key in ("app_file", "bootloader_file", "partition_table_file", "ota_data_file"):
                f = j.get(key)
                if f:
                    out.append(b / f)
        except (json.JSONDecodeError, OSError):
            pass
        out.append(fa)
    out += sorted(b.glob("*.elf")) + sorted(b.glob("*.map")) + [b / "assets.bin"]
    return [p for p in out if p.exists()]


def tc275_artifacts(repo, extra_dirs=()):
    """TASKING 构建产物：只取"最新构建"所在目录（按最新 hex 的 mtime 判定）。

    候选目录 = SCons 的 build/tasking-* + ADS 目录（extra_dirs）；单目录
    取整，避免新旧构建（如 release 半成品、IDE 残留）混进同一份归档。
    factory_full.hex 是 SBL+App 合成整包，不能顶替工程自身的 hex，从
    "最新 hex"候选中排除、存在时单独附带。"""
    cand = []
    for d in sorted((repo / "build").glob("tasking-*")) + [Path(d) for d in extra_dirs]:
        hexes = list(d.glob("*.hex")) if d.is_dir() else []
        if hexes:
            cand.append((max(p.stat().st_mtime for p in hexes), d))
    if not cand:
        return []
    b = max(cand, key=lambda t: t[0])[1]
    out = []
    for pat in ("*.hex", "*.elf", "*.map"):
        hits = sorted((p for p in b.glob(pat) if p.name != "factory_full.hex"),
                      key=lambda p: p.stat().st_mtime)
        if hits:
            out.append(hits[-1])
    if (b / "factory_full.hex").exists():
        out.append(b / "factory_full.hex")
    return out


def _newest_hex(patterns):
    for pat in patterns:
        hits = sorted(glob.glob(str(pat)), key=os.path.getmtime)
        if hits:
            return Path(hits[-1])
    return None


def sbl_hex():
    """SCons 版本化产物优先（tc275_sbl_vX.Y.Z.hex），ADS Debug/ 回退。"""
    d = sbl_dir()
    return _newest_hex([d / "build" / "tasking-*" / "tc275_sbl_v*.hex",
                        d / "build" / "tasking-*" / "tc275_sbl.hex",
                        d / ADS_SBL_BUILD / "tc275_sbl.hex"])


def app_hex():
    """tc275_car 的镜像：SCons 版本化产物优先，ADS 目录回退（myCar 为旧名兼容）。"""
    r = ROOT / "tc275_car"
    return _newest_hex([r / "build" / "tasking-*" / "tc275_car_v*.hex",
                        r / "build" / "tasking-*" / "tc275_car.hex",
                        r / ADS_APP_BUILD / "tc275_car.hex",
                        r / ADS_APP_BUILD / "myCar.hex"])


ARTIFACTS = {
    "esp32c6_car": lambda: esp_artifacts(ROOT / "esp32c6_car"),
    "smartcar_remote": lambda: esp_artifacts(ROOT / "smartcar_remote"),
    "tc275_car": lambda: tc275_artifacts(ROOT / "tc275_car", [ROOT / "tc275_car" / ADS_APP_BUILD]),
    "tc275_sbl": lambda: tc275_artifacts(sbl_dir(), [sbl_dir() / ADS_SBL_BUILD]),
}


# ---------------------------------------------------------------- 各工程实现

def build_esp32c6_car(_args):
    """委托项目自带的 flash.py（EIM 自动发现 IDF 环境，含 assets 打包）。"""
    return run([sys.executable, ROOT / "esp32c6_car" / "flash.py", "build"])


def find_idf_env():
    """(venv python, IDF_PATH, 激活脚本) — 与 esp32c6_car/flash.py 同一套发现逻辑。"""
    cands = [Path(r"C:\Espressif\tools\eim_idf.json")] if sys.platform == "win32" \
        else sorted(Path.home().glob(".espressif/tools/eim_idf.json"))
    for meta in cands:
        if meta.exists():
            try:
                inst = json.loads(meta.read_text(encoding="utf-8"))["idfInstalled"][0]
                return Path(inst["python"]), Path(inst["path"]), Path(inst.get("activationScript", ""))
            except (json.JSONDecodeError, KeyError, IndexError, TypeError):
                pass
    home = Path.home()
    if sys.platform == "win32":
        return (Path(r"C:\Espressif\tools\python\v6.1-beta1\venv\Scripts\python.exe"),
                Path(r"C:\esp\v6.1-beta1\esp-idf"),
                Path(os.environ.get("FW_IDF_PROFILE",
                                    r"C:\Espressif\tools\Microsoft.v6.1-beta1.PowerShell_profile.ps1")))
    pys = sorted(home.glob(".espressif/tools/python/*/venv/bin/python"))
    acts = sorted(home.glob(".espressif/tools/activate_idf_v*.sh"))
    return (pys[-1] if pys else Path(), Path(), acts[-1] if acts else Path())


def esp_idf_cmd(repo, idf_args):
    """在激活的 ESP-IDF 环境里执行一条 idf.py 命令，返回 returncode。"""
    py, idf_path, act = find_idf_env()
    if sys.platform == "win32":
        # flash.bat 同款：dot-source EIM 的 PowerShell profile；MSYSTEM 会干扰
        # idf.py（Git Bash 里发起时），先清掉。
        profile = os.environ.get("FW_IDF_PROFILE", str(act))
        if not Path(profile).exists():
            die(f"ESP-IDF 激活脚本不存在: {profile}（用 FW_IDF_PROFILE 覆盖，"
                f"或先装 EIM/ESP-IDF v6.1）")
        env = {k: v for k, v in os.environ.items() if k != "MSYSTEM"}
        # 末尾 exit $LASTEXITCODE：powershell 不回传原生命令退出码，不加会假成功
        ps = (f". '{profile}'; Set-Location '{repo}'; idf.py "
              + " ".join(f"'{a}'" for a in idf_args)
              + "; exit $LASTEXITCODE")
        return run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                    "-Command", ps], env=env)
    if not (py.exists() and idf_path.exists() and act.exists()):
        die("未从 ~/.espressif 发现 EIM 安装的 ESP-IDF（activate_idf_v*.sh），"
            "检查安装或用 IDF_PATH/IDF_PYTHON_ENV_PATH 环境变量")
    env = dict(os.environ, IDF_PATH=str(idf_path))
    sh = f"source '{act}' && cd '{repo}' && idf.py " + " ".join(f"'{a}'" for a in idf_args)
    return run(["bash", "-c", sh], env=env)


def detect_serial_port():
    """按 USB VID 找一块 ESP 板（Espressif 303A / CP210x / CH34x）。"""
    try:
        from serial.tools import list_ports
        vids = {0x303A, 0x10C4, 0x1A86}
        for p in list_ports.comports():
            if p.vid in vids:
                return p.device
    except ImportError:
        pass
    if sys.platform == "win32":
        ps = ("Get-CimInstance Win32_SerialPort -Filter \"PNPDeviceID LIKE "
              "'%VID_303A%' OR PNPDeviceID LIKE '%VID_10C4%' OR PNPDeviceID "
              "LIKE '%VID_1A86%'\" | Select-Object -First 1 -ExpandProperty DeviceID")
        r = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                           capture_output=True, text=True, timeout=30)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    else:
        for pat in ("/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/ttyUSB*", "/dev/ttyACM*"):
            hits = glob.glob(pat)
            if hits:
                return sorted(hits)[0]
    return None


def build_smartcar_remote(_args):
    return esp_idf_cmd(ROOT / "smartcar_remote", ["build"])


def build_tc275_sbl(args):
    """python -m SCons（tc275_sbl，解析 .cproject 与 ADS 同源）；余参透传。"""
    return run([sys.executable, "-m", "SCons", *args], cwd=sbl_dir())


def build_tc275_car(args):
    """python -m SCons（tc275_car，解析 .cproject 与 ADS 同源）；余参透传。"""
    return run([sys.executable, "-m", "SCons", *args], cwd=ROOT / "tc275_car")


BUILDERS = {
    "esp32c6_car": build_esp32c6_car,
    "smartcar_remote": build_smartcar_remote,
    "tc275_car": build_tc275_car,
    "tc275_sbl": build_tc275_sbl,
}


def flash_esp32c6_car(args):
    """全参数透传 flash.py（full/assets/all/mon、-p COMx、-b -m）。"""
    args = args or ["full"]
    return run([sys.executable, ROOT / "esp32c6_car" / "flash.py", *args])


def flash_smartcar_remote(args):
    """idf.py [-p PORT] flash [monitor]；未给 -p 时按 USB VID 自动识别。"""
    idf_args = []
    rest = list(args)
    for flag in ("-p", "--port"):
        if flag in rest:
            i = rest.index(flag)
            if i + 1 >= len(rest):
                die(f"{flag} 后缺串口号（如 {flag} COM7）")
            idf_args += ["-p", rest[i + 1]]
            del rest[i:i + 2]
            break
    else:
        port = detect_serial_port()
        if not port:
            die("未识别到遥控器串口（Espressif/CP210x/CH34x VID）；插好板子"
                "或显式指定: fw.py flash smartcar_remote -p COM7")
        idf_args += ["-p", port]
        info(f"自动识别串口: {port}")
    idf_args += ["flash"] + rest
    return esp_idf_cmd(ROOT / "smartcar_remote", idf_args)


def flash_tc275(hex_path, args):
    """走 tc275_sbl/tools/flash.py（AURIXFlasher CLI，需 DAS 服务在跑）。"""
    if not hex_path.exists():
        die(f"固件不存在: {hex_path}（先 fw.py build 对应工程）")
    flash_py = sbl_dir() / "tools" / "flash.py"
    return run([sys.executable, flash_py, "flash", str(hex_path), *args])


def cmd_build(a):
    p = require_project(a.project)
    rc = BUILDERS[p](a.args)
    if rc == 0 and not a.no_collect:
        rc = collect_one(p)
    if rc == 0:
        info(f"{p} 构建完成")
    return rc


def cmd_flash(a):
    p = require_project(a.project)
    if p == "esp32c6_car":
        return flash_esp32c6_car(a.args)
    if p == "smartcar_remote":
        return flash_smartcar_remote(a.args)
    if p == "tc275_car":
        hx = app_hex() or die("tc275_car 没有可烧的 hex（先 fw.py build tc275_car）")
        return flash_tc275(hx, a.args)
    return flash_tc275(sbl_hex(), a.args)


def cmd_factory(a):
    """出厂整包：SBL + App 槽 A。--flash 时用 flash.py factory 一步合成+烧录。"""
    info("factory = tc275_car(App 槽 A) + tc275_sbl(SBL)")
    rc = build_tc275_car([])
    if rc:
        return rc
    rc = build_tc275_sbl([])
    if rc:
        return rc
    hx = app_hex() or die("tc275_car 构建后仍找不到 hex")
    if a.flash:
        flash_py = sbl_dir() / "tools" / "flash.py"
        return run([sys.executable, flash_py, "factory", str(hx)])
    # 不烧录也要合成一份 factory_full.hex 供 collect / 调试器烧录
    out = sbl_hex().parent / "factory_full.hex"
    rc = run([sys.executable, sbl_dir() / "tools" / "merge_hex.py",
              str(out), str(sbl_hex()), str(hx)])
    if rc:
        return rc
    info(f"整包已合成: {out}（烧录: fw.py factory --flash）")
    return collect_one("tc275_sbl") if not a.no_collect else 0


# ---------------------------------------------------------------- 归档 / 状态

_FW_VER_RE = re.compile(r'^#define\s+APP_VERSION_STRING\s+"([^"]+)"', re.M)


def fw_version(p):
    """TC275 工程的固件版本（mw/app_version.h）；ESP 工程返回 None。"""
    h = (repo_dir(p) / "mw" / "app_version.h")
    if not h.exists():
        return None
    m = _FW_VER_RE.search(h.read_text(encoding="utf-8"))
    return m.group(1) if m else None


def collect_one(p):
    """把工程产物复制到 dist/<工程>/[v版本-]<时间戳-g哈希>/ 并写 manifest.json。

    归档目录即版本管理单元：TC275 工程目录名携带固件版本（mw/app_version.h），
    Git 信息入 manifest；固件镜像（hex/bin）+ manifest 入库，elf/map 留本地。
    """
    arts = ARTIFACTS[p]()
    if not arts:
        die(f"{p} 没有可归档的构建产物（先 fw.py build {p}）")
    repo = repo_dir(p)
    h, branch, dirty = git_info(repo)
    stamp = time.strftime("%Y%m%d-%H%M%S") + f"-g{h}" + ("-dirty" if dirty else "")
    ver = fw_version(p)
    if ver:
        stamp = f"v{ver}-" + stamp
    dest = DIST / p / stamp
    if dest.exists():
        dest = DIST / p / (stamp + f"-{int(time.time()) % 1000}")
    dest.mkdir(parents=True)
    manifest = {"project": p, "collected_at": time.strftime("%Y-%m-%d %H:%M:%S"),
                "fw_version": ver,
                "git": {"commit": h, "branch": branch, "dirty": dirty},
                "artifacts": []}
    for src in arts:
        shutil.copy2(src, dest / src.name)
        rel = src.relative_to(repo)
        manifest["artifacts"].append(
            {"name": src.name, "bytes": src.stat().st_size, "source": str(rel)})
        print(f"  {src.name:28s} {src.stat().st_size:>10,d} B   <- {rel}")
    (dest / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    info(f"{p} 已归档 -> {dest.relative_to(ROOT)}")
    return 0


def cmd_collect(a):
    ps = [a.project] if a.project else list(PROJECTS)
    rc = 0
    for p in ps:
        try:
            collect_one(require_project(p))
        except SystemExit as e:
            print(f"  （{p}: {e}）")
            rc = 1
    return rc


def cmd_list(_a):
    print(f"{'工程':<16} {'产物':<4} {'最近构建':<16} {'git':<12} {'最新归档'}")
    for p in PROJECTS:
        arts = ARTIFACTS[p]()
        when = time.strftime("%Y-%m-%d %H:%M", time.localtime(
            max(s.stat().st_mtime for s in arts))) if arts else "-"
        h, _branch, dirty = git_info(repo_dir(p))
        marks = [d.name for d in sorted((DIST / p).glob("*"), key=lambda d: d.name,
                                        reverse=True)] if (DIST / p).exists() else []
        n = len(arts)
        print(f"{p:<16} {str(n) + ' 个':<4} {when:<16} "
              f"{('@' + h + ('*' if dirty else '')):<12} {marks[0] if marks else '-'}")
    if not DIST.exists():
        print("（firmware/dist/ 尚无归档；fw.py collect 生成）")


def cmd_clean(a):
    if not DIST.exists():
        info("dist/ 为空")
        return 0
    if not a.yes:
        r = input(f"删除 {DIST} 全部归档? [y/N] ").strip().lower()
        if r != "y":
            info("已取消")
            return 0
    shutil.rmtree(DIST)
    info("firmware/dist/ 已清空")
    return 0


# ---------------------------------------------------------------- 入口

def main(argv):
    ap = argparse.ArgumentParser(prog="fw.py", description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="工程: " + " | ".join(PROJECTS)
                                        + "   别名: c6 / remote / app / sbl")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("list", help="各工程构建产物 / 归档状态一览")

    b = sub.add_parser("build", help="编译一个工程")
    b.add_argument("project")
    b.add_argument("--no-collect", action="store_true",
                   help="构建后不归档（默认成功即归档到 firmware/dist/）")
    b.add_argument("args", nargs="*", help="透传给各工程构建入口")

    f = sub.add_parser("flash", help="烧录（余参透传给各工程烧录入口）")
    f.add_argument("project")
    f.add_argument("args", nargs="*",
                   help="esp32c6_car: full|assets|all|mon -p COMx -m；remote: -p COMx monitor；"
                        "tc275_car/tc275_sbl: --id N --log x.xml 等 flash.py 参数")

    c = sub.add_parser("collect", help="归档产物到 firmware/dist/")
    c.add_argument("project", nargs="?", help="省略则四工程全归档（无产物的跳过并提示）")

    fa = sub.add_parser("factory", help="SBL+App 出厂整包（构建+合成，--flash 烧录）")
    fa.add_argument("--flash", action="store_true", help="合成后用 AURIXFlasher 整包烧录")
    fa.add_argument("--no-collect", action="store_true", help="不归档 SBL 侧产物")

    cl = sub.add_parser("clean", help="清空 firmware/dist/")
    cl.add_argument("--yes", action="store_true")

    a = ap.parse_args(argv)
    return {"list": cmd_list, "build": cmd_build, "flash": cmd_flash,
            "collect": cmd_collect, "factory": cmd_factory, "clean": cmd_clean}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
