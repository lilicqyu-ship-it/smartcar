#!/usr/bin/env python3
"""fw.py - smartcar 五工程固件统一管理：编译 / 烧录 / 归档（纯标准库）。

本脚本是 meta 仓库的固件总入口，不重复实现各工程的构建烧录，只做发现、
委托与归档：

  esp32c6_car     build/flash 委托 esp32c6_car/flash.py（EIM 环境自动发现）
  s3-gateway      build/flash 委托 s3-gateway/flash.py（C6 的 S3-CAM 替代固件，
                  同一套 click CLI：full/assets/all/mon -p PORT -b -m）
  smartcar_remote 自行发现 EIM 环境后调用 idf.py build / -p PORT flash
  tc275_sbl       python -m SCons（解析 .cproject 与 ADS 同源；产物名带版本）、
                  tools/flash.py（AURIXFlasher CLI 烧录）
  tc275_car       python -m SCons（同上）；烧录走 tc275_sbl/tools/flash.py

用法（或 just fw-* 配方）:
  python firmware/fw.py list                      各工程构建产物 / 归档状态一览
  python firmware/fw.py build <project>           编译一个工程（成功即自动归档 dist）
  python firmware/fw.py flash <project> [args…]   编译 + 烧录（--no-build 跳过编译，
                                                  直接烧最近构建产物；余参透传）
  python firmware/fw.py collect [project]         补充归档到 firmware/dist/<工程>/
  python firmware/fw.py factory [--flash]         SBL+App 出厂整包（合成，可选烧录）
  python firmware/fw.py ota <project> [options]   OTA：编译 + 打签包 -> SCFW 暂存进 S3
                                                  （遥控器 FIRMWARE 页点推送）；
                                                  --direct 跳过 S3 由 PC 直推；
                                                  --no-build / --file 跳过编译
  python firmware/fw.py clean [--yes]             清空 firmware/dist/

  project ∈ esp32c6_car | s3-gateway | smartcar_remote | tc275_car | tc275_sbl
  别名: c6 / gw-s3 / r-s3 / app / sbl

烧录认板：C6 / S3 网关 / S3 遥控器的 USB 描述符完全相同（303A:1001），原生 USB 只
有序列号（= 芯片 MAC）能区分物理板子；未给 -p 时读一次 flash 里的 esp_app_desc_t
认出工程（该板复位一次），按序列号缓存于 ~/.cache/smartcar-fw/board_projects.json，
删掉该文件即重新认板（跨板复用固件后必须删，或直接用 -p 指定）。

环境覆盖（自动探测失败时）: TASKING_TRICORE_HOME（SCons 工具链发现）/ FW_IDF_PROFILE
"""
import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "firmware" / "dist"
PROJECTS = ("esp32c6_car", "s3-gateway", "smartcar_remote", "tc275_car", "tc275_sbl")
ALIASES = {"c6": "esp32c6_car", "gw-s3": "s3-gateway",
           "r-s3": "smartcar_remote", "app": "tc275_car", "sbl": "tc275_sbl"}

# ESP-IDF 工程里版本号写在 CMakeLists 的 PROJECT_VER（无 mw/app_version.h）
ESP_PROJECT_VER = ("esp32c6_car", "s3-gateway")

# IDF 产物名 = CMakeLists 的 project() 名，可能与目录名不同（s3-gateway → s3_gateway.bin）
ESP_APP_BIN = {"esp32c6_car": "esp32c6_car.bin", "s3-gateway": "s3_gateway.bin"}

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
    "s3-gateway": lambda: esp_artifacts(ROOT / "s3-gateway"),
    "smartcar_remote": lambda: esp_artifacts(ROOT / "smartcar_remote"),
    "tc275_car": lambda: tc275_artifacts(ROOT / "tc275_car", [ROOT / "tc275_car" / ADS_APP_BUILD]),
    "tc275_sbl": lambda: tc275_artifacts(sbl_dir(), [sbl_dir() / ADS_SBL_BUILD]),
}


# ---------------------------------------------------------------- 各工程实现

def build_esp32c6_car(_args):
    """委托项目自带的 flash.py（EIM 自动发现 IDF 环境，含 assets 打包）。"""
    return run([sys.executable, ROOT / "esp32c6_car" / "flash.py", "build"])


def build_s3_gateway(_args):
    """同样委托自带 flash.py：s3-gateway 是 C6 的 S3-CAM 替代固件，构建入口一致
    （idf.py build 已自动打 assets.bin）。"""
    return run([sys.executable, ROOT / "s3-gateway" / "flash.py", "build"])


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
    "s3-gateway": build_s3_gateway,
    "smartcar_remote": build_smartcar_remote,
    "tc275_car": build_tc275_car,
    "tc275_sbl": build_tc275_sbl,
}


def flash_esp32c6_car(args):
    """全参数透传 flash.py（full/assets/all/mon、-p COMx、-b -m）。"""
    args = args or ["full"]
    if args[0] != "build" and not {"-p", "--port"} & set(args):
        args = [*args, "-p", find_board_port("esp32c6_car", "C6 车端")]
    return run([sys.executable, ROOT / "esp32c6_car" / "flash.py", *args])


def flash_s3_gateway(args):
    """透传 s3-gateway/flash.py（full/assets/all/mon、-p、-b、-m）。
    遥控器也是 ESP32-S3（同 VID/PID），靠 flash 里的 esp_app_desc_t 认板。"""
    args = args or ["full"]
    if args[0] != "build" and not {"-p", "--port"} & set(args):
        args = [*args, "-p", find_board_port("s3-gateway", "S3-CAM 网关")]
    return run([sys.executable, ROOT / "s3-gateway" / "flash.py", *args])


def flash_smartcar_remote(args):
    """idf.py [-p PORT] flash [monitor]；未给 -p 时读 flash 认板。"""
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
        idf_args += ["-p", find_board_port("smartcar_remote", "S3 遥控器")]
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
    # REMAINDER 会把透传流里的开关一并吞进 args，--no-build 放任何位置都认
    if "--no-build" in a.args:
        a.args.remove("--no-build")
        a.no_build = True
    if not a.no_build:
        info(f"{p}: 先编译再烧录（--no-build 跳过，烧最近构建产物；编译不归档）")
        rc = BUILDERS[p]([])
        if rc:
            info(f"{p} 编译失败，未烧录")
            return rc
    if p == "esp32c6_car":
        return flash_esp32c6_car(a.args)
    if p == "s3-gateway":
        return flash_s3_gateway(a.args)
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


# ---------------------------------------------------------------- OTA 推送
#
# 默认走 S3 中转（PC 不需要在车网络里）：
#   fw.py 打签包 -> SCFW 容器写入 S3 暂存分区（smartcar_remote/tools/
#   stage_fw.py，esptool 走 USB）-> 遥控器 FIRMWARE 页点更新 -> S3 用自己的
#   token POST /ota/c6 | /ota/tc275。TC275 完成事件经 WS 回 S3。
# 包契约（复用，不重实现）：
#   esp32c6_car / s3-gateway
#                POST /ota/c6    payload = C6FW 包（C6: components/c6_ota/ota_self.c；
#                                 S3-CAM 网关: components/s3_ota/ota_self.c，
#                                 magic 与 URI 是移植时冻结的协议面，未改名）
#                POST /ota/tc275 payload = TCFW 包（bridge 中继 → TC275 SBL）
#   包格式见 contracts/ota/tcfw_bundle.h（TCFW 与 C6FW 字节级同构，148 B 头，
#   ed25519 签名覆盖前 84 B，payload 摘要 = SHA-512 前 32 B）。
#   SCFW 头：magic/target/size/crc32(zlib)/version/built，与 S3 scr_svc.c 一致。
#   TC275 写入非活动槽（SBL writeApp 按 槽基址+off 落 flash），swap/回滚
#   结果经 WS 广播（{"t":"otaswap"}）。
#   --direct：跳过 S3，PC 直推（需在车网络；C6 台架免 token 由
#   CONFIG_C6_OTA_NO_AUTH=y 提供，量产关掉后用 --token/--pair）。

OTA_SLOT_A_BASE = 0x80008000        # contracts/ota/ota_layout.h
OTA_SLOT_B_BASE = 0x80208000
OTA_SLOT_SIZE   = 0x1F8000
TCFW_HDR_LEN    = 148
TCFW_SIGNED_LEN = 84
OTA_UPLOAD_MAX  = 3 * 1024 * 1024   # C6 侧 OTA_TOTAL_MAX
DEFAULT_C6_HOST = "192.168.4.1"     # softAP 网关（smartcar_remote SCR_C6_IP 同源）
# 走 C6FW 包契约（magic C6FW + POST /ota/c6）的工程：s3-gateway 原位替换 C6，
# 这两个标识是冻结的协议面，所以遥控器侧的暂存分区与推送 URI 都不用改
C6_OTA_PROJECTS = ("esp32c6_car", "s3-gateway")


def c6_seed(args, p="esp32c6_car"):
    """签名种子路径与内容；默认用该工程自己的 dev seed（与两端公钥同源）。"""
    seed_path = Path(args.seed) if args.seed \
        else ROOT / p / "tools" / "keys" / "ed25519_dev.seed"
    if not seed_path.exists():
        seed_path = ROOT / "esp32c6_car" / "tools" / "keys" / "ed25519_dev.seed"
    if not seed_path.exists():
        die(f"OTA 签名种子不存在: {seed_path}（--seed 覆盖）")
    return seed_path, bytes.fromhex(seed_path.read_text(encoding="utf-8").strip())


def check_dev_pubkey(seed):
    """种子派生公钥必须等于 contracts/ota/ota_keys.h 的 OTA_KEYS_DEV——
    SBL/TC275 里烧死的就是它，不一致的包只会白传一趟被拒签。"""
    text = (ROOT / "contracts" / "ota" / "ota_keys.h").read_text(encoding="utf-8")
    m = re.search(r"OTA_KEYS_DEV\[OTA_KEYS_DEV_LEN\]\s*=\s*\{(.*?)\}", text, re.S)
    if not m:
        return
    sys.path.insert(0, str(ROOT / "esp32c6_car" / "tools"))
    import ed25519_ref
    pub = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
    if ed25519_ref.secret_to_public(seed) != pub:
        die("种子公钥与 contracts/ota/ota_keys.h 的 OTA_KEYS_DEV 不一致——设备会拒收该包")


def c6fw_bundle(repo_name, seed_path):
    """C6 口径的自更新包：委托各自仓库的 tools/sign_bundle.py（C6FW 格式真源）。
    s3-gateway 是 C6 的原位替代，上传 URI `/ota/c6` 与 magic `C6FW` 都是冻结的
    协议面，所以打包路径完全复用，只是工程名/产物名不同。"""
    repo = ROOT / repo_name
    b = repo / "build"
    app = None
    fa = b / "flasher_args.json"
    if fa.exists():
        try:
            f = json.loads(fa.read_text(encoding="utf-8")).get("app_file")
            app = b / f if f else None
        except (json.JSONDecodeError, OSError):
            pass
    if not (app and app.exists()):
        app = b / ESP_APP_BIN.get(repo_name, f"{repo_name}.bin")   # IDF 产物名 = project() 名
    if not app.exists():
        junk = ("bootloader", "partition", "ota_data", "assets", "_flashed")
        hits = [p for p in sorted(b.glob("*.bin"))
                if not any(k in p.name for k in junk)]
        app = hits[0] if hits else None
    if not app:
        die(f"{repo_name} 没有可打包的 app .bin（先 fw.py build {repo_name}）")
    out = b / "c6fw.bundle"
    cmd = [sys.executable, repo / "tools" / "sign_bundle.py",
           "--c6", str(app), "--seed-file", str(seed_path), "--out", str(out)]
    if (b / "assets.bin").exists():
        cmd += ["--assets", str(b / "assets.bin")]
    if run(cmd, cwd=repo):
        die("sign_bundle.py 打包失败")
    return out


def tc275_slot_image(hex_path):
    """App Intel HEX -> OTA 槽内镜像。SBL writeApp 以 槽基址+off 落 flash
    （tc275_sbl/bsp/flash_ota.c），payload 必须从槽字节 0 起；槽 A/B 构建
    镜像对称，两种地址都归一化成槽内偏移，空洞补 0xFF（flash 擦除态）。"""
    sys.path.insert(0, str(sbl_dir() / "tools"))
    import merge_hex
    mem = {}
    lo, hi, _entry = merge_hex.parse_hex(str(hex_path), mem)
    if lo is None:
        die(f"{hex_path} 里没有数据记录")
    for base in (OTA_SLOT_A_BASE, OTA_SLOT_B_BASE):
        if base <= lo and hi < base + OTA_SLOT_SIZE:
            break
    else:
        die(f"hex 地址 0x{lo:08X}..0x{hi:08X} 不在 OTA 槽范围"
            f"（0x{OTA_SLOT_A_BASE:08X}+ 或 0x{OTA_SLOT_B_BASE:08X}+）")
    img = bytearray(b"\xff" * (hi - base + 1))
    for a, v in mem.items():
        img[a - base] = v
    return bytes(img)


def tcfw_pack(hex_path, seed):
    """TCFW 包（contracts/ota/tcfw_bundle.h）：148 B 头 + App 槽内镜像，
    签名覆盖前 84 B，app_sha = SHA-512 前 32 B。产物落在 hex 旁边。"""
    sys.path.insert(0, str(ROOT / "esp32c6_car" / "tools"))
    import ed25519_ref as ed
    img = tc275_slot_image(hex_path)
    head = bytearray()
    head += b"TCFW"
    head += struct.pack("<BBH", 1, 1, TCFW_HDR_LEN)     # fmt=1, flags bit0=槽无关
    head += struct.pack("<II", TCFW_HDR_LEN + len(img), len(img))
    head += hashlib.sha512(img).digest()[:32]
    head += struct.pack("<I", 0) + bytes(32)            # rsv_len / rsv_sha（TC275 单镜像不用）
    head += ed.sign(seed, bytes(head))
    out = hex_path.with_suffix(".tcfw")
    out.write_bytes(bytes(head) + img)
    try:
        shown = out.relative_to(ROOT)
    except ValueError:
        shown = out
    info(f"TCFW 包: {shown}（app {len(img):,} B）")
    return out


def pair_token(host):
    """车端按钮开窗期间换取 control token；轮询 60 s 覆盖一次按键。"""
    import urllib.error
    import urllib.request
    info("请求配对：按住车端按钮 3 s 打开窗口（60 s 内自动重试）...")
    deadline = time.time() + 60
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(f"http://{host}/api/pair", timeout=8) as r:
                j = json.loads(r.read().decode("utf-8", "replace"))
            if j.get("ok") and j.get("token"):
                info("配对成功")
                return j["token"]
        except (urllib.error.URLError, urllib.error.HTTPError, OSError, ValueError):
            pass    # 403 no window / busy / 车不在线：窗口期到再试
        time.sleep(2)
    die("60 s 内未换到 token（车端窗口未开 / 已被占用 / 车不在线）")


def _probe_auth(host, uri, tries=3):
    """上传中断后用 0 长度 POST 探测 OTA 端点。旧固件（token 门）先查 token
    就 401；新固件（C6_OTA_NO_AUTH=y）走完 begin 才报错。RST 之后服务端要
    几秒恢复 accept，所以间隔 2 s 探三次，全失败才算掉线。"""
    import http.client
    for i in range(tries):
        try:
            conn = http.client.HTTPConnection(host, 80, timeout=5)
            conn.request("POST", uri, body=b"", headers={"Content-Length": "0"})
            r = conn.getresponse()
            r.read()
            conn.close()
            return r.status
        except (OSError, http.client.HTTPException):
            if i + 1 < tries:
                time.sleep(2)
    return None


def ota_post(host, uri, token, bundle_path):
    data = Path(bundle_path).read_bytes()
    if len(data) > OTA_UPLOAD_MAX:
        die(f"包 {len(data):,} B 超过 C6 上限 {OTA_UPLOAD_MAX:,} B")
    info(f"POST http://{host}{uri}  <- {bundle_path.name}（{len(data):,} B）")
    import http.client
    conn = http.client.HTTPConnection(host, 80, timeout=120)
    try:
        conn.putrequest("POST", f"{uri}?token={token}" if token else uri)
        conn.putheader("Content-Type", "application/octet-stream")
        conn.putheader("Content-Length", str(len(data)))
        conn.endheaders()
        t0, sent = time.time(), 0
        while sent < len(data):
            n = min(4096, len(data) - sent)
            conn.send(data[sent:sent + n])   # C6 credit window 会在 send 侧背压
            sent += n
            print(f"\r  上传 {sent:,}/{len(data):,} B "
                  f"({sent * 100 // len(data)}%)", end="", flush=True)
        print(f"  {time.time() - t0:.1f} s")
        resp = conn.getresponse()
        body = resp.read().decode("utf-8", "replace")
    except (OSError, http.client.HTTPException) as e:
        st = _probe_auth(host, uri)
        if st == 401:
            die(f"上传失败: {e}\n"
                f"  诊断: 车端 OTA 端点返回 401 —— 固件仍开着 token 门。\n"
                f"  处理: 烧一次新版 C6（just fw-build c6 && just fw-flash c6），\n"
                f"        或本次加 --pair / --token <hex>。")
        if st is None:
            die(f"上传失败: {e}（车端无响应：已重启 / 掉线，等它起来再试）")
        die(f"上传失败: {e}（车端在线且免鉴权已生效，端点探得 HTTP {st}——疑似链路瞬断，重试）")
    finally:
        conn.close()
    if resp.status == 401:
        die("C6 要求 control token（该台固件未开台架免鉴权）："
            "--token <hex> / --pair 换一个；或在 C6 保持 CONFIG_C6_OTA_NO_AUTH=y 重新编译烧录")
    info(f"HTTP {resp.status}: {body}")
    ok = resp.status == 200 and '"ok":true' in body
    if ok and uri.endswith("/ota/tc275"):
        info("TC275 已收包写入非活动槽；swap / 回滚事件经 WS 广播"
             '（{"t":"otaswap"}，S3 About / 诊断页可见）')
    return 0 if ok else 1


_PROJECT_VER_RE = re.compile(r'set\(PROJECT_VER\s+"([^"]+)"\)')


def fw_version_for(p):
    """SCFW 头的 version 字段：TC275 读 mw/app_version.h，
    ESP 工程读各自 CMakeLists 的 PROJECT_VER。"""
    if p in ESP_PROJECT_VER:
        m = _PROJECT_VER_RE.search(
            (repo_dir(p) / "CMakeLists.txt").read_text(encoding="utf-8"))
        return m.group(1) if m else ""
    return fw_version(p) or ""


def detect_serial_ports():
    """全部候选串口（Espressif VID 优先 + 常见 usb-serial 模式）。"""
    out = []
    try:
        from serial.tools import list_ports
        vids = {0x303A, 0x10C4, 0x1A86}
        out += [x.device for x in list_ports.comports() if x.vid in vids]
    except ImportError:
        pass
    for pat in ("/dev/cu.usbmodem*", "/dev/cu.usbserial*",
                "/dev/ttyUSB*", "/dev/ttyACM*"):
        out += glob.glob(pat)
    return sorted(set(out))


# 遥控器与网关都是 ESP32-S3，和 C6 一样走原生 USB-Serial-JTAG：VID/PID 完全相同
# （303A:1001），USB 层面只有序列号（= 芯片 MAC）能区分物理板子；而"这块板跑的是
# 哪个工程"只有 flash 里的 esp_app_desc_t 知道。所以首次见到某个序列号时读一次
# flash 认板（该板会复位一次），按序列号缓存归属，之后不再复位。
# 缓存可能过期（把网关固件烧到遥控器板上这类跨板复用），删掉该文件即重新认板；
# 每次解析都会打印 port=工程(版本)，挑错板子一眼可见，必要时用 -p 显式指定。
_BOARD_CACHE = Path.home() / ".cache" / "smartcar-fw" / "board_projects.json"
# esp_app_desc_t 落在 app 分区起始 +0x20（镜像头 + 段头之后）：
#   magic_word(0xABCD5432)@0 / version[32]@16 / project_name[32]@48
_APP_DESC_OFF = 0x20
_APP_DESC_MAGIC = b"\x32\x54\xcd\xab"
# 分区表默认在 0x8000，表项 32 B：magic(0xAA50) / type@2 / offset@4 / size@8 / label@12
_PT_OFF = 0x8000
_PT_MAGIC = b"\xaa\x50"
_PT_TYPE_APP = 0
# 一次 read-flash 同时覆盖分区表与三块板的 app 头（remote 的 factory 0x10000、
# 网关与 C6 的 ota_0 0x20000），省掉逐段复位的多次探测
_BLOB_OFF = _PT_OFF
_BLOB_SIZE = 0x20000 + _APP_DESC_OFF + 0x100 - _PT_OFF
_LIST_PORTS_PY = (
    "import json\n"
    "from serial.tools import list_ports\n"
    "print(json.dumps({p.device: p.serial_number for p in list_ports.comports()"
    " if p.vid}))\n")


def _port_serials(py):
    """{device: USB 序列号}；pyserial 只在 IDF venv 里有。失败返回 {}。"""
    try:
        r = subprocess.run([str(py), "-c", _LIST_PORTS_PY],
                           capture_output=True, text=True, timeout=15)
        return json.loads(r.stdout) if r.returncode == 0 else {}
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return {}


def _read_flash(py, port, offset, size, timeout=60):
    """esptool 读一段 flash -> bytes；连不上或读失败返回 None（该板会复位一次）。"""
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "blob.bin"
        try:
            r = subprocess.run(
                [str(py), "-m", "esptool", "--port", str(port),
                 "--connect-attempts", "1", "read-flash",
                 hex(offset), str(size), str(out)],
                capture_output=True, text=True, timeout=timeout)
        except (OSError, subprocess.TimeoutExpired):
            return None
        if r.returncode != 0:
            return None
        try:
            return out.read_bytes()
        except OSError:
            return None


def _probe_board(py, port):
    """读一次 flash，认出这块板跑的工程 -> (project 或 None, 固件版本)。"""
    blob = _read_flash(py, port, _BLOB_OFF, _BLOB_SIZE)
    if not blob:
        return None, ""
    for i in range(0, len(blob) - 31, 32):
        e = blob[i:i + 32]
        if e[:2] != _PT_MAGIC:
            break                     # 表项按序排列，首个非表项即表尾
        if e[2] != _PT_TYPE_APP:
            continue
        desc = struct.unpack_from("<I", e, 4)[0] + _APP_DESC_OFF - _BLOB_OFF
        h = blob[desc:desc + 80]
        if len(h) < 80 or h[:4] != _APP_DESC_MAGIC:
            continue                  # 该 app 分区空着（未 OTA 过的 ota_1）
        name = h[48:80].split(b"\x00")[0].decode("ascii", "replace")
        ver = h[16:48].split(b"\x00")[0].decode("ascii", "replace")
        proj = next((p for p in PROJECTS
                     if name in (p, p.replace("-", "_"), p.replace("_", "-"))), None)
        if proj:
            return proj, ver
    return None, ""                   # 空白片 / 跑着别的工程的板子


def find_board_port(project, label):
    """在所有候选串口里认出跑 project 固件的那块板（'esp32c6_car' /
    's3-gateway' / 'smartcar_remote'）。只有一块候选也要认：拔掉目标板后
    剩下的那块未必是要烧的那块。"""
    ports = detect_serial_ports()
    if not ports:
        die(f"未识别到 {label} 串口；插好板子或用 --port / -p 指定")
    py, _idf, _act = find_idf_env()
    if not py.exists():
        if len(ports) > 1:
            die(f"有多个串口（{', '.join(ports)}），但找不到 IDF venv 无法认板；"
                f"用 --port / -p 指定 {label}")
        info(f"自动识别 {label} 串口: {ports[0]}（唯一候选，无 IDF venv 故未认板）")
        return ports[0]
    try:
        cache = json.loads(_BOARD_CACHE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        cache = {}
    serials = _port_serials(py)
    seen, hits = {}, []
    for dev in ports:
        sn = serials.get(dev)
        ent = cache.get(sn) if sn else None
        if not (isinstance(ent, dict) and ent.get("project")):
            info(f"读取 {dev} 的固件信息认板（该板会复位一次）...")
            proj, ver = _probe_board(py, dev)
            ent = {"project": proj or "", "version": ver}
            if sn:
                cache[sn] = ent
        seen[dev] = ent
        if ent["project"] == project:
            hits.append(dev)
    try:
        _BOARD_CACHE.parent.mkdir(parents=True, exist_ok=True)
        _BOARD_CACHE.write_text(json.dumps(cache, indent=1), encoding="utf-8")
    except OSError:
        pass
    found = ", ".join(f"{d}={e['project'] or '未知'}({e['version'] or '-'})"
                      for d, e in seen.items())
    if len(hits) != 1:
        die(f"无法唯一确定 {label} 串口（{found}）；用 --port / -p 指定，"
            f"或删掉 {_BOARD_CACHE} 重新认板")
    info(f"自动识别 {label} 串口: {hits[0]}（{found}）")
    return hits[0]


def s3_stage(p, bundle, a):
    """SCFW 容器写入 S3 暂存分区：委托 smartcar_remote/tools/stage_fw.py
    （esptool 在 IDF venv 里，系统 python 没有）。写完 S3 重启后自检 CRC，
    推送动作在遥控器 FIRMWARE 页触发——那是 S3 自己的 token 在推。"""
    stage_py = ROOT / "smartcar_remote" / "tools" / "stage_fw.py"
    if not stage_py.exists():
        die(f"找不到 {stage_py}（smartcar_remote 未检出？）")
    py, _idf, _act = find_idf_env()
    if not py.exists():
        die("需要 ESP-IDF 环境（stage_fw.py 依赖 esptool）；先装 EIM/ESP-IDF")
    target = "c6" if p in C6_OTA_PROJECTS else "tc275"
    ver = a.version or fw_version_for(p)
    port = a.port
    if not port:
        port = find_board_port("smartcar_remote", "S3 遥控器")
    info(f"经 S3 中转: SCFW 暂存 -> {port}（fw_{target} 分区, version {ver or '-'}, "
         f"payload {bundle.name}）")
    rc = run([str(py), stage_py, "--target", target, "--image", str(bundle),
              "--version", ver, "--port", port])
    if rc:
        die(f"暂存失败 (rc={rc})")
    info("已写入 S3 暂存区：遥控器重启后打开 Settings > FIRMWARE（RESCAN 刷新），"
         "点对应卡片推送；进度与结果在该页显示，TC275 完成事件经 WS 回报。")
    return 0


def cmd_ota(a):
    p = require_project(a.project)
    if p not in C6_OTA_PROJECTS + ("tc275_car",):
        die(f"{p} 不支持 OTA：tc275_sbl 从不经 OTA 更新（SBL 区不动）；"
            f"smartcar_remote 走 idf.py flash")
    seed_path, seed = c6_seed(a, p)     # 配置类错误（种子/公钥）先暴露，不浪费一次编译
    check_dev_pubkey(seed)
    if a.file:
        bundle = Path(a.file)
        if not bundle.exists():
            die(f"包不存在: {bundle}")
    else:
        if not a.no_build:
            info(f"{p}: 先编译最新代码再打包（--no-build 打包最近构建产物）")
            rc = BUILDERS[p]([])
            if rc:
                info(f"{p} 编译失败，未打包")
                return rc
        if p in C6_OTA_PROJECTS:
            bundle = c6fw_bundle(p, seed_path)
        else:
            hx = app_hex() or die("tc275_car 没有可打包的 hex（先 fw.py build app）")
            bundle = tcfw_pack(hx, seed)

    if not a.direct:
        return s3_stage(p, bundle, a)

    if not a.token and not a.pair:
        info("直推模式：未给 --token/--pair，按台架免 token 尝试"
             "（C6 需 CONFIG_C6_OTA_NO_AUTH=y）")
    token = a.token if a.token else (pair_token(a.host) if a.pair else None)
    return ota_post(a.host, "/ota/c6" if p in C6_OTA_PROJECTS else "/ota/tc275",
                    token, bundle)


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
                                         + "   别名: " + " / ".join(ALIASES))
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("list", help="各工程构建产物 / 归档状态一览")

    b = sub.add_parser("build", help="编译一个工程")
    b.add_argument("project")
    b.add_argument("--no-collect", action="store_true",
                   help="构建后不归档（默认成功即归档到 firmware/dist/）")
    b.add_argument("args", nargs="*", help="透传给各工程构建入口")

    f = sub.add_parser("flash", help="编译 + 烧录（--no-build 只烧最近构建产物）")
    f.add_argument("project")
    f.add_argument("--no-build", action="store_true",
                   help="跳过编译，直接烧最近一次构建的产物")
    f.add_argument("args", nargs=argparse.REMAINDER,
                   help="透传给各工程烧录入口（REMAINDER，选项原样过）: "
                        "esp32c6_car / s3-gateway: full|assets|all|mon -p COMx -b -m；"
                        "remote: -p COMx monitor；"
                        "tc275_car/tc275_sbl: --id N --log x.xml 等 flash.py 参数")

    c = sub.add_parser("collect", help="归档产物到 firmware/dist/")
    c.add_argument("project", nargs="?", help="省略则各工程全归档（无产物的跳过并提示）")

    fa = sub.add_parser("factory", help="SBL+App 出厂整包（构建+合成，--flash 烧录）")
    fa.add_argument("--flash", action="store_true", help="合成后用 AURIXFlasher 整包烧录")
    fa.add_argument("--no-collect", action="store_true", help="不归档 SBL 侧产物")

    o = sub.add_parser("ota", help="OTA：编译+打签包暂存进 S3（FIRMWARE 页推送）；--direct 为 PC 直推")
    o.add_argument("project", help="esp32c6_car | tc275_car")
    o.add_argument("--no-build", action="store_true",
                   help="跳过编译，打包最近一次构建的产物（--file 同效）")
    o.add_argument("--port", help="S3 遥控器串口（多设备插着时必须指定；省略则唯一候选自动选）")
    o.add_argument("--version", help="SCFW 头的版本串（默认读工程版本真源）")
    o.add_argument("--direct", action="store_true",
                   help="跳过 S3 暂存，PC 直推 C6（需在车网络）")
    o.add_argument("--host", default=os.environ.get("FW_C6_HOST", DEFAULT_C6_HOST),
                   help="--direct 时 C6 的 IP（默认 192.168.4.1，FW_C6_HOST 可覆盖）")
    o.add_argument("--token", help="--direct 时已配对的 control token")
    o.add_argument("--pair", action="store_true",
                   help="--direct 时先 POST /api/pair 现场换 token（按住车端按钮 3 s）")
    o.add_argument("--file", help="跳过打包，直接使用现成的 C6FW/TCFW bundle 文件")
    o.add_argument("--seed", help="ed25519 种子文件（默认 esp32c6_car dev seed）")

    cl = sub.add_parser("clean", help="清空 firmware/dist/")
    cl.add_argument("--yes", action="store_true")

    a = ap.parse_args(argv)
    return {"list": cmd_list, "build": cmd_build, "flash": cmd_flash,
            "collect": cmd_collect, "factory": cmd_factory, "ota": cmd_ota,
            "clean": cmd_clean}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
