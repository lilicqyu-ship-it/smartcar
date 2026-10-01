# smartcar

[![contracts](https://github.com/lilicqyu-ship-it/smartcar/actions/workflows/contracts.yml/badge.svg)](https://github.com/lilicqyu-ship-it/smartcar/actions/workflows/contracts.yml) [![version](https://img.shields.io/github/v/tag/lilicqyu-ship-it/smartcar?label=version&sort=semver)](https://github.com/lilicqyu-ship-it/smartcar/tags)

智能车固件 **monorepo**。四个固件工程与共享接口契约、固件工具链同仓管理——
一次提交一次推送，`git status` 只会出现真实文件差异，跨工程改动天然原子。
本仓库承担三件事：

1. **四个固件工程** — `esp32c6_car/`（C6 小车主控）、`smartcar_remote/`（S3 遥控器）、
   `tc275_car/`（TC275 车体控制）、`tc275_sbl/`（TC275 OTA 二级引导）。
2. **共享接口契约** — `contracts/` 是跨工程接口的唯一权威版本，
   `scripts/check-contracts.sh` 逐字节校验各工程副本，杜绝拷贝漂移。
3. **固件工具链** — `firmware/fw.py` 统一编译/烧录/归档/OTA 入口，`justfile` 一条命令直达。

> 📘 **完整使用指南见 [doc/00-usage.md](doc/00-usage.md)**：概念速成、六大日常场景分步操作、
> 全部命令/脚本手册、CI 说明与故障排查 FAQ。
>
> 🗄️ 历史说明：2026-10 前本仓库是 submodule 总控仓库，四个工程曾各自独立成仓
> （`lilicqyu-ship-it/esp32c6_car` 等，已随 monorepo 化归档只读）。工程历史经
> subtree 合并完整保留在本仓库中，旧 tag 仍可从旧仓库查阅。

## 布局

```
smartcar/
├── esp32c6_car/       ESP32-C6 小车主控固件（ESP-IDF）
├── smartcar_remote/   ESP32-S3 遥控器（LVGL + proto）
├── tc275_car/         TC275 车体控制固件（TriCore/AUTOSAR 风格）
├── tc275_sbl/          TC275 OTA 二级引导（PFlash 双 bank + 自动回滚）
├── contracts/
│   ├── link/          LINK 通信协议: proto_frames.[ch]（帧格式/命令表/CRC）
│   └── ota/           SBL↔App 接口: ota_layout.h（槽位/分区）、
│                      ota_meta.h（DFlash 启动元数据）、tcfw_bundle.h（TCFW 包头/签名范围）
├── scripts/
│   ├── sync-gh.sh     旧多仓 GitHub 配置下发（旧仓库归档后可删）
│   └── check-contracts.sh  接口副本一致性校验（--apply 用 contracts/ 覆盖副本）
├── firmware/
│   ├── fw.py          四工程固件统一入口: 编译/烧录/归档（详见 firmware/README.md）
│   └── dist/          归档产物（时间戳 + 提交号，gitignore 部分入库）
└── justfile           批量命令
```

## 新机器初始化

```bash
git clone https://github.com/lilicqyu-ship-it/smartcar.git
just doctor   # 检查本机环境
```

## 常用命令

| 命令 | 作用 |
|---|---|
| `just contracts` | 校验接口副本与 contracts/ 一致 |
| `just contracts-apply` | 用 contracts/ 覆盖各工程副本（接口变更后同步） |
| `just tag <proj> <ver>` | 打工程版本 tag（如 `just tag c6 v1.0.1` → `c6/v1.0.1`） |
| `just fw-list` | 四工程固件产物 / 归档状态一览 |
| `just fw-build <工程>` | 编译固件（esp32c6_car/smartcar_remote/tc275_car/tc275_sbl） |
| `just fw-flash <工程>` | 烧录固件（串口自动识别，参数透传各工程入口） |
| `just fw-collect` | 归档产物到 firmware/dist/（含 manifest 记录源码版本） |
| `just fw-factory` | SBL+App 出厂整包合成（--flash 顺带烧录） |
| `just scons-car` / `just scons-sbl` | TASKING SCons 直编 TC275 两工程（免 ADS 生成文件） |

版本 tag 带工程前缀（与 fw.py 别名一致）：`c6/`（esp32c6_car）、`r-s3/`（smartcar_remote）、
`app/`（tc275_car）、`sbl/`（tc275_sbl）——四工程曾有同名 `v1.0.0`，前缀避免 tag 撞名。

## 改共享接口的流程

以 LINK 协议为例（`contracts/link/proto_frames.[ch]`）：

1. 修改 `contracts/link/`；
2. `just contracts-apply` 同步到 `esp32c6_car/components/c6_proto/` 与
   `smartcar_remote/main/proto/`；
3. **一个提交**同时包含契约与两侧实现副本，push 后各工程 CI 分别验证
   （按 paths 触发），契约与实现天然原子闭环。

## 固件编译 / 烧录 / 归档

`firmware/fw.py` 是四工程固件的统一入口（详见 [firmware/README.md](firmware/README.md)）：
编译委托各工程自己的构建链（ESP-IDF EIM 自动发现；TC275 走完整版 TASKING
命令行），烧录串口自动识别，产物按 `时间戳-g提交号` 归档进 `firmware/dist/`
并写 manifest 记录源码版本。出厂整包一条命令：

```bash
just fw-build tc275_car          # 编译（--collect 顺带归档）
just fw-flash esp32c6_car        # 烧录（串口自动识别，-p COMx 可指定）
just fw-factory --flash          # SBL + App 槽 A 合成整包并一次烧录
```

> tc275_car / tc275_sbl 的命令行编译需本机装完整版 TASKING（ADS 内置版
> 许可禁止 IDE 外运行）；tc275_car 首次需在 ADS 里构建一次以生成构建文件。

### TC275 的 SCons 直编路线

`just scons-car` / `just scons-sbl`（或进工程目录跑 `scons`）走各工程根目录的
SConstruct：include 路径、宏、源码排除列表直接解析 `.cproject`，编译/链接
参数复刻 ADS 生成 makefile 的命令行，**不依赖任何 ADS 生成文件**，
新 clone 即可直接命令行全量编译。产物在 `<工程>/build/tasking-debug/`（elf/hex/map）。

```bash
just scons-car                   # 全量编译（默认 8 并行），增量续编
just scons-sbl size              # 只看体积（elfsize）
just scons-car cfg=release       # Release 源集（.cproject 的该配置目前不完整，编不过属正常）
cd tc275_sbl && scons -c         # 清理
```

> 两条 TC275 路线并存：`fw-build` 复用 ADS 构建目录（产物即 IDE 所见），
> SCons 独立成树（`build/`）。tc275_sbl 已验证 SCons 与 ADS 产物体积逐字节
> 一致；tc275_car 差 <0.1%（ADS 目录构建后源码有改名提交）。

## 校验覆盖范围

| contracts/ 文件 | 各工程副本 |
|---|---|
| link/proto_frames.h/.c | esp32c6_car `components/c6_proto/`，smartcar_remote `main/proto/` |
| ota/ota_layout.h, ota_meta.h, tcfw_bundle.h, ota_keys.h | tc275_sbl `mw/ota/`，tc275_car `mw/ota/` |
| crypto/ed25519v.[ch], sha512.[ch], c6_consts.h | esp32c6_car `components/c6_ota/`，tc275_sbl `mw/crypto/`，tc275_car `mw/crypto/` |

CI：`.github/workflows/contracts.yml` 在 push/PR 时运行同一校验（Linux + Windows 双平台）；
四工程 CI 按路径触发（`esp32c6-car.yml` / `smartcar-remote.yml` / `tc275-car.yml` / `tc275-sbl.yml`），
contracts/ 变更会同时触发全部——副本与源头同仓，漂移无处可藏。

> `tc275_car/mw/proto/protocol.[ch]` 是 LINK 协议的旧版实现，待其采纳
> `proto_frames.[ch]`（见 proto_frames.h 头注释的既定计划）后，
> 在 `scripts/check-contracts.sh` 的声明表中追加对应路径即可纳入校验。

## 发版

```bash
just tag c6 v1.0.1               # 例：esp32c6_car 发 v1.0.1
just tag app v1.0.1              # tc275_car（app/）；同理 r-s3/、sbl/
git push origin main --tags      # push tag 后对应 Release workflow 自动构建/发布
```

## GitHub 配置

`just gh-sync`（旧架构遗留）可给四个旧工程仓库统一下发标签/分支保护/看板；
旧仓库归档后本配方与 `scripts/sync-gh.sh` 可一并删除。

## Windows 开发环境

TC275 / tc275_sbl 只能在 Windows 的 AURIX Development Studio（TASKING）下编译，
ESP32 两工程在 Windows / macOS 均可。

```powershell
winget install Git.Git Casey.Just GitHub.cli
git config --global core.longpaths true    # FreeRTOS 深层路径，必开
git config --global core.autocrlf false    # 换行由 .gitattributes 统一为 LF
gh auth login
git clone https://github.com/lilicqyu-ship-it/smartcar.git C:\Code\smartcar
cd C:\Code\smartcar; just doctor
```

- 根目录放短路径（`C:\Code\smartcar`），避免超 260 字符。
- `justfile` 在 Windows 固定用 `C:/Program Files/Git/bin/bash.exe`，不用 WSL bash。
- 换行：各工程与根目录都有 `.gitattributes`（`eol=lf`，`.bat` 为 CRLF），两平台字节一致。
- ADS：`File → Import → Existing Projects into Workspace`，选 `tc275_car` 和 `tc275_sbl`
  （工程名已与目录名一致），**不要勾 Copy into workspace**，否则改动不在 git 里。
  从旧工作台迁移的：先移除 workspace 里残留的 `myCar`/`myCarSbl` 旧工程再重新导入；
  Debug Configuration 如 elf 指向旧路径则重选一次（产物现为 `tc275_car.elf`/`tc275_sbl.elf`）。
- ESP-IDF 用官方 Windows 安装器；`esp32c6_car/flash.bat` 可直接用（串口 `COMx`）。
