# AURIX SmartDrive（myCar）

[![CI](https://github.com/lilicqyu-ship-it/tc275_car/actions/workflows/ci.yml/badge.svg)](https://github.com/lilicqyu-ship-it/tc275_car/actions/workflows/ci.yml)

以 **Infineon TC275**（KIT-AURIX-TC275-LITE）为实时运动控制核心、**ESP32-C6**（DevKitC-1 V1.2）为 Wi-Fi 通信模块、**TB6612 四路驱动板（轮趣 D24A）**驱动 4 个 MG310 直流减速电机的智能双轮差速小车。手机通过 Wi-Fi 连接小车的 AP，用网页或自定义二进制协议下发运动指令。

> **两条线并存，别混淆**：本仓库**当前代码**是 demo 现状（C6 跑官方 esp-at、开环 PWM）；**量产设计基准**是 [doc/20-design/21-software-design.md](doc/20-design/21-software-design.md)（SDD V1.5：C6 自研固件、板间 SPI + SF 帧、编码器闭环、OTA/产测/安全）。工程与文档改动以 SDD 为准，本文其余章节描述的是现状。
>
> **板间链路口径（2026-09-26）**：**UART 作为板间链路已弃用，SPI 是唯一板间链路**——`TriCore Debug (TASKING)` 与 `TriCore Release (TASKING)` 两个构建配置都定义了 `USE_SPI_LINK`，日常构建产出的就是 SF-over-QSPI3 路径；`com/wifi_at.c` 的 AT/UART 分支只有手动删掉该符号才编得出（真源 `21 §5.6` 末条、`21 §18 C15`）。UART 那组线（P15.0/P15.1 ↔ GPIO6/7）**保留不拆**，只作 C6 调试控制台。下面的架构图与"快速上手"仍描述 demo/UART 通路，做教程要先删符号（步骤见 `doc/01-getting-started.md` 第 1 步）。
>
> **接线电气形态**：两板之间所有连线均为**杜邦线一对一直连，无任何外部元件**（无上拉/端接/电平转换），coding 后果见 `23-wiring.md §9.3`。

核心设计原则：**通信与控制解耦、按实时特性分核、安全逻辑只在一侧** —— ESP32-C6 只做通信（不碰电机，挂死最多导致停车）；TC275 内部三核分区：CPU0（FreeRTOS）跑控制任务与安全状态机、CPU1（裸机 1 kHz）跑电机算法、CPU2（裸机）跑板间链路，核间通过 `mw/xcore` 共享内存交换命令、目标与状态。

## 系统架构（当前代码 / demo）

```
 手机/PC ── Wi-Fi(AP: AURIX-SmartDrive) ──► ESP32-C6-DevKitC-1 (esp-at, 透明TCP)
                                              │ ★板间链路 = SPI（实物已接线，杜邦线直连无外部元件）：
                                              │   QSPI3 主 ↔ C6 SPI2 从，P33.11/12/13 + P23.4 CS + P23.0 IRQ
                                              │   （23-wiring.md §9.1/§9.3）；两个 TASKING 配置均定义
                                              │   USE_SPI_LINK → 构建产出的就是这条路径（启用待 G1 门禁）
                                              │ UART1 115200（P15.0/P15.1）：已不是板间链路，
                                              │   仅作 C6 调试控制台（下图 CPU2 行为删符号后的 demo 分支）
                                              ▼
 ┌───────────────────────── TC275 三核分区 ─────────────────────────┐
 │ CPU2 (裸机)        com/wifi_at.c（demo 分支，删 USE_SPI_LINK 才编出）│
 │   softAP+TCP 服务 · AT 收发 · +IPD 帧解码 · HTTP 控制页          │
 │        │ xcore 命令队列（急停另有直达 CPU1 的旁路位）            │
 │        ▼                                                         │
 │ CPU0 (FreeRTOS)    app/robot.c + 控制任务 (10 ms)                │
 │   命令执行 · 心跳超时/故障锁存 · 状态发布 · 调试串口 ASCLIN0     │
 │        │ xcore 电机目标（左右 -1000..+1000 + 急停位 + seq）      │
 │        ▼                                                         │
 │ CPU1 (裸机 1 kHz)  rt/motor_algo.c                               │
 │   斜率限幅 · 失联看门狗 · 急停立即刹车                           │
 │        │                                                         │
 │        ▼ bsp/motor.c → GTM ATOM PWM 20 kHz + 方向 GPIO           │
 └──────────────────────────────────────────────────────────────────┘
                                    │ PWM/DIR
                                    ▼
                    D24A 四路驱动板 (J4=电机A/B · J6=电机C/D) ── 4×MG310
                                    │ 编码器 E1~E4（§接线图）── 规划接入 GTM TIM，见 23-wiring.md §8
```

## 文档导航

完整文档地图（含阅读顺序、谁是真源、缺口清单）见 **[doc/00-index.md](doc/00-index.md)**。命名规则：`编号-域名`，十位段 = 归属域（10 产品 / 20 设计与硬件 / 30 TC275 / 40 ESP32-C6），**基准 = 21-software-design.md（SDD）**，其余与它冲突时以 SDD 为准。

| 域 | 文档 | 定位 |
|---|---|---|
| 入口 | [doc/00-index.md](doc/00-index.md) · [doc/01-getting-started.md](doc/01-getting-started.md) | 文档地图 · 整机 bring-up 教程（两板烧录 → 接线 → 手机遥控；**做本教程要临时删 `USE_SPI_LINK`**，UART 链路已弃用） |
| 10 产品 | [11-requirements.md](doc/10-product/11-requirements.md) · [12-demo-evaluation.md](doc/10-product/12-demo-evaluation.md) | 需求说明书 V1.3 · demo 评估报告（问题基线，冻结快照） |
| 20 设计/硬件 | [21-software-design.md](doc/20-design/21-software-design.md) · [22-link-spi-design.md](doc/20-design/22-link-spi-design.md) · [23-wiring.md](doc/20-design/23-wiring.md) | **★量产 SDD V1.5（基准，含 §18 工程级实现约束 C1~C15）** · 板间 SPI/SF 链路详细设计 V1.5 · **接线真源 V1.10**（§9.3 = 杜邦线直连无外部元件） |
| 30 TC275 | [31-firmware-architecture.md](doc/30-tc275/31-firmware-architecture.md) · [32-tc275-dev-guide.md](doc/30-tc275/32-tc275-dev-guide.md) | 当前代码（demo）架构参考 V2.1 · **TC275 开发指南 V1.3**（新增中断/跨核消息/构建烧录/排障） |
| 40 ESP32-C6 | [41-c6-docs-map.md](doc/40-esp32c6/41-c6-docs-map.md) | C6 侧文档地图：本仓库负责的接口真源 + 指向 `c6_car/doc/` 的 14 篇 LLDD |

已删除的历史文档：`ux-performance-plan.md`（卡顿根因与 Track B 方案，结论全部并入 SDD §3.5/§11/§14）、`esp32c6-fw-design.md` 与 `esp32c6-fw-coding-plan.md`（C6 固件 LLDD，已由同级工程 `c6_car/doc/` 承载）。

## 目录结构

2026-09-26 起按 SDD §3.4 目标态目录组织（demo 布局 `App/ Middleware/ Bsp/` 已重排；`app/robot.c→mission+drive_policy`、`rt/motor_algo→servo` 等文件级拆分仍属后续里程碑）：

```
myCar/
├── Cpu0_Main.c            # CPU0 入口：XCORE 初始化 + FreeRTOS 任务（blinky/echo/robot 控制任务）
├── Cpu1_Main.c            # CPU1 入口：MOTOR_ALGO_run() 1 kHz 裸机超循环（电机算法）
├── Cpu2_Main.c            # CPU2 入口：板间链路裸机超循环（默认 = USE_SPI_LINK 的 QSPI3 SF 帧泵；删符号才走 wifi_at AT/UART）
├── app/                   # 应用服务层（CPU0）：robot 运动状态机（目标态拆分 mission + drive_policy）
├── rt/                    # 实时域（CPU1）：motor_algo 电机算法、encoder 霍尔编码器 ×4 测速
├── com/                   # 通信域（CPU2）：link QSPI3 SF 帧主机泵、spi_hal_pins 引脚与定长事务、wifi_at AT/UART 链路（已弃用分支）
├── mw/                    # 中间件：xcore 跨核共享内存、proto 命令码表/v2 帧、sf SF 帧编解码
├── bsp/                   # 板级驱动：uart 调试串口(CPU0)、motor GTM PWM+D24A(CPU1)、stime 毫秒时基(CPU1/CPU2)
├── Configurations/        # FreeRTOSConfig.h、Ifx_Cfg.h
├── FreeRtos/              # FreeRTOS 内核源码（TriCore 移植，仅 CPU0 运行）
├── Libraries/             # Infineon iLLD 驱动库
├── test/host/             # 主机端单元测试（SF 帧/遥测契约，gcc 直编，见 32-tc275-dev-guide.md）
└── doc/                   # 本项目文档
```

## 硬件清单

| 器件 | 型号 | 说明 |
|---|---|---|
| 主控 | KIT-AURIX-TC275-LITE | TriCore 三核 TC275，115200 调试串口见接线图 |
| Wi-Fi | ESP32-C6-DevKitC-1 V1.2 | 板载双 Type-C + 5V→3.3V LDO；量产固件为自研 `c6_car`（SPI 从机），esp-at 只作应急返修镜像 |
| 电机驱动 | TB6612 四路驱动稳压模块（轮趣 D24A） | 单板驱动 4 电机；编码器信号经 J4（E1/E2）、J6（E3/E4）引出 |
| 电源 | 2S~3S 电池 + DC-DC 5V ≥1A | 5V 轨同时供 TC275 与 C6（详见接线图 §5） |
| 电机 | 轮趣 MG310 直流减速电机 ×4 | 内置 260 线霍尔 AB 编码器，减速比 1:20.409；差速双轮布局，每侧 2 电机并联驱动逻辑 |

## 快速上手（摘要）

```bash
# 0. 本快速上手走 demo/UART 通路：先在工程属性里临时删除 Debug 与 Release
#    两个 TASKING 配置的 USE_SPI_LINK（否则镜像里没有 AT/UART 通路，做完加回）
# 1. AURIX Development Studio 导入本工程，构建 "TriCore Debug (TASKING)"
# 2. USB 连接 TC275 kit，启动调试器烧录
# 3. ESP32-C6 刷入 esp-at 固件（esp-at 工程目录，target=esp32c6）
# 4. 按 doc/20-design/23-wiring.md 接线并上电
# 5. 手机连 Wi-Fi "AURIX-SmartDrive"（密码 12345678），
#    浏览器打开 http://192.168.4.1:8080 即可遥控
```

详细步骤、验证方法与故障排查见 [doc/01-getting-started.md](doc/01-getting-started.md)。

## 通信协议一览（当前代码：demo 自定义二进制帧）

```
| AA | 55 | CMD | LEN | DATA[LEN] | CRC |     CRC = CMD^LEN^DATA 逐字节异或
```

命令：`0x01` STOP、`0x02` FORWARD、`0x03` BACKWARD、`0x04/0x05` LEFT/RIGHT、`0x06/0x07` 弧线、`0x08/0x09` 原地旋、`0x10` SET_SPEED、`0x20` GET_STATUS、`0x21` HEARTBEAT（50 ms 周期、100 ms 超时自动停车）、`0x32` 急停。完整定义见 [doc/30-tc275/31-firmware-architecture.md](doc/30-tc275/31-firmware-architecture.md)。

> 量产协议为**两段两帧**（SDD §6）：手机 WS 段 = v2 帧（`AA 55 VER CMD SEQ LEN DATA CRC16`），板间 LINK 段 = **SF 帧**（SPI 专用，CRC16 + SEQ 分片）；命令语义集合与上表兼容，帧编码不再继承 demo。

## 当前状态与已知事项

- ✅ 三核分区（CPU0 控制 / CPU1 电机算法 / CPU2 WiFi）、双轮 4 电机驱动、FreeRTOS、UART 调试输出、AP + HTTP 控制页、心跳超时与急停保护
- ✅ Wi-Fi 模块已从 ESP8266 更换为 **ESP32-C6 esp-at**：驱动为 `com/wifi_at.c`（CPU2 裸机超循环），AT 串口 P15.0/P15.1；跨核通信见 `mw/xcore`。**该通路已于 2026-09-26 从默认构建里移出**（UART 板间链路弃用，删 `USE_SPI_LINK` 才编得出）
- ✅ **目录已重排为 SDD §3.4 目标态**（2026-09-26）：`App/Middleware/Bsp` → `app/ rt/ com/ mw/ bsp/`，include 改为模块限定路径（如 `"com/link.h"`、`"mw/sf/sf_frame.h"`），`.cproject` 四个构建配置同步
- ✅ **TC275 侧 SPI 链路代码落地并构建链接闭合**（2026-09-26，Debug 配置 0 错误）：`com/spi_hal_pins.c`（QSPI3 主机 + P23.0 电平采样）、`com/link.c`（握手寄存器 + 事务泵 + 38 B 遥测）、`mw/sf/`（SF 帧与遥测 codec）、挂在 `Cpu2_Main.c` 的 `#ifdef USE_SPI_LINK`；主机单测两份在 `test/host/`（帧层 2855 断言 + 遥测层 154 断言，后者编入从机解码器交叉验证）
- ⚠️ 迁移遗留项（仅影响删符号后的 demo/UART 分支）：esp-at 的 UART1 默认开启 RTS 流控，建议在 AT 初始化序列开头补发 `AT+UART_CUR=115200,8,1,0,0`（见 [23-wiring.md](doc/20-design/23-wiring.md) §2）
- ⚠️ `com/wifi_at.c` 的 HTTP keep-alive 修复（控制页按键不灵敏根因）**待烧录验证**，未提交
- 📋 **板间链路 = SPI 单链路**（SDD V1.5 §3.7/§5.6，接线表 23-wiring.md V1.10 §9.1 + §9.3，详细设计 22-link-spi-design.md V1.5）：TC275 QSPI3 主机 ↔ C6 SPI2 从机（`spi_slave_hd`）+ P23.0 电平握手，LINK 段 SF 帧。**实物接线（五行 + 共地）与两侧固件代码均已完成，且 `USE_SPI_LINK` 已进 Debug/Release 两个 TASKING 配置**；IRQ 无外部上拉（高电平靠 TC275 片内上拉，`23 §9.3`）。**唯一未完成的主线 = 从未通电联调，G1 台架波形门禁未过**（验证 AURIX QSPI 无 CMD/ADDR 前导相位能否被 Espressif HD 从机解析，风险 R7）；G1 失败的退路代价已升高（删符号重编 + C6 重刷 esp-at，双侧动作）
- 📋 C6 侧自研固件在同级工程 `c6_car/`（审查修复未提交）
- 📋 **编码器：固件已落地，8 线待接**：`rt/encoder.c`（CPU1）用 GTM TIM0 八通道 TIEM 双边沿中断 + 软件 ×4 正交（该器件上 UDC 硬件正交计数不可用，见 23-wiring.md §8.3 的纠错），接线方案 P33.0~7 ↔ X2-28~35；接好后可拉取实测速度（详见 23-wiring.md §8、SDD §5.1）
- 📋 版本路线与量产里程碑：11-requirements.md 路线图（编码器闭环 → IMU → 毫米波雷达）对应 SDD §15 M0–M4

## 构建

IDE 构建：AURIX Development Studio（TASKING TriCore 编译器），Debug 配置为 `TriCore Debug (TASKING)`，链接脚本 `Lcf_Gnuc_Tricore_Tc.lsl` / `Lcf_Tasking_Tricore_Tc.lsl`。本工程是 IDE 工作区工程（`.cproject`/`.project`），暂无独立命令行构建脚本。
