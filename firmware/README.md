# firmware/ — 固件统一存放、编译、烧录

smartcar 五个工程固件的总入口：一条命令编译、烧录、归档产物，
不再需要分别记每个工程的构建烧录方式。

```bash
python firmware/fw.py list                    # 各工程产物 / 归档状态一览
python firmware/fw.py build <project>         # 编译（成功即自动归档 dist；--no-collect 跳过）
python firmware/fw.py flash <project> [...]   # 编译 + 烧录一条指令（--no-build 只烧最近构建；参数透传）
python firmware/fw.py ota <project> [options] # 编译 + 打签包 -> 暂存进 S3（--direct 直推；--no-build/--file 跳过编译）
python firmware/fw.py collect [project]       # 补充归档到 firmware/dist/
python firmware/fw.py factory [--flash]       # SBL+App 出厂整包合成（可烧录）
python firmware/fw.py clean [--yes]           # 清空 firmware/dist/
```

工程名：`esp32c6_car` | `s3-gateway` | `smartcar_remote` | `tc275_car` | `tc275_sbl`，
别名 `c6` / `gw-s3` / `r-s3` / `app` / `sbl`（tc275_sbl 历史上叫 myCarSbl，旧目录名也认）。
推荐走 just：`just fw-list` / `just fw-build <project>` / `just fw-flash <project>` /
`just fw-ota <project>` / `just fw-collect` / `just fw-factory` / `just fw-dist`。

`flash` / `ota` 默认**先增量编译**对应工程再烧/再打包——改完代码一条指令到位，
也不会把旧构建产物烧上板或推上车；`--no-build`（ota 另有 `--file` 直接用现成包）
跳过编译。flash/ota 里的编译**不触发归档**（归档走 `build` / `collect`）。

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
| s3-gateway | 委托 `s3-gateway/flash.py build`（C6 的 S3-CAM 替代固件，同一套 click CLI，含 assets 打包） | 委托同一个 `flash.py`：`full`/`assets`/`all`/`mon`，`-p <串口>` 指定，`-b` 先编 `-m` 烧后监视 |
| smartcar_remote | EIM 环境 + `idf.py build` | `idf.py -p <串口> flash`，串口自动认板（可 `-p COMx` 覆盖，余参透传如 `monitor`） |
| tc275_car | `python -m SCons`（解析 `.cproject`，与 ADS 同源零漂移；产物名带版本） | `tc275_sbl/tools/flash.py flash <App槽A.hex>`（AURIXFlasher CLI） |
| tc275_sbl | `python -m SCons`（同上） | `tc275_sbl/tools/flash.py flash`（自动取 SCons 最新版本化 hex） |

TC275 烧录透传 `flash.py` 的参数：`--id <n>` 选 DAS 端口、`--log x.xml`
出详细日志等（需 DAS 服务在跑，装 ADS 即有）。烧录透传参数（`-m`/`-p`/`--id`
等）原样透传，fw.py 自己的开关只有 `--no-build`。

### 烧录认板（三块 ESP 板在 USB 层长得一样）

遥控器与网关都是 ESP32-S3，和 C6 一样走原生 USB-Serial-JTAG：**VID/PID 完全相同
（303A:1001）**，连芯片型号都区分不了两块 S3，USB 侧只有序列号（= 芯片 MAC）能区分
物理板子。所以未给 `-p` 时按"这块板跑的是哪个工程"来认板：一次 `read-flash` 覆盖
分区表（0x8000）与 `factory`/`ota_0` 两处 app 头，读 app 分区起始 +0x20 的
`esp_app_desc_t`（magic `0xABCD5432`，`project_name` 在 +48），拿到工程名与固件版本。

- 结果按 USB 序列号缓存到 `~/.cache/smartcar-fw/board_projects.json`
  （`{序列号: {project, version}}`）：命中就不再读 flash，**未命中的板子会被复位一次**。
- 每次解析都打印全表 `串口=工程(版本)`，选错板子当场可见；只插一块候选时同样会认板，
  拔掉目标板不会把剩下那块当成目标。
- 认板按分区表里 `type=app` 的分区扫，而不是死记偏移：网关板的 nvs 区间里就残留过
  一份旧 app 头（0x10000 处），照偏移硬读会把数据区当成 app 认成别的工程。
- 缓存过期（同一块板换了别的工程固件）→ 删掉该文件重新认板，或直接用 `-p` 指定。
  空白片 / 跑着未知固件的板子认不出，多块候选时会报错要求 `-p`。
- 台架实测（2026-10-02，两块 S3 同时插着）：`/dev/cu.usbmodem11301 =
  smartcar_remote(1.0.0)`、`/dev/cu.usbmodem11401 = s3-gateway(1.1.0)`，
  `fw.py flash gw-s3` / `flash r-s3` 各自命中正确的那块。

### tc275 双仓 SCons 命令行编译的说明

两仓各持一份同源的 `SConstruct` + `site_scons/aurix_tasking.py`：源集、
include、宏、排除表直接解析 `.cproject`，编译/链接参数复刻 IDE 生成的
命令行（TASKING TriCore v6.3r1），与 IDE **零漂移**，Clean 后也能独立出
产物——**不依赖** ADS 的生成构建文件。

- 产物在 `build/tasking-<cfg>/`，文件名自动携带 `mw/app_version.h` 的
  版本号（`tc275_car_v0.2.2.elf/.hex/.map`）。
- 常用参数：`cfg=release`（对应 IDE Release 源集）、`opt=-O2`、`size`
  （只看体积）、`-c` 清理；并行度默认 -j8（SConstruct 内置，`-jN` 可覆盖）。
- TASKING 工具链由 `aurix_tasking.find_tasking()` 自动发现
  （`TASKING_TRICORE_HOME`/`TASKING_HOME` 可覆盖）。

### ota（C6FW 签包与 S3 暂存）

`fw.py ota c6|gw-s3|app`：`c6` 与 `gw-s3` 打同一套 C6FW 签包（bundle magic `C6FW`、
上传 URI `/ota/c6`、proto v2 / SF 帧布局）——网关整体替换 C6 时这些协议面**冻结不改名**，
手机、遥控器与 TC275 侧无需变更；`app`（TC275）走 SCFW → `/ota/tc275`。签包写进遥控器
的 `fw_c6` / `fw_tc` 暂存分区（委托 `smartcar_remote/tools/stage_fw.py`，串口同样自动认板），
真正的推送在遥控器 Settings > FIRMWARE 页点（用的是遥控器自己的 token）。台架私钥
`tools/keys/ed25519_dev.seed` 各仓一份且字节相同，`--seed` 可覆盖，**不入库**。

### factory（出厂整包）

`fw.py factory` = 编译 tc275_car（App 槽 A）+ tc275_sbl（SBL）→ Intel-HEX
层合成 `tc275_sbl/build/tasking-debug/factory_full.hex`（一次烧录整片：
SBL 0x80000000 + App 0x80008000，合成工具校验地址不重叠）。`--flash` 直接
用 AURIXFlasher 合成+烧录一步到位。

## iOS 遥控器 App（ios_remote/，不经 fw.py）

iPhone 上的 S3 遥控器（proto v2 客户端，与 smartcar_remote 同构，见
`ios_remote/README.md`）。不在 fw.py 管辖内：无固件镜像/OTA 链路，产物是
签名的 .app 包，走 Xcode 工具链（需 macOS + 完整 Xcode 26）。

```bash
just ios-build           # 编译真机包（Xcode 自动签名；个人团队签的 App 7 天有效期）
just ios-install         # 编译 + USB 下载到 iPhone（同 fw-flash 口径：先编译再装，不装旧包）
just ios-install <UDID>  # 指定设备；设备列表用 xcrun devicectl list devices 查
just ios-test            # 主机单测（模拟器免签名：协议/控制语义/安全/电池防抖/信号分档）
just ios-icon            # 重新生成 App 图标（ios_remote/tools/gen_icon.swift）
```

- 设备/团队/模拟器是 justfile 顶部的 `ios-device` / `ios-team` / `ios-sim`
  变量，`just ios-device=00008101-XXXX ios-install` 可覆盖。
- 首次安装要在手机上信任开发者证书（设置 > 通用 > VPN 与设备管理），App 首启
  允许"本地网络"权限；个人团队签名 7 天过期，到期重跑 `just ios-install`。
- 图标/页面截图等交付物在 `ios_remote/doc/`；App 版本与协议口径见
  `ios_remote/README.md` 的功能对照表。

## 环境要求

- **ESP 三工程**：EIM 安装的 ESP-IDF v6.1（Windows: `C:\Espressif`，
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
- **烧录串口识别不到** → ESP 板插好再跑（数据线，不是充电线）；或显式 `-p COM7`
  / `-p /dev/cu.usbmodemXXXX`。TC275 侧
  确认 DAS 服务在跑、MiniWiggler 连接正常（ADS 里能连上即可）。
- **`无法唯一确定 xx 串口`** → 见上面「烧录认板」：报错里带了全表
  `串口=工程(版本)`，按它显式 `-p`；换过板子固件时先删
  `~/.cache/smartcar-fw/board_projects.json` 重新认。
