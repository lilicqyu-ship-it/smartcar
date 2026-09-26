# myCar（AURIX SmartDrive）工程评估报告

| 项 | 内容 |
|---|---|
| 评估日期 | 2026-09-26 |
| 评估对象 | `myCar` 工程（KIT-AURIX-TC275-LITE + ESP32-C6 esp-at + 2×TB6612 四电机差速小车） |
| 基线版本 | `acce1f8`（fix(wifi): 冒号仅在 +IPD 帧头作为分隔符），另含工作区未提交改动（keep-alive / g_activeLink） |
| 评估范围 | 全部自研代码（三核入口 + App/Middleware/Bsp 共约 2400 行）、FreeRTOS 配置、链接脚本、`.cproject`、仓库内容、doc/ 四篇文档 |
| 评估方式 | 人工逐行通读全部自研源码 + 构建配置/链接脚本/仓库内容核查 + 文档-代码一致性比对 |

---

## 1. 执行摘要

**总体结论：架构设计优秀、文档质量高、阶段性功能完整；但存在 1 个可导致急停丢失的跨核竞态、1 个 CPU2 用户栈溢出隐患，以及"硬件看门狗全禁用 + 零测试"两个工程化短板。当前状态适合开发调试与演示，距"可长期无人值守运行"还差一批安全加固与工程化工作。**

分项评分（5 分制）：

| 维度 | 得分 | 简评 |
|---|---|---|
| 架构设计 | ★★★★☆ | 三核按实时特性分区、通信/控制解耦、多层软件看门狗，设计思路清晰且执行到位 |
| 功能完整度 | ★★★★☆ | V1.0 需求功能基本全部落地（差速转向语义除外），HTTP + 二进制协议双通道 |
| 安全性（Safety） | ★★★☆☆ | 软件级保护链完整，但急停竞态可击穿，且硬件看门狗全禁用 |
| 健壮性（Robustness） | ★★☆☆☆ | AT 字节流无回推机制、阻塞式收发互相挤兑、CPU2 栈偏小 |
| 代码质量 | ★★★★☆ | 命名统一、注释解释"为什么"、防御性检查普遍存在，少数遗留死代码 |
| 构建/仓库管理 | ★★☆☆☆ | 113 MB FreeRTOS 全量克隆 + 18 MB 未用 AWS SDK 入库；无命令行构建、无 CI |
| 文档 | ★★★★☆ | 四篇文档覆盖全流程，个别处与代码漂移（详见 §4.3） |
| 测试 | ★☆☆☆☆ | 无任何自动化测试；核心纯逻辑（协议解析/状态机）非常适合补主机端单测 |

必须优先处理的 4 件事（详见 §4.1、§6）：

1. **急停竞态**：`XCORE_cmdPush` 失败被忽略 + CPU0 每 10 ms 无条件清除急停旁路位，队列满时急停会**整体丢失**。
2. **CPU2 用户栈仅 2 KB**，而 HTTP 页面响应调用链的局部数组累计约 3.3 KB——当前是**静默溢出**进空闲 DSPR，属潜伏地雷。
3. **心跳超时不锁存故障**：超时只停车一次，之后任何一条运动命令都能让小车在无心跳状态下重新跑起来。
4. **硬件看门狗三核 + 安全看门狗全部禁用**且无喂狗代码（文档已自我声明为已知项，量产阻塞）。

---

## 2. 工程概况

- **硬件**：TC275（TriCore 三核 200 MHz）+ ESP32-C6（esp-at 固件，AT 透传）+ 2×TB6612（4 电机）。
- **软件分区**（README/architecture.md 与代码一致）：
  - CPU0：FreeRTOS，`vRobotControlTask`（10 ms）命令执行 + 安全状态机 + 状态发布 + 调试串口；
  - CPU1：裸机 1 kHz 超循环 `MOTOR_ALGO_run()`，斜率限幅 + 失联看门狗 + 急停立即刹车；
  - CPU2：裸机超循环 `WIFI_main()`，AT 收发、+IPD 帧解码、HTTP 控制页。
- **核间通信**：`Middleware/xcore.c` 单硬件自旋锁保护的共享内存：命令队列（CPU2→CPU0）、电机目标+seq（CPU0→CPU1）、应用状态（CPU1→遥测）、状态块（CPU0→CPU2）、日志环形缓冲（CPU1/CPU2→CPU0）、急停旁路位（CPU2→CPU1 直达）。
- **协议**：`AA 55 CMD LEN DATA CRC`（逐字节异或 CRC），GET_STATUS 由 CPU2 直接应答，急停命令在入队后额外锁存 CPU1 旁路位。
- **代码规模**：自研 8 个 .c + 8 个 .h + 3 个核入口，共 2380 行；其余为 iLLD（523 个文件）与 FreeRTOS 内核（7975 个文件）。

---

## 3. 工程优点（值得保持）

1. **分核架构与实时特性匹配得很好**。1 kHz 电机环放 CPU1 裸机、WiFi 阻塞式 AT 会话放 CPU2、需要 OS 服务的状态机放 CPU0，每核职责单一；"No FreeRTOS API may be used here" 的约束在头文件注释中明确声明（`Cpu1_Main.c:27-29`、`Cpu2_Main.c:27-29`）。
2. **启动时序正确**：`XCORE_init()` 在 `IfxCpu_waitEvent` 之前执行（`Cpu0_Main.c:127-132`），保证共享内存在任何核越过同步点前完成清零，自旋锁 `g_lock` 不存在未初始化竞态。
3. **多层软件保护链完整**（这是本工程最大的亮点）：
   - 手机心跳 100 ms 超时停车（`robot.c:166`）；
   - CPU1 对 CPU0 目标流的 150 ms 失联看门狗，靠 `seq` 计数判失联而非时间戳猜测（`motor_algo.c:56-68`）；
   - 急停走 CPU2→CPU1 直达旁路位，不等 CPU0 的 10 ms 节拍（`protocol.c:130-134`、`motor_algo.c:104-108`）；
   - 斜率限幅 0..1000 需 0.5 s（`motor_algo.h:20`），急停则跳过限幅立即刹车；
   - BSP 层再兜底钳位 ±1000（`motor.c:115-122`）。
4. **跨核日志桥设计 thoughtful**：整行拷贝入环、写索引后移，读侧免锁；环将满时整行丢弃而非写半行（`xcore.c:197-213`）。
5. **+IPD 二进制安全解析**：仅在 `+IPD,` 前缀下把冒号当帧头分隔符，修复了 esp-at v3 事件行（如 `+STA_CONNECTED:"..."`）被截断的问题，且有注释解释缘由（`wifi_at.c:437-453`）。
6. **方向反接集中处理**：`g_dirInvert` 查表（`motor.c:36-41`），接线保持对称，排障不动线。
7. **文档体系完整**：README（架构图/快速上手）+ getting-started（新手全流程）+ architecture（协议/引脚/API）+ wiring（接线表）+ requirement（需求与路线图），且需求文档诚实地把硬件看门狗标记为 🔶（requirement.md F09 注）。
8. **提交纪律好**：约定式提交 + 中文说明，每个 fix 都能在 doc 中找到对应记录（如 CPU2 中断向量表 0 的坑，`wifi_at.c:25-31`）。

---

## 4. 问题清单

严重级别定义：**P0**＝可造成急停/安全功能失效；**P1**＝可造成功能错误、静默数据损坏或明确与文档相悖；**P2**＝健壮性/规范缺陷，特定条件下劣化；**P3**＝工程化/仓库/文档改进项。

### 4.1 P0 —— 安全与正确性

#### P0-1 急停命令入队失败被忽略，CPU0 周期性清除旁路位 → 急停可能整体丢失

- **位置**：`protocol.c:122-134`（push 后无条件 `XCORE_estopRequest()`）、`protocol.c:128`（`(void)XCORE_cmdPush(&msg)` 返回值被丢弃）、`Cpu0_Main.c:104-107`（无故障时每 10 ms 调 `XCORE_estopClear()`）、`xcore.c:125-135`。
- **机理**：急停走两条腿——命令入队（CPU0 据此锁存 `ROBOT_FAULT_EMERGENCY_STOP`）+ 旁路位（CPU1 立即刹车，CPU0 在"自身无故障"时负责清除）。代码注释假设"先入队、后锁存旁路，CPU0 见到命令后就不会再清"，但该假设在两种情况下被打破：
  1. **队列满（严重）**：命令队列仅 8 深（`xcore.h` 中 `XCORE_CMD_QUEUE_LEN 8`）。若 CPU0 卡顿 > 400 ms（10 ms×8 条，心跳 20 Hz + 运动命令很容易填满），`XCORE_cmdPush` 返回 FALSE 被忽略 → CPU0 **永远不知道**发生过急停 → 故障未锁存 → 下一个 10 ms 节拍 `ROBOT_isEmergencyStop()==FALSE && faultCode==0` 成立 → `XCORE_estopClear()` 把旁路位抹掉 → CPU1 解除刹车、按旧目标继续跑。**急停彻底丢失，且无任何日志**。
  2. **时序窗（轻微）**：push 发生在 CPU0 本轮 drain 之后，本轮末尾的 `estopClear()` 先抹掉旁路位，电机恢复最多一个节拍（10 ms）后在下一轮重新锁存。仅是抖动，可接受，但说明"电平式清理"设计脆弱。
- **影响**：安全功能在最需要大负载/卡顿的场景下失效，后果是车辆失控。
- **建议**（组合采用）：
  1. 检查 `XCORE_cmdPush` 返回值；失败至少 `XCORE_logln` 告警并计数，急停命令失败时必须重试或直接置故障；
  2. 把 `XCORE_estopClear()` 从"每节拍电平清理"改为**边沿触发**：仅在 `ROBOT_cmdClearFault()`/`ROBOT_cmdReset()` 内部、确认故障确已解除时调用一次；
  3. 兜底：CPU0 在控制任务中轮询 `XCORE_estopIsActive()`，见 TRUE 即自行调 `ROBOT_cmdEmergencyStop()` 锁存故障（不依赖 CPU2 入队成功）。

#### P0-2 CPU2 用户栈 2 KB，HTTP 页面响应调用链需 ~3.3 KB —— 静默栈溢出

- **位置**：`Lcf_Tasking_Tricore_Tc.lsl:38`（`LCF_USTACK2_SIZE 2k`）、`wifi_at.c:37`（`WIFI_HTTP_RESPONSE_MAX 1536`）、`wifi_at.c:184-197`（`response[1536]` 局部数组）、`wifi_at.c:213`（`body[192]`）、`wifi_at.c:345-347`（`WIFI_sendRaw` 内 `cmd[256]+line[256]`）、`wifi_at.c:576-634`（`WIFI_main` 内 `line[256]+frame[512]+statusLine[256]`）。
- **机理**：TriCore/TASKING 下大局部数组位于用户栈（ustack）。调用链 `WIFI_main → wifiHttpHandleRequest → wifiHttpSend → WIFI_sendRaw` 同帧活跃的局部数组合计 512+256+256+192+1536+256+256 ≈ **3.3 KB**，超出 ustack2 的 2 KB。当前 USTACK2 布局下方是空闲 DSPR2（120 KB 中占用极少），**溢出写进空闲区，暂时不崩溃**——这正是"能跑"的假象，属最危险的潜伏缺陷：一旦 CPU2 数据占用增长（加功能/换链接脚本），故障将以随机内存损坏的形式出现，极难排查。
- **影响**：静默内存损坏；当前每次手机打开控制页都会触发一次约 1.3 KB 的越界写。
- **建议**（任选其一，推荐 1+3 组合）：
  1. 把 HTTP 响应改为**分段发送**：先拼 200 字节级响应头直接发送，再原样发送页面（页面本就是 `static const`，在 rodata 不占栈）与 JSON body，使单帧栈占用 < 512 B；
  2. 或将 `LCF_USTACK2_SIZE` 提到 4 KB（DSPR2 120 KB 空间充裕），并静态断言最坏路径栈深；
  3. 长效机制：用 TASKING `-csa-refill`/栈水印或链接期 stack usage 分析，把三核 ustack 余量写进 architecture.md。

#### P0-3 心跳超时不锁存故障，超时后运动命令仍可重新驱动车辆

- **位置**：`robot.c:163-175`（`ROBOT_task` 超时仅清速度、置 `heartbeatOk=FALSE`，不动 `fault`）、`robot.c:66-87`（`ROBOT_cmdMotion` 不检查 `heartbeatOk`）、`robot.c:10-12`（`ROBOT_FAULT_COMM_TIMEOUT` 定义后从未使用——死枚举佐证了这一遗漏）。
- **机理**：需求 §22 的语义是"通信异常 → 停车"，当前实现只在超时沿执行一次停车；此后 `fault == NONE`，任何后续 FORWARD/SET_SPEED 命令都会照常执行——小车可以在**完全没有心跳**的情况下被单条命令驱动到无限远。对"网页按钮 onpointerup 丢包"或恶意客户端尤其危险。
- **影响**：失联保护可被绕过，等同保护强度从"失联即锁死"降级为"失联停一次"。
- **建议**：超时时锁存 `ROBOT_FAULT_COMM_TIMEOUT` 并拒绝一切运动命令；收到合法 HEARTBEAT（或 CLEAR_FAULT）时解除。这样 `faultCode` 也能如实上报给手机端。

#### P0-4 硬件看门狗（三核 CPU 看门狗 + 安全看门狗）全部禁用，且无喂狗代码

- **位置**：`Cpu0_Main.c:121-125`、`Cpu1_Main.c:43-46`、`Cpu2_Main.c:43-46`。
- **机理**：三核上电即关狗，全工程无任何 `IfxScuWdt_service` 调用。软件保护链再完整也覆盖不了"单核跑飞/死循环但仍处在线状态"的场景：例如 CPU1 的 1 kHz 循环若在 `MOTOR_setDuty` 之后挂死且 PWM 保持非零占空比，TB6612 STBY 又是硬件常拉高（未受 GPIO 控制），电机会**带着最后一次占空比一直转**——没有任何机制能切断。requirement.md F09 已自我声明"量产前必须重新启用"，本报告将其升级为 P0 以免被长期搁置。
- **建议**：
  1. 三核 CPU 看门狗按核使能并在各自超循环/任务喂狗（CPU0 可在 robot 任务喂，CPU1 在 `MOTOR_ALGO_run` 喂，CPU2 在 `WIFI_main` 喂）；安全看门狗一并启用；
  2. 硬件兜底：把 TB6612 STBY 引脚改接 GPIO，故障/看门狗超时时拉低断开电机供级（一阶故障容错）；
  3. 在 getting-started 中注明"调试期可关狗"的开关宏（如 `#if DEBUG_WATCHDOG`），避免量产配置漂移。

### 4.2 P1 —— 功能错误 / 明确与文档相悖

#### P1-1 LEFT/RIGHT 与 ROTATE_LEFT/RIGHT 完全同速，与文档"差速转"定义相悖

- **位置**：`robot.c:79-84`（`LEFT: (-S, S)` 与 `ROTATE_LEFT: (-S, S)` 逐参数相同；RIGHT 同理）vs `doc/architecture.md:122-124`（0x04/0x05 LEFT/RIGHT＝"差速转"，0x08/0x09＝"原地旋"）。
- **影响**：8 条运动命令实际只有 6 种行为，手机端"左转/右转"变成原地自旋，文档承诺的功能缺失。
- **建议**：LEFT/RIGHT 改为差速弧线，如 `LEFT: (0, S)`、`RIGHT: (S, 0)`（FORWARD_LEFT/RIGHT 已是半速弧线，层次正好拉开：直行→弧线→差速转→原地旋）；改后同步 architecture.md 的状态表。

#### P1-2 +IPD 背靠背帧会被"尾部 drain"吞掉（字节流无回推机制）

- **位置**：`wifi_at.c:533-548`（`wifiReadMessage` 读完全部 `len` 字节后再无条件下最多 8 字节、遇到 `\n` 才停，其间字节直接丢弃）。
- **机理**：esp-at 完全可能在一次 +IPD 之后立刻跟上下一条 +IPD（浏览器 keep-alive 心跳 50 ms 一发 + 二进制客户端并发）。被丢弃的恰是下一帧的 `+IPD,<len>:` 头部或负载前几个字节 → CRC 失败、解析器复位。同类问题也存在于 `WIFI_sendRaw` 等 "SEND OK" 等待期把到达的 +IPD 当普通行丢弃（`wifi_at.c:374-392`）。根因是整个 AT 读取层没有**单字节回推（pushback）**能力。
- **建议**：给读取层加 1 字节 pushback（静态 `g_unread` 缓存 + 读取函数优先消费）；`WIFI_sendRaw` 等待期间把非期望行交给解析器而不是丢弃；尾部 drain 只消费 CR/LF，见其他字节立即回推。

#### P1-3 SET_SPEED 双字节形式未钳位，状态上报越界

- **位置**：`robot.c:106-129`（`ROBOT_cmdSetSpeeds` 无 ±100 钳位，`sint8` 原始值 -128..127 直接进 targets）→ `Cpu0_Main.c:102`（×10 后 -1280..1270）→ 幸有 `motor.c:115-122` BSP 钳位兜底；但 `XCORE_statusPublish`/`PROTO_sendStatus` 把 ±127 原样上报，超出协议文档承诺的 "-100..+100"（`architecture.md:137-138`）。
- **影响**：弧线比例语义（S/2）失真、状态字段越文档范围；BSP 钳位属于"下游兜底上游"，防御层次倒挂。
- **建议**：在 `ROBOT_cmdSetSpeeds` 入口钳位 ±100，使"协议承诺-robot 层-BSP 层"三层一致。

#### P1-4 `wifiHttpSpeed` 数字解析存在有符号溢出 UB

- **位置**：`wifi_at.c:163-171`：`value = value*10 + (*p-'0')` 循环无位数上限，`speed=99999999999` 这类输入触发 `sint32` 溢出（未定义行为），之后才被钳位到 100。
- **影响**：畸形 HTTP 查询串 → 未定义行为（TASKING 下通常回绕，危害有限，但属于必须消除的 UB）。
- **建议**：循环内 `if (value > 1000) break;` 之类的提前截断；顺手支持负号与十六进制以外的非法字符即停。

### 4.3 P2 —— 健壮性 / 并发 / 配置

| # | 问题 | 位置 | 说明与建议 |
|---|---|---|---|
| P2-1 | 阻塞式 AT 会话挤兑接收：`IfxAsclin_Asc_write` 用 `TIME_INFINITE`，`CIPSEND` 等 `>` 与 `SEND OK` 各最长 2 s；期间 1536 B 页面发送约 134 ms，RX 软件缓冲 1024 B（≈89 ms 量）在客户端突发时可能溢出丢字节 | `wifi_at.c:90`、`wifi_at.c:33-41`、`wifi_at.c:365-392` | 缩小 HTTP 响应（见 P0-2）；评估把 RX 缓冲提到 2048；给 write 设有限超时并重试 |
| P2-2 | 单把自旋锁保护所有跨核块：CPU0 任务持锁可被更高优先级（如 timer 任务 prio 9）抢占，另两核自旋等待拉长；且约束"ISR 不得调用 XCORE_*"只存在于口头 | `xcore.c:20,58-68` | 目前各函数都在任务/循环上下文调用，无死锁实据；建议在 xcore.h 写明"仅任务上下文"约束，锁内代码保持现在的几条拷贝即可；若后续上 ISR 收包再按块拆锁 |
| P2-3 | 日志环形缓冲的 `g_logWr/g_logRd` 未声明 `volatile`，读侧 `XCORE_logService` 依赖编译器"恰好"重载 | `xcore.c:54-56,221-246` | 声明为 `volatile` 并在读侧补 `__isync()`；现靠 `UART_println` 外部调用屏障侥幸正确 |
| P2-4 | `LCF_DEFAULT_HOST = LCF_CPU1`：所有普通 `.data/.bss`——包括 **FreeRTOS 内核、32 KB 堆、xcore 全部共享块**——落在 dsram1，CPU0 每次内核调用/任务切换都走 SRI 总线跨核访问；与 `xcore.h:11-13` "live in shared data RAM" 的注释也不符（实为 CPU1 私有 scratch-pad，跨核经 SRI 可达） | `Lcf_Tasking_Tricore_Tc.lsl:49` | 改回 `LCF_CPU0`（OS 中心布局）；xcore 块如需明确归属可用 `.bss.xcore` 具名 section 独立放置。当前"能用"，但 CPU0 性能白白打折，且 CPU1 应用一旦长大（V1.1 编码器）先撞这 120 KB |
| P2-5 | 任务创建返回值未检查；栈溢出钩子静默 `__nop()` 死循环（故障既不打印也不进安全态）；`configUSE_MALLOC_FAILED_HOOK=0` | `Cpu0_Main.c:144-156,165-172`、`FreeRTOSConfig.h:19` | 检查 `xTaskCreate` 返回值并 `XCORE_logln` 报错；钩子里至少 `UART_println` 任务名后进入"急停+LED 快闪"安全态 |
| P2-6 | echo 回环任务是调试遗留：优先级 1 无阻塞空转（不打 delay，常态烧 CPU、饿死 idle） | `Cpu0_Main.c:62-68`、`uart.c:99-111` | 循环加 `vTaskDelay(pdMS_TO_TICKS(10))`，或编译开关隔离 |
| P2-7 | Release 下 `configASSERT(x)` 展开为空；Debug 依赖 `__debug()` 停核 | `FreeRTOSConfig.h:49-58` | Release 至少保留"关中断+急停+LED"故障态，避免断言被无声剥离 |
| P2-8 | `INCLUDE_vTaskDelete=1` 但使用 heap_1（`.cproject` 排除了 heap_2..5）：删除任务即永久泄漏 | `FreeRTOSConfig.h:39`、`.cproject` 排除表 | 当前没有删任务代码，属埋雷；要么改 heap_4，要么关 vTaskDelete |
| P2-9 | HTTP 控制页两条命令非原子：`wifiQueueSpeedAndMotion` 先 SET_SPEED 后 motion，队列满时速度已改、运动被丢，车停在非预期状态 | `wifi_at.c:151-155` | 队列满时丢弃整对（先探测剩余深度），或把 speed 并入运动命令负载 |
| P2-10 | esp-at UART1 默认开 RTS 流控未处理（README 已知遗留），初始化序列未发 `AT+UART_CUR=115200,8,1,0,0` | `README.md:93`、`wifi_at.c:591-625` | 按接线文档在 ATE0 之前补发；失败重试计入日志 |
| P2-11 | WiFi 初始化失败不 fail-fast：模组不在/接线错时 ATE0 重试 5 次后照常进主循环，仅有一条日志，整机"看似正常"实则无链路 | `wifi_at.c:590-627` | 初始化结果汇总上报状态块（如 faultCode 或 WiFi 子状态字段），手机端可见 |

### 4.4 P3 —— 工程化 / 仓库 / 文档

| # | 问题 | 位置 | 建议 |
|---|---|---|---|
| P3-1 | **仓库体积失控**：FreeRTOS 官方整仓克隆 113 MB / 7975 个文件 + `FreeRtos/aws` 18 MB 六个 AWS SDK（coreMQTT/corePKCS11/FreeRTOS-Plus-TCP 等），而 `.cproject` 实际只引用 Kernel 公共源 + `portable/Tasking/AURIX_TC27x`；克隆/切换分支/备份代价全部翻倍 | `FreeRtos/`（113 MB）、`.cproject` include/exclude 表 | 精简为实际编译所需的十几个文件目录；或改 git submodule 指向 FreeRTOS-Kernel 上游；aws/ 六个 SDK 直接删除（无任何代码引用，已核实） |
| P3-2 | **零自动化测试**：协议解析器、robot 状态机、motor_algo 限幅逻辑都是无硬件依赖的纯 C，却没有任何主机端单测；解析器这类输入面靠人工回归 | 全工程 | 用 Unity/Ceedling 把 `PROTO_feedByte`、`ROBOT_cmd*`、`MOTOR_ALGO_stepToward` 在主机端测起来（目标：协议 fuzz + 状态机全迁移覆盖）；再挂 GitHub Actions/本地脚本跑 |
| P3-3 | 无命令行构建：仅 ADS IDE 构建，CI 与可重复构建无从谈起 | README.md:98 | ADS 支持 headless 构建（ads.exe -data workspace -import … -build），落一个 build.bat 即可支撑 CI |
| P3-4 | 文档-代码漂移：architecture.md:155 仍写 `Connection: close` + `AT+CIPCLOSE`，与工作区 keep-alive 改动相悖；LEFT/RIGHT 语义相悖（见 P1-1）；心跳一节未写明"超时后命令仍可执行"（P0-3 修复后需同步） | `doc/architecture.md:122-124,155` | 随 P0/P1 修复一并更新；建议在 AGENTS.md 约定"改协议/行为必须同步 architecture.md" |
| P3-5 | 安全属性未声明：softAP 固定弱密码 `12345678`、TCP/HTTP 无任何鉴权，任意连入 AP 的客户端可完全控车（包括绕过网页层 0..100 钳位的二进制协议） | `wifi_at.h:16-18` | 玩具定位可接受，但应在 README/architecture 明示威胁模型；路线图加：强密码 + 简单 token 校验（协议保留字段或 HTTP header） |
| P3-6 | `GEMINI.md` 与 `AGENTS.md` 内容完全重复；`.codegraph/.gitignore` 入库无碍但属工具产物 | 根目录 | GEMINI.md 改为一行 include 指向 AGENTS.md 或删除 |
| P3-7 | 死代码/死 API：`ROBOT_FAULT_COMM_TIMEOUT` 未使用（随 P0-3 复活）；`PROTO_process()` 只返回上次结果、无调用方价值 | `robot.c:10-12`、`protocol.c:213-216` | 随修复清理或删除 |
| P3-8 | `UART_println` 为阻塞逐字节发送，robot 任务里 `XCORE_logService` 一轮最多可连发多行（每字节 ≈87 µs @115200），10 ms 节拍最坏被吃掉数毫秒 | `uart.c:74-97`、`xcore.c:221-246` | 现状可接受；后续可把 console 改中断发送/降低日志频率，或将 logService 移到低优先级任务 |

---

## 5. 分领域评述

### 5.1 架构与实时性
分核方案（CPU0=OS/状态机 10 ms、CPU1=电机 1 kHz、CPU2=WiFi 阻塞会话）与"通信/控制解耦"原则执行得干净利落，核间接口收敛在 xcore 一个文件里，是同类学生/ hobby 项目中少见的好结构。两个结构性瑕疵：一是 P2-4 的默认宿主核选错导致 OS 数据跨核访问；二是 CPU2 超循环内阻塞式 AT 会话（最长 ~4 s）与实时收包天然冲突（P0-2/P1-2/P2-1 同源），V1.1 若上编码器高频上报，建议 CPU2 引入小型命令/数据双缓冲或把 +IPD 收包挪进 RX 环 + 主循环只做无阻塞消费。

### 5.2 安全（Safety）与可靠（Reliability）
软件保护链（心跳超时→目标清零；CPU1 seq 失联→斜坡停车；急停旁路→立即刹车；BSP 钳位）层次分明，方向正确。但 P0-1 的竞态说明**安全机制的"清除路径"没有像"触发路径"那样被认真设计**——电平式每拍清除是最容易出事的模式。修复后建议补一张"故障矩阵"（故障源 × 触发 × 锁存 × 解除条件 × 上报码）进 architecture.md，作为回归测试依据。硬件层（看门狗 + STBY 断电）是当前最大空白（P0-4）。

### 5.3 通信链路
+IPD 冒号修复、keep-alive 改造、link 管理都体现真实调试功力；但读取层缺 pushback 是全局性缺陷（P1-2），RX/TX 缓冲的容量论证缺失（P2-1）。协议本身（XOR CRC、无序号、无鉴权）满足玩具定位，建议至少给帧加 1 字节序号以检测整帧丢失（尤其心跳），配合 P0-3 的锁存可显著提高失控检测率。

### 5.4 代码质量
命名前缀分区（ROBOT_/MOTOR_ALGO_/XCORE_/PROTO_/WIFI_/UART_/STIME_）一致，注释解释"为什么"而非复述代码（例如 `xcore.c:18-19`、`wifi_at.c:440-445`），`__dsync()` 的使用位置基本正确。缺陷集中在**输入边界**（P1-3/P1-4）与**返回值检查**（P0-1/P2-5）。建议开启 TASKING 最高告警档并清零（当前 .cproject 未见显式告警配置），再把 `-Wall` 等价项写进两种构建配置。

### 5.5 构建与仓库
IDE 工程本身可构建可调试，但 131 MB 的第三方目录入库（P3-1）是本工程最不划算的负债；无命令行构建与 CI（P3-3）使所有回归依赖人肉。建议尽早做一次"仓库瘦身 + build.bat + 主机端单测"的组合拳，一次性把工程化地基打好。

### 5.6 文档
覆盖度与诚实度（已知问题明示）都好于平均水平；主要风险是**漂移无守门**（P3-4）。建议每次改协议/安全行为时把 architecture.md 列入同一提交。

---

## 6. 改进路线图

### 第一批（立即，1–2 天，全部为小改动）
1. P0-1：急停竞态——检查 cmdPush 返回值 + `estopClear` 改为 CLEAR_FAULT/RESET 内边沿触发 + CPU0 轮询旁路位兜底锁存。
2. P0-3：心跳超时锁存 `ROBOT_FAULT_COMM_TIMEOUT`，运动命令门控在 `heartbeatOk` 上。
3. P0-2：CPU2 栈——HTTP 响应分段发送（页面上行走 rodata），栈占用压到 <512 B；顺手把 `LCF_USTACK2_SIZE` 提到 4 KB。
4. P1-3/P1-4：`ROBOT_cmdSetSpeeds` 钳位 ±100；`wifiHttpSpeed` 解析加位数上限。
5. P1-1：LEFT/RIGHT 改真差速 `(0,S)/(S,0)`，同步 architecture.md 状态表。

### 第二批（短期，1–2 周）
1. P0-4：三核看门狗 + 安全看门狗使能与喂狗；TB6612 STBY 改 GPIO 受控；调试/量产开关宏。
2. P1-2：AT 读取层加 1 字节 pushback；`WIFI_sendRaw` 等待期不再丢 +IPD；尾部 drain 只吃 CR/LF。
3. P2-5/P2-7：任务创建检查 + 故障钩子进"急停安全态"；Release configASSERT 保留最小行为。
4. P2-4：`LCF_DEFAULT_HOST` 改回 CPU0（构建后回归一次三核功能）。
5. P2-3/P2-6/P2-8/P2-9/P2-10：volatile 修复、echo 任务加延时、heap 策略收敛、命令对原子性、`AT+UART_CUR` 补发。
6. P3-4：文档与代码同步（keep-alive、故障矩阵、栈预算写进 architecture.md §10）。

### 第三批（中期，随 V1.1 编码器闭环一起）
1. P3-1：仓库瘦身（FreeRTOS 精简为必需目录或 submodule；删除 aws/ 六 SDK）。
2. P3-2/P3-3：主机端单测（协议 fuzz + 状态机覆盖）+ headless 构建 + CI。
3. P3-5：威胁模型声明 + token 鉴权 + 强密码。
4. 编码器接入后把"实际转速与目标偏差超限→故障"加入保护矩阵（安全闭环，弥补开环时代无法检测堵转的空白）。

---

## 7. 附录：评估覆盖清单

| 文件 | 行数 | 结论摘要 |
|---|---|---|
| Cpu0_Main.c | 172 | estopClear 电平清除（P0-1）、任务创建未检查（P2-5）、echo 任务空转（P2-6） |
| Cpu1_Main.c / Cpu2_Main.c | 63 / 65 | 看门狗禁用（P0-4）；启动时序正确 |
| App/robot.c/.h | 180+37 | 心跳不锁存（P0-3）、LEFT==ROTATE（P1-1）、cmdSetSpeeds 未钳位（P1-3）、死故障码 |
| App/motor_algo.c/.h | 128+27 | 设计良好；seq 失联看门狗、限幅、急停刹车实现正确 |
| Middleware/xcore.c/.h | 246+49 | 锁与日志环设计合理；volatile 缺失（P2-3）、单锁共享（P2-2）、estop 位清除语义（P0-1 根源） |
| Middleware/protocol.c/.h | 250+70 | 状态机健壮（长帧/坏 CRC/帧头重同步均处理）；cmdPush 返回值忽略（P0-1）、PROTO_process 死 API（P3-7） |
| Middleware/wifi_at.c/.h | 717+27 | 栈超支（P0-2）、pushback 缺失（P1-2）、解析 UB（P1-4）、阻塞挤兑（P2-1/P2-11）；+IPD 冒号处理正确 |
| Bsp/motor.c/.h | 160+20 | 钳位兜底正确；STBY 未受控（P0-4 建议）、g_dirInvert 集中处理良好 |
| Bsp/uart.c/.h、stime.c/.h | 111+30+17 | 实现简单正确；阻塞打印（P3-8）；stime 回绕安全 |
| Configurations/FreeRTOSConfig.h | 73 | 栈 256 字、Release 断言为空、vTaskDelete×heap_1 陷阱（P2-7/P2-8） |
| Lcf_Tasking_Tricore_Tc.lsl | ~460 行 | LCF_DEFAULT_HOST=CPU1（P2-4）、USTACK2=2k（P0-2） |
| .cproject / 仓库 | — | 仅 Kernel+Tasking port 被引用；113 MB+18 MB 第三方入库（P3-1）、heap_2..5 被排除 |
| doc/*.md ×4 | — | 覆盖完整；architecture.md 与代码两处漂移（P3-4、P1-1） |

> 本报告基于静态通读与配置核查，未做硬件在环验证；标注"静默溢出/竞态"的结论均给出了可复现的触发路径，修复后建议按 §6 顺序回归。
