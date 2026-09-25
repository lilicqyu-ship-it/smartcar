# 软件架构与技术参考

适用版本：V2.0（三核分区 + Wi-Fi 模块为 ESP32-C6 / esp-at）。本文是信息型参考，按“查得到”组织；设计动机见文末「设计决策」。接线与引脚电气细节在 [wiring.md](wiring.md)，产品需求在 [requirement.md](requirement.md)。

## 1. 核间分区（TC275 三核）

| 核 | 运行环境 | 职责 | 入口 |
|---|---|---|---|
| CPU0 | FreeRTOS（单核内核，仅此核跑调度器） | 控制任务：命令执行、安全状态机、状态发布、日志桥、控制台 UART、LED | Cpu0_Main.c → `core0_main` |
| CPU1 | 裸机 1 kHz 超循环 | 电机算法：斜率限幅、失联看门狗、急停刹车，驱动 GTM/TB6612 | Cpu1_Main.c → `core1_main` |
| CPU2 | 裸机超循环 | ESP32-C6 AT 链路：softAP+TCP 服务、协议解码、HTTP 控制接口 | Cpu2_Main.c → `core2_main` |

规则：**FreeRTOS API 只允许 CPU0 调用**（移植层绑定 CPU0：STM0 产生 tick、上下文切换中断、CCPN 屏蔽均只作用于 CPU0）。CPU1/CPU2 的时基来自 `Bsp/stime.c`（读 STM0 自由计数，unsigned 回绕安全）。

三核共用一个二进制；启动时各自 `IfxCpu_emitEvent/waitEvent` 同步，CPU0 在同步点**之前**完成 `XCORE_init()`，保证共享内存在任何核使用前已初始化。

## 2. 分层结构

```
┌──────────────────────────────────────────────────────┐
│ App/     robot.c/h      运动状态机·安全·心跳   (CPU0) │
│          motor_algo.c/h  电机算法(斜率/看门狗) (CPU1) │
├──────────────────────────────────────────────────────┤
│ Middleware/ protocol.c/h  协议解码(CPU2)/执行(CPU0)    │
│             xcore.c/h     跨核共享内存+自旋锁         │
│             wifi_at.c/h   ESP32-C6 AT 驱动    (CPU2) │
├──────────────────────────────────────────────────────┤
│ Bsp/     uart.c/h    ASCLIN0 调试串口        (CPU0)  │
│          motor.c/h   GTM PWM+TB6612          (CPU1)  │
│          stime.c/h   STM0 毫秒时基       (CPU1/CPU2) │
├──────────────────────────────────────────────────────┤
│ FreeRtos/ + Configurations/FreeRTOSConfig.h  (CPU0)   │
│ Libraries/  Infineon iLLD                             │
└──────────────────────────────────────────────────────┘
```

依赖方向自上而下；`App` 不感知通信方式，`Middleware` 不碰电机/LED 寄存器（wifi_at 例外，它是 ASCLIN1 的属主）。

## 3. 任务与中断

**CPU0（FreeRTOS）**

| 任务 | 源码 | 优先级 | 栈 | 周期/行为 |
|---|---|---|---|---|
| blinky | Cpu0_Main.c | 1 | 最小 | LED1(P00.5) 250 ms 翻转，系统存活指示 |
| echo | Cpu0_Main.c | 1 | 最小 | ASCLIN0 回显测试 |
| robot | Cpu0_Main.c `vRobotControlTask` | 2 | 2×最小 | 10 ms：排空命令队列→`ROBOT_task()` 安全检查→发布状态块→发布电机目标→清急停旁路→日志桥出队 |

ASCLIN0 中断（CPU0）：TX 优先级 8、RX 4、ER 12。FreeRTOS 内核中断：tick(STM0) 优先级 2、上下文切换 1。

**CPU1（裸机）**：无任务/中断，`MOTOR_ALGO_run()` 1 kHz 节拍（STM0 忙等）。

**CPU2（裸机）**：`WIFI_main()` 超循环。ASCLIN1 中断：`wifiRxISR` 优先级 5、`wifiTxISR` 7、`wifiErISR` 13，`IFX_INTERRUPT(fn, 2, prio)` 声明且 `typeOfService = IfxSrc_Tos_cpu2`（中断表三核共用，`__INTTAB_CPU0/1/2` 同址）。

## 4. 跨核通信（Middleware/xcore.c/h）

共享数据落在默认数据段（CPU0 DSPR）；TC275 无数据 Cache，跨核共享无需 Cache 维护。所有块由**一把自旋锁**保护（`IfxCpu_acquireMutex` = cmpAndSwap 原子交换，临界区仅几字节拷贝），写后 `__dsync()`。

| 通道 | 方向 | 内容与语义 |
|---|---|---|
| 命令队列 (深 8) | CPU2 → CPU0 | 解码后的协议帧 `XcoreCmdMsg{cmd,len,data}`；满则丢弃并入日志 |
| 电机目标 | CPU0 → CPU1 | 左右目标速度 -1000..+1000 + estop 位 + `seq` 计数（每 10 ms 递增，CPU1 据此判失联） |
| 电机实际值 | CPU1 → 遥测 | 算法输出的左右侧速度（斜率后） |
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
                                    TB6612 #1(A/B) #2(C/D) → 4 电机
```

应答路径：HTTP 请求由 TC275 现场生成页面/JSON，经 `AT+CIPSEND=<id>,<len>` 回给手机，随后 `AT+CIPCLOSE`。

## 6. ESP32-C6 AT 桥（Middleware/wifi_at.c，CPU2）

固件来源：`../esp-at`（esp32c6_default 模块，IDF 5.4）。初始化序列（上电延迟 3 s 后执行，均带重试/超时）：

```
ATE0 → AT+CWMODE=2 → AT+CWSAP="AURIX-SmartDrive","12345678",11,3
     → AT+CIPMUX=1 → AT+CIPSERVER=1,8080
```

运行期：`+IPD,<id>,<len>` 二进制安全读取（先读 ASCII 头到 `:`，再读恰好 len 字节）；`CONNECT/CLOSED` 状态行维护 `g_clientConnected`。发送走 `AT+CIPSEND=0,<len>` → 等 `>` → 数据 → 等 `SEND OK`。调试输出经 xcore 日志环转 CPU0 控制台（ASCLIN0 属主是 CPU0，禁止跨核直接打印）。

## 7. 自定义二进制协议参考（protocol.c/h）

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
| 0x32 | EMERGENCY_STOP | — | 急停：CPU2 置旁路位（CPU1 立即刹车）+ 命令入队（CPU0 锁存故障） |

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

响应统一 JSON：`{"state":<u8>,"left":<-100..100>,"right":<..>,"heartbeat":0|1}`，内容取自 xcore 状态块（≤10 ms 旧），`Connection: close` 后主动 `AT+CIPCLOSE`。

## 9. 电机映射参考（motor.c，属主 CPU1）

| 逻辑电机 | 位置 | PWM | IN1 | IN2 | 驱动 | 软件方向翻转 |
|---|---|---|---|---|---|---|
| MOTOR_A | 侧1 前 | P21.0 (ATOM2_4) | P21.4 | P21.5 | TB6612#1-A | 否 |
| MOTOR_B | 侧1 后 | P21.3 (ATOM4_1) | P21.2 | P22.3 | TB6612#1-B | **是** |
| MOTOR_C | 侧2 后 | P00.0 (ATOM1_0) | P00.2 | P00.6 | TB6612#2-A | **是** |
| MOTOR_D | 侧2 前 | P00.8 (ATOM0_7) | P00.10 | P00.12 | TB6612#2-B | 否 |

PWM 20 kHz，`MOTOR_setSpeed(id, -1000..+1000)`；robot 层 -100..+100 → ×10 后经 xcore 下发。轮侧映射（左=A+B，右=C+D）现在定义在 motor_algo.c。

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
没有编码器反馈，PID 无被控量可用。状态机输出的“速度”实际是占空比期望值；加编码器后在 CPU1 的 motor_algo 内替换实现即可，协议、xcore 与接线不变——这正是把算法独立成核内模块的原因之一。

**为什么心跳 50 ms / 超时 100 ms？**
容忍 1 次丢帧不误停车，2 次丢失（100 ms）必须停车——遥控车上电机制响应上限 0.1 s。HTTP 模式由页面 JS 以 50 ms 轮询 `/api/heartbeat` 实现，与二进制协议 `0x21` 语义一致。

**为什么 CRC 用异或和而不是 CRC-8/16？**
帧 ≤20 字节、链路为板上短距离 115200 UART，主要防护干扰毛刺与帧错位；帧头双字节 + LEN + XOR 已够，省 C6/TC275 两侧实现复杂度。上 CAN/无线远控时再升级。

**为什么 B/C 电机软件翻转方向？**
两侧电机镜像安装，物理正接时转向定义相反。统一在 `g_dirInvert` 表处理，接线保持对称，排障时“查表即得”，不动线。

**为什么 CPU1/CPU2 的调试打印不直接走 ASCLIN0？**
iLLD ASC 驱动的软件 FIFO 与临界区只在属主核内互斥；两个核并发调用会踩 FIFO 状态。因此核间日志统一走 xcore 日志环，由属主 CPU0 落串口。

**看门狗现状**：`Cpu*_Main.c` 目前显式关闭了 CPU/安全看门狗（调试期行为），量产化必须重新启用并在各核循环喂狗——这是 requirement.md F09 的已知未完成项。
