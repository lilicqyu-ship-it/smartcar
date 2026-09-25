# AURIX SmartDrive（myCar）

以 **Infineon TC275**（KIT-AURIX-TC275-LITE）为实时运动控制核心、**ESP32-C6**（DevKitC-1 V1.2，运行官方 esp-at AT 固件）为 Wi-Fi 通信模块、**2×TB6612** 驱动 4 个直流电机的智能双轮差速小车。手机通过 Wi-Fi 连接小车的 AP，用网页或自定义二进制协议下发运动指令。

核心设计原则：**通信与控制解耦、按实时特性分核** —— ESP32-C6 只做 AT 透传；TC275 内部三核分区：CPU0（FreeRTOS）跑控制任务与安全状态机、CPU1（裸机 1 kHz）跑电机算法、CPU2（裸机）跑 WiFi 链路，核间通过 `Middleware/xcore` 共享内存交换命令、目标与状态。

## 系统架构

```
 手机/PC ── Wi-Fi(AP: AURIX-SmartDrive) ──► ESP32-C6-DevKitC-1 (esp-at, 透明TCP)
                                              │ UART1 115200 (AT + 透传, P15.0/P15.1)
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
                          TB6612 #1 (电机A/B) ── 同侧两轮
                          TB6612 #2 (电机C/D) ── 另一侧两轮
```

## 文档导航

| 文档 | 内容 |
|---|---|
| [doc/getting-started.md](doc/getting-started.md) | **新手教程**：环境搭建 → 编译烧录 → 接线 → 手机遥控全流程 |
| [doc/architecture.md](doc/architecture.md) | **技术参考**：软件分层、任务/中断、通信协议、HTTP API、引脚映射 |
| [doc/requirement.md](doc/requirement.md) | **需求说明书 V1.1**：产品定义、功能列表、版本路线 |
| [doc/wiring.md](doc/wiring.md) | **接线图**：TC275 ↔ ESP32-C6 ↔ TB6612 ↔ 电机 ↔ 电源 完整引脚表 |

## 目录结构

```
myCar/
├── Cpu0_Main.c            # CPU0 入口：XCORE 初始化 + FreeRTOS 任务（blinky/echo/robot 控制任务）
├── Cpu1_Main.c            # CPU1 入口：MOTOR_ALGO_run() 1 kHz 裸机超循环（电机算法）
├── Cpu2_Main.c            # CPU2 入口：WIFI_main() 裸机超循环（ESP32-C6 AT 链路）
├── App/                   # 应用层：robot 运动状态机(CPU0)、motor_algo 电机算法(CPU1)
├── Middleware/            # 中间件：protocol 解码(CPU2)/执行(CPU0)、xcore 跨核共享内存、wifi_at AT 驱动(CPU2)
├── Bsp/                   # 板级驱动：uart 调试串口(CPU0)、motor GTM PWM+TB6612(CPU1)、stime 毫秒时基(CPU1/CPU2)
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
| 电机驱动 | TB6612FSG 模块 ×2 | 每块驱动同侧前/后两个电机 |
| 电源 | 2S~3S 电池 + DC-DC 5V ≥1A | 5V 轨同时供 TC275 与 C6（详见接线图 §5） |
| 电机 | 直流减速电机 ×4 | 差速双轮布局，每侧 2 电机并联驱动逻辑 |

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

## 通信协议一览（自定义二进制帧）

```
| AA | 55 | CMD | LEN | DATA[LEN] | CRC |     CRC = CMD^LEN^DATA 逐字节异或
```

命令：`0x01` STOP、`0x02` FORWARD、`0x03` BACKWARD、`0x04/0x05` LEFT/RIGHT、`0x06/0x07` 弧线、`0x08/0x09` 原地旋、`0x10` SET_SPEED、`0x20` GET_STATUS、`0x21` HEARTBEAT（50 ms 周期、100 ms 超时自动停车）、`0x32` 急停。完整定义见 [doc/architecture.md](doc/architecture.md)。

## 当前状态与已知事项

- ✅ 三核分区（CPU0 控制 / CPU1 电机算法 / CPU2 WiFi）、双轮 4 电机驱动、FreeRTOS、UART 调试输出、AP + HTTP 控制页、心跳超时与急停保护
- ✅ Wi-Fi 模块已从 ESP8266 更换为 **ESP32-C6 esp-at**：驱动为 `Middleware/wifi_at.c`（CPU2 裸机超循环），AT 串口 P15.0/P15.1；跨核通信见 `Middleware/xcore`
- ⚠️ 迁移遗留项：esp-at 的 UART1 默认开启 RTS 流控，建议在 AT 初始化序列开头补发 `AT+UART_CUR=115200,8,1,0,0`（见 doc/wiring.md §2）
- 📋 路线图：V1.1 编码器闭环 → V2.0 IMU → V3.0 毫米波雷达（见 requirement.md §33）

## 构建

IDE 构建：AURIX Development Studio（TASKING TriCore 编译器），Debug 配置为 `TriCore Debug (TASKING)`，链接脚本 `Lcf_Gnuc_Tricore_Tc.lsl` / `Lcf_Tasking_Tricore_Tc.lsl`。本工程是 IDE 工作区工程（`.cproject`/`.project`），暂无独立命令行构建脚本。
