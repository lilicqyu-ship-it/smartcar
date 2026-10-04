# 33 · AI 协作指南（TC275 侧代码库导航与作业规约）

| 项 | 内容 |
|---|---|
| 文档编号 | 33 |
| 域 | TC275 侧（3x） |
| 版本 | V1.8（2026-10-04，编码器健康按侧判定随 BUG-ENC-1 / [51](../51-encoder-health-bugfix.md)）：§4 块表 `XcoreEncoder` 行补 `edgeAgeMs[2]` 健康原语、`alive` 改注运动指示；§6"闭环失活兜底"补 measValid 按侧判定与融合 `FUSION_ENCODER_OK` 新语义（发布新鲜 + 被驱侧 500 ms 宽限内持续出边沿）。V1.7（2026-10-04，日志泵去阻塞随 APPFW 1.3.1）：`XCORE_logService` 从"每调用阻塞排一行"改为**非阻塞字节泵**（`bsp/uart` 新增 `UART_printTry`，`\n`→CRLF 成对入队）——旧实现最坏 256 B@115200 ≈ 22 ms，天然超过 CPU0 的 10 ms 控制周期（P0）；现在控制任务只把字节排进驱动 256 B 软件 TX FIFO（TX ISR 线速后台移出），FIFO 满即停、下周期续排，积压仍按线速清空、满环丢整行不变；`test_xcore` 契约同步改写（1109→1386 断言）。V1.6（2026-10-04，STBY 受控化随 23 号 V1.23 / 21 号 V1.15）：第 9 节电机速查新增 STBY 条目——**P22.2（X1-16）→ D24A J4-2，GPIO 受控**，`bsp/motor.c` 的 `MOTOR_setEnabled()` + `MOTOR_init()` 末步使能，一根线管四路；写清"急停/故障路径目前不调用它"这条现状与接线前提（跳线帽必须拔掉）。V1.5（2026-10-03，IMU 驱动落地随 35 号 V1.0）：第 2 节代码地图加 `bsp/imu`；第 4 节块表补 `XcoreImu` 行；第 7 节验证加 `test_imu` 命令（55 断言）、`test_xcore` 1095→1109；第 9 节硬件速查补 IMU 段（QSPI1/INT1/优先级 11·14·15·24）。V1.4（2026-10-03，命令队列抗突发随 22 号 V1.8）：第 3 节数据流图与第 4 节块表的命令队列改 **16 深 + `SET_SPEED` 走 `XCORE_cmdPushLatest` 新者胜**；第 7 节新增 `test_xcore` 可粘贴命令（1095 断言）并把覆盖盲区改为"已有覆盖"口径；CI（`tc275-car.yml`）同步加跑。V1.3（2026-09-30，编码器刻度修正随 34 V1.4）：第 9 节编码器 260 线→**13 PPR**（52 计数/电机转、1061.27 计数/轮转、0.1421 mm/计数），默认轮径 65→**48**（`CALIB_WHEELDIA_DEF` 同批）。V1.2（2026-09-27，随 34 号 V1.3 闭环使能门同步）：第 2 节代码地图 `rt/motor_algo` 条目补门；第 5.2 节"方向不对"条目补门上电行为；第 6 节安全机制新增**闭环使能门**（src=0 记录强制开环等价，34 §13）。V1.1（2026-09-27，随 34 号标定/DPT 落地同步）：第 2 节代码地图加 `mw/calib/`；第 4 节 xcore 块表补 CalibResult/Jog/RecordLive/EVT 出站队列四行 + "用版本计数不要用 valid 位"规约；第 5.1 节写清 DPT op 家族（0x70~0x74）与 EVT 0x22/0x23 的组帧侧；第 5.2 节改为"标定参数是运行时变量、换轮径走 0x73"；第 6 节加 DFlash 写入的安全姿态；第 7 节单测命令加入 `mw/calib/calib_record.c`（2948 断言）并扩写覆盖盲区；第 8 节红线增至 10 条（新增"只有 CPU0 能写 DFlash"）。V1.0 = 2026-09-27 首版 |
| 代码基线 | `main`（默认构建 `USE_SPI_LINK`，含 servo 闭环 + CPU 看门狗 + 0x70 方向标定） |
| 读者 | **AI 编码助手**（Kiro / Claude / Copilot 等）在本仓库作业前必读 |
| 上级索引 | [00-index.md](../00-index.md) |

> **本文定位**：给 AI 用的"上手即用"手册——一页看懂代码在哪、数据怎么流、改哪里安全、红线在哪。它**不是设计基准**（那是 `21-software-design.md`），也**不重复** `31`（现状架构叙述）与 `32`（人类开发指南）的全部细节；冲突时按 `00-index §3` 裁决顺序：`21 > 23 > 22 > 32/31 > 11 > 12`，本文属实现层参考，优先级同 `31/32`。
>
> **诚实标注**：本文描述的是**评估后的当前代码**，已包含 `50-tech-assessment.md`（评估快照 `v0.2.1`）之后落地的三项改进——**闭环伺服、CPU 看门狗、方向自标定**。若你读到 `50` 里"开环/看门狗全关/方向空操作"的旧结论，以本文与代码为准。

---

## 1. 30 秒全局：这是什么

双固件产品的 TC275 侧。**Infineon TC275 三核**做实时运动控制，**ESP32-C6** 做 Wi-Fi 通信（独立仓库 `esp32c6_car`）。手机 → Wi-Fi → C6 → **板间 SPI（SF 帧）** → TC275 → 驱动 4 个 MG310 有刷直流电机（差速双轮，每侧 2 电机并联）。

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
tc275_car/
├── Cpu0_Main.c   CPU0 入口：XCORE_init → FreeRTOS 任务(blinky/echo/robot)；启用 CPU 看门狗
├── Cpu1_Main.c   CPU1 入口：MOTOR_init → ENCODER_init → MOTOR_ALGO_run()（永不返回）
├── Cpu2_Main.c   CPU2 入口：LINK_init → 超循环 LINK_main + 20ms 遥测（#ifdef USE_SPI_LINK）
│
├── app/robot.c/.h        运动状态机：命令→侧速目标、心跳超时、故障锁存、状态发布（CPU0）
├── rt/motor_algo.c/.h    1kHz 主循环：读目标→斜率限幅→SERVO_update→驱动；含 0x70 方向标定 + 0x71 直驱 jog + 闭环使能门 g_closedLoopOk（src=0 强制开环等价，34 §13）（CPU1）
├── rt/servo.c/.h         ★闭环伺服：PI + 前馈，每侧一实例；编码器失活时退化为开环（CPU1）
├── rt/encoder.c/.h       8 路 GTM TIM0 边沿中断软件正交解码，8 ms 均值窗测速+里程（CPU1）
├── com/link.c/.h         ★板间链路泵：寄存器握手 + 事务泵 + 命令分派 + 遥测（CPU2）
├── com/spi_hal_pins.c/.h QSPI3 主机 HAL：定长事务、P23.0 电平采样、时钟档位（CPU2）
├── com/wifi_at.c/.h       esp-at AT 桥（已弃用分支，仅删 USE_SPI_LINK 才编入）
├── mw/xcore/xcore.c/.h   ★跨核共享内存：单自旋锁保护的所有数据块 + 日志环
├── mw/proto/protocol.c/.h AA55 命令码表 + 解析（现状：命令语义真源，被 SF 复用）
├── mw/sf/sf_frame.c/.h    ★SF 帧编解码：CRC16 + SEQ 窗口 + 重锁（纯 C99，主机可测）
├── mw/sf/sf_telemetry.c/.h 38 字节遥测定长布局编解码
├── mw/calib/calib_record.c/.h ★标定记录 20B blob + EVT 0x22/0x23 编解码（纯 C99，主机可测）
├── mw/calib/calib_store.c/.h  CPU0 专属：DFlash0 扇区 15 单槽持久化 + 静止后才写 + 结果转 EVT
├── bsp/motor.c/.h        GTM ATOM 20kHz PWM × 4 + TB6612 方向 GPIO + STBY 使能 P22.2（CPU1）
├── bsp/imu.c/.h          LSM6DSV16BX 六轴 IMU：QSPI1 主机（SPI 模式 3，1 MHz 档位）
│                         + ERU INT1 计数 + 1 kHz 采样/失联 1 Hz 重探（CPU1；
│                         设计真源 [35](35-imu-driver.md)，接线真源 23 §10）
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
                              │ XCORE_cmdPush(命令) / XCORE_cmdPushLatest(SET_SPEED 新者胜)
                              │ / XCORE_estopRequest(急停旁路)
                              ▼
                        mw/xcore 命令队列(16深, SET_SPEED 新者胜)
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
| XcoreEncoder | CPU1 → CPU0/CPU2 | 双单位域：pct×10（显示）+ mm/s + 里程 + `alive`（运动指示）+ **`edgeAgeMs[2]` 按侧边沿龄（健康判定原语，51）** |
| ProtocolStatus | CPU0 → CPU2 | robot 状态镜像，供 GET_STATUS/HTTP 回复 |
| 命令队列(16深) | CPU2 → CPU0 | 解码后的命令；**周期性状态命令（30 Hz `SET_SPEED` 摇杆/心跳流）走 `XCORE_cmdPushLatest` 新者胜**——队里已有同 cmd 未消费消息则原地覆盖最新一条，链路泵停摆后的积压突发塌缩成一条（22 §5.4 V1.8）；一次性/安全命令仍走严格 FIFO 的 `XCORE_cmdPush` |
| 急停旁路 `g_estopReq` | CPU2 置位 / CPU0 清除 | 无锁 volatile 快旁路（⚠️ 见第 8 节竞态说明） |
| 方向标定请求 | CPU0 置位 / CPU1 消费 | 0x70 触发一次性标定 |
| CalibResult | CPU1 → CPU0 | 标定结束发布 `{op, status(DONE/ABORTED/BUSY), invert[4], delta[4]}`；CPU0 排空后组 EVT 0x22 并在 DONE 时持久化 |
| Jog | CPU0 → CPU1 | 0x71 单电机开环 duty（percent×10，钳 ±500），带 `jogSeq`；CPU1 300ms 未续约自动停车并打 `JOG timeout` |
| RecordLive（带 `version`） | CPU0 → CPU1/CPU2 | 当前生效标定记录（invert/满量程/轮径）；CPU1 按 `version` 边沿应用，CPU2 取 `vTarget` |
| EVT 出站队列(8×≤32B) | CPU0 → CPU2 | 产测事件 0x22/0x23 的出帧口；CPU2 `link_sendPendingEvents()` 弹出，满则丢弃 |
| XcoreImu（带 `seq`） | CPU1 → CPU0/CPU2 | IMU 采样信箱（`bsp/imu` 每 5 ms 发布一次）：mg / mdps / 0.01 °C 单位域 + `alive`/`drdyCount`/`errCount`；`seq` 由 `XCORE_imuPublish` 内部递增，消费者看它判新鲜度（35 §5） |
| 日志环(2KB) | CPU1/CPU2 写 / CPU0 落地 | 无 printf 的核靠这个把行喂给 CPU0 的 UART；CPU0 用**非阻塞字节泵**落地（满环丢整行，TX FIFO 满则下周期续排，绝不阻塞控制任务） |

**AI 作业规约（xcore）：**
1. 新增跨核数据 → 在 `xcore.h` 加块 + `xcore.c` 加 lock/unlock 访问器 + `XCORE_init` 里清零。**不要**让别的核直接摸这个静态变量。
2. 访问器内**只拷几字节**，保持临界区极小（CPU1 是硬实时，会为锁自旋）。
3. 无 printf 的核（CPU1/CPU2）要输出，用 `XCORE_logln` / `XCORE_logu`(无符号) / `XCORE_logi`(有符号)，**不要**试图在这些核上直接调 UART。
4. **"发布过没有"需要判边沿时用版本计数（`seq`/`version`/`jogSeq`），不要靠数据本身**：`XCORE_init` 把共享 RAM 整片清零，而"全零"在这套固件里常常是**合法值**（清零后的 `CalibRecord` 恰好是一组看着合理的参数），于是"从未发布"与"发布了默认值"不可区分——`RecordLive`/`Jog` 因此带计数器，消费者按边沿取用（`MOTOR_ALGO_applyRecord`）。反过来，纯**信箱**语义（`CalibResult.pending`，取走即原子清零）用 0/1 标志是对的，它的零态正是"没有结果"。

---

## 5. 三条最容易被要求改动的路径（含正确改法）

### 5.1 加一个新命令
1. `mw/proto/protocol.h`：定义 `PROTO_CMD_xxx`（命令码是**唯一真源**，SF 链路复用它）。
2. `mw/proto/protocol.c` 的 `PROTO_handleCommand`（CPU0 执行侧）加 `case`；驾驶类命令要先调 `ROBOT_cmdHeartbeat()`（否则 100ms 看门狗会把摇杆流归零）。
3. 若命令要从 SPI 链路进来：`com/link.c` 的 `link_dispatch` 白名单在 CID `DRIVE/DIAG/DPT`；**注意** payload[0] 只在 op 前缀型 CID 上才是命令，CFG/PAIR 是整包语义，别把首字节当命令执行（这是本链路唯一能让车乱动的错误）。
4. 若是 DPT（`0x70..0x7F`）下的新 op：`payload[0]=op`，其余是它的参数——SF 命令载荷上限 `PROTO_MAX_PAYLOAD`(16 B) **装不下就多段或换 CFG 载体**，别偷偷扩 16。现有 op：`0x70` 判向 / `0x71` jog / `0x72` 取记录 / `0x73` 写记录(12B) / `0x74` 清记录。回传走 **EVT CID `0x22`(结果,23B)/`0x23`(记录回显,15B)**，**组帧在 CPU0**（只有它知道 flash 是否写成功）、发送在 CPU2。
5. 同批更新文档：帧→`21 §6`/`22 §5`，命令语义→`31`，产测规程→`23 §8.4`，标定/DPT 真源→`34`。

### 5.2 调运动/控制行为
- 斜率、超时：`rt/motor_algo.h`（`MOTOR_ALGO_MAX_STEP`、`MOTOR_ALGO_CMD_TIMEOUT_MS`）。
- 闭环增益：`rt/servo.h`（`SERVO_KP/KI/FF_GAIN`），**台架用 `SRV=` 日志行调，不要拍脑袋改**。
- 满量程速度基准：`rt/encoder.h` 的 `ENCODER_FULL_SCALE_MM_S`——它定义了整个 percent 域，改它等于重调整个环。**当前生效值是运行时变量** `g_fullScaleMmS`/`g_wheelDiaMm`（宏只提供默认值），由 `mw/calib` 的生效记录经 `RecordLive` 下发；换轮径/换齿比走 `0x73 REC_SET` 改数据，**不要**去算法里改写死常数，也不要动换算公式（改了历史标定全废）。
- 方向不对：**不要手改** `g_encInvert`，跑一次 `0x70`（DPT_CAL_DIR）自动标定（轮子离地，约 1.4s，结果打在 `ENCCAL=` 日志行）。**DONE 自动持久化到 DFlash0**，重启不用重判。
- **闭环使能门（34 §13）**：上电若记录 `src=0`（首次烧录/校验失败/0x74 清过），`g_closedLoopOk=FALSE` 强制开环等价（duty=target），串口一次性打 `SERVO open-loop (record src=0; calibrate via 0x70)`——此时 `SRV=` 里 duty 恒等于 target 是**设计行为不是 bug**。0x70 DONE 后自动开门（`SERVO closed-loop enabled`）。门只挡"从未标定"，不挡"标定过时"（换电机忘重标照样正反馈）。

### 5.3 改板间链路/帧
- 帧格式：`mw/sf/sf_frame.c`（纯 C99，**改完必须跑主机单测**，见第 7 节）。
- 链路泵行为：`com/link.c`——寄存器快照单读+消费端钳位、失联只由 `SF_ALIVE` 500ms 判定、SEQ 越窗 8 帧重锁，这些都是**修过现场故障的设计**（"空载正常满载断连"就是双读回归导致的），改前务必读懂注释里的复盘。

---

## 6. 安全机制（现状，改动前必须知道不能破坏哪些）

- **CPU 看门狗**已启用（`bsp/wdg.h`）：CPU0 在 robot 任务(10ms)、CPU1 在 1kHz 循环各自喂狗（`WDG_serviceCpu()`）。timeout ~**1.4 s**（`REL=0xF800`；旧稿 0.3~0.5 s 是算术错误）。**超时反应已闭合**（tc275_car v1.1.2，`21 §7.2` V1.13）：TC27x 的 WDT 到期只发 NMI 不自行复位，`Configurations/Ifx_Cfg.h` 的 NMI 钩子接 `IfxCpu_triggerSwReset()`——**别拆它**，拆了到期即整车假死；也**别往钩子里加打印/xcore 调用**（NMI 上下文会死锁）。喂狗用 ENDINIT clear+set 对，**不要**改成 `IfxScuWdt_serviceCpuWatchdog`（会把 ENDINIT 饱和计数器顶死，卡住 flash/OTA）。**加长循环/加阻塞调用前，确认喂狗节奏不被破坏。**
- **急停三通道**：SF `EMERGENCY_STOP`(0x32) → CPU2 置 `g_estopReq` 旁路 + 入队；链路失联 → `LINK_LOST` 触发急停；CPU1 每周期检查 `g_targetEstop || XCORE_estopIsActive()` 立即刹车（不走斜坡）。
- **双看门狗（软）**：CPU0 心跳超时 100ms、CPU1 命令失联 150ms，都会归零侧速。
- **闭环失活兜底**：编码器测量无效时 `SERVO_update` 退化为开环 `duty=target` 并丢弃积分（防重锁踢腿）——这是"8 根编码器线还没接也能安全出厂"的设计。**V1.7 起 measValid 按侧判定**（BUG-ENC-1，51）：被驱侧边沿龄 >100 ms 即该侧回退开环，死侧不再被对侧运动的合并 `alive` 掩蔽而对假 0 测量积分 windup；`cmd==0` 豁免（未驱动侧无可测量）。融合侧同批：`FUSION_ENCODER_OK` 要求发布新鲜 + 被驱侧 500 ms 宽限内持续出边沿，速度锚/轮速陀螺一致性只用边沿新鲜的侧。
- **闭环使能门**（34 §13，2026-09-27 随台架跑飞现象落地）：方向记录 `src=0`（从未标定/回落默认）时 `MOTOR_ALGO_controlStep` 把 `measValid` 钳 FALSE，与失活兜底同路径强制开环——堵死"编码器已接线但符号未标定 → 闭环正反馈、目标 0 也停不下"的洞（23 §8.4 的"开环等价"从规程变固件执行）。门随 `RecordLive.version` 边沿翻转：0x70 DONE/0x73 开、0x74 关。**改标定/记录流时不要破坏这条门链**。
- **故障锁存**：`app/robot.c` 故障态阻断所有运动命令，须 `CLEAR_FAULT`/`RESET` 解除。
- **标定记录持久化**（`mw/calib/calib_store.c`，CPU0）：写 DFlash0 扇区 15 的序列是 `喂狗 → __disable() → 擦扇区 → 逐页(8B)编程 → 回读比对 → __enable() → 喂狗`，一次几十毫秒内关中断；**只用 `bsp/wdg.h` 的 `WDG_serviceCpu()`，禁止 `IfxScuWdt_serviceCpuWatchdog`**（同上条看门狗）。写被**延后到车辆静止**（目标/jog duty/实测轮速全 0 且安静 500ms）才发生，失败会安静重试并在 EVT 0x22 的 `saved` 字节里如实报告（`2=失败`）。**新增任何 flash 写入前先想：谁在擦、关多久的中断、三核谁会被 stall。**

---

## 7. 验证：改完怎么自证没坏

**主机单测（gcc，CI 在跑，改 SF/遥测/ADC 换算层后必跑）：**
```bash
# SF 帧编解码 + 标定记录/EVT 0x22/0x23 编解码（含 200 万字节模糊测试，2948 断言）
gcc -std=c99 -Wall -Wextra -Werror -O2 -I . \
    test/host/test_sf.c mw/sf/sf_frame.c mw/calib/calib_record.c \
    -o test/host/out/test_sf && test/host/out/test_sf

# 38 字节遥测布局 + 与 C6 从机解码器交叉验证（需同级 esp32c6_car 仓库）
gcc -std=c99 -Wall -Wextra -O2 -DC6_CROSS_CHECK -I . \
    -I esp32c6_car/components/c6_proto -I esp32c6_car/components/c6_sf \
    test/host/test_sf_telemetry.c mw/sf/sf_telemetry.c mw/sf/sf_frame.c \
    esp32c6_car/components/c6_proto/proto_frames.c -o test/host/out/test_sf_telemetry && test/host/out/test_sf_telemetry

# 电池 VIN 换算链（counts -> 分压点 mV -> VIN mV，纯函数在 bsp/adc.h；含
# DIV_NUM x1000 量纲回归守卫——曾把 11000 写成 11，读数小 1000 倍）
gcc -std=c99 -Wall -Wextra -Werror -O2 -I . -I test/host/stub \
    test/host/test_adc.c -o test/host/out/test_adc && test/host/out/test_adc

# IMU 换算链（LSM6DSV16BX，mg/mdps/温度 + SPI 命令字节，纯函数在 bsp/imu.h；
# 灵敏度锚点取自 ST 官方驱动，FS 码 0xC=4000dps 的乱序映射有专项守卫）
gcc -std=c99 -Wall -Wextra -Werror -O2 -I . -I test/host/stub \
    test/host/test_imu.c -o test/host/out/test_imu && test/host/out/test_imu

# xcore 命令队列/日志环纪律（FIFO、容量、SET_SPEED 新者胜塌缩、日志非阻塞字节泵
# （TX FIFO 满即停、跨调用续排、字节序/CRLF 不变）、IMU 信箱 seq；1386 断言。注意
# stub 目录必须排在 -I 首位——bsp/uart.h 与
# IfxCpu.h 用的是 host 替身，不是 iLLD 原件）
gcc -std=c99 -Wall -Wextra -Werror -O2 -I test/host/stub -I . \
    test/host/test_xcore.c mw/xcore/xcore.c \
    -o test/host/out/test_xcore && test/host/out/test_xcore

# 实际标定保存调度代码，flash/xcore/时钟边界使用主机桩；不编译硬件原语
python3 test/host/test_calib_store.py
```

**固件构建**：只能在 AURIX Development Studio（TASKING 编译器）里构建 `TriCore Debug (TASKING)`，**主机/CI 无法编译固件**（专有编译器）。AI 不要假装能在命令行编出固件；能做的是保证主机单测通过 + 代码符合 iLLD/MISRA 习惯。

**覆盖盲区（诚实告知）**：`link.c`/`motor_algo`/`encoder`/`robot.c`/`imu.c` 无单测（依赖 iLLD/FreeRTOS/硬件），改这些只能靠代码审查 + 台架（IMU 的换算半截在 `bsp/imu.h` 纯函数里、有 `test_imu`；驱动主体无）。改动这类文件时，AI 应在回复里明确说明"此改动未被单测覆盖，需台架验证"。**同类盲区**：xcore 新块（CalibResult/Jog/RecordLive/EVT 队列/XcoreImu 的跨核半截）、`0x71` jog 时序、**DFlash 擦写与回读**——只有 `mw/calib/calib_record.c` 的纯编解码有单测，**存储与跨核那半截没有**。**已有覆盖**：命令队列/日志环的入队出队纪律（FIFO、容量、新者胜、日志非阻塞字节泵：受 256 B 软件 TX FIFO 限界的每次调用、跨调用续排、CRLF 成对入队）自 2026-10-03 起有 `test_xcore.c`（锁与核间原子性仍只有目标机上有，主机替身是空锁）。

---

2026-10-03 补充：`test_calib_store.py` 覆盖实际保存队列和回执时序；上节所述盲区仍包括真实三核同步、FMU 擦写与回读，主机桩测试不能替代台架验证。

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
10. **写 DFlash 只有 CPU0、只有控制任务、必须喂狗+关中断**：见 `mw/calib/calib_store.c` 的 `WDG_serviceCpu() → __disable() → 擦/逐页写 → 回读 → __enable()`。三核抢同一个 FMU，CPU1/CPU2 在擦写的几十毫秒里只是 stall；**别在 CPU1 的 1kHz 硬实时环里加 flash 访问**，也别把 `DFlash0` 当通用 scratch（扇区 15 = 标定记录独占，其余留给 §4.3 配置页；DF0 容量 16 还是 48 扇区尚未定论，见 `34 §11.4 C1`）。

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
- **STBY 总使能 = P22.2（X1-16）→ D24A J4-2，GPIO 受控**（`bsp/motor.c` 的 `MOTOR_STBY_PORT/PIN` + `MOTOR_setEnabled(boolean)`，属主 CPU1）：两片 TB6612 的 STBY 在板内是**同一网络**，一根线管四路，J6 无独立 STBY。`MOTOR_init()` 把拉高 STBY 放在**最后一步**（此时 8 根 IN/PWM 已置低、duty=0），所以"CPU1 还没跑到 BSP init"＝驱动未解锁；`MOTOR_setEnabled(FALSE)` 会先 `MOTOR_stopAll()` 再拉低。**急停/堵转/欠压路径目前不调用它**（现急停是 IN1=IN2=H 短接刹车，拉低 STBY 会退化成高阻滑行、刹车距离变长——语义待裁决，`21 §5.2`）。接线侧前提：**J4-1↔J4-2 跳线帽必须拔掉**，否则 GPIO 白接（`23 §3.2` V1.23）。

**编码器（MG310 内置 13 PPR 霍尔 AB，×4 = 52 计数/电机转，减速比 1:20.409 → ≈1061.27 计数/轮转，48 mm 胎 = 0.1421 mm/计数）：**
- 8 路信号 P33.0~P33.7 ↔ X2-28~35，GTM TIM0 八通道双边沿中断（CPU1），软件 ×4 正交。
- E1~E4 与电机 A~D **1:1 对应**（motor id 兼作编码器索引）。

**六轴 IMU（LSM6DSV16BX，`bsp/imu.c/.h`，设计真源 [35](35-imu-driver.md)、接线真源 `23 §10`）：**
- QSPI1 主机：SCLK=P11.6 / MTSR=P11.9 / MRST=P11.3 / **CS=SLSO3 P11.10**（Shield2Go 2 号座共享此脚，用 IMU 时禁插板）；INT1=P15.4（SCU ERU REQ0，**可边沿中断**——与 P23.x/P33.x 不同）；INT2=P15.5 固件未配；VEXT 3.3 V 供电。
- SPI **模式 3**（CPOL/CPHA=1）、MSB first、首字节 bit7=R/W；默认 ±4 g/±500 dps/240 Hz，1 kHz 环每 5 ms 读 0x20 起 14 B（温度+陀螺+加计）；时钟档位 1/2/5/10 MHz，默认 1 MHz（杜邦线直连）。
- 中断优先级（21 §18 C2）：QSPI1 TX/RX/ER = **11/14/15**（低于编码器 16~23）、ERU DRDY = **24**（ISR 只计数）。
- 失联 1 Hz 重探，接线下**免重启**自愈；台架行 `IMU=`（0.5 Hz，whoAmI/ax/ay/az/gx/gy/gz/tempC/drdy/err）。**硬件尚未接线**，判读按 35 §7。

**板间 SPI：** QSPI3 主机 P33.11/12/13（SCLK/MTSR/MRST）+ P23.4 CS + P23.0 IRQ（电平轮询）↔ C6 SPI2 从机；杜邦线直连、无外部元件（`23 §9.3`）。

---

## 10. 一句话给 AI 的作业心法

> 先按第 2 节定位文件、按第 3 节看懂数据流与单位域；核间只走 `mw/xcore`；改 SF 层必跑主机单测；改控制/链路层要声明“未被单测覆盖、需台架验证”；改行为同批改文档；别碰第 8 节的十条红线。**代码里的注释常常解释了“为什么”和踩过的坑，动手前先读它。**

2026-10-04 v1.3.0 补充：新诊断事务见 [39](39-diagnostic-transactions.md)，源码为 `app/diag_txn`（纯 C99）与 `mw/diag/diag_service`（CPU0 adapter），新增独立诊断请求/数据 xcore 队列。主机覆盖 engine、生产 adapter/dispatcher、队列与 SF 黄金向量；真实三核调度、通信压力及精度工装仍需目标机验证。当前本机已配置 TASKING 许可证，`just fw-build app` 可通过 SCons 构建，旧“只能 IDE”说明不适用于此环境。
