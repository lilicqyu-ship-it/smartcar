AURIX SmartDrive V1.0 产品需求与功能设计

版本： V1.3
平台： KIT-AURIX-TC275-LITE + D24A 四路驱动板（双 TB6612 通道 A/B/C/D）+ ESP32-C6（自研固件，**板间链路唯一为 SPI**；esp-at 仅作 UART 应急返修镜像）+ 4×MG310 编码器减速电机（双轮差速布局）
产品定位： Wi-Fi 双轮智能运动控制平台

> 文档编号 **11** · 域 产品 · 状态：需求层（当前 V1.3） · 上级索引 [00-index.md](../00-index.md)

> 文档定位：本文件是**产品需求层**（做什么、验收标准）。软件设计基准是 [21-software-design.md](../20-design/21-software-design.md)（SDD，当前 V1.5）——两者冲突时以 SDD 为准，本文件只描述需求与范围。

变更记录：
V1.1 — Wi-Fi 模块由 ESP8266 更换为 ESP32-C6（运行 Espressif 官方 esp-at AT 固件，AT 指令集向下兼容 ESP8266），接线详见 23-wiring.md
V1.2 — **板间主链路 UART → SPI**（TC275 QSPI3 主机 ↔ C6 SPI2 从机，1 MHz 起 / 5 MHz 量产基线），UART 降级为调试控制台与回退通道；C6 固件改为自研（`esp32c6_car` 工程），esp-at 退居回退；链路帧分为两段：板间 SF 帧 + 手机 v2 帧。详见 SDD §3.7/§6 与 [22-link-spi-design.md](../20-design/22-link-spi-design.md)。文中 esp-at / UART 相关段落（§2 目标、§3 架构图、§5）描述的是 **V1.1 demo 现状**，保留作为回退通道与产线返工依据。
V1.3 — **UART 作为板间链路弃用（2026-09-26 用户决策）**：SPI 是**唯一**板间链路，TC275 的两个 TASKING 构建配置都只编 SPI 路径；UART/esp-at 只保留为 C6 调试控制台与 G1 失败时的**应急返修**（恢复要删 `USE_SPI_LINK` 重编 + C6 重刷 esp-at，双侧动作）。本文上述"回退通道"字样按此理解，验收与排障不再假设 UART 可用；决策依据见 SDD §5.6 末条与 §18 C15。

1. 产品概述

AURIX SmartDrive 是一个以 Infineon AURIX TC275 为实时运动控制核心、ESP32-C6 为 Wi-Fi 通信模块、D24A 四路驱动板（单板 4 通道 TB6612，J4=A/B、J6=C/D）驱动 4 个 MG310 减速电机的智能双轮差速运动控制平台（每侧 2 电机并联，差速转向）。

核心设计原则：

ESP32-C6 负责通信，TC275 负责实时控制，TB6612 负责功率驱动。

V1.0 的目标不是简单实现“手机控制电机”，而是建立一个具备：

通信协议
运动状态机
PWM 控制
安全保护
Watchdog
状态监控

的嵌入式产品级控制架构。

2. 产品目标

V1.0 实现以下核心能力：

手机/PC 通过 Wi-Fi 控制机器人
ESP32-C6 与 TC275 通过板间链路通信（V1.1 demo = UART 115200；V1.2 起量产主链路 = SPI；**V1.3 起 UART 作为板间链路弃用，只剩 C6 调试控制台**，见 SDD §3.7/§5.6、§18 C15）
TC275 独立控制左右两个电机
支持前进、后退、左右转向
支持原地旋转
支持弧线运动
支持 0~100% PWM 开环速度控制
建立自定义二进制通信协议
支持 Heartbeat 心跳机制
通信超时自动停车
TC275 Watchdog 安全保护
提供机器人状态监控
提供 Fault 状态
预留编码器、IMU、毫米波雷达和 CAN 扩展能力
3. 系统总体架构

> 下图是 **V1.0 demo 现状**（架构示意，一图看懂分层）。量产目标态（三段：手机 WS ↔ C6 ↔ TC275 三核 ↔ 闭环伺服，含 SF 帧与 OTA/产测通道）见 SDD §3.2/§3.5。

                 手机 / PC
                     │
                  Wi-Fi
                     │
                     ▼
              ┌─────────────┐
              │  ESP32-C6   │
              │ Wi-Fi 接入层 │
              └──────┬──────┘
                     │ SPI（V1.2 主链路；V1.0 demo 为 UART + esp-at）
                     ▼
          ┌─────────────────────┐
          │        TC275        │
          │                     │
          │  命令解析           │
          │  运动状态机         │
          │  运动控制           │
          │  PWM控制            │
          │  安全管理           │
          │  状态管理           │
          │  Watchdog           │
          └───────┬──────┬──────┘
                  │      │
                 PWM    GPIO
                  │      │
          ┌───────▼──────▼───────┐
          │       TB6612         │
          │    Motor Driver      │
          └───────┬──────┬───────┘
                  │      │
                左电机  右电机
4. 功能模块
ID	功能模块	V1.0
F01	Wi-Fi 连接	✅
F02	ESP32-C6 通信	✅
F03	双电机控制	✅
F04	基础运动控制	✅
F05	PWM 开环速度控制	✅
F06	运动状态机	✅
F07	自定义通信协议	✅
F08	通信超时保护	✅
F09	Watchdog	🔶 见注
F10	状态监控	✅
F11	Emergency Stop	✅
F12	编码器闭环	V1.1
F13	IMU	V2.0
F14	IWR6843 毫米波雷达	V3.0

> F09 说明：软件级看门狗已实现（F08 心跳 100 ms 超时停车、CPU1 电机算法 150 ms 目标失联保护、急停旁路）；CPU/安全**硬件**看门狗在调试期被显式关闭（Cpu0/1/2_Main.c），量产前必须重新启用并周期喂狗，详见 SDD §7.2（看门狗链）与 §18 C8。
5. Wi-Fi 功能设计
5.1 ESP32-C6 工作模式

V1.0 推荐使用 AP 模式。

量产（V1.2 起）：C6 运行**自研固件**（同级工程 `esp32c6_car/`，其模块级设计见 `esp32c6_car/doc/`），承载 softAP/STA + Captive Portal + HTTP/WS 服务，与 TC275 之间走 SPI + SF 帧；安全逻辑全部留在 TC275，C6 挂死只导致停车。

以下为 V1.0 demo 现状（esp-at 通道，保留作 R7 回退与产线返工）：

ESP32-C6 模组运行 Espressif 官方 esp-at AT 固件（本工程对应 `C:\Code\TC275\AURIX-v1.10.36-workspace\esp-at`，target=esp32c6，module_esp32c6_default）。硬件使用 **ESP32-C6-DevKitC-1 V1.2** 开发板（ESP32-C6-WROOM-1 模组，8 MB flash，板载 USB-UART 桥 + 原生 USB 双 Type-C、5V→3.3V LDO）。ESP32-C6 为 2.4 GHz Wi-Fi 6 芯片（支持 BLE 5 / 802.15.4），其 AT 固件兼容 ESP8266 AT 指令集，原有初始化流程可直接复用。AT 口引脚、板载资源占用与接线详见 23-wiring.md。

SSID:
AURIX-SmartDrive


Password:
12345678

工作方式：

手机
 │
 │ Wi-Fi (2.4G AP)
 ▼
ESP32-C6
 │ SPI 主链路（demo：UART1 AT 透传）
 ▼
TC275

不依赖：

路由器
Internet
云服务器

机器人可以独立运行。

5.2 AT 初始化流程（**V1.0 demo / esp-at 回退通道**，量产自研固件无此流程）

| 步骤 | AT 指令 | 说明 |
|---|---|---|
| 1 | ATE0 | 关闭回显 |
| 2 | AT+CWMODE=2 | SoftAP 模式 |
| 3 | AT+CWSAP="AURIX-SmartDrive","12345678",11,3 | 设置 AP：信道 11，加密 WPA2_PSK |
| 4 | AT+CIPMUX=1 | 允许多路 TCP 连接 |
| 5 | AT+CIPSERVER=1,8080 | 监听 TCP 8080 |
| 6 | +IPD\<id\>,\<len\> | 收到手机数据（透传上行） |
| 7 | AT+CIPSEND=\<id\>,\<len\> | 向手机发送数据（透传下行） |
| 8 | AT+CIPCLOSE=\<id\> | 关闭连接（HTTP 应答后） |

注意（与 ESP8266 的差异）：

- 串口参数：esp-at 的 AT 端口为 UART1，默认 115200 8N1，引脚接线见 23-wiring.md；ESP8266 使用 UART0。
- esp-at 固件 UART1 默认开启 RTS 流控（`CONFIG_AT_UART_DEFAULT_FLOW_CONTROL=1`），若不接线流控，需在下发初始化前执行 AT+UART_CUR=115200,8,1,0,0 关闭流控，或在 esp-at 编译时将其配置为 0。
- AT+CIPSEND 提示符、+IPD 帧格式与 ESP8266 一致，TC275 侧解析逻辑不变。
- 若启用 AT+SYSSTORE=1，AP 配置会保存到 NVS，重启后仍生效。
5.3 通信流程

V1.2 量产（两段两帧，详见 SDD §6）：

手机
 ↓ Wi-Fi + WebSocket（v2 帧，≤30 Hz 指令 / 20 ms 遥测）
ESP32-C6（自研固件：网络栈 + WS 服务 + bridge 字段映射）
 ↓ SPI + SF 帧（TC275 QSPI3 主机拉取，握手寄存器 + IRQ）
TC275（认证/心跳/安全状态机/闭环伺服全在此侧）

V1.0 demo（esp-at 透明 TCP，保留为回退与产线返工）：

手机
 ↓ Wi-Fi
ESP32-C6 (esp-at 透明 TCP)
 ↓ UART1 (AT + 透传数据)
TC275
 ├── Web Server (HTTP 页面/REST API 由 TC275 实现)
 └── 自定义二进制协议 (运动指令)
6. 手机控制界面

建议 V1.0 提供一个简单 Web UI。

┌─────────────────────────────┐
│      AURIX SmartDrive       │
├─────────────────────────────┤
│                             │
│             ↑               │
│                             │
│       ←    STOP    →        │
│                             │
│             ↓               │
│                             │
│ Speed                       │
│ ─────────●────────── 50%    │
│                             │
│ Connection      ONLINE      │
│ Robot State     FORWARD     │
│ Left Motor      +50%        │
│ Right Motor     +50%        │
│ Heartbeat       OK          │
│ Watchdog        OK          │
└─────────────────────────────┘
7. 双电机控制

系统定义两个独立电机：

Motor_Left
Motor_Right

每个电机包含三个核心控制量：

Direction
PWM Duty
Enable

目标速度范围：

-100 ~ +100

定义：

+100     正转最大速度
+50      正转 50%
0        停止
-50      反转 50%
-100     反转最大速度
8. 运动模式

V1.0 定义以下运动模式：

状态	左轮	右轮
STOP	0	0
FORWARD	+S	+S
BACKWARD	-S	-S
LEFT	-S	+S
RIGHT	+S	-S
FORWARD_LEFT	S×0.5	S
FORWARD_RIGHT	S	S×0.5
ROTATE_LEFT	-S	+S
ROTATE_RIGHT	+S	-S

其中 S 为目标速度。

9. 前进

例如：

Speed = 50%

TC275：

Left  = +50%
Right = +50%
10. 后退
Left  = -50%
Right = -50%
11. 原地旋转
左旋
Left  = -50%
Right = +50%
右旋
Left  = +50%
Right = -50%
12. 弧线运动

为了获得更好的运动体验，V1.0 建议加入差速弧线控制。

左弧线
Left  = 30%
Right = 60%
右弧线
Left  = 60%
Right = 30%

这样机器人不会突然改变方向，而是平滑转向。

13. PWM 速度控制

V1.0 暂时采用 PWM 开环控制。

速度范围：

0 ~ 100%

例如：

Speed = 50%
PWM Duty = 50%

需要特别明确：

V1.0 的 PWM 50% 并不代表电机实际转速为额定转速的 50%。

因为当前没有编码器，所以系统不知道电机的真实 RPM。

14. 为什么 V1.0 不使用 PID？

因为当前系统没有：

Motor
 ↓
Encoder
 ↓
TC275

因此无法获得真实速度反馈。

V1.0：

目标 PWM
   ↓
TB6612
   ↓
Motor

V1.1：

目标 RPM
   ↓
PID
   ↓
PWM
   ↓
Motor
   ↓
Encoder
   └────────► TC275
15. 运动状态机

建议使用状态机管理机器人。

             ┌──────────┐
             │   INIT   │
             └────┬─────┘
                  │
                  ▼
             ┌──────────┐
             │   IDLE   │
             └────┬─────┘
                  │
        ┌─────────┼─────────┐
        ▼         ▼         ▼
     FORWARD    BACKWARD   ROTATE
        │         │         │
        └─────────┼─────────┘
                  ▼
                STOP
                  ▲
                  │
            Fault/Timeout
16. 状态定义

建议：

ROBOT_INIT


ROBOT_IDLE


ROBOT_FORWARD
ROBOT_BACKWARD


ROBOT_LEFT
ROBOT_RIGHT


ROBOT_FORWARD_LEFT
ROBOT_FORWARD_RIGHT


ROBOT_ROTATE_LEFT
ROBOT_ROTATE_RIGHT


ROBOT_FAULT
17. 通信协议设计

ESP32-C6 与 TC275 不建议直接使用：

"FORWARD 50"

这种字符串协议。

建议采用自定义二进制协议。

18. 数据帧格式
┌────┬────┬─────┬─────┬──────────┬─────┐
│ AA │ 55 │ CMD │ LEN │   DATA   │ CRC │
└────┴────┴─────┴─────┴──────────┴─────┘

字段：

字段	长度	说明
Header1	1 Byte	0xAA
Header2	1 Byte	0x55
CMD	1 Byte	命令
LEN	1 Byte	数据长度
DATA	N Byte	数据
CRC	1/2 Byte	CRC 校验
19. 命令定义
CMD	功能
0x01	STOP
0x02	FORWARD
0x03	BACKWARD
0x04	LEFT
0x05	RIGHT
0x06	FORWARD_LEFT
0x07	FORWARD_RIGHT
0x08	ROTATE_LEFT
0x09	ROTATE_RIGHT
0x10	SET_SPEED
0x20	GET_STATUS
0x21	HEARTBEAT
0x30	RESET
0x31	CLEAR_FAULT
0x32	EMERGENCY_STOP
20. SET_SPEED

建议统一采用：

LeftSpeed
RightSpeed

表示运动。

范围：

-100 ~ +100

例如：

Left  = +50
Right = +50

表示前进。

Left  = -50
Right = +50

表示原地左旋。

这种设计可以统一表达：

前进
后退
左转
右转
原地旋转
弧线运动
21. Heartbeat

手机经 ESP32-C6 周期性发送：

HEARTBEAT

建议：

周期：50 ms

TC275 保存：

lastHeartbeatTick

并计算：

currentTick - lastHeartbeatTick
22. 通信超时保护

如果：

currentTick - lastHeartbeatTick > 100 ms

则认为通信异常。

执行：

Communication Timeout
        ↓
Target Speed = 0
        ↓
PWM逐渐下降
        ↓
Motor Stop
        ↓
Robot State = IDLE

建议参数：

参数	默认值
Heartbeat 周期	50 ms
Timeout	100 ms

后续可以通过实际测试调整。

23. Watchdog

TC275 开启 Watchdog。

正常：

Main Loop
   ↓
Feed Watchdog
   ↓
Feed Watchdog
   ↓
Feed Watchdog

程序死锁：

Main Loop
   X
   ↓
Watchdog Timeout
   ↓
TC275 Reset
24. Reset Reason

系统重新启动后记录：

NORMAL
POWER_ON
WATCHDOG
SOFTWARE_RESET
FAULT

例如：

Last Reset:
WATCHDOG

可以在 Web 页面显示。

25. Fault 状态

发生严重异常时：

ROBOT_FAULT

Fault 状态：

PWM = 0
Motor Disable
禁止普通运动命令

必须通过：

CLEAR_FAULT

或系统复位后才能恢复。

26. Emergency Stop

建议提供独立的紧急停止机制。

          EMERGENCY STOP
                 │
                 ▼
              TC275
                 │
        ┌────────┴────────┐
        ▼                 ▼
     PWM = 0         Motor Disable

Emergency Stop 优先级高于普通运动指令。

27. 系统状态监控

手机端建议显示：

Product:
AURIX SmartDrive


Version:
V1.0.0


Connection:
ONLINE


Robot State:
FORWARD


Speed:
50%


Left Motor:
+50%


Right Motor:
+50%


Heartbeat:
OK


Watchdog:
OK


Last Reset:
POWER_ON

后续可扩展：

电池电压
电机温度
MCU 温度
CPU 使用率
故障码
运行时间
通信统计
28. TC275 软件任务周期

建议划分多个周期任务。

1 ms
System Tick
PWM相关实时处理
10 ms
Command Processing
Robot Control
Safety Check
50 ms
Heartbeat
Status Update
100 ms
Status Upload
Communication Statistics
1 s
System Statistics
Runtime Counter
29. TC275 软件架构
                 main()
                   │
                   ▼
            System Initialize
                   │
        ┌──────────┼──────────┐
        │          │          │
        ▼          ▼          ▼
      PWM        UART       Timer
        │          │          │
        │          ▼          │
        │      Protocol       │
        │          │          │
        │          ▼          │
        │     Command Queue   │
        │          │          │
        └──────────┼──────────┘
                   ▼
            Robot Controller
                   │
                   ▼
             State Machine
                   │
                   ▼
            Motor Controller
                   │
          ┌────────┴────────┐
          ▼                 ▼
       Left Motor        Right Motor
30. 推荐工程目录

> 本节与 §31 是 **V1.0 demo 目录/数据流快照**（历史上下文）。量产目录以 SDD §3.4 为准（`app/ rt/ mw/ bsp/ com/ test/` 分层 + `mw/sf`、`com/link.c`）。

AURIX_SmartDrive_V1
│
├── Application
│   ├── App_Main.c
│   ├── App_Robot.c
│   └── App_Command.c
│
├── Control
│   ├── Robot_Control.c
│   ├── Motor_Control.c
│   └── Safety_Control.c
│
├── Driver
│   ├── Motor_Driver.c
│   ├── TB6612.c
│   ├── PWM.c
│   ├── UART.c
│   └── LED.c
│
├── Communication
│   ├── Protocol.c
│   ├── Protocol.h
│   └── ESP32C6.c   (原 ESP8266.c，待迁移)
│
├── System
│   ├── System_Timer.c
│   ├── Watchdog.c
│   └── System_Status.c
│
└── Config
    └── SmartDrive_Config.h
31. 核心数据流
                手机
                 │
                 │ Wi-Fi
                 ▼
             ESP32-C6
                 │
                 │ UART
                 ▼
          ┌──────────────┐
          │ Protocol     │
          │ Parser       │
          └──────┬───────┘
                 │
                 ▼
          Command Manager
                 │
                 ▼
          Robot State Machine
                 │
                 ▼
          Motion Controller
                 │
          ┌──────┴──────┐
          ▼             ▼
      Left PWM       Right PWM
          │             │
          ▼             ▼
       TB6612         TB6612
          │             │
          ▼             ▼
       左电机          右电机
32. V1.0 验收标准
32.1 电机控制
 左电机正转
 左电机反转
 右电机正转
 右电机反转
 双电机同时运行
 PWM 0~100% 可调
32.2 运动控制
 前进
 后退
 左转
 右转
 原地左旋
 原地右旋
 左弧线
 右弧线
 STOP
32.3 Wi-Fi
 ESP32-C6 AP 启动
 手机连接
 Web 控制页面
 发送运动指令
 TC275 正确接收
 TC275 执行运动
 状态返回手机
32.4 通信
 帧头检测
 长度检测
 CRC 检测
 非法命令处理
 Heartbeat
 通信超时处理
32.5 安全
 通信断开自动停车
 Watchdog
 Fault 状态
 Fault 状态禁止电机运行
 Reset Reason 记录
 Emergency Stop
33. 产品版本路线

> **口径更新（V1.2）**：本节是 demo 期路线。**编码器已到货且接线方案定稿，闭环速度控制已从 V1.1 提前进 V1.0 量产范围**（SDD §1.2 第 1 条、§5.1/§5.2）；量产阶段的权威划分见 SDD §15 里程碑（M0–M4），IMU/雷达等仍按下述感知层顺序推进。
V1.0 — Wi-Fi 遥控
TC275
+
2×TB6612
+
ESP32-C6

实现：

Wi-Fi
双电机
PWM
运动状态机
通信协议
Heartbeat
超时保护
Watchdog
Web 控制
状态监控
V1.1 — 闭环速度控制

增加：

编码器
 ↓
TC275
 ↓
RPM计算
 ↓
PID
 ↓
PWM

实现真正的速度闭环。

V2.0 — 姿态控制

增加：

IMU
 ↓
TC275
 ↓
姿态估计
 ↓
运动控制
V3.0 — 毫米波雷达

增加：

IWR6843
 ↓
目标检测 / 点云
 ↓
TC275
 ↓
避障算法
 ↓
电机控制
V4.0 — 多传感器融合

加入：

IMU
编码器
毫米波雷达
超声波
CAN

最终形成一个完整的移动机器人控制平台。

34. V1.0 产品定义总结
┌───────────────────────────────────┐
│       AURIX SmartDrive V1.0       │
├───────────────────────────────────┤
│                                   │
│  手机                              │
│   │                               │
│  Wi-Fi                            │
│   │                               │
│ ESP32-C6                          │
│   │                               │
│ UART                              │
│   │                               │
│ TC275                             │
│   ├── Command Parser              │
│   ├── State Machine               │
│   ├── Motion Controller           │
│   ├── PWM Controller              │
│   ├── Safety Manager              │
│   ├── Watchdog                    │
│   └── Status Manager              │
│          │                        │
│       2×TB6612                    │
│          │                        │
│      左/右 DC Motor               │
│                                   │
└───────────────────────────────────┘
核心产品原则

通信与控制解耦、实时控制由 TC275 完成、安全机制优先、接口标准化，并为后续闭环控制和智能感知预留扩展能力。

35. 下一阶段开发顺序

建议严格按照以下顺序开发：

TC275 双电机底层驱动
两块 TB6612 完整接线与验证
TC275 PWM API
TC275 Motor API
Robot Motion API
Robot State Machine
UART 驱动
通信协议 Parser
ESP32-C6 Wi-Fi AT 固件调试
ESP32-C6 Web 控制页面
Heartbeat
Communication Timeout
Watchdog
Fault Management
Status Monitor
V1.0 整机测试
V1.0 最终目标

最终形成：

一个真正可以运行的 TC275 + 双 TB6612 + ESP32-C6 Wi-Fi 智能双轮运动控制平台。

在这个 V1.0 基础上，后续可以自然升级到：

V1.0
Wi-Fi遥控
   ↓
V1.1
编码器 + PID
   ↓
V2.0
IMU姿态控制
   ↓
V3.0
IWR6843毫米波雷达
   ↓
V4.0
多传感器融合
   ↓
V5.0
CAN/机器人控制平台