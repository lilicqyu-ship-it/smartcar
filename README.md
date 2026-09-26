# AURIX SmartDrive（myCar）

以 **Infineon TC275**（KIT-AURIX-TC275-LITE）为实时运动控制核心、**ESP32-C6**（DevKitC-1 V1.2）为 Wi-Fi 通信模块、**TB6612 四路驱动板（轮趣 D24A）**驱动 4 个 MG310 直流减速电机的智能双轮差速小车。手机通过 Wi-Fi 连接小车的 AP，用网页或自定义二进制协议下发运动指令。

> **两条线并存，别混淆**：本仓库**当前代码**是 demo 现状（C6 跑官方 esp-at、板间走 UART 115200、开环 PWM）；**量产设计基准**是 [doc/production-software-design.md](doc/production-software-design.md)（SDD V1.2：C6 自研固件、板间 SPI + SF 帧、编码器闭环、OTA/产测/安全）。工程与文档改动以 SDD 为准，本文其余章节描述的是现状。

核心设计原则：**通信与控制解耦、按实时特性分核、安全逻辑只在一侧** —— ESP32-C6 只做通信（不碰电机，挂死最多导致停车）；TC275 内部三核分区：CPU0（FreeRTOS）跑控制任务与安全状态机、CPU1（裸机 1 kHz）跑电机算法、CPU2（裸机）跑板间链路，核间通过 `Middleware/xcore` 共享内存交换命令、目标与状态。

## 系统架构（当前代码 / demo）

```
 手机/PC ── Wi-Fi(AP: AURIX-SmartDrive) ──► ESP32-C6-DevKitC-1 (esp-at, 透明TCP)
                                              │ UART1 115200 (AT + 透传, P15.0/P15.1)
                                              │   量产主链路改 SPI：QSPI3 主 ↔ C6 SPI2 从
                                              │   P33.11/12/13 + P23.4 CS + P23.0 IRQ（wiring.md §9）
                                              ▼
 ┌───────────────────────── TC275 三核分区 ─────────────────────────┐
 │ CPU2 (裸机)        Middleware/wifi_at.c                          │
 │   softAP+TCP 服务 · AT 收发 · +IPD 帧解码 · HTTP 控制页          │
 │        │ xcore 命令队列（急停另有直达 CPU1 的旁路位）            │
 │        ▼                                                         │
 │ CPU0 (FreeRTOS)    App/robot.c + 控制任务 (10 ms)                │
 │   命令执行 · 心跳超时/故障锁存 · 状态发布 · 调试串口 ASCLIN0     │
 │        │ xcore 电机目标（左右 -1000..+1000 + 急停位 + seq）      │
 │        ▼                                                         │
 │ CPU1 (裸机 1 kHz)  App/motor_algo.c                              │
 │   斜率限幅 · 失联看门狗 · 急停立即刹车                           │
 │        │                                                         │
 │        ▼ Bsp/motor.c → GTM ATOM PWM 20 kHz + 方向 GPIO           │
 └──────────────────────────────────────────────────────────────────┘
                                    │ PWM/DIR
                                    ▼
                    D24A 四路驱动板 (J4=电机A/B · J6=电机C/D) ── 4×MG310
                                    │ 编码器 E1~E4（§接线图）── 规划接入 GTM TIM，见 wiring.md §8
```

## 文档导航

**基准 = SDD**：其余文档与它冲突时以 SDD 为准。

| 文档 | 定位 | 内容 |
|---|---|---|
| [doc/production-software-design.md](doc/production-software-design.md) | **设计基准 V1.2** | 量产版软件设计文档：三核分区/闭环控制/OTA/安全/信息安全/产测/质量工程/里程碑/风险，§18 是工程级实现约束（向量表、ISR 优先级等铁律） |
| [doc/spi-link-design.md](doc/spi-link-design.md) | SDD 的展开 | 板间链路换向 UART→SPI 详细设计：接线表、`spi_slave_hd` 事务模型、SF 帧、两固件改动清单、台架门禁 G1–G6 |
| [doc/wiring.md](doc/wiring.md) | **接线真源 V1.3** | TC275 ↔ ESP32-C6 ↔ D24A 四路驱动板 ↔ 电机/编码器 ↔ 电源 完整引脚表（§2 UART 调试/回退、§8 编码器、§9 SPI 主链路） |
| [doc/requirement.md](doc/requirement.md) | 需求层 V1.2 | 产品定义、功能列表、版本路线 |
| [doc/architecture.md](doc/architecture.md) | **现状参考（demo）** | 当前代码的软件分层、任务/中断、协议、HTTP API、引脚映射 |
| [doc/evaluation-report.md](doc/evaluation-report.md) | 问题基线 | demo 的架构/安全/健壮性/工程化评估（P0~P3 已作为约束吸收进 SDD） |
| [doc/getting-started.md](doc/getting-started.md) | 教程 | 环境搭建 → 编译烧录 → 接线 → 手机遥控全流程（esp-at + UART 通路） |

已删除的历史文档：`ux-performance-plan.md`（卡顿根因与 Track B 方案，结论全部并入 SDD §3.5/§11/§14）、`esp32c6-fw-design.md` 与 `esp32c6-fw-coding-plan.md`（C6 固件 LLDD，已由同级工程 `c6_car/doc/` 承载）。

## 目录结构

```
myCar/
├── Cpu0_Main.c            # CPU0 入口：XCORE 初始化 + FreeRTOS 任务（blinky/echo/robot 控制任务）
├── Cpu1_Main.c            # CPU1 入口：MOTOR_ALGO_run() 1 kHz 裸机超循环（电机算法）
├── Cpu2_Main.c            # CPU2 入口：WIFI_main() 裸机超循环（ESP32-C6 AT 链路）
├── App/                   # 应用层：robot 运动状态机(CPU0)、motor_algo 电机算法(CPU1)
├── Middleware/            # 中间件：protocol 解码(CPU2)/执行(CPU0)、xcore 跨核共享内存、wifi_at AT 驱动(CPU2)
├── Bsp/                   # 板级驱动：uart 调试串口(CPU0)、motor GTM PWM+D24A(CPU1)、stime 毫秒时基(CPU1/CPU2)
├── Configurations/        # FreeRTOSConfig.h、Ifx_Cfg.h
├── FreeRtos/              # FreeRTOS 内核源码（TriCore 移植，仅 CPU0 运行）
├── Libraries/             # Infineon iLLD 驱动库
└── doc/                   # 本项目文档
```

## 硬件清单

| 器件 | 型号 | 说明 |
|---|---|---|
| 主控 | KIT-AURIX-TC275-LITE | TriCore 三核 TC275，115200 调试串口见接线图 |
| Wi-Fi | ESP32-C6-DevKitC-1 V1.2 | 板载双 Type-C + 5V→3.3V LDO，烧录 esp-at 固件 |
| 电机驱动 | TB6612 四路驱动稳压模块（轮趣 D24A） | 单板驱动 4 电机；编码器信号经 J4（E1/E2）、J6（E3/E4）引出 |
| 电源 | 2S~3S 电池 + DC-DC 5V ≥1A | 5V 轨同时供 TC275 与 C6（详见接线图 §5） |
| 电机 | 轮趣 MG310 直流减速电机 ×4 | 内置 260 线霍尔 AB 编码器，减速比 1:20.409；差速双轮布局，每侧 2 电机并联驱动逻辑 |

## 快速上手（摘要）

```bash
# 1. AURIX Development Studio 导入本工程，构建 "TriCore Debug (TASKING)"
# 2. USB 连接 TC275 kit，启动调试器烧录
# 3. ESP32-C6 刷入 esp-at 固件（esp-at 工程目录，target=esp32c6）
# 4. 按 doc/wiring.md 接线并上电
# 5. 手机连 Wi-Fi "AURIX-SmartDrive"（密码 12345678），
#    浏览器打开 http://192.168.4.1:8080 即可遥控
```

详细步骤、验证方法与故障排查见 [doc/getting-started.md](doc/getting-started.md)。

## 通信协议一览（当前代码：demo 自定义二进制帧）

```
| AA | 55 | CMD | LEN | DATA[LEN] | CRC |     CRC = CMD^LEN^DATA 逐字节异或
```

命令：`0x01` STOP、`0x02` FORWARD、`0x03` BACKWARD、`0x04/0x05` LEFT/RIGHT、`0x06/0x07` 弧线、`0x08/0x09` 原地旋、`0x10` SET_SPEED、`0x20` GET_STATUS、`0x21` HEARTBEAT（50 ms 周期、100 ms 超时自动停车）、`0x32` 急停。完整定义见 [doc/architecture.md](doc/architecture.md)。

> 量产协议为**两段两帧**（SDD §6）：手机 WS 段 = v2 帧（`AA 55 VER CMD SEQ LEN DATA CRC16`），板间 LINK 段 = **SF 帧**（SPI 专用，CRC16 + SEQ 分片）；命令语义集合与上表兼容，帧编码不再继承 demo。

## 当前状态与已知事项

- ✅ 三核分区（CPU0 控制 / CPU1 电机算法 / CPU2 WiFi）、双轮 4 电机驱动、FreeRTOS、UART 调试输出、AP + HTTP 控制页、心跳超时与急停保护
- ✅ Wi-Fi 模块已从 ESP8266 更换为 **ESP32-C6 esp-at**：驱动为 `Middleware/wifi_at.c`（CPU2 裸机超循环），AT 串口 P15.0/P15.1；跨核通信见 `Middleware/xcore`
- ⚠️ 迁移遗留项：esp-at 的 UART1 默认开启 RTS 流控，建议在 AT 初始化序列开头补发 `AT+UART_CUR=115200,8,1,0,0`（见 doc/wiring.md §2）
- ⚠️ `Middleware/wifi_at.c` 的 HTTP keep-alive 修复（控制页按键不灵敏根因）**待烧录验证**，未提交
- 📋 **板间链路 UART → SPI 已定案**（SDD V1.2 §3.7，接线表 wiring.md V1.3 §9，详细设计 spi-link-design.md）：TC275 QSPI3 主机 ↔ C6 SPI2 从机 + IRQ 握手，LINK 段换新 SF 帧；UART 保留为调试控制台与回退。**实施入口 = G1 台架门禁**（验证 AURIX QSPI 无 CMD/ADDR 前导相位能否被 Espressif HD 从机解析，风险 R7）
- 📋 C6 侧自研固件在同级工程 `c6_car/`（审查修复未提交）
- 📋 编码器接线方案已定稿**待确认实施**：MG310 内置 AB 编码器 → GTM0 TIM UDC 硬件正交计数，四对引脚集中在 X2-28~35（详见 wiring.md §8、SDD §5.1）
- 📋 版本路线与量产里程碑：requirement.md 路线图（编码器闭环 → IMU → 毫米波雷达）对应 SDD §15 M0–M4

## 构建

IDE 构建：AURIX Development Studio（TASKING TriCore 编译器），Debug 配置为 `TriCore Debug (TASKING)`，链接脚本 `Lcf_Gnuc_Tricore_Tc.lsl` / `Lcf_Tasking_Tricore_Tc.lsl`。本工程是 IDE 工作区工程（`.cproject`/`.project`），暂无独立命令行构建脚本。
