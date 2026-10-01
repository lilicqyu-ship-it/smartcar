# firmware/ — 固件统一存放、编译、烧录

smartcar 四个工程固件的总入口：一条命令编译、烧录、归档产物，
不再需要分别记每个工程的构建烧录方式。

```bash
python firmware/fw.py list                    # 各工程产物 / 归档状态一览
python firmware/fw.py build <project>         # 编译（成功即自动归档 dist；--no-collect 跳过）
python firmware/fw.py flash <project> [...]   # 烧录（参数透传给各工程入口）
python firmware/fw.py collect [project]       # 补充归档到 firmware/dist/
python firmware/fw.py factory [--flash]       # SBL+App 出厂整包合成（可烧录）
python firmware/fw.py clean [--yes]           # 清空 firmware/dist/
```

工程名：`esp32c6_car` | `smartcar_remote` | `tc275_car` | `tc275_sbl`，
别名 `c6` / `remote` / `app` / `sbl`（tc275_sbl 历史上叫 myCarSbl，旧名也认）。
推荐走 just：`just fw-list` / `just fw-build <project>` / `just fw-flash <project>` /
`just fw-collect` / `just fw-factory` / `just fw-dist`。

## 目录

```
firmware/
├── fw.py        统一入口（纯 Python 标准库，Windows / macOS / Linux 通用）
└── dist/        归档产物（固件镜像入库做版本管理）
    └── <工程>/[v版本-]<时间戳-g提交号[-dirty]>/   bin/hex/elf/map + manifest.json
```

`manifest.json` 记录归档时间、固件版本（TC275 读自 `mw/app_version.h`）、
子仓库 commit/分支/是否脏、每个产物的大小与来源路径——烧出问题的板子
可以对回源码版本。

## 归档的版本管理与 GitHub 推送

`just fw-build <project>` 构建成功即自动归档；归档目录名携带固件版本
（TC275 工程：`v0.2.2-20261001-1530-g9ac84b1/`）。**固件镜像（hex/bin）与
manifest.json 入库**，作为整车固件的版本档案；elf/map/mdf 调试符号体积大
（单 elf ~2MB），只留本地（.gitignore 过滤），需要时从本地归档取。

```bash
just fw-build tc275_sbl        # 构建 + 自动归档
just fw-dist                   # git add firmware/dist + commit + push（GitHub 上可见全部镜像版本）
```

历史镜像回溯：直接翻 `firmware/dist/<工程>/` 的目录名（版本+时间+提交号），
或 `git log -- firmware/dist/<工程>`；某次烧录出问题，用 manifest.json 里的
commit 精确回到源码。

## 各工程的编译 / 烧录链（fw.py 只做发现与委托）

| 工程 | 编译 | 烧录 |
|---|---|---|
| esp32c6_car | 委托 `esp32c6_car/flash.py build`（自动发现 EIM/IDF 环境，含 assets 打包） | 委托 `flash.py`：`full`（默认）/`assets`/`all`，`-p COMx` 指定串口，`-m` 烧后监视 |
| smartcar_remote | EIM 环境 + `idf.py build` | `idf.py -p <串口> flash`，串口按 USB VID 自动识别（可 `-p COMx` 覆盖，余参透传如 `monitor`） |
| tc275_car | `python -m SCons`（解析 `.cproject`，与 ADS 同源零漂移；产物名带版本） | `tc275_sbl/tools/flash.py flash <App槽A.hex>`（AURIXFlasher CLI） |
| tc275_sbl | `python -m SCons`（同上） | `tc275_sbl/tools/flash.py flash`（自动取 SCons 最新版本化 hex） |

TC275 烧录透传 `flash.py` 的参数：`--id <n>` 选 DAS 端口、`--log x.xml`
出详细日志等（需 DAS 服务在跑，装 ADS 即有）。

### tc275 双仓 SCons 命令行编译的说明

两仓各持一份同源的 `SConstruct` + `site_scons/aurix_tasking.py`：源集、
include、宏、排除表直接解析 `.cproject`，编译/链接参数复刻 IDE 生成的
命令行（TASKING TriCore v6.3r1），与 IDE **零漂移**，Clean 后也能独立出
产物——**不依赖** ADS 的生成构建文件。

- 产物在 `build/tasking-<cfg>/`，文件名自动携带 `mw/app_version.h` 的
  版本号（`tc275_car_v0.2.2.elf/.hex/.map`）。
- 常用参数：`cfg=release`（对应 IDE Release 源集）、`opt=-O2`、`size`
  （只看体积）、`-c` 清理、`-j8` 并行。
- TASKING 工具链由 `aurix_tasking.find_tasking()` 自动发现
  （`TASKING_TRICORE_HOME`/`TASKING_HOME` 可覆盖）。

### factory（出厂整包）

`fw.py factory` = 编译 tc275_car（App 槽 A）+ tc275_sbl（SBL）→ Intel-HEX
层合成 `tc275_sbl/build/tasking-debug/factory_full.hex`（一次烧录整片：
SBL 0x80000000 + App 0x80008000，合成工具校验地址不重叠）。`--flash` 直接
用 AURIXFlasher 合成+烧录一步到位。

## 环境要求

- **ESP 两工程**：EIM 安装的 ESP-IDF v6.1（Windows: `C:\Espressif`，
  macOS/Linux: `~/.espressif`），fw.py 自动发现；失败时用 `FW_IDF_PROFILE`
  指定 PowerShell 激活脚本。
- **TC275 两工程**：完整版 TASKING（如 v6.3r1，`C:/Program Files/TASKING/`；
  ADS 内置版许可禁止 IDE 外运行）+ ADS（首次生成构建文件、AURIXFlasher、
  DAS）。SCons 侧不在默认位置时用 `TASKING_TRICORE_HOME` 环境变量覆盖。
- Windows 上 fw.py 需在 Git Bash / CMD / PowerShell 任一里以 `python` 运行。

## FAQ

- **`找不到 TASKING TriCore 工具链`** → 装完整版或设 `TASKING_TRICORE_HOME`；
  只装了 ADS 时 tc275 两工程只能在 IDE 里编。
- **`没有 ADS 生成的构建文件`** → 先在 ADS 里 import + 构建一次（见上）。
- **tc275_sbl 找不到目录** → 子模块已从 `myCarSbl` 改名 `tc275_sbl`，
  两个名字 fw.py 都认；`just init` 未跑导致子仓库缺失时先初始化。
- **烧录串口识别不到** → ESP 板插好再跑；或显式 `-p COM7`。TC275 侧
  确认 DAS 服务在跑、MiniWiggler 连接正常（ADS 里能连上即可）。
