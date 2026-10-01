# firmware/ — 固件统一存放、编译、烧录

smartcar 四个工程固件的总入口：一条命令编译、烧录、归档产物，
不再需要分别记每个工程的构建烧录方式。

```bash
python firmware/fw.py list                    # 各工程产物 / 归档状态一览
python firmware/fw.py build <project>         # 编译（--collect 顺带归档）
python firmware/fw.py flash <project> [...]   # 烧录（参数透传给各工程入口）
python firmware/fw.py collect [project]       # 归档产物到 firmware/dist/
python firmware/fw.py factory [--flash]       # SBL+App 出厂整包合成（可烧录）
python firmware/fw.py clean [--yes]           # 清空 firmware/dist/
```

工程名：`esp32c6_car` | `smartcar_remote` | `tc275_car` | `tc275_sbl`，
别名 `c6` / `remote` / `app` / `sbl`（tc275_sbl 历史上叫 myCarSbl，旧名也认）。
推荐走 just：`just fw-list` / `just fw-build <project>` / `just fw-flash <project>` /
`just fw-collect` / `just fw-factory`。

## 目录

```
firmware/
├── fw.py        统一入口（纯 Python 标准库，Windows / macOS / Linux 通用）
└── dist/        归档产物（gitignore，不入库）
    └── <工程>/<时间戳-g提交号[-dirty]>/   bin/hex/elf/map + manifest.json
```

`manifest.json` 记录归档时间、子仓库 commit/分支/是否脏、每个产物的
大小与来源路径——烧出问题的板子可以对回源码版本。

## 各工程的编译 / 烧录链（fw.py 只做发现与委托）

| 工程 | 编译 | 烧录 |
|---|---|---|
| esp32c6_car | 委托 `esp32c6_car/flash.py build`（自动发现 EIM/IDF 环境，含 assets 打包） | 委托 `flash.py`：`full`（默认）/`assets`/`all`，`-p COMx` 指定串口，`-m` 烧后监视 |
| smartcar_remote | EIM 环境 + `idf.py build` | `idf.py -p <串口> flash`，串口按 USB VID 自动识别（可 `-p COMx` 覆盖，余参透传如 `monitor`） |
| tc275_car | 解析 ADS 生成的 `subdir.mk`，用完整版 TASKING 命令行增量重编 | `tc275_sbl/tools/flash.py flash <App槽A.hex>`（AURIXFlasher CLI） |
| tc275_sbl | 委托 `tc275_sbl/tools/build_sbl.sh` | `tc275_sbl/tools/flash.py flash Debug/tc275_sbl.hex` |

TC275 烧录透传 `flash.py` 的参数：`--id <n>` 选 DAS 端口、`--log x.xml`
出详细日志等（需 DAS 服务在跑，装 ADS 即有）。

### tc275_car 命令行编译的说明

ADS 生成的 makefile 把目标/依赖名整个用双引号括起，GNU make 与 TASKING
mktc 都无法直接驱动；fw.py 改为解析 `TriCore Debug (TASKING)/` 里的
`subdir.mk`，提取每个 `.c` 的 cctc 命令与链接命令直接执行——编译标志取自
ADS 生成的文件本身，与 IDE **零漂移**。限制：

- **首次必须在 ADS 里 import 并构建一次**（生成 makefile/subdir.mk/.opt）；
  之后命令行即可增量重编，两边可以混用（产物都在同一构建目录）。
- 增量只看 `.c` 的 mtime：改了**头文件**后请 `touch` 引用它的 `.c`，或
  在 ADS 里构建。
- 在 ADS 的 Project Properties 里改了编译选项后，需在 ADS 里重新构建一次
  让生成文件刷新。

### factory（出厂整包）

`fw.py factory` = 编译 tc275_car（App 槽 A）+ tc275_sbl（SBL）→ Intel-HEX
层合成 `tc275_sbl/Debug/factory_full.hex`（一次烧录整片：SBL 0x80000000 +
App 0x80008000，合成工具校验地址不重叠）。`--flash` 直接用 AURIXFlasher
合成+烧录一步到位。

## 环境要求

- **ESP 两工程**：EIM 安装的 ESP-IDF v6.1（Windows: `C:\Espressif`，
  macOS/Linux: `~/.espressif`），fw.py 自动发现；失败时用 `FW_IDF_PROFILE`
  指定 PowerShell 激活脚本。
- **TC275 两工程**：完整版 TASKING（如 v6.3r1，`C:/Program Files/TASKING/`；
  ADS 内置版许可禁止 IDE 外运行）+ ADS（首次生成构建文件、AURIXFlasher、
  DAS）。不在默认位置时用 `FW_TASKING=<ctc/bin 目录>` 覆盖。
- Windows 上 fw.py 需在 Git Bash / CMD / PowerShell 任一里以 `python` 运行。

## FAQ

- **`未找到完整版 TASKING`** → 装完整版或 `FW_TASKING=<ctc/bin>`；
  只装了 ADS 时 tc275 两工程只能在 IDE 里编。
- **`没有 ADS 生成的构建文件`** → 先在 ADS 里 import + 构建一次（见上）。
- **tc275_sbl 找不到目录** → 子模块已从 `myCarSbl` 改名 `tc275_sbl`，
  两个名字 fw.py 都认；`just init` 未跑导致子仓库缺失时先初始化。
- **烧录串口识别不到** → ESP 板插好再跑；或显式 `-p COM7`。TC275 侧
  确认 DAS 服务在跑、MiniWiggler 连接正常（ADS 里能连上即可）。
