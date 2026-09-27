# 软件架构与技术参考（**当前代码现状 / demo V2.0**）

> 文档编号 **31** · 域 TC275 · 状态：现状参考（描述当前代码） · 上级索引 [00-index.md](../00-index.md) · 操作指南见 [32-tc275-dev-guide.md](32-tc275-dev-guide.md)

> **文档定位（重要）**：本文档描述**工作区里现在跑的这套代码**（esp-at + UART 链路、开环 PWM），用于排障、回归与理解现状。量产目标态的设计基准是 [21-software-design.md](../20-design/21-software-design.md)（SDD，当前 V1.6）——**两者不一致时以 SDD 为准**，本文不描述待实现内容。**读前先注意一件事**：UART 板间链路已于 2026-09-26 弃用（见下一条），所以本文 §5/§6/§8 的 UART/AT 通路描述的是"删掉 `USE_SPI_LINK` 才会编出来的那条分支"，不是当前构建产物。
> **目录重排（2026-09-26）**：仓库已按 SDD §3.4 重排，本文下图的 `App/`、`Middleware/`、`Bsp/` 现对应 `app/`、`mw/`（xcore/proto/sf）+`com/`（link/spi_hal_pins/wifi_at）、`bsp/`；`encoder.c`/`motor_algo.c` 在 `rt/`。正文行文保留 demo 期目录名，按本映射读。
> 本文唯一不可从代码推导、且已被 SDD 吸收的结论是 §3 的**中断向量表/优先级铁律**，权威版本见 **SDD §18**；本文不再维护该条。
> 历史版本记录：V2.0 = 三核分区 + Wi-Fi 模块换为 ESP32-C6（esp-at）。板间链路已在 V1.2 决策改为 SPI（SDD §3.7），本文相关段落（§1/§5/§6/§8）仍是 UART 口径，属**demo 现状描述**——SPI 五行实物线已接好（2026-09-26），两侧 SPI 固件代码也已落地（TC275 侧 `com/`+`mw/sf/`；C6 侧 `c6_car` `22e15f2`），TC275 侧 IDE 构建链接闭合已达成（2026-09-26）。**2026-09-26 起 UART 板间链路弃用**：`USE_SPI_LINK` 定义进 Debug 与 Release 两个 TASKING 构建配置，本文 §1/§5/§6/§8 描述的 UART/AT 通路**不再被默认构建产出**（删掉该符号才编得出），只作 C6 调试控制台与 G1 失败的应急返修（`21 §5.6` 末条、`21 §18 C15`、`23 §2`）。**SPI 链路本身从未通电联调**，启用验证由 G1 把关（见 `22 §8`）。**V2.1（2026-09-26）** = 只加上述口径（本文档结构未动）：UART 段落自此是"删符号才编得出的分支"的描述，不再是默认构建产物。

本文是信息型参考，按"查得到"组织；设计动机见文末「设计决策」。接线与引脚电气细节在 [23-wiring.md](../20-design/23-wiring.md)，产品需求在 [11-requirements.md](../10-product/11-requirements.md)。

## 1. 核间分区（TC275 三核）

| 核 | 运行环境 | 职责 | 入口 |
|---|---|---|---|
| CPU0 | FreeRTOS（单核内核，仅此核跑调度器） | 控制任务：命令执行、安全状态机、状态发布、日志桥、控制台 UART、LED | Cpu0_Main.c → `core0_main` |
| CPU1 | 裸机 1 kHz 超循环 | 电机算法：斜率限幅、失联看门狗、急停刹车，驱动 GTM/D24A；编码器×4 测速（TIEM 边沿中断 + 1 kHz 测速任务） | Cpu1_Main.c → `core1_main` |
| CPU2 | 裸机超循环 | ESP32-C6 AT 链路：softAP+TCP 服务、协议解码、HTTP 控制接口 | Cpu2_Main.c → `core2_main` |

规则：**FreeRTOS API 只允许 CPU0 调用**（移植层绑定 CPU0：STM0 产生 tick、上下文切换中断、CCPN 屏蔽均只作用于 CPU0）。CPU1/CPU2 的时基来自 `Bsp/stime.c`（读 STM0 自由计数，unsigned 回绕安全）。

三核共用一个二进制；启动时各自 `IfxCpu_emitEvent/waitEvent` 同步，CPU0 在同步点**之前**完成 `XCORE_init()`，保证共享内存在任何核使用前已初始化。

## 2. 分层结构

```
┌──────────────────────────────────────────────────────┐
│ app/     robot.c/h      运动状态机·安全·心跳   (CPU0) │
├──────────────────────────────────────────────────────┤
│ rt/      motor_algo.c/h  电机算法(斜率/看门狗) (CPU1) │
│          encoder.c/h GTM TIM 编码器×4        (CPU1)  │
├──────────────────────────────────────────────────────┤
│ mw/proto/  protocol.c/h  协议解码(CPU2)/执行(CPU0)    │
│ mw/xcore/  xcore.c/h     跨核共享内存+自旋锁         │
│ mw/sf/     sf_frame.c/h  SF 帧编解码(SPI 链路,CPU2)  │
│ com/       wifi_at.c/h   ESP32-C6 AT 驱动    (CPU2)  │
│            link.c/spi_hal_pins.c  QSPI3 泵 (USE_SPI_LINK) │
├──────────────────────────────────────────────────────┤
│ bsp/     uart.c/h    ASCLIN0 调试串口        (CPU0)  │
│          motor.c/h   GTM PWM+D24A            (CPU1)  │
│          stime.c/h   STM0 毫秒时基       (CPU1/CPU2) │
├──────────────────────────────────────────────────────┤
│ FreeRtos/ + Configurations/FreeRTOSConfig.h  (CPU0)   │
│ Libraries/  Infineon iLLD                             │
└──────────────────────────────────────────────────────┘
```

依赖方向自上而下；`app` 不感知通信方式，`mw`/`com` 不碰电机/LED 寄存器（wifi_at 例外，它是 ASCLIN1 的属主）。

## 3. 任务与中断

**CPU0（FreeRTOS）**

| 任务 | 源码 | 优先级 | 栈 | 周期/行为 |
|---|---|---|---|---|
| blinky | Cpu0_Main.c | 1 | 最小 | LED1(P00.5) 250 ms 翻转，系统存活指示 |
| echo | Cpu0_Main.c | 1 | 最小 | ASCLIN0 回显测试 |
| robot | Cpu0_Main.c `vRobotControlTask` | 2 | 2×最小 | 10 ms：排空命令队列→`ROBOT_task()` 安全检查→发布状态块→发布电机目标→清急停旁路→日志桥出队 |

ASCLIN0 中断（CPU0）：TX 优先级 8、RX 4、ER 12。FreeRTOS 内核中断：tick(STM0) 优先级 2、上下文切换 1。

**CPU1（裸机）**：`MOTOR_ALGO_run()` 1 kHz 节拍（STM0 忙等）+ 编码器边沿中断。GTM TIM0 CH0~CH7 各接一路编码器相（TIEM 输入事件模式、双边沿），`enc0Isr..enc7Isr` 优先级 **16~23**、`typeOfService = IfxSrc_Tos_cpu1`，同样 `IFX_INTERRUPT(fn, 0, prio)` 声明（0 号表共用）；ISR 内做 ×4 正交解码，主循环无每边沿轮询负担。

**CPU2（裸机）**：`WIFI_main()` 超循环。ASCLIN1 中断：`wifiRxISR` 优先级 5、`wifiTxISR` 7、`wifiErISR` 13，`IFX_INTERRUPT(fn, 0, prio)` 声明且 `typeOfService = IfxSrc_Tos_cpu2`。

> 为什么是 `0` 号表、优先级怎么分配、踩错的症状与验证方法 —— 见 **SDD §18 C1/C2**（权威版本，本文不再维护）。

## 4. 跨核通信（Middleware/xcore.c/h）

共享数据落在默认数据段（CPU0 DSPR）；TC275 无数据 Cache，跨核共享无需 Cache 维护。所有块由**一把自旋锁**保护（`IfxCpu_acquireMutex` = cmpAndSwap 原子交换，临界区仅几字节拷贝），写后 `__dsync()`。

| 通道 | 方向 | 内容与语义 |
|---|---|---|
| 命令队列 (深 8) | CPU2 → CPU0 | 解码后的协议帧 `XcoreCmdMsg{cmd,len,data}`；满则丢弃并入日志 |
| 电机目标 | CPU0 → CPU1 | 左右目标速度 -1000..+1000 + estop 位 + `seq` 计数（每 10 ms 递增，CPU1 据此判失联） |
| 电机实际值 | CPU1 → 遥测 | 算法输出的左右侧速度（斜率后） |
| 编码器实测 | CPU1 → 遥测 | `XCORE_encoderPublish/Read`（`XcoreEncoder` 快照）：percent×10（-1000..+1000，alive 时 CPU0 覆盖状态块 leftSpeed/rightSpeed）+ 物理域 mm/s 与左右侧里程 mm（CPU2 填 SF 遥测 `vMeasL/R`、`odoSession`）+ alive 位（500 ms 内有边沿）。另有 `XCORE_logi`：`XCORE_logu` 的带符号版（负轮速） |
| 状态块 `ProtocolStatus` | CPU0 → CPU2 | robot 状态镜像，10 ms 刷新；CPU2 直接用于 0x40 应答与 HTTP JSON |
| 急停旁路 | CPU2 置位 / CPU0 清除 | `XCORE_estopRequest()` 让 CPU1 **不等** 10 ms 控制拍直接刹车 |
| 日志环 (1 KB) | CPU1/CPU2 → CPU0 | 整行拷贝入环（满则整行丢弃），CPU0 控制任务 `XCORE_logService()` 出环打印 |

## 5. 数据流

```
手机浏览器 ──HTTP GET──► C6(+IPD,link,len) ──UART──► [CPU2] wifi_at.c 逐字节读帧
PC 上位机 ──TCP:8080──►                              │ GET /api/... → 命令入队
                                                     │ 二进制帧 → PROTO_feedByte()
                                                     ▼
                                          [CPU2] protocol.c 解码
                                                     │ 0x20 GET_STATUS → 读状态块立即应答
                                                     │ 0x32 EMERGENCY_STOP → 急停旁路(直达CPU1)
                                                     ▼  xcore 命令队列
[CPU0] 控制任务 (10 ms)：出队 → PROTO_handleCommand → robot.c 状态机
        ├─ 心跳 100 ms 超时→停车
        ├─ FAULT/急停锁存
        └─ 左右轮目标 -100..+100 ──xcore(×10)──► [CPU1] motor_algo.c 1 kHz
                斜率限幅(±2/ms) → 失联 150 ms 看门狗 → estop 刹车
                                                     ▼
                                    motor.c → GTM ATOM PWM 20kHz
                                    D24A 四路驱动板（J4=A/B · J6=C/D）→ 4 电机
```

应答路径：HTTP 请求由 TC275 现场生成页面/JSON，经 `AT+CIPSEND=<id>,<len>` 回给手机，随后 `AT+CIPCLOSE`。

## 6. ESP32-C6 AT 桥（Middleware/wifi_at.c，CPU2）

固件来源：`../esp-at`（esp32c6_default 模块，IDF 5.4）。初始化序列（上电延迟 3 s 后执行，均带重试/超时）：

```
ATE0 → AT+CWMODE=2 → AT+CWSAP="AURIX-SmartDrive","12345678",11,3
     → AT+CIPMUX=1 → AT+CIPSERVER=1,8080
```

运行期：`+IPD,<id>,<len>` 二进制安全读取（先读 ASCII 头到 `:`，再读恰好 len 字节）；`CONNECT/CLOSED` 状态行维护 `g_clientConnected`。发送走 `AT+CIPSEND=0,<len>` → 等 `>` → 数据 → 等 `SEND OK`。调试输出经 xcore 日志环转 CPU0 控制台（ASCLIN0 属主是 CPU0，禁止跨核直接打印）。

## 7. 自定义二进制协议参考（protocol.c/h，**demo 帧，已非任何链路的真源**）

> 量产协议分两段：板间 LINK 段 = **SF 帧**（SDD §6.1a），手机 WS 段 = **v2 帧**（SDD §6.1b）。本节 `AA 55 CMD LEN DATA XOR-CRC` 只在当前代码里活着，用于回归对照与 esp-at **应急返修**通道（UART 作为板间链路已于 2026-09-26 弃用，且这套 demo 帧本身不在 SF 链路里，见 `21 §6.1a`）。

### 7.1 帧格式

```
偏移  0    1    2    3    4..4+LEN-1        4+LEN
     AA   55   CMD   LEN   DATA[LEN]          CRC
```

- `LEN` ≤ `PROTO_MAX_PAYLOAD`(16)；CRC = `CMD ^ LEN ^ DATA[0] ^ ... ^ DATA[LEN-1]`（异或和，种子 0x00）
- 解析器为 6 状态机，逐字节喂入（`PROTO_feedByte`，CPU2），非法帧自动重同步
- 校验失败/非法 CMD：静默丢弃，不 NAK
- **跨核分工**：解码在 CPU2（`PROTO_routeFrame`），执行在 CPU0（`PROTO_handleCommand`）

### 7.2 命令表（主机 → TC275）

| CMD | 名称 | DATA | 动作 |
|---|---|---|---|
| 0x01 | STOP | — | 停车 |
| 0x02/0x03 | FORWARD / BACKWARD | — | 用已存速度 S 运动 |
| 0x04/0x05 | LEFT / RIGHT | — | 差速转 |
| 0x06/0x07 | FORWARD_LEFT / FORWARD_RIGHT | — | 弧线（一侧半速） |
| 0x08/0x09 | ROTATE_LEFT / ROTATE_RIGHT | — | 原地旋 |
| 0x10 | SET_SPEED | 1 B：S（±）或 2 B：left,right（signed） | len≥2 时直接左右轮目标 |
| 0x20 | GET_STATUS | — | CPU2 读状态块立即回 0x40 |
| 0x21 | HEARTBEAT | — | 刷新 lastHeartbeatTick |
| 0x30 | RESET | — | 复位状态机 |
| 0x31 | CLEAR_FAULT | — | 解除 FAULT（CPU0 同时清急停旁路） |
| 0x32 | EMERGENCY_STOP | — | 急停：命令**先入队**（CPU0 锁存 FAULT），**随后**置旁路位（CPU1 当拍刹车）——该顺序防止控制拍在入队前清掉旁路 |

### 7.3 状态应答（TC275 → 主机），CMD = 0x40，LEN = 6

| 偏移 | 字段 | 类型 | 说明 |
|---|---|---|---|
| 0 | state | u8 | 0x00 INIT, 0x01 IDLE, 0x02 FORWARD, 0x03 BACKWARD, 0x04/0x05 LEFT/RIGHT, 0x06/0x07 弧线, 0x08/0x09 旋转, 0x0A FAULT |
| 1 | leftSpeed | s8 | -100..+100 |
| 2 | rightSpeed | s8 | -100..+100 |
| 3 | heartbeatOk | u8 | 1 = 心跳在时限内 |
| 4 | faultCode | u8 | 0 = 无故障 |
| 5 | emergencyStop | u8 | 1 = 急停生效 |

## 8. HTTP API 参考（浏览器控制页）

服务地址 `http://192.168.4.1:8080`（C6 AP 模式，TC275 生成响应）。

| 路径 | 作用 |
|---|---|
| `GET /` | 控制页（内联 HTML/JS，按钮 onpointerdown 持续发心跳） |
| `GET /api/forward?speed=N` 等 `backward/left/right` | 设置速度并执行；N 钳位 0..100（SET_SPEED + 运动命令依次入队） |
| `GET /api/stop` | 停车并停心跳轮询 |
| `GET /api/heartbeat` / `/api/status` | 仅刷新心跳/取状态 |
| `GET /api/<其他>` | 视为非法 → 停车 |

响应统一 JSON：`{"state":<u8>,"left":<-100..100>,"right":<..>,"heartbeat":0|1}`，内容取自 xcore 状态块（≤10 ms 旧）。响应头 `Connection: keep-alive`，回包走来源链路（`+IPD` 解析出的 link id），页面不再每请求重连（旧版 `Connection: close` + `AT+CIPCLOSE` 方案因拥塞已废弃，待烧录验证）。

## 9. 电机映射参考（motor.c，属主 CPU1）

| 逻辑电机 | 位置 | PWM | IN1 | IN2 | 驱动 | 软件方向翻转 |
|---|---|---|---|---|---|---|
| MOTOR_A | 侧1 前 | P21.0 (ATOM2_4) | P21.4 | P21.5 | D24A J4-A | 否 |
| MOTOR_B | 侧1 后 | P21.3 (ATOM4_1) | P21.2 | P22.3 | D24A J4-B | **是** |
| MOTOR_C | 侧2 后 | P00.0 (ATOM1_0) | P00.2 | P00.6 | D24A J6-C | **是** |
| MOTOR_D | 侧2 前 | P00.8 (ATOM0_7) | P00.10 | P00.12 | D24A J6-D | 否 |

PWM 20 kHz，`MOTOR_setSpeed(id, -1000..+1000)`；robot 层 -100..+100 → ×10 后经 xcore 下发。轮侧映射（左=A+B，右=C+D）现在定义在 motor_algo.c。

### 9.1 编码器映射（MG310 内置 260 线 AB 正交，固件已实现 `rt/encoder.c`，接线见 23-wiring.md §8）

| 电机 | A 相 | B 相 | GTM0 TIM 通道（TIEM 中断） | 排针 |
|---|---|---|---|---|
| A（电机1） | P33.4 | P33.5 | TIM0_0 / TIM0_1 | X2-32/33 |
| B（电机2） | P33.6 | P33.7 | TIM0_2 / TIM0_3 | X2-34/35 |
| C（电机3） | P33.0 | P33.1 | TIM0_4 / TIM0_5 | X2-28/29 |
| D（电机4） | P33.2 | P33.3 | TIM0_6 / TIM0_7 | X2-30/31 |

A 相进偶数通道、B 相进奇数通道，八通道全部配 TIEM 双边沿 + NEWVAL 中断（优先级 16~23，TOS=CPU1），ISR 按 4x 正交表软解码，≈**21225 计数/轮转**（减速比 1:20.409）。**实现注记**：原方案的 "TIM UDC 硬件正交"（`UDCCTRL/CLS/DUTC`）是 GTM gen2 的寄存器，本芯片 TC27D 的 GTM gen3 TIM **没有 UDC**（`IfxGtm_regdef.h` 无此寄存器，模式仅 TPWM/TPIM/TIEM/TIPM/TBCM/TGPS），GPT12 增量口与 ERU 输入又都不在引出脚上，故改为"硬件边沿中断 + 软件正交"。实测值经 `ENCODER_task()`（1 kHz，8 ms 中值窗）换算 mm/s 并经 xcore 出遥测；`ENCODER_getRawCounts` 供产测判向。

## 10. 设计决策（解释）

**为什么这样分核？**
控制、算法、通信三者的实时特性不同：控制要 10 ms 确定拍并与协议解耦；算法要 1 kHz 硬节拍且绝不能被长临界区（HTTP 字符串拼接等）拖住；AT 交互有大量毫秒级阻塞等待。分到三个核后互不抢占，FreeRTOS 单核内核只跑 CPU0 免去 SMP 移植。代价是所有共享状态必须显式过 xcore（拷贝 + 自旋锁），命令多一拍 10 ms 延迟——对遥控车无感。

**为什么急停要有独立旁路位？**
如果急停只走命令队列，最快也要等 CPU0 下一个 10 ms 控制拍才动作。CPU2 收到 0x32 直接置 `XCORE_estopRequest()`，CPU1 的 1 kHz 算法当拍即刹车，两条路径（旁路 + 队列锁存 FAULT）互为冗余；旁路由 CPU0 在故障清除/复位后回收。

**为什么 CPU1 要有失联看门狗？**
三核架构引入了“CPU0 活着但 CPU1 在跑”的可能错位。电机属主 CPU1 以 `seq` 计数检测目标流（正常 10 ms 一拍），超过 150 ms 没有新目标就视为控制核失联，自行斜降停车，而不是保持最后的速度。

**为什么 HTTP/协议解析放在 TC275 而不是 C6？**
C6 只做 AT 透明 TCP 桥，不跑业务。好处：换任何 AT 模组代码零改动；控制逻辑与运动控制同处一个实时域，心跳超时、急停的执行不依赖协议栈之上再通信一次。代价：C6 无法独立提供页面，TC275 要处理 HTTP 文本（本实现只支持单行 GET 请求，够用且可控）。

**为什么 V2.0 仍是开环 PWM，不上 PID？**
没有编码器反馈，PID 无被控量可用。状态机输出的“速度”实际是占空比期望值；加编码器后在 CPU1 的 motor_algo 内替换实现即可，协议、xcore 与接线不变——这正是把算法独立成核内模块的原因之一。V1.1 编码器硬件路线已定稿（MG310 内置 AB 编码器 → GTM0 TIM UDC 硬件计数，引脚映射见 §9.1 与 23-wiring.md §8），软件侧只替换 motor_algo 的反馈量来源。

**为什么心跳 50 ms / 超时 100 ms？**
容忍 1 次丢帧不误停车，2 次丢失（100 ms）必须停车——遥控车上电机制响应上限 0.1 s。HTTP 模式由页面 JS 以 50 ms 轮询 `/api/heartbeat` 实现，与二进制协议 `0x21` 语义一致。

**为什么 CRC 用异或和而不是 CRC-8/16？**
帧 ≤20 字节、链路为板上短距离 115200 UART，主要防护干扰毛刺与帧错位；帧头双字节 + LEN + XOR 已够，省 C6/TC275 两侧实现复杂度。上 CAN/无线远控时再升级。

**为什么 B/C 电机软件翻转方向？**
两侧电机镜像安装，物理正接时转向定义相反。统一在 `g_dirInvert` 表处理，接线保持对称，排障时“查表即得”，不动线。

**为什么 CPU1/CPU2 的调试打印不直接走 ASCLIN0？**
iLLD ASC 驱动的软件 FIFO 与临界区只在属主核内互斥；两个核并发调用会踩 FIFO 状态。因此核间日志统一走 xcore 日志环，由属主 CPU0 落串口。

**看门狗现状**：`Cpu*_Main.c` 目前显式关闭了 CPU/安全看门狗（调试期行为），量产化必须重新启用并在各核循环喂狗——这是 11-requirements.md F09 的已知未完成项，目标态设计见 SDD §7.2 与 §18 C8。
