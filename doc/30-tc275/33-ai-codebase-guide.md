# 33 · AI 协作指南（TC275 侧代码库导航与作业规约）

| 项 | 内容 |
|---|---|
| 文档编号 | 33 |
| 域 | TC275 侧（3x） |
| 版本 | V1.0（2026-09-27，首版） |
| 代码基线 | `main`（默认构建 `USE_SPI_LINK`，含 servo 闭环 + CPU 看门狗 + 0x70 方向标定） |
| 读者 | **AI 编码助手**（Kiro / Claude / Copilot 等）在本仓库作业前必读 |
| 上级索引 | [00-index.md](../00-index.md) |

> **本文定位**：给 AI 用的"上手即用"手册——一页看懂代码在哪、数据怎么流、改哪里安全、红线在哪。它**不是设计基准**（那是 `21-software-design.md`），也**不重复** `31`（现状架构叙述）与 `32`（人类开发指南）的全部细节；冲突时按 `00-index §3` 裁决顺序：`21 > 23 > 22 > 32/31 > 11 > 12`，本文属实现层参考，优先级同 `31/32`。
>
> **诚实标注**：本文描述的是**评估后的当前代码**，已包含 `50-tech-assessment.md`（评估快照 `v0.2.1`）之后落地的三项改进——**闭环伺服、CPU 看门狗、方向自标定**。若你读到 `50` 里"开环/看门狗全关/方向空操作"的旧结论，以本文与代码为准。

---

## 1. 30 秒全局：这是什么

双固件产品的 TC275 侧。**Infineon TC275 三核**做实时运动控制，**ESP32-C6** 做 Wi-Fi 通信（独立仓库 `c6_car`）。手机 → Wi-Fi → C6 → **板间 SPI（SF 帧）** → TC275 → 驱动 4 个 MG310 有刷直流电机（差速双轮，每侧 2 电机并联）。

**三核职责（铁律，不要跨核乱放代码）：**

| 核 | 运行环境 | 职责 | 禁忌 |
|---|---|---|---|
| **CPU0** | FreeRTOS | robot 控制任务(10ms)、安全状态机、状态发布、日志落地、调试 UART | — |
| **CPU1** | 裸机 1 kHz 超循环 | 电机算法（斜率+闭环伺服）、编码器 8 路解码、方向标定 | **禁用任何 FreeRTOS API** |
| **CPU2** | 裸机超循环 | 板间 SF-over-QSPI3 链路泵、20ms 遥测 | **禁用任何 FreeRTOS API** |

三核只通过 `mw/xcore` 共享内存交换数据，**不要在核间直接调用对方的函数或访问对方的静态变量**。

---

## 2. 代码地图：要改什么，去哪个文件

目录按 SDD §3.4 组织（`app/`=应用、`rt/`=实时域、`com/`=通信域、`mw/`=中间件、`bsp/`=板级）。

```
myCar/
├── Cpu0_Main.c   CPU0 入口：XCORE_init → FreeRTOS 任务(blinky/echo/robot)；启用 CPU 看门狗
├── Cpu1_Main.c   CPU1 入口：MOTOR_init → ENCODER_init → MOTOR_ALGO_run()（永不返回）
├── Cpu2_Main.c   CPU2 入口：LINK_init → 超循环 LINK_main + 20ms 遥测（#ifdef USE_SPI_LINK）
│
├── app/robot.c/.h        运动状态机：命令→侧速目标、心跳超时、故障锁存、状态发布（CPU0）
├── rt/motor_algo.c/.h    1kHz 主循环：读目标→斜率限幅→SERVO_update→驱动；含 0x70 方向标定（CPU1）
├── rt/servo.c/.h         ★闭环伺服：PI + 前馈，每侧一实例；编码器失活时退化为开环（CPU1）
├── rt/encoder.c/.h       8 路 GTM TIM0 边沿中断软件正交解码，中值窗测速+里程（CPU1）
├── com/link.c/.h         ★板间链路泵：寄存器握手 + 事务泵 + 命令分派 + 遥测（CPU2）
├── com/spi_hal_pins.c/.h QSPI3 主机 HAL：定长事务、P23.0 电平采样、时钟档位（CPU2）
├── com/wifi_at.c/.h       esp-at AT 桥（已弃用分支，仅删 USE_SPI_LINK 才编入）
├── mw/xcore/xcore.c/.h   ★跨核共享内存：单自旋锁保护的所有数据块 + 日志环
├── mw/proto/protocol.c/.h AA55 命令码表 + 解析（现状：命令语义真源，被 SF 复用）
├── mw/sf/sf_frame.c/.h    ★SF 帧编解码：CRC16 + SEQ 窗口 + 重锁（纯 C99，主机可测）
├── mw/sf/sf_telemetry.c/.h 38 字节遥测定长布局编解码
├── bsp/motor.c/.h        GTM ATOM 20kHz PWM × 4 + TB6612 方向 GPIO（CPU1）
├── bsp/wdg.h             ★CPU 看门狗 helper（内联，各核自服务）
├── bsp/stime.c/.h        STM 毫秒时基（CPU1/CPU2）
├── bsp/uart.c/.h         调试串口 ASCLIN0（CPU0）
├── test/host/            主机端 gcc 单测（SF 帧 + 遥测契约），CI 在跑
└── doc/                  文档（本文件在 doc/30-tc275/）
```

**加 ★ 的是"改动最需谨慎、最好先读全"的核心文件。**

---

## 3. 数据是怎么流的（改之前先看懂这张图）

```
手机/WS客户端
   │ v2 帧 / SF CMD 帧
   ▼
[C6 从机] ──SF-over-QSPI3──► com/link.c (CPU2)
                              │ link_dispatch：按 CID/op 解码
                              │ XCORE_cmdPush(命令) / XCORE_estopRequest(急停旁路)
                              ▼
                        mw/xcore 命令队列(8深)
                              │
                              ▼
app/robot.c ◄── PROTO_handleCommand ◄── vRobotControlTask(10ms, CPU0)
   │ 状态机：算出左右侧速 -100..+100，故障锁存/心跳
   ├─► XCORE_statusPublish(状态) ──► CPU2 遥测/GET_STATUS 回复
   └─► XCORE_motorSetTarget(±1000, estop, seq++) 
                              │
                              ▼
rt/motor_algo.c (CPU1 1kHz)：
   ENCODER_task() → 读目标 → 斜率限幅 → SERVO_update(闭环/失活退开环) → MOTOR_setSpeed
                              │
                              ▼
   编码器实测速度 ──XCORE_encoderPublish──► CPU0 状态显示 + CPU2 遥测 vMeas/odo
```

**关键单位域（改控制逻辑必须记住，弄错方向会跑飞）：**
- 协议/robot 层：`-100..+100`（百分比）
- xcore 电机目标 / 伺服 / 编码器 pct 通道：`-1000..+1000`（percent×10）
- 遥测物理域：`mm/s`、`mm`（里程）
- CPU0 在 `XCORE_motorSetTarget` 处做 `×10`（±100 → ±1000）转换。

---

## 4. 跨核共享内存 `mw/xcore`：唯一的核间通道

所有跨核数据都在这里，**一把硬件 cmpAndSwap 自旋锁**保护，写后 `__dsync()`（TC275 无数据 Cache，无需 Cache 维护）。

| 块 | 写者 → 读者 | 用途 |
|---|---|---|
| MotorTarget（带 `seq`） | CPU0 → CPU1 | 左右侧速目标 + 急停位；`seq` 让 CPU1 检测 CPU0 停发（失联） |
| MotorStatus | CPU1 → 遥测 | 伺服实际输出占空 |
| XcoreEncoder | CPU1 → CPU0/CPU2 | 双单位域：pct×10（显示）+ mm/s + 里程 + `alive` |
| ProtocolStatus | CPU0 → CPU2 | robot 状态镜像，供 GET_STATUS/HTTP 回复 |
| 命令队列(8深) | CPU2 → CPU0 | 解码后的命令 |
| 急停旁路 `g_estopReq` | CPU2 置位 / CPU0 清除 | 无锁 volatile 快旁路（⚠️ 见第 8 节竞态说明） |
| 方向标定请求 | CPU0 置位 / CPU1 消费 | 0x70 触发一次性标定 |
| 日志环(2KB) | CPU1/CPU2 写 / CPU0 落地 | 无 printf 的核靠这个把行喂给 CPU0 的 UART |

**AI 作业规约（xcore）：**
1. 新增跨核数据 → 在 `xcore.h` 加块 + `xcore.c` 加 lock/unlock 访问器 + `XCORE_init` 里清零。**不要**让别的核直接摸这个静态变量。
2. 访问器内**只拷几字节**，保持临界区极小（CPU1 是硬实时，会为锁自旋）。
3. 无 printf 的核（CPU1/CPU2）要输出，用 `XCORE_logln` / `XCORE_logu`(无符号) / `XCORE_logi`(有符号)，**不要**试图在这些核上直接调 UART。

---

## 5. 三条最容易被要求改动的路径（含正确改法）

### 5.1 加一个新命令
1. `mw/proto/protocol.h`：定义 `PROTO_CMD_xxx`（命令码是**唯一真源**，SF 链路复用它）。
2. `mw/proto/protocol.c` 的 `PROTO_handleCommand`（CPU0 执行侧）加 `case`；驾驶类命令要先调 `ROBOT_cmdHeartbeat()`（否则 100ms 看门狗会把摇杆流归零）。
3. 若命令要从 SPI 链路进来：`com/link.c` 的 `link_dispatch` 白名单在 CID `DRIVE/DIAG/DPT`；**注意** payload[0] 只在 op 前缀型 CID 上才是命令，CFG/PAIR 是整包语义，别把首字节当命令执行（这是本链路唯一能让车乱动的错误）。
4. 同批更新文档：帧→`21 §6`/`22 §5`，命令语义→`31`。

### 5.2 调运动/控制行为
- 斜率、超时：`rt/motor_algo.h`（`MOTOR_ALGO_MAX_STEP`、`MOTOR_ALGO_CMD_TIMEOUT_MS`）。
- 闭环增益：`rt/servo.h`（`SERVO_KP/KI/FF_GAIN`），**台架用 `SRV=` 日志行调，不要拍脑袋改**。
- 满量程速度基准：`rt/encoder.h` 的 `ENCODER_FULL_SCALE_MM_S`——它定义了整个 percent 域，改它等于重调整个环。
- 方向不对：**不要手改** `g_encInvert`，跑一次 `0x70`（DPT_CAL_DIR）自动标定（轮子离地，约 1.4s，结果打在 `ENCCAL=` 日志行）。

### 5.3 改板间链路/帧
- 帧格式：`mw/sf/sf_frame.c`（纯 C99，**改完必须跑主机单测**，见第 7 节）。
- 链路泵行为：`com/link.c`——寄存器快照单读+消费端钳位、失联只由 `SF_ALIVE` 500ms 判定、SEQ 越窗 8 帧重锁，这些都是**修过现场故障的设计**（"空载正常满载断连"就是双读回归导致的），改前务必读懂注释里的复盘。

---

## 6. 安全机制（现状，改动前必须知道不能破坏哪些）

- **CPU 看门狗**已启用（`bsp/wdg.h`）：CPU0 在 robot 任务(10ms)、CPU1 在 1kHz 循环各自喂狗（`WDG_serviceCpu()`）。timeout ~0.3–0.5s。喂狗用 ENDINIT clear+set 对，**不要**改成 `IfxScuWdt_serviceCpuWatchdog`（会把 ENDINIT 饱和计数器顶死，卡住 flash/OTA）。**加长循环/加阻塞调用前，确认喂狗节奏不被破坏。**
- **急停三通道**：SF `EMERGENCY_STOP`(0x32) → CPU2 置 `g_estopReq` 旁路 + 入队；链路失联 → `LINK_LOST` 触发急停；CPU1 每周期检查 `g_targetEstop || XCORE_estopIsActive()` 立即刹车（不走斜坡）。
- **双看门狗（软）**：CPU0 心跳超时 100ms、CPU1 命令失联 150ms，都会归零侧速。
- **闭环失活兜底**：编码器 `!alive` 时 `SERVO_update` 退化为开环 `duty=target` 并丢弃积分（防重锁踢腿）——这是"8 根编码器线还没接也能安全出厂"的设计。
- **故障锁存**：`app/robot.c` 故障态阻断所有运动命令，须 `CLEAR_FAULT`/`RESET` 解除。

---

## 7. 验证：改完怎么自证没坏

**主机单测（gcc，CI 在跑，改 SF/遥测层后必跑）：**
```bash
# SF 帧编解码（含 200 万字节模糊测试）
gcc -std=c99 -Wall -Wextra -Werror -O2 -I . \
    test/host/test_sf.c mw/sf/sf_frame.c -o test/host/out/test_sf && test/host/out/test_sf

# 38 字节遥测布局 + 与 C6 从机解码器交叉验证（需同级 c6_car 仓库）
gcc -std=c99 -Wall -Wextra -O2 -DC6_CROSS_CHECK -I . \
    -I c6_car/components/c6_proto -I c6_car/components/c6_sf \
    test/host/test_sf_telemetry.c mw/sf/sf_telemetry.c mw/sf/sf_frame.c \
    c6_car/components/c6_proto/proto_frames.c -o test/host/out/test_sf_telemetry && test/host/out/test_sf_telemetry
```

**固件构建**：只能在 AURIX Development Studio（TASKING 编译器）里构建 `TriCore Debug (TASKING)`，**主机/CI 无法编译固件**（专有编译器）。AI 不要假装能在命令行编出固件；能做的是保证主机单测通过 + 代码符合 iLLD/MISRA 习惯。

**覆盖盲区（诚实告知）**：`link.c`/`motor_algo`/`encoder`/`robot.c` 无单测（依赖 iLLD/FreeRTOS），改这些只能靠代码审查 + 台架。改动这类文件时，AI 应在回复里明确说明"此改动未被单测覆盖，需台架验证"。

---

## 8. 已知红线与陷阱（AI 高频踩坑点）

1. **别在 CPU1/CPU2 用 FreeRTOS API**——它们是裸机核，只有 CPU0 跑内核。
2. **急停旁路 `g_estopReq` 有置位/清位竞态**：CPU2 置位与 CPU0"无故障即清"可能撞车。若被要求改急停逻辑，倾向"置位后须显式 CLEAR/RESET 才清"，别扩大这个竞态。
3. **单位域别串**：±100（协议）/ ±1000（电机+伺服+编码器 pct）/ mm/s（遥测）。转换点在 CPU0 的 `XCORE_motorSetTarget`。
4. **payload[0]≠总是命令**：SF 分派里只有 op 前缀型 CID 才这样，见 5.1。
5. **方向标定是运行时的**：不要手改 `g_encInvert` 常量去"修方向"，用 0x70。
6. **`Middleware/` 已删除**：现役目录是 `app/ rt/ com/ mw/ bsp/`；文档里出现的 `App/Bsp/Middleware` 是历史叙述，按 SDD §3.4 映射读。
7. **UART 板间链路已弃用**：默认构建两个 TASKING 配置都定义 `USE_SPI_LINK`，`com/wifi_at.c` 只在删符号后才编入，只作 C6 调试控制台/应急返修。别默认它在链路上。
8. **改行为必须同批改文档**（`00-index §5`）：协议帧/引脚/超时/看门狗数值/xcore 通道语义任一变动，同一提交更新对应真源。
9. **最大未验证项**：板间 SPI 链路**从未两板通电联调**（G1 门禁未过）。涉及 `com/link.c`/`com/spi_hal_pins.c` 的改动，AI 应提醒"物理层尚未联调，改动需 G1 台架验证"。

---

## 9. 硬件事实速查（引脚真源在 `23-wiring.md`，此处仅备查）

**电机（TB6612 ×2，D24A 四路驱动板，`bsp/motor.c` / `bsp/motor.h`）：**

| 电机 | 位置 | PWM | IN1 | IN2 | 侧 |
|---|---|---|---|---|---|
| A | 前 | P21.0 (ATOM2_4) | P21.4 | P21.5 | 左 |
| B | 后 | P21.3 (ATOM4_1) | P21.2 | P22.3 | 左 |
| C | 后 | P00.0 (ATOM1_0) | P00.2 | P00.6 | 右 |
| D | 前 | P00.8 (ATOM0_7) | P00.10 | P00.12 | 右 |

- PWM 20 kHz（人耳之上）；`g_dirInvert = {A:F, B:T, C:T, D:F}`（镜像安装，B/C 反向使"前进"统一）。
- 左侧=A+B（TB6612#1=U8），右侧=C+D（TB6612#2=U2），每侧两电机同命令。
- **驱动板 D24A（REV1.0）板内电路**：12V VIN 经 KEY 开关 → `TB6288GSP`(U1) 降 5V → `RT9013-33`(U4) 降 3.3V；`P_EN`(NTC 过温) + 过流网络门控输出使能。**逻辑电全板载自产**，别从 kit/外部灌 5V/3V3；编码器 3V3 参考就是这块 LDO（禁接 5V，超 TC275 IO 耐压）；动力走 TB6612 VM（带 220µF 储能），不经 3V3。完整板内框图/器件清单见 **`23-wiring.md §3.1`**，对外脚位见 §3/§4。
- **两个易踩的板载复用**（`23-wiring.md §4`）：P00.0(电机C PWM) 兼板载 CAN 收发器 TXD——运行时别接外部 CAN；P00.6(电机C 方向2) 兼板载 LED2，翻转时 LED 闪属正常。

**编码器（MG310 内置 260 线霍尔 AB，减速比 1:20.409）：**
- 8 路信号 P33.0~P33.7 ↔ X2-28~35，GTM TIM0 八通道双边沿中断（CPU1），软件 ×4 正交。
- E1~E4 与电机 A~D **1:1 对应**（motor id 兼作编码器索引）。

**板间 SPI：** QSPI3 主机 P33.11/12/13（SCLK/MTSR/MRST）+ P23.4 CS + P23.0 IRQ（电平轮询）↔ C6 SPI2 从机；杜邦线直连、无外部元件（`23 §9.3`）。

---

## 10. 一句话给 AI 的作业心法

> 先按第 2 节定位文件、按第 3 节看懂数据流与单位域；核间只走 `mw/xcore`；改 SF 层必跑主机单测；改控制/链路层要声明"未被单测覆盖、需台架验证"；改行为同批改文档；别碰第 8 节的九条红线。**代码里的注释常常解释了"为什么"和踩过的坑，动手前先读它。**
