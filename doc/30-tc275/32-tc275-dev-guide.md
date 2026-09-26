# 32 · TC275 开发指南（怎么做，不是为什么）

| 项 | 内容 |
|---|---|
| 文档编号 | 32 |
| 域 | TC275（本仓库固件工程） |
| 版本 | V1.1（2026-09-26，SPI 链路按已烧录 C6 固件契约完善后回写：§1 给出两份主机单测的**可直接粘贴命令**（含编译从机解码器的交叉验证），§8 排障表补遥测整帧丢弃 / 分派不看 CID / `DRIVE` 有意拒收 / `ERRSTAT` 位表 / `GEN` 无回执五条） |
| 前置阅读 | `21 §18`（工程级实现约束，**动手前必读**）、`23`（接线真源） |
| 与 31 的分工 | 31 描述"现在代码是什么样"，本文描述"要加东西该按什么步骤动、去哪验证" |

---

## 1. 环境与构建

| 项 | 值 |
|---|---|
| IDE / 工具链 | AURIX Development Studio + **TASKING TriCore 编译器**（工程为 IDE 工作区工程：`.cproject` / `.project`） |
| 构建配置 | `TriCore Debug (TASKING)`，产物 `.elf` 在该配置目录下 |
| 链接脚本 | `Lcf_Gnuc_Tricore_Tc.lsl`（Gnuc）/ `Lcf_Tasking_Tricore_Tc.lsl`（TASKING，实际用这份） |
| 依赖 | `Libraries/`（Infineon iLLD，随仓库）、`FreeRtos/` + `Configurations/FreeRTOSConfig.h`（TriCore 移植，仅 CPU0） |
| 命令行构建 | **本工程无独立 CLI 构建脚本**，只能走 IDE；出现"能编不能刷"先确认用的是 TASKING 配置 |

导入：`File > Import > General > Existing Projects into Workspace` → 选仓库根目录。

常见编译错误：`FreeRTOS.h not found` = `FreeRtos/` 或 `Configurations/` 未完整检出（它们是工程的一部分，不需要另外安装）。

**切换链路固件（UART ↔ SPI）**：`Cpu2_Main.c` 用 `USE_SPI_LINK` 分流——**不定义 = demo UART/AT 路径（当前默认）**，定义 = SF-over-SPI 量产路径（`Middleware/com/link.c` 泵）。打开方式：`Project > Properties > C/C++ Build > Settings > TASKING C/C++ Compiler > Preprocessor > Defined symbols (-D)` 加一行 `USE_SPI_LINK`（两个构建配置各加一次）。为什么默认还是 UART：SPI 的波形兼容性门禁 G1 未过（`22 §8`），过完之后翻转默认值并把开关改成 `USE_WIFI_AT`（`22 §7.2`、`21 §5.6`）。

**SF 层的主机单测**（不需要 TriCore 工具链，**改过 `Middleware/sf/` 或 `c6_car/components/c6_sf|c6_proto` 必须跑**）。本机用 MSYS2/MinGW gcc，在**仓库根目录**执行：

```bash
# ① 帧编解码（2855 断言，含 400 万随机字节风暴）
gcc -std=c99 -Wall -Wextra -Werror -O2 -I Middleware/sf \
    test/host/test_sf.c Middleware/sf/sf_frame.c \
    -o test/host/out/test_sf.exe && ./test/host/out/test_sf.exe

# ② 38 B 遥测布局（154 断言）—— 加 -DC6_CROSS_CHECK 会把**从机自己的**
#    proto_frames.c 编进同一个可执行文件做双向交叉，这才是跨侧证据
gcc -std=c99 -Wall -Wextra -O2 -DC6_CROSS_CHECK -I Middleware/sf \
    -I ../c6_car/components/c6_proto -I ../c6_car/components/c6_sf \
    test/host/test_sf_telemetry.c Middleware/sf/sf_telemetry.c \
    Middleware/sf/sf_frame.c ../c6_car/components/c6_proto/proto_frames.c \
    -o test/host/out/test_sf_telemetry.exe && ./test/host/out/test_sf_telemetry.exe
```

①的判据见 `22 §7.2` 首行；②是 `21 §18 C11/C12` 那条铁律的落地手段——**文档写"两侧一致"不算验证，编译对方源码才算**。②不带 `-DC6_CROSS_CHECK` 也能跑，但那只测自己，退化成同义反复。

## 2. 烧录与观测

| 通道 | 位置 | 用途 |
|---|---|---|
| 调试 | TC275 kit X4 micro-USB（一根线：供电 + DAS 调试 + 虚拟 COM） | `Debug As > myCar TriCore Debug (TASKING)` |
| 控制台串口 | kit 板载 FT2232 虚拟 COM，**115200 8N1** | ASCLIN0（P14.0/P14.1 **板内互连，没有引到外接排针**，想外接打印是不可能的） |
| 存活指示 | LED1 = P00.5（blinky 250 ms 翻转） | 三核调度是否活着 |
| C6 AT 口 | mikroBUS pin13(TX=P15.0)/pin14(RX=P15.1) ↔ DevKitC-1 J1-5/6 | 当前链路与回退通道 |

调试期约定：`Cpu*_Main.c` 显式关掉了 CPU/安全看门狗（见 `21 §18 C8`）。**任何"跑着跑着复位"的现象，先确认是不是自己把看门狗重新打开了**。

## 3. 任务地图（谁在哪核上）

| 核 | 形态 | 代码 | 加东西时的落点 |
|---|---|---|---|
| CPU0 | FreeRTOS | `Cpu0_Main.c` + `App/robot.c` | 新任务 = 在 `Cpu0_Main.c` 建任务；控制类逻辑进 `robot.c` 的 10 ms 拍 |
| CPU1 | 裸机 1 kHz | `Cpu1_Main.c` + `App/motor_algo.c` + `Bsp/encoder.c` | 算法进 `motor_algo.c`，**不许阻塞、不许打印**（打印走日志环，§6）；编码器 ISR 属 `encoder.c`，优先级 16~23 见 `21 §18 C2` |
| CPU2 | 裸机超循环 | UART：`Cpu2_Main.c` + `Middleware/wifi_at.c`；SPI：`Cpu2_Main.c` + `Middleware/com/{link,spi_hal_pins}.c` + `Middleware/sf/`（`-D USE_SPI_LINK`，见 §1） | 链路层改动进 `Middleware/`；SF 帧格式改动要同时改 `21 §6.1a`、`22 §5` 与 C6 侧 `c6_sf`（两处独立实现，靠文档对齐） |

命名与风格（照现有代码，勿另立）：文件名小写下划线、模块前缀大写 `MODULE_`（`ROBOT_task` / `MOTOR_ALGO_run` / `PROTO_feedByte` / `XCORE_estopRequest` / `WIFI_sendRaw`）；头文件守卫 `MODULE_H`；**代码注释用英文，文档与 commit message 用中文**。

## 4. 新增一个中断（最常踩、代价最高）

步骤，一条都不能省：

1. **查引脚与外设符号**：在本仓库 `Libraries/iLLD/TC27D/Tricore/_PinMap/` 下找 `Ifx<模块>_PinMap.h` 里的符号名（例如 `IfxQspi3_SLSO5_P23_4_OUT`、`IfxGtm_TIM0_0_TIN26_P33_4_IN`）。**不要凭记忆写引脚**。
2. **查冲突**：与 `23` 的接线状态一览和各表对一遍（尤其 P33.0~7 编码器、P00.0 CAN、P00.5/P00.6 LED、P15.0/P15.1 WiFi、P2x 组 JTAG/Shield2Go）。
3. **声明 ISR**：`IFX_INTERRUPT(myIsr, 0, prio)` —— **向量表参数固定写 0**，不论这个中断属于哪个核；目标核由 SRC 的 `typeOfService = IfxSrc_Tos_cpu0|cpu1|cpu2` 决定。原因见 `21 §18 C1`。
4. **挑优先级**：按 `21 §18 C2` 的已占用表（CPU0 用了 1/2/4/8/12，CPU2 用了 5/7/13 + **6/9/10 = QSPI3 TX/RX/ER**）取空闲档，并在该表登记。
5. **想给排针 GPIO 加边沿中断？TC275 上不行**：P23.x 这类 GPIO 既没有 ERU 通路也不在 IOM 监视输入内，配不出"电平跳变触发 ISR"（`21 §18 C9`）。握手/就绪类信号一律**输入+内部上拉 + 主循环采电平**（SPI 链路就是这么做的，`22 §2 E11`）。
6. **验证真的进表了**：构建后打开 `.map`，在 **Removed Sections** 里搜 `Isr`。出现你的 ISR 名 = 表号写错了，中断永远不会进、且编译链接全程无报错。
7. 上板验证：在 ISR 里累加计数器，经日志环（§6）或状态字段出到控制台，不要靠"感觉它在跑"。

## 5. 新增一条跨核消息（xcore）

现状模式（`31 §4`）：共享数据落在默认数据段（CPU0 DSPR）；TC275 **无数据 Cache**，不需要任何 Cache 维护；所有块由**同一把自旋锁**保护（`IfxCpu_acquireMutex` = cmpAndSwap），临界区只做几字节拷贝，**写后 `__dsync()`**。

加一条通道的步骤：

1. 在 `Middleware/xcore.h` 定义消息结构体（**定长、无指针**）与 `XCORE_xxxPublish()` / `XCORE_xxxTake()` 接口。
2. 实现里：`acquireMutex` → 拷贝 → `releaseMutex` → `__dsync()`；队列满时的策略要么"丢弃 + 记日志"，要么"覆盖最新"，**不允许阻塞**（CPU1 是 1 kHz 硬节拍）。
3. 需要"失联可判"的通道带 `seq` 计数（参考电机目标通道：CPU1 用它做 150 ms 失联看门狗）。
4. 同步更新：`31 §4` 通道表 + `21 §5.5`（量产 xcore v2 目标态）。

**急停类语义不要走普通队列**：需要"不等控制拍"的动作，参考 `XCORE_estopRequest()` 的旁路位模式（`21 §5.3`、`31 §10`）。

## 6. 加一条打印（跨核日志）

ASCLIN0 属主是 CPU0，iLLD ASC 的软件 FIFO 与临界区**只在属主核内互斥**，两个核直接 `printf` 会踩状态（乱码/偶发卡死）。

CPU1/CPU2 打印 = 整行拷进日志环（1 KB，满则整行丢弃），由 CPU0 的 robot 控制任务 `XCORE_logService()` 出环落串口。新代码在 CPU1/CPU2 里**不要**直接调 `uart.c` 的接口。

## 7. 时基

| 需求 | 用什么 |
|---|---|
| CPU0 | FreeRTOS tick（STM0，优先级 2）与任务延时 |
| CPU1 / CPU2 | `Bsp/stime.c`：直读 STM0 自由计数，unsigned 减法天然回绕安全；**禁止调 FreeRTOS API**（移植层只绑 CPU0，见 `21 §18 C4`） |

## 8. 排障速查

| 现象 | 第一嫌疑 | 怎么确认 |
|---|---|---|
| 某个核的外设"完全不响应"，无任何报错 | ISR 声明在非 0 号表 → 被链接器删了 | `.map` 的 Removed Sections 搜 `Isr`（§4 步骤 6） |
| 中断进了表但打到错的核 | SRC 的 `TOS` 位没设 / 优先级与别的核撞了 | 读对应 `SRC*` 寄存器；对照 `21 §18 C2` 占用表 |
| AT 链路全超时、三核启动正常 | CPU2 的 ASCLIN1 中断从未触发 | 同第 1 行；再看日志环有没有 RX 计数 |
| 控制台乱码、偶发卡死 | 跨核直接 printf | 全量搜非 CPU0 代码里的 `IfxAsclin`/`UART_` 调用 |
| 跑一段时间后复位 | 看门狗被重新启用 / 某核没喂 | 确认 `Cpu*_Main.c` 的看门狗开关状态（`21 §18 C8`） |
| C6 一加速就重启 | 5V 供电裕量不足（brownout） | `23 §5`；并加 ≥470 µF |
| 电机一侧转向相反 | `motor.c` 的 `g_dirInvert` 表 vs 实际线序 | `23 §4`，**查表，不要交叉猜测** |
| 遥测速度一直等于指令值（不显示实测） | 编码器 8 线未接/未动过 → `alive=FALSE`，CPU0 回退指令回显 | 手转轮子 ≥1 圈（alive 窗口 500 ms）；台架判向步骤见 `23 §8.4` |
| 手转一圈计数 ≈ 5306 而非 ≈ 21225 | 编码器某相未接好，×4 退化成 ×1；或 `g_encInvert` 未按台架结论修正 | 按 `23 §8.4` 步骤 2/3 复测两相计数 |
| SPI 链路"通一帧后就再也不动" | 漏发突发收尾事务（`RDDMA` 后没补 `INT0`、`WRDMA` 后没补 `WR_END`）→ 从机槽位卡死 | 看 `Link_Health.stats` 的 `rdSegments/wrSegments` 是否停止增长；规则见 `21 §18 C10` |
| SPI 握手寄存器读回全 0 / 值不稳定 | ① 前导相位波形从机不认（G1 未过的预期现象）② 连读两次取稳定值没生效 | 先看 `regUnstable`/`spiErrors`，再上逻辑分析仪比 `22 §4.4` 的事务表 |
| 手机页面能连上但**遥测一个字段都没有**（不是数值 0，是根本不刷新） | 遥测载荷短于 38 B → 从机**整帧丢弃**（`21 §18 C12`）。短载荷的表现是"全无"，不是"部分" | 看 `stats.txFrames` 在涨（说明主机发了）而 C6 侧无 TEL 计数；跑 `test/host/test_sf_telemetry.c`（§1），布局真源是 `Middleware/sf/sf_telemetry.h` |
| 发了配置/配对命令，车却**动了或停了** | 分派没看 CID，把 `CFG` 的键号 / `PAIR` 的 token 首字节当成命令码执行（`21 §18 C13`） | 看 `stats.cmdUnsupportedCid` 是否在涨（白名单生效的证据）；对照 `22 §5.2` 的五条通道载荷形状 |
| 摇杆推了没反应，但链路"健康" | `DRIVE 0x50` 被**有意拒收**（mm/s→两轮要 §11 运动学 + 编码器闭环，本构建没有） | `stats.cmdUnsupportedOp` 涨即为此因。验链路请改用**离散方向命令**（`0x02` 前进等，只带 op）或 `SET_SPEED 0x10`（±100 百分比，已支持窄化） |
| 链路错误位读出来"时对时错" | `SF_ERRSTAT` 位表与从机不一致（历史上主机侧是臆造值，`22 §2 E13`） | 只认 `22 §4.3` 列出的七位；改过任一侧常量必须重跑 §1 的两份单测 |
| `LINK_gen()`/`LINK_setClock()` 返回 `LINK_GEN_ERR_NOACK` | 回执要跨**两次从机任务调度**才可见（`22 §2 E14`），20 ms 超时偏紧或从机任务被占 | 看 `stats.genWrites` vs `stats.genNoAck`；台架上先确认链路是 `LINK_UP` 再调 `GEN` |
| 想看 SPI 链路状态但没打印 | CPU2 不允许 printf（`21 §18 C3`） | 调试器 Watch 表达式 `g_health`（`Middleware/com/link.c` 的静态 `Link_Health`，每圈刷新） |

## 9. 提交前自检（文档同步义务）

- [ ] 改了引脚 → `23` 的表与接线状态一览同步；确认没和 `21 §18 C7` 的固定片选冲突
- [ ] 改了帧/命令/超时数值 → `21 §6` 与（涉及链路时）`22 §5` 同步；**涉及 `Middleware/sf/` 或 `c6_car/components/c6_sf|c6_proto` 常量/布局的，必须重跑 §1 两份主机单测并贴出断言数**（`21 §18 C11`：文档写"两侧一致"不算验证）
- [ ] 加了 ISR → `21 §18 C2` 优先级表登记
- [ ] 加了跨核通道 → `31 §4` + `21 §5.5`
- [ ] 改了构建/烧录方式 → 本文 §1/§2
- [ ] commit message 用中文（`type:` 前缀可保留英文）
