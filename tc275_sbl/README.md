# tc275_sbl — TC275 OTA 二级引导（SBL）工程

[![CI](https://github.com/lilicqyu-ship-it/tc275_sbl/actions/workflows/ci.yml/badge.svg)](https://github.com/lilicqyu-ship-it/tc275_sbl/actions/workflows/ci.yml) [![version](https://img.shields.io/github/v/tag/lilicqyu-ship-it/tc275_sbl?label=version&sort=semver)](https://github.com/lilicqyu-ship-it/tc275_sbl/tags)

智能车 TC275 主控的 OTA 升级引导程序：上电由 SBL 接管 reset 向量，读取
DFlash 中的 OTA 元数据，决定跳入 PFlash 双 bank（Slot A / Slot B）中的哪个
App 镜像，并在升级失败时自动回滚，保证车辆不变砖。

设计方案见 [doc/24-ota-sbl-dualbank.md](doc/24-ota-sbl-dualbank.md)
（双 bank 分区、OtaMeta 双页元数据、SBL/App 状态机、"TCFW" 固件包格式）。

## 当前状态

**软件层已实现，未上板**（2026-09-30）：

- ✅ host 单测 282 断言全绿（验收门 G-OTA-1/2）：TCFW 验签器、SF OTA 帧布局
  （与 C6 逐字节一致）、ota_rx 接收状态机（12 场景）、OtaMeta 双页掉电恢复、
  §5.1 回滚决策梯度
- ✅ SBL 经 TASKING 命令行编译链接验证：11.2 KB 代码，落在 32 KB SBL 区域
  内，入口 0x80000020（用本机完整版 TASKING v6.3r1 验证；正式产物请从 ADS 构建）
- ✅ 三个 linker 文件冒烟链接通过：AppA `.start`@0x80008020、
  AppB `.start`@0x80208020（槽基址+0x20 入口约定）
- ⬜ 上板项 G-OTA-3/4/5/6：ADS 构建烧录、SBL→App 跳转小样、写 B 换槽回滚、
  传输中断/验签失败/自检失败三种回滚路径、掉电恢复
- ✅ tc275_car（App 工程）已接入（另一仓库，独立提交）：默认 lsl 换为槽 A 布局
  （入口 0x80008020，冒烟链接验证）+ `Lcf_AppB.lsl`；`com/ota_app.c` 装配
  OtaRxOps（槽位自识别/flash/元数据/复位/LINK_send）与 §5.2 自检确认；
  `link.c` 分发 OTA 帧；`protocol.h` 补 PROTO_CMD_OTA_*；mw/ota、mw/crypto、
  bsp/flash_ota 为本工程同源拷贝

## 目录结构

| 路径 | 说明 |
|---|---|
| `Cpu0_Main.c` | SBL 入口（watchdog 处理后进 `SBL_boot()`，不返回） |
| `sbl/sbl_boot.[ch]` | §5.1 引导决策落地：元数据加载→决策→LED 信号→跳槽/安全态 |
| `bsp/flash_ota.[ch]` | IfxFlash 封装：槽扇区擦除、32B 页 staging 写、DFlash 元数据后端、入口探测 |
| `mw/ota/` | 可移植 OTA 栈（纯 C99，host/TriCore 同源编译）：`ota_layout.h` 地址真源、`ota_meta` 双页、`ota_boot` 决策、`tcfw_bundle` 验签、`ota_rx` 接收状态机、`crc32`、`ota_keys.h` 公钥 |
| `mw/crypto/` | `ed25519v`/`sha512`/`c6_consts` — 自 esp32c6_car 逐字拷贝（验签与 C6 共用真源） |
| `mw/sf/sf_frame.h` | SF 帧编码头 — 自 tc275_car 逐字拷贝（线格式真源） |
| `test/host/sf_frame.c` | SF 帧编解码实现 — 同样逐字拷贝；只由 host 测试编译（tc275_car 有自己的正本，SBL 镜像不引用） |
| `Lcf_SBL.lsl` | **本工程构建用**：SBL 定位 32 KB（0x80000000..0x80007FFF） |
| `Lcf_AppA.lsl` / `Lcf_AppB.lsl` | App 槽 linker（给 tc275_car 工程切换构建；App 不占物理 reset） |
| `test/host/` | host 单测 + mock 后端 + `make check` |
| `tools/` | `flash.py`（DAS/AURIXFlasher 烧录）、`merge_hex.py`（SBL+App 整包合成）、`gen_test_vectors.py`（TCFW 测试向量） |
| `SConstruct`、`site_scons/` | SCons 命令行构建（与 ADS 工程同源：解析 `.cproject`，详见 SConstruct 头注释） |
| `Cpu1/2_Main.c` | 模板遗留（SBL 不启动 CPU1/2，保留以提供 g_cpuSyncEvent 同步事件） |
| `sbl_led.c/h` | 原 Blinky_LED 模板改名；LED 决策指示/安全模式闪烁在用（Cpu0 调 initLED） |
| `Libraries/`、`Configurations/` | Infineon iLLD（TC27D）与芯片配置 |
| `doc/` | 设计文档 |

## 分区与地址（mw/ota/ota_layout.h 为真源）

| 区域 | 范围（cached） | 大小 | 说明 |
|---|---|---|---|
| SBL | 0x80000000..0x80007FFF | 32 KB | 持有 reset 向量，永不参与 OTA |
| Slot A | 0x80008000..0x801FFFFF | 2040 KB | PF0 S2..S26，出厂镜像 |
| Slot B | 0x80208000..0x803FFFFF | 2040 KB | PF1 S2..S26，OTA 目标槽（镜像对称） |
| OtaMeta | DFlash0 0xAF01A000 / 0xAF01C000 | 2×8 KB | 双页提交；扇区 15 归 tc275_car calib |

槽入口 = 槽基址 + 0x20（App 的 `.start` 段，镜像 BMHD 约定），SBL 跳该地址。

## 构建

**ADS（正式）**：导入本工程直接 build —— `.cproject` 已指向 `Lcf_SBL.lsl`，
新目录（`sbl/ bsp/ mw/`）会自动纳入构建；`test/ tools/ doc/` 已从目标构建排除。
编译器 include 路径的第一项是工程根 `${ProjDirPath}`（`mw/...`、`bsp/...`
这类仓库根相对包含依赖它——若 IDE 里工程是改 `.cproject` 前导入的，确认
Project Properties → C/C++ Build → Compiler → Include paths 里能看到该条）。

## 固件版本（mw/app_version.h 为唯一真源）

`mw/app_version.[ch]` 定义 SemVer（当前 0.1.0）。发布流程：bump 宏 → 提交 →
打同名 git tag（如 `v0.1.0`）。SBL 无 UART 输出，版本经产物可见：

- SCons（命令行构建）产物名自动带版本：`build/tasking-debug/tc275_sbl_v0.1.0.elf/.hex/.map`；
- 产物内可检索：`strings tc275_sbl_v0.1.0.elf | grep SBLFW`（烧到板上后调试器扫内存同样可见，
  Cpu0_Main 的 volatile 读锚点保证链接期死码消除不剔除该串）；
- **固定地址 0x80007E00**：`.sbl_version` 组由 `Lcf_SBL.lsl` 定死在 32KB SBL 区尾部
  （`#pragma section farrom "sbl_version"` 声明），App 从该地址直读 SBL 版本（见下）。


**SBL+App 整包**（工厂/调试器一次烧录）：

```sh
# 两边各自构建出 hex 后（SCons 产物名带版本，版本号见 mw/app_version.h）：
python tools/merge_hex.py build/tasking-debug/factory_full.hex        build/tasking-debug/tc275_sbl_v0.1.0.hex        "../tc275_car/TriCore Debug (TASKING)/tc275_car.hex"
# 可选 --bin build/tasking-debug/factory_full.bin 输出整片二进制（空隙 0xFF 填充）
```

两个镜像各自独立链接（各有 CStart/库），在 Intel-HEX 层合并；工具校验
地址重叠（重叠即报错拒绝），合并后入口 = SBL 的 0x80000020。

**命令行烧录**（`tools/flash.py`，封装 ADS 自带 AURIXFlasher 的 CLI，
需 DAS 服务在跑——ADS 装好即有）：

```sh
python tools/flash.py devices                                   # 支持的器件列表
python tools/flash.py flash Debug/tc275_sbl.hex                  # 单独烧 SBL
python tools/flash.py factory "../tc275_car/TriCore Debug (TASKING)/tc275_car.hex"
                                                                # 合成+整包烧录一步到位
```

默认 `-erase on`（只擦镜像覆盖的逻辑扇区——烧 SBL/App 不会误清另一个
OTA 槽）、烧后校验并复位运行；`--id <n>` 选 DAS 端口，`--log x.xml` 出
详细日志；底层原始参数（`-connect 0|6`、`-ucb`、TAS `-script` 等）见
`python tools/flash.py --help` 与 AURIXFlasher 的用法。

**命令行（SCons，自动化唯一入口）**：`python -m SCons`（需本机完整版
TASKING v6.3r1，ADS 内置版许可禁止 IDE 外运行）。源集/include/宏直接解析
`.cproject`，与 ADS 同源零漂移；产物在 `build/tasking-<cfg>/`（已 gitignore），
文件名自动携带版本（`tc275_sbl_vX.Y.Z.elf/.hex/.map`）。用法：`scons cfg=release`、
`scons opt=-O2`、`scons size`、`scons -c`（详见 SConstruct 头注释）。

**host 单测**：

```sh
cd test/host
make check        # CC 默认 C:/msys64/ucrt64/bin/gcc，可 CC=gcc 覆盖
```

测试向量由 `python tools/gen_test_vectors.py` 生成（依赖 esp32c6_car 的
ed25519 参考实现与 dev 密钥；`test_vectors.h` 已提交，格式或密钥变更时重生成）。

## 首次上板步骤（G-OTA-3 起）

1. ADS 分别构建 SBL 与 tc275_car（槽 A 布局，产物即 AppA hex）
2. 合成整包 `Debug/factory_full.hex`（SBL@0x80000000 + AppA@0x80008000，
   见上节），调试器/AURIXFlasher 一次烧录即可；也可分开烧两个 hex
3. 上电：无元数据时 SBL 探测 Slot A 入口非擦除态 → 闪 1 下 → 跳 A
   （`SBL_ALLOW_FIRST_BOOT`，首次 OTA 后即由元数据接管）
4. App 内接入 `OTARX_init` + 自检 `OTABOOT_confirmSelftest`，C6 推 TCFW 走 §5.3
5. 验证 SWAP → 重启进 B → 自检 VALID；断电/坏包/不自检三种回滚路径

## 开发环境

- AURIX Development Studio 1.10.36+（内含 TASKING；本机另有完整版 v6.3r1 用于命令行验证）
- 目标芯片：Infineon AURIX TC275（TC27xTP D-Step，三核 200 MHz，2×2 MB PFlash 双 bank）
- host 测试：MSYS2 ucrt64 gcc（或任意 C99 编译器）+ Python 3
