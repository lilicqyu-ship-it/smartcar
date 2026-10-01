#!/usr/bin/env python3
"""fw.py - smartcar 四工程固件统一管理：编译 / 烧录 / 归档（纯标准库）。

本脚本是 meta 仓库的固件总入口，不重复实现各工程的构建烧录，只做发现、
委托与归档：

  esp32c6_car     build/flash 委托 esp32c6_car/flash.py（EIM 环境自动发现）
  smartcar_remote 自行发现 EIM 环境后调用 idf.py build / -p PORT flash
  tc275_sbl       委托 tools/build_sbl.sh（完整版 TASKING 命令行编译）、
                  tools/flash.py（AURIXFlasher CLI 烧录）
  tc275_car       解析 ADS 生成的 subdir.mk 提取编译/链接命令，用完整版
                  TASKING 命令行增量重编；烧录同样走 tc275_sbl/tools/flash.py

用法（或 just fw-* 配方）:
  python firmware/fw.py list                      各工程构建产物 / 归档状态一览
  python firmware/fw.py build <project>           编译一个工程
  python firmware/fw.py flash <project> [args…]   烧录（余参透传给各工程入口）
  python firmware/fw.py collect [project]         归档产物到 firmware/dist/<工程>/
  python firmware/fw.py factory [--flash]         SBL+App 出厂整包（合成，可选烧录）
  python firmware/fw.py clean [--yes]             清空 firmware/dist/

  project ∈ esp32c6_car | smartcar_remote | tc275_car | tc275_sbl
  别名: c6 / remote / app / sbl / myCarSbl

环境覆盖（自动探测失败时）: FW_TASKING / FW_SH / FW_IDF_PROFILE
"""
import argparse
import glob
import json
import os
import re
import shlex
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

# TC275 侧固定约定（与各仓库 README 一致）
TASKING_GLOB = r"C:/Program Files/TASKING/TriCore */ctc/bin"
TC275_APP_BUILD = "TriCore Debug (TASKING)"
SBL_BUILD = "Debug"


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

def find_tasking():
    v = os.environ.get("FW_TASKING")
    if v:
        p = Path(v)
        if p.exists():
            return p
        die(f"FW_TASKING={v} 指向的 TASKING ctc/bin 不存在")
    hits = sorted(glob.glob(TASKING_GLOB))
    return Path(hits[-1]) if hits else None


def tasking_env():
    """带完整版 TASKING ctc/bin 前置的 PATH 环境。"""
    tctc = find_tasking()
    if not tctc:
        die("未找到完整版 TASKING（如 v6.3r1，装在 C:/Program Files/TASKING/）。"
            "ADS 的内置 TASKING 许可禁止 IDE 外运行；或用环境变量 "
            "FW_TASKING=<ctc/bin 目录> 指定。")
    env = dict(os.environ)
    env["PATH"] = str(tctc) + os.pathsep + env.get("PATH", "")
    return env


def find_sh():
    """sh.exe：PATH 优先，其次 Git 安装常见位置（脚本里可能没挂 PATH）。"""
    p = shutil.which("sh") or shutil.which("bash")
    if p:
        return p
    for c in (r"C:/Program Files/Git/bin/sh.exe", r"C:/Program Files/Git/usr/bin/sh.exe"):
        if Path(c).exists():
            return c
    return None


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


def tc275_artifacts(repo, build_dir):
    """ADS/TASKING 构建目录里的 hex/elf/map（多个时取最新）。

    factory_full.hex 是 SBL+App 的合成整包，不能顶替工程自身的 hex，
    从"最新 hex"候选中排除、存在时单独附带。"""
    b = repo / build_dir
    out = []
    for pat in ("*.hex", "*.elf", "*.map"):
        hits = sorted((p for p in b.glob(pat) if p.name != "factory_full.hex"),
                      key=lambda p: p.stat().st_mtime)
        if hits:
            out.append(hits[-1])
    if (b / "factory_full.hex").exists():
        out.append(b / "factory_full.hex")
    return out


def sbl_hex():
    return sbl_dir() / SBL_BUILD / "tc275_sbl.hex"


def app_hex():
    """tc275_car 的 App 槽 A 镜像；工程名历史上叫 myCar，兼容两者。"""
    b = ROOT / "tc275_car" / TC275_APP_BUILD
    for name in ("tc275_car.hex", "myCar.hex"):
        if (b / name).exists():
            return b / name
    hits = sorted(b.glob("*.hex"), key=lambda p: p.stat().st_mtime)
    return hits[-1] if hits else None


ARTIFACTS = {
    "esp32c6_car": lambda: esp_artifacts(ROOT / "esp32c6_car"),
    "smartcar_remote": lambda: esp_artifacts(ROOT / "smartcar_remote"),
    "tc275_car": lambda: tc275_artifacts(ROOT / "tc275_car", TC275_APP_BUILD),
    "tc275_sbl": lambda: tc275_artifacts(sbl_dir(), SBL_BUILD),
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
    """tools/build_sbl.sh；带 App hex 参数时顺带产出 factory_full.hex。"""
    sh = find_sh()
    if not sh:
        die("找不到 sh/bash（Windows 需安装 Git）")
    cmd = [sh, "tools/build_sbl.sh"]
    extra = [a for a in args if not a.startswith("-")]
    if extra:
        cmd.append(extra[0])
    return run(cmd, cwd=sbl_dir())


def build_tc275_car(_args):
    """tc275_car 命令行增量重编。

    ADS 生成的 makefile 把目标/依赖名整个用双引号括起来，GNU make 与
    TASKING mktc 都无法直接驱动（引号被当文件名字面量）；这里改为解析
    build 目录里的 subdir.mk，取出每个 .c 的 cctc 命令与链接命令，用完整
    版 TASKING 直接执行——编译标志取自 ADS 生成的文件本身，与 IDE 零漂移。
    """
    repo = ROOT / "tc275_car"
    b = repo / TC275_APP_BUILD
    if not (b / "makefile").exists():
        die(f"{b} 下没有 ADS 生成的构建文件——先在 AURIX Development Studio 里"
            " import 本工程并构建一次（生成 makefile/subdir.mk/.opt），之后命令行"
            "即可增量重编。")
    env = tasking_env()

    # 每条编译规则: "X.src": "../S.c" + 紧随的 tab 缩进 cctc 命令行
    rule_re = re.compile(r'^"([^"]+)"\s*:\s*"(\.\./[^"]+)"')

    def instantiate(recipe, out, src, stem):
        r = recipe.replace("$@", out).replace("$<", src).replace("$*", stem)
        return shlex.split(r, posix=True)

    jobs = []  # (repo 相对源文件, obj 相对构建目录, 编译 argv, 汇编 argv)
    for mk in sorted(b.rglob("subdir.mk")):
        lines = mk.read_text(encoding="utf-8", errors="replace").splitlines()
        for i, line in enumerate(lines):
            m = rule_re.match(line)
            if not (m and m.group(1).endswith(".src") and m.group(2).endswith(".c")):
                continue
            if i + 1 >= len(lines) or not lines[i + 1].startswith("\t"):
                continue
            target, src = m.group(1), m.group(2)
            obj = target[:-4] + ".o"
            cc = instantiate(lines[i + 1].strip(), target, src, target[:-4])
            asm = None  # .o 规则（astc）在编译规则后几行内
            for j in range(i + 2, min(i + 8, len(lines))):
                if lines[j].startswith("\t") and "astc" in lines[j]:
                    asm = instantiate(lines[j].strip(), obj, target, obj[:-2])
                    break
            jobs.append((src, obj, cc, asm))
    if not jobs:
        die("未从 subdir.mk 解析到任何编译规则（构建目录可能损坏，请在 ADS 里"
            " Clean 后重新构建一次）")

    # 链接命令：makefile 里 tab 缩进的 "@argfile <名> <标志...> $(OBJS)" 一行
    mf = (b / "makefile").read_text(encoding="utf-8", errors="replace")
    lm = re.search(r'^\s*@argfile\s+(\S+)\s+(.+)\$\(OBJS\)\s*$', mf, re.M)
    if not lm:
        die("makefile 里找不到链接命令（@argfile 行）")
    link_flags = shlex.split(lm.group(2), posix=True)

    stale = []
    for j in jobs:
        src_file = repo / j[0][3:]
        if not src_file.exists():
            die(f"subdir.mk 引用的源文件不存在: {j[0]}（构建目录与源码不同步，"
                "在 ADS 里重新构建一次以刷新 makefile）")
        if not (b / j[1]).exists() or src_file.stat().st_mtime > (b / j[1]).stat().st_mtime:
            stale.append(j)
    info(f"{len(jobs)} 个编译单元，{len(stale)} 个需要重编")
    for src, _obj, cc, asm in stale:
        print(f"  cctc {src}")
        if run(cc, cwd=b, env=env):
            return 1
        if asm and run(asm, cwd=b, env=env):
            return 1

    elf = next((m.group(1) for f in link_flags
                if (m := re.search(r'-o"?(.+\.elf)"?$', f))), None)
    objs = [j[1] for j in jobs]
    newest = max((b / o).stat().st_mtime for o in objs)
    if stale or not (b / elf).exists() or (b / elf).stat().st_mtime < newest:
        info(f"链接 {elf}（{len(objs)} 个对象）")
        if run(["cctc", *link_flags, *objs], cwd=b, env=env):
            return 1
        run(["elfsize", elf], cwd=b, env=env)
    else:
        info(f"{elf} 已是最新，跳过链接")
    return 0


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
    if rc == 0 and a.collect:
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
    return collect_one("tc275_sbl") if a.collect else 0


# ---------------------------------------------------------------- 归档 / 状态

def collect_one(p):
    """把工程产物复制到 dist/<工程>/<时间戳-g哈希>/ 并写 manifest.json。"""
    arts = ARTIFACTS[p]()
    if not arts:
        die(f"{p} 没有可归档的构建产物（先 fw.py build {p}）")
    repo = repo_dir(p)
    h, branch, dirty = git_info(repo)
    stamp = time.strftime("%Y%m%d-%H%M%S") + f"-g{h}" + ("-dirty" if dirty else "")
    dest = DIST / p / stamp
    if dest.exists():
        dest = DIST / p / (stamp + f"-{int(time.time()) % 1000}")
    dest.mkdir(parents=True)
    manifest = {"project": p, "collected_at": time.strftime("%Y-%m-%d %H:%M:%S"),
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
    b.add_argument("--collect", action="store_true", help="构建成功后顺带归档产物")
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
    fa.add_argument("--collect", action="store_true", help="顺带归档 SBL 侧产物")

    cl = sub.add_parser("clean", help="清空 firmware/dist/")
    cl.add_argument("--yes", action="store_true")

    a = ap.parse_args(argv)
    return {"list": cmd_list, "build": cmd_build, "flash": cmd_flash,
            "collect": cmd_collect, "factory": cmd_factory, "clean": cmd_clean}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
