# AURIX SmartDrive（myCar）

以 **Infineon TC275**（KIT-AURIX-TC275-LITE）为实时运动控制核心、**ESP32-C6**（DevKitC-1 V1.2，运行官方 esp-at AT 固件）为 Wi-Fi 通信模块、**2×TB6612** 驱动 4 个直流电机的智能双轮差速小车。手机通过 Wi-Fi 连接小车的 AP，用网页或自定义二进制协议下发运动指令。

核心设计原则：**通信与控制解耦** —— ESP32-C6 只做 AT 透传，协议解析、运动状态机、PWM、安全保护全部在 TC275 上跑（FreeRTOS）。

## 系统架构

```
 手机/PC ── Wi-Fi(AP: AURIX-SmartDrive) ──► ESP32-C6-DevKitC-1 (esp-at, 透明TCP)
                                              │ UART1 115200 (AT + 透传)
                                              ▼
                                    TC275 (FreeRTOS, CPU0)
                                     ├── ESP8266/AT 桥任务  Middleware/esp8266.c
                                     ├── 二进制协议解析      Middleware/protocol.c
                                     ├── 运动状态机+安全     App/robot.c
                                     ├── GTM PWM 20kHz+GPIO  Bsp/motor.c
                                     └── 调试日志 UART       Bsp/uart.c
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
├── Cpu0_Main.c            # CPU0 入口：初始化 + 创建 FreeRTOS 任务
├── Cpu1_Main.c / Cpu2_Main.c  # CPU1/CPU2 入口（当前仅核间同步）
├── App/                   # 应用层：机器人状态机、运动控制、心跳超时保护
├── Middleware/            # 中间件：自定义二进制协议、ESP AT 透传桥 + HTTP 服务
├── Bsp/                   # 板级驱动：调试串口(ASCLIN0)、TB6612 电机(GTM ATOM PWM)
├── Configurations/        # FreeRTOSConfig.h、Ifx_Cfg.h
├── FreeRtos/              # FreeRTOS 内核源码（TriCore 移植）
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

- ✅ 双轮 4 电机驱动、FreeRTOS、UART 调试输出、AP + HTTP 控制页、心跳超时保护
- 🔶 Wi-Fi 模块已从 ESP8266 更换为 **ESP32-C6 esp-at**：文档与接线图已更新（V1.1），代码仍沿用 `Middleware/esp8266.c` 旧命名；两者 AT 指令集兼容，可直接工作
- ⚠️ 迁移遗留项：esp-at 的 UART1 默认开启 RTS 流控，建议在 AT 初始化序列开头补发 `AT+UART_CUR=115200,8,1,0,0`（见 doc/wiring.md §2）
- 📋 路线图：V1.1 编码器闭环 → V2.0 IMU → V3.0 毫米波雷达（见 requirement.md §33）

## 构建

IDE 构建：AURIX Development Studio（TASKING TriCore 编译器），Debug 配置为 `TriCore Debug (TASKING)`，链接脚本 `Lcf_Gnuc_Tricore_Tc.lsl` / `Lcf_Tasking_Tricore_Tc.lsl`。本工程是 IDE 工作区工程（`.cproject`/`.project`），暂无独立命令行构建脚本。
