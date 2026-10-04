# SmartDrive 量产版软件设计文档（SDD）

| 项 | 内容 |
|---|---|
| 文档编号 | **21**（域：设计·硬件）· **`doc/` 唯一设计基准** · 上级索引 [00-index.md](../00-index.md) |
| 文档版本 | **V1.16**（2026-10-04：**§5.1 编码器健康按侧判定（BUG-ENC-1 P0 修复）**——4 路霍尔健康原先合并为单一 `alive` 布尔（任一侧 500 ms 内动过即真），融合又把"发布序号在变"当"编码器有效"（xcore 每毫秒发布无条件自增 seq、`FusionInput` 结构上无 alive 字段）：单侧失效被对侧运动掩蔽，`FUSION_ENCODER_LOST` 硬停止对传感器本身失效是死代码，死侧以假 0 测量参与闭环（PI windup）与融合（车速锚减半、wheelYaw 污染）。修复 = 快照新增按侧 `edgeAgeMs[2]` 原语（`ENCODER_EDGE_FRESH_MS=100`）+ 消费端"被驱侧必须持续出边沿、500 ms 起步宽限（松杆清零，杜绝静止起步死锁）"，速度锚与轮速/陀螺一致性只用边沿新鲜的侧，`motor_algo` measOk 按侧判定死侧回退开环；`alive` 降级为明示的"运动指示"、seq 保留为发布活性；`[WHEELS]` 日志补按侧边沿龄。详见 [51](../51-encoder-health-bugfix.md)，融合行为真源 [37](../30-tc275/37-sensor-fusion.md) V1.3。主机单测新增按侧失效回归，TASKING 构建通过（tc275_car v1.3.1，ROM 198243）；**encoder/motor_algo 硬件半截无主机单测，按侧拔线注入需台架**）。V1.15（2026-10-04：**TB6612 STBY 由跳线常使能改为 TC275 GPIO 受控**——接线真源 [23 §3/§3.2](23-wiring.md) V1.23：**P22.2（X1-16）→ D24A J4-2**，一根线管四路（两片 TB6612 的 STBY 板内同一网络、J6 无独立 STBY），**原 J4-1↔J4-2 跳线帽须拔掉，改线与刷固件同批**。固件落点 `bsp/motor.c`：`MOTOR_STBY_PORT/PIN` + 新函数 `MOTOR_setEnabled(boolean)`（拉低前先 `MOTOR_stopAll()`），`MOTOR_init()` 在 8 根 IN/PWM 置低、duty=0 之后**末步拉高**，属主 CPU1；**未新开 `bsp/stby`**。本文同批回写四处：① §4 术语"安全态"补落地注记；② §5.2 保护联动"STBY 硬线"条改为落地状态 + **登记未裁决的语义冲突**（本条）；③ §7.1 表第 1 行与表下口径注——V1.13 的"复位期间电机断电未被证明"结论**只对未改线的旧接法成立**，改线后凭 TB6612 **全部控制输入内部 200 kΩ 下拉**（STBY 浮空=低=standby，`L L/PWM=L/STBY=H`=OFF 高阻）给出纸面答案，实测仍是 §7.2 台架项；④ §7.2 复位安全态段补"驱动未解锁"这层硬件保证与两条台架动作。**语义裁决待定（本批不擅自改）**：急停/堵转/欠压是否按本文定义叠加"STBY 拉低"——现急停是 `MOTOR_ALGO_brakeAll()` 的**短接刹车**，STBY 一低即退化成高阻滑行、**刹车距离变长**，需用户放行后与 `motor_guard` 同批做。**电气账（诚实登记）**：D24A 的 TB6612 VCC 接板载 5 V ⇒ `VIH(STB)=3.5 V`，3.3 V GPIO 低于保证门限（与原跳线同级裕量、非新引入缺陷，实车已验证可用）；spec-clean 需 PMOS 抬到 5 V，本批不做。**未过 TASKING 编译**（无新增源文件，`.cproject` 不改），0 err/0 warn 由用户在 IDE 侧构建确认；改线/台架未做前**不得宣称"复位即断电"已闭环**。V1.14（2026-10-03：**IMU 驱动落地**——新增 `bsp/imu.c/.h`（CPU1 属主，按 23 §10 锁位）：LSM6DSV16BX 六轴 IMU 走 **QSPI1 主机**（SPI 模式 3，CPOL/CPHA=1，iLLD 映射 `idleHigh`+`shiftTransmitDataOnLeadingEdge`=ECON CPOL/CPH=1；时钟 1/2/5/10 MHz 档位、默认 1 MHz），WHO_AM_I=0x70 探测 + SW_RESET/BDU/IF_INC 初始化，默认 **±4 g/±500 dps/240 Hz**，1 kHz 环每 5 ms 一次 0x20 起 14 B 突发（温度+陀螺+加计，BDU 保证原子），失联 100 ms 判丢 + 1 Hz 重探（接线下**免重启自愈**）；INT1 P15.4 走 **SCU ERU**（REQ0/输入通道 0→OGU0→`SRC_SCUERU0`（iLLD 路径 `MODULE_SRC.SCU.SCU.ERU[0]`），ISR 清 `SCU_FMR` 事件标志 + 仅边沿计数作 23 §10.2 ③ 台架证据，不在 ISR 里发起 SPI）；**§18 C2 优先级表新增占用：CPU1 QSPI1 TX/RX/ER=11/14/15（低于编码器块）、ERU DRDY=24**；xcore 新块 `XcoreImu`（mg/mdps/0.01 °C 单位域，`seq` 由 publish 内部递增，33 §4 规约）；主机单测新增 `test_imu`（55 断言，CI 同步）、`test_xcore` 1095→**1109**（IMU 块语义）；**未过 TASKING 编译、硬件未接线（23 §10 待实物接线），`bsp/imu.c` 驱动主体无单测需台架**，设计真源 [35-imu-driver.md](../30-tc275/35-imu-driver.md) V1.0）。V1.13（2026-10-03：**§7.2/§18 C8 看门狗假死根治**——TC27x 硬件事实回写：CPU WDT 到期只发 NMI 不自行复位，iLLD 默认 NMI 钩子为空 → V1.8 的"断喂→复位"链从未闭合，看门狗到期即整车假死（台架 2026-10-03 复现）；修复 = `Configurations/Ifx_Cfg.h` 定义 `IFX_CFG_CPU_TRAP_NMI_HOOK` → `IfxCpu_triggerSwReset()`（`SWRSTCON` 无密码寄存器写，NMI 上下文唯一安全复位原语，钩子不返回；不得在其中打印/走 xcore）；窗口数字更正 `REL=0xF800` ≈**1.4 s**（0xF800>0xE000 线性，旧稿"0.3~0.5 s"与其换算锚点算术矛盾），源注释 Cpu0/Cpu1_Main 同步；随 tc275_car v1.1.2，代码先行提交 `d53fd07`；复位窗口与 NMI 复位行为待台架实测）。V1.12（2026-09-30：**§5.1 编码器刻度同步**——实物 MG310 编码器为 **13 PPR** 霍尔 AB（×4 = 52 计数/电机转，×20.409 = 1061.27 计数/轮转，48 mm 胎 = 0.1421 mm/计数），ISR 负载数据按新刻度更正（空载 500 rpm 四路 ≈1.7k 中断/s，≪1% CPU1），默认轮径 65→**48**（`ENCODER_WHEEL_DIA_MM`/`CALIB_WHEELDIA_DEF`，随 34 V1.4 / 23 V1.17）；换算公式一字未动，主机单测照常回归，**本轮未过 TASKING 编译**）。V1.11（V1.10 + §5.2：**闭环使能门固件化**——34 号 V1.3 新增 `g_closedLoopOk`：标定记录 `src=0`（首次烧录/校验失败回落/0x74 清记录）时 CPU1 把 SERVO `measValid` 钳 FALSE 强制开环等价（duty=目标、清积分），0x70 DONE/0x73 自动开门、0x74 关门，翻变各打一行门日志；根治"编码器已接线但方向未标定 → 闭环正反馈、原点回中电机仍转"的台架现象；不动任何 SF/遥测字节，主机单测照常回归，**门翻转逻辑在 1 kHz 环内无主机单测覆盖、需台架**（34 §13.5）；随 34 V1.3 / 23 V1.16 / 33 V1.2，**本轮未过 TASKING 编译**。V1.10（V1.9 + §4.1/§4.2/§5.1/§5.2/§6.2/§17：**判向标定 + DFlash 持久化 + DPT 命令族落地**，真源 [34-calib-dpt.md V1.2](../30-tc275/34-calib-dpt.md)——① §5.1：`ENCODER_FULL_SCALE_MM_S`/轮径 65 mm **参数运行时化**（公式一字未动，只换数据源），值从 **DF0 扇区 15（0xAF01E000）** 的 `CalibRecord`（magic+CRC16+范围三重校验，失败整体回落默认 `src=0`）加载，CPU0 → xcore `RecordLive{version,rec}` → CPU1 边沿应用；CPU2 vTarget 换算读同一块；② §4.1：DF0 登记第二个写者（标定记录占扇区 15，§5.4 的"config 唯一写 DF0"口径随之改写，§4.3 页分配须避开），并留下 **iLLD 48 逻辑扇区 vs 本行 128 KB 的冲突待裁决**；③ §5.2：**逐电机 jog `0x71`**（钳 ±500、故障门禁、不发心跳、300 ms 无刷新自动停，优先级 急停>标定>jog>servo）+ **判向结果回传 EVT 0x22**（含 `saved`，DONE 自动持久化；**BUSY 只拒绝不重启**）；④ §6.2：0x71~0x74 在 TC275 侧的新语义登记，并写明与 C6 常量表（LED/MOTOR_RUN/ENC_READ/CAL）**同字节双语义**这条 hazard；⑤ §17：新增 `mw/calib/`、SF 单测断言 2873→**2948**。验证：主机单测两份全绿（2948 / 116）；**本轮未过 TASKING 编译**，xcore 新块 / jog / DFlash 擦写 **无主机单测覆盖，需台架**（34 §11.5）。V1.9 = V1.8 + §6.3/§16 Q1：**F04 欠压判据的数据源落地**——新增 `bsp/adc.c`：D24A J6-1 电池分压（**VIN/11**）接 kit **X2-23 = AN4 = VADC G0 CH4**（X2-16~27 整列空闲模拟脚，23 §5.5 为接线真源），iLLD 询问式单发转换 12 bit/1 µs 采样、100 Hz + 1/16 EMA，CPU1 属主经 xcore 电池块出遥测 `batteryMv/batteryPct`（转 ✅，pct 按 3S 估算待节数确认）；`.cproject` 解除 `Vadc/{,Std,Adc}` 排除；**欠压保护动作本身仍在 motor_guard 未做清单**。V1.8 = V1.7 + §7.2/§18 C8：**评估报告 §6 两条 🔴 落地——跑飞自恢复 + 栈溢出上报**——CPU0/CPU1 的 CPU 看门狗启用并周期喂狗（新增 `bsp/wdg.h`，平衡 ENDINIT clear+set 喂狗；CPU0=robot 任务 10 ms、CPU1=`MOTOR_ALGO_run` 1 kHz；REL=0xF800≈0.3~0.5 s，换算锚点：Infineon 官方示例 Watchdog_1_KIT_TC275_LK 的 0xE000≈1.3 s），`vApplicationStackOverflowHook` 从静默空转变为"串口上报（`UART_flushPolling` 轮询泵）→ 停喂"，由 CPU0 看门狗复位整机、回电机停止的安全启动态；SM 看门狗与 CPU2 看门狗**有意保持关闭**（理由见 C8），属 §7.2 量产链项；**V1.9：电池电压采集落地（2026-09-27）**——① 接线与选脚（23 §5.5 真源）：D24A 板内 R13 10k+R15 1k 分压（**VIN/11**）引出 J6-1 "ADC"，接 kit **X2-23 = AN4 = VADC G0 CH4**（kit 手册 Figure 4 核对：X2-16~27 为空闲模拟脚列，X2-27/AN0 被板载电位器占用故避开；VAREF1=X2-15，基准 3.3 V 待台架万用表核对）；② 固件：新增 `bsp/adc.c/.h`（CPU1 属主）——iLLD `IfxVadc_Adc` 询问式单发转换（`LOSUP` 按本板 3.3 V 模拟轨设 3V，12 bit 右对齐、采样 1 µs 覆盖 909 Ω 源阻、waitForRead 免手动清 VF），100 Hz + 1/16 EMA，初始化预充一帧避免遥测从 0 爬坡；`.cproject` 四配置解除 `Vadc/{,Std,Adc}` 排除（与 Qspi/Dma 同款解锁）；③ 数据通路：xcore 新增电池块（`XCORE_battSetMv/GetMv`，CPU1 写 CPU0/CPU2 读），Cpu2 遥测 `batteryMv/batteryPct` 转 ✅——pct 是按 `ADC_BATT_CELLS=3` 的每节 3.3~4.2 V 估算（节数待 §16 Q1 确认后配置化），mV 才是测量；④ **欠压保护动作仍未做**（motor_guard 清单不变），本项只交付判据的数据源）（V1.7 = V1.6 + §5.1/§5.2/§6.3/§17：**F02 速度闭环落地 + 判向标定从"空操作"转正**——新增 `rt/servo`（每侧 PI：前馈 + 抗饱和积分限幅 ±30% duty + 输出钳位 ±1000，编码器 alive 无效时回退开环斜坡），`rt/motor_algo` 1 kHz 接入；`g_encInvert` 改运行时可写并新增 **0x70 台架自动判向**（CPU1 脉冲法，`ENCCAL=` 结果行，23 §8.4 步骤 2 的自动化）；遥测 `vTargetL/R` 转 ✅（percent×10 经 `ENCODER_FULL_SCALE_MM_S` 换算 mm/s）。增益/前馈为待台架标定初值；motor_guard（堵转/滑差/欠压/STBY）仍未做）（V1.6 = V1.5 + §3.7/§5.6/§6.1a/§7.1/§16/§17/§18 C14：**链路泵按台架故障根因简化**——寄存器快照从"连读两次比对"改为**单读 + 消费端钳位容错**，失联判定**只由 `SF_ALIVE` 500 ms** 承担，移除 READY 沿的 `RESET_LINK` 重同步握手，SEQ 改"连续 8 帧越窗即丢窗重锁"；依据 `com/link.c` 提交 `ae10aac` 与 [22 V1.6](22-link-spi-design.md) §4.2/§5.4）（V1.5 = V1.4 + §3.7/§5.6 末条/§16 R7/§17：**UART 板间链路弃用**，`USE_SPI_LINK` 已定义进两个 TASKING 构建配置，SPI 为唯一量产链路）（V1.4 = §3.7/§16 R11/**§18 新增 C14**：实物确认两板之间所有连线均为杜邦线直连、**不含任何外部元件**，IRQ 高电平仅由 TC275 片内上拉提供，据此给出写代码时的三条硬约束）（V1.2c + §3.4 目标态目录落地：仓库由 demo 布局重排为 `app/ rt/ com/ mw/ bsp/`，`Bsp/encoder.c` 随之迁至 `rt/encoder.c`） |
| 日期 | 2026-09-27 |
| 修订记录 | **V1.16 = V1.15 + §5.1：编码器健康按侧判定（BUG-ENC-1 P0，真源 [51](../51-encoder-health-bugfix.md)）**——`edgeAgeMs[2]` 原语 + "被驱侧 500 ms 宽限内必须出边沿"消费语义，`ENCODER_isAlive` 明示为运动指示；§5.2 伺服 measOk 同批改按侧；`[WHEELS]` 补 age 字段（38 V1.1）；test_fusion/test_xcore 新增按侧失效回归；TASKING 过（v1.3.1），按侧拔线注入待台架；V1.15（STBY 受控化）起详见版本行）。V1.0 初版；V1.1 新增 §3.6 多核 OS 形态选型（SMP/AMP 决策为 AMP 及演进条件）、§3.7 板间通信方式选型（UART/SPI/TWAI 对比，当时选定 UART @2 Mbps）及 SPI 主从角色分配（TC275 主机 / C6 从机 + CMD_RDY 握手）；**V1.2 板间链路换向：V1.0 主链路改为 SPI（TC275 QSPI3 主机 ↔ C6 SPI2 从机 `spi_slave_hd`，1 MHz 起 / 5 MHz 量产基线），UART 降级为调试与回退通道；LINK 段帧协议改为新定 SF 帧，手机 WS 段仍用 v2 帧；§1.1/§1.3/§2.2/§3.2/§3.4/§3.5/§3.7/§5.6/§6/§12/§14/§15/§16/§17 同步。详细设计、接线表、验证门禁与两固件改动清单见 [22-link-spi-design.md](../20-design/22-link-spi-design.md)。**V1.2 同时确立本文档为 `doc/` 唯一设计基准**：新增 §18 工程级实现约束（吸收 31-firmware-architecture.md 的实测坑），`ux-performance-plan.md` 结论全部并入本文（§3.5/§11/§14），C6 固件详细设计移出本仓库、由 `esp32c6_car/doc/` 承载**；**V1.2a：TC275 侧 SPI 链路代码落地后回写**——§3.4 加落地进度注记（目标态目录 vs 当前 `Middleware/` 布局）、§3.5 命令流补 auth 未落地、§3.7/§5.6 握手改为 **P23.0 电平轮询**（原稿边沿中断在 TC275 上做不出来）、§16 R11 口径随之调整、§17 映射表标出已落地文件、§18 新增 **C9（P23.x 无 GPIO 边沿中断）** 与 **C10（`RDDMA`/`WRDMA` 突发必须用 `INT0`/`WR_END` 收尾，线上命令字节以 `spi_ll.h` 为准）**，C2 登记 QSPI3 优先级 6/9/10。依据见 [22 §2 E11/E12](../20-design/22-link-spi-design.md)**；**V1.2b：C6 已烧录固件后按契约完善 TC275 侧的回写**——SF 常量表以从机为真源更正三处（`FLAGS bit1=FRAG_END` 非 ACK、OTA CID 补 `0x35 ABORT`、`ERRSTAT` 位图整张重写，原 V1.2 值系臆造，22 §2 E13）；§6.1c 映射表按"五条 CMD 通道形状不统一"重写并落**三层白名单分派**规则；§6.3 遥测表补**逐字节偏移 + 38 B 定长硬约束 + 当前有源/填 0 字段**；§6.3 遥测表补**逐字节偏移 + 38 B 定长硬约束 + 当前有源/填 0 字段**；22 §4.3 补 `SF_REG_GEN(24)` 寄存器与 `GEN` 命令表、本文 §5.6 补 `LINK_gen/LINK_setClock`（含 `CLOCK_SET` payload 单位 MHz、回执跨两次从机任务故必须带超时，22 §2 E14）；§18 新增 **C11（跨侧常量整表比对 + 编译对方源码的测试）**、**C12（定长载荷短一点=全丢）**、**C13（不得假设首字节是命令码）**；**V1.3：§3.4 目标态目录落地（M1 目录项）**——仓库由 demo 布局（`App/ Middleware/ Bsp/`）重排为 `app/ rt/ com/ mw/ bsp/`，全部内部 include 改为工程根模块限定路径（`"com/link.h"`、`"mw/sf/sf_frame.h"`…），`.cproject` 四个构建配置 include 路径同步；主机单测回归全绿（SF 2855 断言 + 遥测跨侧 154 断言）；§3.4 注记、§5.1、§6.1c、§6.3、§15、§16、§17、§18 C2 路径同步。**同轮达成首次 TASKING IDE 构建链接闭合**（Debug 配置 0 错误，QSPI+DMA 链接证实闭合），构建暴露并修复 `rt/encoder.c`/`encoder.h` 的 `int32`→`sint32` 类型错误（该文件此前从未被 TriCore 编译过，依据见 [22 §8 执行状态](22-link-spi-design.md)）；**V1.4：实物确认板间连线全部为杜邦线直连、不含任何外部元件**——新增 §18 **C14**，§3.7 接线前言与线注、§16 R11 口径随之更正，电气形态真源落在 [23 §9.3](23-wiring.md)；**V1.5：UART 板间链路弃用**（用户决策）——`.cproject` 的 **Debug 与 Release（TASKING）两个配置都定义 `USE_SPI_LINK`**，`Cpu2_Main.c` 的 AT/UART 分支不再是任何配置的默认，`com/wifi_at.c` 的定位降为"G1 失败应急返修 + C6 调试控制台"；§3.7、§5.6 末条、§17 同步，§16 R7 的退路②随之改写（回退 UART 需删符号重编 + 重刷 esp-at，代价一次刷机）。**G1 波形门禁仍未过**：本决策只取消 UART 的默认地位，不改变"SPI 波形兼容性尚未经台架证明"这一事实；**V1.6：链路泵简化（2026-09-27，`ae10aac`，台架"无法连接"类故障的主机侧根因修复）**——① §3.7/§5.6：寄存器快照从"连读两次取相同值 + 重试 3 次"改为**单读 + 消费端钳位容错**（从机每段完成/10 ms ALIVE 跳变都会即时改写寄存器，链路越忙双读比对越容易失败并升级成失联急停——"空载正常、满载断连"的根因；READY 魔数不对改为**静默 LINK_DOWN**，下周期好读即恢复，`RX_ROOM` 新增 1024 钳位）；② §5.6/§7.1：**失联判定只由 `SF_ALIVE` 500 ms 承担**，事务失败/CRC/魔数错一律只计数不判死，电机停止仍由 CPU0 的 100 ms 心跳看门狗兜底；③ §6.1a：SF_Parser 新增 **SEQ 越窗重锁**——连续 `SF_SEQ_RELOCK_RUN`(8) 帧越窗即丢窗、下一帧重锁（对端复位后最坏拒收 8 帧，原为最多 224 帧 ≈7.5 s），`test_sf` 新增 `test_seq_relock`，帧层断言升至 **2873**；④ §5.6：**移除 READY 沿的 `GEN RESET_LINK` 重同步握手**（从机不回执时泵被无限占住——状态 READY 却永不行数据事务、遥测队列静默溢出，"复位后无法连接"的第二根因），READY 沿仅 `SF_parserInit` 重锁 RX，本机 TX SEQ 保持连续，`LINK_gen` 保留作台架/诊断入口；**V1.7：F02 速度闭环 + 判向标定转正（2026-09-27）**——① §5.2：**新增 `rt/servo`**（每侧 PI，SDD §5.2 控制结构的落地：前馈 `FF_GAIN` + `Kp` + 抗饱和积分（限幅 ±30% duty）+ 输出钳位 ±1000 与输出死区；**编码器 `alive` 无效时整侧回退开环斜坡并清积分**——这是闭环能在 8 根编码器线尚未接线时就安全上车的原因）；`rt/motor_algo` 演进为任务属主：目标斜坡（单速率 `MAX_STEP`，双斜率参数待配置系统）→ servo 出 duty → BSP；急停/150 ms 失联看门狗/刹车语义不变；新增 1 Hz `SRV=` 台架行（目标/实测/duty，仅供运动时输出）。增益 `Kp=0.8/Ki=0.01/ms/FF=1.0` 为**待标定初值**（§15.3 台架整定，不许在无数据时改）。② §5.1：`g_encInvert` 从 const 全 +1 的"纸面标定"转正为**运行时可写**（`ENCODER_setInvert`，CPU1 属主），并新增 **`0x70` 台架自动判向**：CPU0 `XCORE_dirCalibRequest` → CPU1 逐轮 +12% duty 脉冲 250 ms、按计数符号翻转 `g_encInvert`（23 §8.4 步骤 2 的自动化 = §15.3 产测自动判向的前身；**轮子必须离地**，结果 `ENCCAL=` 行，急停即中止）；xcore 相应加 `dirCalib` 请求位（31 §4）。③ §6.3：`vTargetL/R` 转 ✅——CPU0 目标（percent×10）经 `ENCODER_FULL_SCALE_MM_S` 换算成 mm/s，与 `vMeas` 共用同一标定常数（vTarget 是"被要求跟的速度"，servo 闭环的作用对象，不再是另一个单位的谎言）。④ §17：`rt/motor_algo 演进 servo` 完成拆分的第一半（`motor_guard` 堵转/滑差/欠压/STBY 仍未做）。验证：`rt/servo`、`rt/motor_algo`、`mw/xcore`、`mw/proto`、`Cpu0/Cpu2_Main` 过主机 gcc `-Wall -Wextra -Werror`（桩）；SF 2873 + 遥测 116 断言回归全绿；闭环增益与判向结果**待台架**；**V1.8：跑飞自恢复 + 栈溢出上报（2026-09-27，[50-tech-assessment.md](../50-tech-assessment.md) §6 两条 🔴 落地）**——§7.2/§18 C8：**CPU0/CPU1 硬件看门狗启用**（新增 header-only `bsp/wdg.h`，`WDG_enableCpu`/`WDG_serviceCpu`；CPU1 在 1 kHz 环每圈喂、CPU0 robot 任务 10 ms 喂；窗口 `REL=0xF800`≈0.3~0.5 s，锚点为官方 Infineon `Watchdog_1_KIT_TC275_LK` 的 `0xE000`≈1.3 s 线性换算，台架实测后收紧到 §7.1 目标）；栈溢出钩子从静默 spin 改为**上报 + 主动断喂复位**（`UART_println`→`UART_flushPolling` 轮询泵 TX→`__disable` 冻结现场→空转待 CPU0 WDT 复位）；**SM 与 CPU2 看门狗有意保留关闭并各自写明理由**（SM=§7.2 量产权项；CPU2"只复位故障核"需 SMU 路由，现在开只会整机复位）；§18 C8 改写为现状 + **ENDINIT 饱和计数铁律**（喂狗必须走 `bsp/wdg.h` 的 clear+set 平衡对，纯 `IfxScuWdt_serviceCpuWatchdog()` 每次给饱和计数器 +1，16 次后 ENDINIT 再也解不开——量产 §9 OTA/Flash 解锁会静默卡死）。本机未过 TASKING 编译，IDE 构建与台架复位窗口实测为遗留项 |
| 产品定位 | 从 **demo（硬件调通验证）** 升级为 **可商业化量产** 的智能遥控底盘平台 |
| 硬件平台 | TC275（AURIX 三核 200 MHz，2×2 MB PFlash 双 bank，128 KB DFlash0 + 64 KB DFlash1）+ ESP32-C6（Wi-Fi 6 + BLE 5，512 KB SRAM，8 MB flash）+ 2×TB6612 + **4× 直流减速电机（带霍尔编码器，A/B 正交输出，预留 C/Index）** |
| 上游文档 | **本文档是 `doc/` 的设计基准**，其余文档按下列定位引用：[22-link-spi-design.md](../20-design/22-link-spi-design.md)（板间 SPI 链路详细设计，V1.2 决策来源，本文 §3.7/§5.6/§6.1a 的展开）、[23-wiring.md](../20-design/23-wiring.md)（引脚与接线真源）、[11-requirements.md](../10-product/11-requirements.md)（产品需求层，V1.2 起与本文口径对齐）、[12-demo-evaluation.md](../10-product/12-demo-evaluation.md)（demo 问题基线，其 P0/P1 已作为架构约束吸收进本文，不再逐条重复）、[31-firmware-architecture.md](../30-tc275/31-firmware-architecture.md)（**demo 代码现状参考**，量产目标态以本文为准；其唯一不可推导的结论已迁入本文 §18）。历史文档《ux-performance-plan》《esp32c6-fw-design/coding-plan》已删除：前者结论并入本文 §3.5/§11/§14，后者由 `esp32c6_car/doc/` 取代 |
| 读者 | 固件/软件工程师、硬件工程师（接口章节）、测试与生产（产测章节） |

---

## 1. 文档概述

### 1.1 与 demo 版的关系

demo 工程验证了：三核分区可行、GTM PWM + TB6612 驱动正确、AT 链路可通、跨核共享内存模式成立。量产版**继承三个不变量**，其余全部重新设计：

| 不变量（继承） | 说明 |
|---|---|
| 三核分区模型 | CPU0=FreeRTOS 服务核 / CPU1=裸机实时核 / CPU2=裸机通信核 |
| 核间共享内存 + 硬件自旋锁 | xcore 模式保留，协议升级为 v2（§5.5） |
| 命令语义集合（驾驶/配置/诊断/OTA/产测） | **帧编码不再继承**：手机 WS 段升级为协议 v2（§6.1b），板间 LINK 段改为 SPI 专用 **SF 帧**（§6.1a，V1.2 决策）。demo 的 `AA 55 CMD LEN DATA XOR-CRC` 帧不再是任何一段的真源 |

demo 的已知缺陷（评估报告 P0-1~P0-4、P1-1~P1-4）作为**架构约束**直接吸收进本设计，不单独重复。

### 1.2 量产新增的关键输入

1. **电机带霍尔编码器** → 闭环速度控制、堵转/滑差保护、里程计、产测自动判向成为 **V1.0 量产范围**（demo 中是 V1.1 路线图项）。
2. **商业化量产** → OTA 双板升级、产测/标定工具链、每机唯一身份与配对、信息安全（secure boot / 签名固件）、黑匣子诊断、质量工程流程全部进入范围。
3. **优秀的用户体验** → 吸收交互方案 Track B（原《ux-performance-plan》，结论已并入本文 §3.5/§11/§14）：C6 网络协处理器 + SPI 帧链路（§3.7）+ 摇杆驾驶流，端到端按下→动 < 50 ms。

### 1.3 术语

| 术语 | 含义 |
|---|---|
| SBL / App A / App B | TC275 二级引导程序 / PFlash0 应用槽 / PFlash1 应用槽 |
| E2E | End-to-End 保护（alive counter + CRC，防消息丢失/重复/损坏/乱序） |
| POST / RUNST | 上电自检 / 运行期自检 |
| DPT | 产测模式（Deep Production Test） |
| LINK | C6↔TC275 高速帧链路：**SPI**（TC275 QSPI3 主机 P33.11/12/13 + CS P23.4 + 握手 P23.0 ↔ C6 SPI2 从机 GPIO19/18/20/23/21） |
| SF 帧 | SPI 链路专用帧（§6.1a）：4 字节对齐、段内可串多帧、带 FRAG 位，承载于半双工 DMA 事务之上 |
| ESSL | Espressif 从机共享寄存器握手模型（主机轮询从机发布的 u32 寄存器决定读/写，§3.7） |
| 安全态 | STBY 拉低 + PWM 0% + 方向脚释放（电机电气隔离）。**落地注记（V1.15）**：STBY 现已 GPIO 受控（23 §3.2），但**故障/急停路径尚未拉它**——现实现的急停是 IN1=IN2=H 短接刹车，拉低 STBY 会退化为高阻滑行；两者的取舍待裁决（§5.2 落地状态） |

---

## 2. 产品与需求概述

### 2.1 V1.0 量产功能范围

| # | 功能 | 说明 |
|---|---|---|
| F01 | 驾驶控制 | 摇杆比例控制（v, ω → 左右轮 mm/s），方向键兼容模式 |
| F02 | **闭环速度控制** | 霍尔编码器 + 1 kHz PI，坡道/负载下速度稳定 |
| F03 | 里程与速度显示 | odometry（mm）、实时 m/s、总里程持久化 |
| F04 | 安全体系 | 三层看门狗链、E2E、堵转/滑差/失速保护、欠压保护、急停、失联停车 |
| F05 | 双板 OTA | TC275 双 bank 切换 + C6 esp_ota A/B，签名验签，失败自动回滚 |
| F06 | 配对与身份 | 每机唯一 SSID/密码（SN 派生）、物理按键确认配对（token） |
| F07 | Web 控制端 | Captive Portal、WebSocket 50 Hz 遥测仪表、多客户端（1 控 + 2 观） |
| F08 | 诊断 | 错误码体系、黑匣子（复位原因/故障快照/里程统计）、网页导出 |
| F09 | 产测与标定 | 产测命令集、电机/编码器自动校验、速度系数标定、SN 写入、老化模式 |
| F10 | 配置管理 | 出厂默认 + 用户配置（限速档、灯效、休眠策略），DFlash 持久化 |

### 2.2 非功能需求（NFR）

| 类别 | 指标 |
|---|---|
| 实时性 | 命令端到端（触屏→车轮响应）≤ 50 ms；控制环 1 kHz ±5 µs 抖动；**板间命令下行（含主机轮询节拍）≤ 5 ms**（§3.7 半双工拉取模型） |
| 可靠性 | 连续运行 72 h 无复位（老化判据）；任一单核挂死 ≤ 200 ms 内进入安全态 |
| 安全（Safety） | 失联/急停/故障 → 安全态 ≤ 100 ms；断电重启后不得自行恢复运动 |
| 信息安全 | 固件签名校验强制；配对需物理确认；调试口出厂关闭 |
| OTA | TC275 1 MB 镜像 ≤ 10 s（SPI 5 MHz 有效吞吐 ≈ 550 KB/s，传输 ≈ 2 s + 校验/编程）；断电/失败可回滚 |
| 可生产性 | 单机产测 ≤ 90 s（含标定）；产测报告机器可读 |
| 合规（软件配合项） | 预留 SRRC/CE/FCC 测试模式（连续发射、跳频参数只读导出） |

---

## 3. 总体架构

### 3.1 设计原则

1. **安全收敛在 TC275**：一切运动许可判定（斜坡、失联、堵转、欠压、急停）只存在于 TC275；C6 是不可信的传输协处理器，它死机只导致停车，不导致失控。
2. **分区隔离**：三核各自独立喂狗、独立栈预算、独立失败域；跨核仅通过 xcore v2 消息（§5.5），禁止跨核直接调用。
3. **一切双份可回滚**：两板固件均 A/B 槽；配置双页提交；关键决策有默认值兜底。
4. **可测试性内建**：产测模式、自检、遥测、日志是架构组成部分，不是事后补丁。
5. **性能预算前置**：每核 CPU 占用、每条链路带宽、每块 RAM 在设计期给出预算（§14），超预算即架构变更。

### 3.2 系统部署视图

```
┌──────────────┐  Wi-Fi 6 AP「SD-XXXXXX」   ┌─────────────────────────────┐
│ 手机控制端 ×1 │◄───── WebSocket(二进制) ───►│ ESP32-C6 网络协处理器        │
│ 观赛端 ×2    │◄───── HTTP 静态页/Portal   │  softAP+BLE · HTTPD/WS      │
└──────────────┘                            │  签名固件缓存 · 自身 A/B OTA │
                                            └──────────────┬──────────────┘
                                                           │ LINK: SPI 5 MHz 半双工事务
                                                           │ TC275 主机拉取 + IRQ 握手 + 心跳
┌──────────────────────────────────────────────────────────┴──────────────┐
│ TC275 三核                                                                │
│  CPU0(FreeRTOS): mission 状态机 · 配置/诊断/黑匣子 · OTA 编排 · 看门狗链根 │
│  CPU1(裸机 1kHz): 编码器×4(TIEM中断×4解码) · 速度PI×2 · 双斜率 · 堵转/滑差 · STBY │
│  CPU2(裸机泵):   LINK 帧收发 · 鉴权 · 固件流式接收→PFlash 编程            │
└──────────────┬────────────────────────┬───────────────────┬─────────────┘
               ▼ GTM ATOM PWM 20kHz     ▼ GTM TIM TIEM ×4      ▼ VADC
           2×TB6612(STBY 受控)      4×霍尔编码器          电池分压采样
```

### 3.3 TC275 核分区与负载预算

| 核 | OS | 周期任务 | V1.0 负载预算 | 上限 |
|---|---|---|---|---|
| CPU0 | FreeRTOS（tick 1 ms） | mission 2 ms、遥测聚合 20 ms、诊断 100 ms、OTA 编排（空闲时） | ~15% | 50% |
| CPU1 | 裸机超循环 1 kHz | 编码器采样 ×4 + PI ×2 + 保护 + 遥测打包 | ~20% | 60% |
| CPU2 | 裸机泵（事件驱动） | LINK 收发、鉴权、OTA 流接收（Flash 编程期间独占） | ~10% | 50% |

### 3.4 软件模块分解（TC275 新目录结构）

```
tc275_car/
├── bl/                      # 二级引导程序（SBL，独立工程，~48KB）
│   ├── bl_main.c            # 槽选择/回滚计数/跳转
│   └── bl_drv/              # 自带最小 Flash/UART/CRC 驱动（不依赖 App）
├── Cpu0_Main.c / Cpu1_Main.c / Cpu2_Main.c
├── app/                     # 应用服务层（CPU0 上下文）
│   ├── mission.c/.h         # 运行模式状态机（替代 demo robot.c 的角色层）
│   ├── drive_policy.c/.h    # 驾驶意图→轮速策略（限速档/转向比/弧线语义修正）
│   ├── telemetry.c/.h       # 50Hz 遥测聚合（轮速/电池/里程/错误码/版本）
│   ├── config.c/.h          # 配置服务（DFlash 双页提交，§4.3）
│   ├── diag.c/.h            # 错误码管理 + 黑匣子（§8.4）
│   ├── ota_mgr.c/.h         # 双板 OTA 编排（§9）
│   └── post.c/.h            # 上电/运行自检（§8.3）
├── rt/                      # 实时域（CPU1）
│   ├── encoder.c/.h         # GTM TIM TIEM 边沿中断 + 软件 ×4 正交解码 + 速度/里程（§5.1；已实现，demo 期位于 Bsp/encoder.c）
│   ├── servo.c/.h           # 速度 PI×2 + 前馈 + 双斜率（替代 motor_algo.c）
│   ├── motor_guard.c/.h     # 堵转/滑差/过流(预留)/欠压联动 + STBY 硬线
│   └── timebase.c/.h        # STM 时基 + 看门狗喂狗节点
├── com/                     # 通信域（CPU2）
│   ├── link.c/.h            # SF 帧主机侧 QSPI 事务调度器（替代 wifi_at.c，§5.6）
│   ├── spi_hal_pins.c/.h    # QSPI3 主机引脚/时钟档位/GEN 事务（新增）
│   ├── auth.c/.h            # token 会话校验、重放防护
│   └── fw_stream.c/.h       # OTA 数据流接收→PFlash1 编程（RAM 执行的 Flash 驱动）
├── mw/                      # 中间件
│   ├── xcore/               # xcore v2：类型化消息 + E2E（§5.5）
│   ├── sf/                  # SF 帧编解码（LINK 段真源，纯 C，主机端可测，§6.1a）
│   ├── proto/               # v2 帧编解码（手机 WS 段，§6.1b）
│   ├── calib/               # 标定记录：`calib_record.*` 纯 C99 编解码（主机可测）+ `calib_store.*` CPU0 DFlash 持久化（§4.1 扇区 15，34 §8）
│   └── log/                 # 跨核日志环（继承 demo 设计，加溢出统计）
├── bsp/                     # 板级：pwm_motor / enc_pins / adc_batt / stby / led / button / uart_dbg
│                            #   已落地额外有 `wdg.h`（header-only CPU 看门狗助手，§7.2/§18 C8；`rt/timebase` 落地后由其承担调用）
├── Configurations/          # FreeRTOSConfig.h（量产版：看门狗/栈检查/断言全开）
└── test/                    # 主机端单元测试（Unity），CI 执行
```

> **落地进度注记（2026-09-26，V1.3 更新）**：**目录重排已落地**——仓库已从 demo 布局（`App/ Middleware/ Bsp/`）迁入上表目标态：`app/robot.*`、`rt/{motor_algo,encoder}.*`、`com/{link,spi_hal_pins,wifi_at}.*`、`mw/{xcore,proto,sf}/`、`bsp/{motor,uart,stime}.*`；`Cpu*_Main.c` 与 `Configurations/` 留在工程根；全部内部 include 已改为工程根限定的模块路径，`.cproject` 四个构建配置的 include 路径同步（工程根本就是 include 根之一）。**尚未做的只有"文件级拆分/新建"类工作**：`app/robot.c` 拆分 `mission` + `drive_policy`、`rt/motor_algo` 演进 `servo` + `motor_guard`、`bsp/stime` 归属 `rt/timebase`（并承担喂狗）、`mw/log` 独立目录、未实现模块 `app/{config,diag,telemetry,post,ota_mgr}`、`com/{auth,fw_stream}` 与 `bl/`（SBL，独立工程）。逐文件状态见 [22 §7.2](22-link-spi-design.md)。

ESP32-C6 固件（独立 IDF 工程 `esp32c6_car/`，与本工程同级目录；其模块级设计与编码计划见 `esp32c6_car/doc/`，本仓库不再维护 C6 侧 LLDD）：

```
components: net(softAP/STA/Portal/mDNS) · httpd+ws · bridge(WS↔SF 映射) ·
            link(spi_slave_hd 从机 + 握手寄存器 + IRQ；UART 仅控制台) · c6_sf(SF 帧) ·
            auth_pair(配对token) · ota_self(A/B+签名) · assets(gzip页面) ·
            factory(NVS: SN/密钥) · maint(BLE 维护通道, 可选)
```

### 3.5 关键数据流（命令与遥测）

```
命令: 手机 ──WS(≤30Hz)──► C6 bridge ──SF/SPI(C6 拉 IRQ，主机采电平后开读事务)──► CPU2 auth→xcore队列 ──► CPU0 mission(2ms)
                                                                        │ 目标轮速(mm/s)+E2E
遥测: CPU1(1kHz 实测轮速/保护状态) ──xcore──► CPU0 聚合(50Hz) ──xcore──► CPU2 ──SF/SPI(主机写)──► C6 ──WS 广播──► 全部客户端
```

> 当前实现进度：命令方向的 `auth`（§7）尚未落地，`com/link.c` 校验 SF 帧后直投 xcore 命令队列；急停（`PROTO_CMD_EMERGENCY_STOP`）不经队列，直接走 `XCORE_estopRequest()`（§5.3）。

端到端时延预算（继承交互方案 Track B 并收紧）：命令 ≤50 ms；遥测 20 ms 周期；**链路段时延 ≤ 2.3 ms**（半双工主机拉取模型：泵每圈采电平 + 2 ms 保活轮询兜底 + 事务 210 µs，握手线为高时按泵节拍取，见 §18 C9）；断链→停车 ≤100 ms。

### 3.6 多核 OS 形态选型：SMP 与 AMP

**概念**：

- **SMP（对称多处理）**：一个内核实例管理所有核，任务放共享就绪队列，由调度器动态分配并在核间迁移（负载均衡）。例：Linux/iOS、ESP32 官方双核 FreeRTOS。适合负载动态变化、任务同质、只求吞吐的通用计算。
- **AMP（非对称多处理）**：每核运行独立软件栈（各自的 OS 实例，或裸机超循环），哪个核干什么在设计期静态定死，核间只经共享内存消息通信。例：汽车 ECU 分区、本产品。适合职责固定、实时性优先的系统。
- 另有**绑核 SMP**（SMP + 任务钉核）：名义 SMP、实际确定性损失照付，两头不占，不推荐。

| 维度 | SMP | AMP |
|---|---|---|
| 任务放置 | 调度器动态分配/迁移 | 设计期静态定核 |
| 时延确定性 | 差（共享就绪队列锁、任务可能漂移） | 好（每核 WCET 可独立分析） |
| 故障归因 | 难（"谁在哪个核"是动态的） | 易（核级失败域，看门狗链精准定位） |
| 调度开销 | 跨核锁 + IPI 抖动 | 各核独立，零调度抖动 |
| 移植成本 | TriCore 无官方 SMP 移植层，自研月级 | 本工程移植层原生支持（编译期 `configCPU_NR` 绑核） |
| 适合负载 | 动态、同质、吞吐优先 | 固定功能、实时优先 |

**决策：采用 AMP（混合形态：CPU0 一个 FreeRTOS 实例 + CPU1/CPU2 裸机超循环）**，即维持 §3.3 分区。理由：

1. 本产品是**固定功能分核**，没有动态负载——SMP 的核心收益（任务迁移、负载均衡）无使用场景，把 1 kHz 控制任务迁去别的核是退化而非优化；
2. CPU1 的 1 kHz 闭环要求 WCET 可分析，裸机零抖动是最优解而非妥协；
3. 安全架构依赖核级失败域：§7.2 看门狗链之所以能精准归因"哪个核异常"，正是功能按核静态划分的结果；
4. 成本不对称：工程所用移植层（`portable/Tasking/AURIX_TC27x`）是单核实现（单一 `pxCurrentTCB`，无核间中断原语），TriCore 无官方 SMP 移植，自研需月级；而 AMP 仅需编译期参数——该移植层按多实例设计：`__vector_table(configCPU_NR)` 绑核、SRC TOS 自动映射（`configCPU_NR>0 ? +1`，对应 TC275 的 TOS 0/2/3）、每核独立 STM 心跳。

**AMP 落地要点**（若为 CPU1/CPU2 实例化内核）：

- 内核静态数据必须每核一份：同一套内核源码经符号重命名（TASKING `--rename` 或预处理命名空间包装）链接三份，各自配套 `FreeRTOSConfig.h`、heap、CSA 区；
- 跨核通信**不用** FreeRTOS 队列（实例私有），统一走 xcore v2（§5.5）；
- 各实例独立接入 §7.2 看门狗链，失败域不共享。

**演进触发条件**：某核出现"多个任务需要阻塞/唤醒/互斥"（如 CPU2 未来承载 TLS、BLE 维护通道、多协议栈）时，为**该核**实例化 FreeRTOS；单一确定性循环的核保持裸机。**不规划 SMP**：收益不成立（无任务迁移需求），成本确定（移植自研 + 确定性损失 + 安全归因复杂化）。

### 3.7 板间通信方式选型（LINK 物理层决策）

候选方案与排除项：USB、以太网直接排除（C6 无以太网 MAC/PHY；TC275 无 USB 主机/设备控制器，C6 的 USB 仅 Serial/JTAG 调试用，双方无交集）。SDIO 排除（TC275 无 SDIO 主机外设，ESP32 侧即使部分型号有 SDIO 从机能力也无从对接）。实际候选为 UART、SPI、TWAI(CAN)：

| 方案 | 有效带宽 | 全双工 | 驱动成熟度（TC275 / C6） | 抗干扰 | 布线 | OTA 1 MB 耗时 | 结论 |
|---|---|---|---|---|---|---|---|
| SPI（QSPI3 主 ↔ `spi_slave_hd` 从）@5 MHz | ~550 KB/s | 半双工（主机拉取 + IRQ 握手） | IDF 从机 HD 官方支持（`SOC_SPI_SUPPORT_SLAVE_HD_VER2`）/ TC275 侧**全新代码**，前导相位兼容性待台架验证（R7） | 时钟同步，较好 | ≥5 线（含握手） | ≈2 s | **V1.0 选定（V1.2 决策）** |
| UART @2 Mbps | ~200 KB/s | 是 | iLLD ASCLIN / IDF UART，两侧均零风险 | 依赖 CRC16 + 降速，需控线长 | 2 线 | ≈6 s | **板间链路已弃用**（2026-09-26，§5.6 末条）：只保留 ASCLIN1 作 C6 调试控制台 + G1 失败应急返修，接线不拆 |
| TWAI/CAN 1 Mbps | ~50 KB/s 有效 | 半双工（有仲裁） | MultiCAN（CAN 2.0）/ TWAI（需外部收发器） | 差分，车规最优 | 2 线差分 | 数十秒（8 B 帧分片低效） | 不作主链路；V2 预留收发器焊位（车队/多底盘协同） |

**决策（V1.2）**：V1.0 主链路 = **SPI：TC275 QSPI3 主机（1 MHz 起 / 5 MHz 量产基线，探索档 10/20 MHz）↔ C6 SPI2 从机（IDF `spi_slave_hd`）+ 1 根 IRQ 握手线 + LINK 段 SF 帧（§6.1a）**。UART 通道保留物理接线，作为调试控制台与 G1 验证失败时的回退链路——**2026-09-26 追加决策：UART 作为板间链路弃用，SPI 是唯一板间链路**，UART 只剩 C6 调试控制台，回退变成"删符号重编 + 两侧重刷"的双侧动作（§5.6 末条、§18 C15）。

理由：① C6 改为自研固件后，SPI 从机不再是 esp-at 的黑盒，"驱动有没有"的风险消失，剩下的唯一真风险收敛为"主机波形是否被 Espressif HD 从机正确解析"，可用一次台架验证（R7/G1）关死；② 半双工 + 共享寄存器握手 + 4 字节段对齐是**硬件强制约束**，与其把 UART 上长出来的自描述字节流帧硬套上去，不如为这一段重定 SF 帧，换来分片位、4 B 对齐与更大 CHUNK（OTA 分片从 62 B 提到 240 B）；③ 稳态流量占用从 UART 的 <2% 降到 <1%，且天花板从 200 KB/s 提到 5 MHz 档 ≈550 KB/s（20 MHz 探索档 ≈2 MB/s），V2 的高频遥测/大流量日志回传不必再换链路；④ 主从时序仍归安全/实时域，失效方向仍然正确（下段）。

**角色分配（不做二次论证）**：**TC275 = SPI 主机（QSPI 主机模式，提供 SCLK 与硬件 CS）；C6 = SPI 从机（`spi_slave_hd` + DMA）**。原则是"总线时序归安全/实时域"：主机身份决定总线节拍由谁的时域调度，C6 的 Wi-Fi/HTTPD 任务抖动（毫秒级、不可控）不得污染总线时序；失效方向也正确——从机挂死不会卡住总线，TC275 主机事务超时 / 心跳寄存器不推进 → 判 LINK 丢失 → 走失联停车，延续"C6 死机只导致停车、不可能导致失控"（§3.1）。生态先例一致：esp-at "AT over SPI"、ESP-Hosted 等协处理器方案均为 ESP 从机、主控 MCU 主机。

**接线（四线 + 一根握手）**——**2026-09-26 已按此表完成实物接线**（孔位与线束见 [23-wiring.md](../20-design/23-wiring.md) V1.10 §1/§9.1；**电气形态：两板之间全部为杜邦线直连，无任何外部元件**——无外部上拉/端接/电平转换，IRQ 高电平仅靠 TC275 片内上拉，coding 后果见 23 §9.3 与本文 §18 C14；两侧固件代码均已改为 SPI，**UART 板间链路已于 2026-09-26 弃用**（两个 TASKING 配置都定义 `USE_SPI_LINK`，见 §5.6 末条）；链路**从未通电联调**，启用后第一件事是过 G1）：

```
TC275 (QSPI3 主机)                 ESP32-C6 (spi_slave_hd + DMA)
  SCLK  P33.11 ────────────────►     GPIO19  时钟，TC275 提供
  MTSR  P33.12 ────────────────►     GPIO18  MOSI（遥测/OTA/命令帧下发）
  MRST  P33.13 ◄────────────────     GPIO20  MISO（命令/ACK 上行）
  SLSO5 P23.4  ────────────────►     GPIO23  CS，TC275 硬件片选
  GPIO  P23.0  ◄────────────────     GPIO21  IRQ（开漏）"我有帧待取"（杜邦线直连，无外部上拉电阻）
  （可选预留不接：TX_RDY 流控线 P23.5 ↔ C6 空闲 GPIO）
```

**事务模型（摘要，全量见 [22-link-spi-design.md](../20-design/22-link-spi-design.md) §4）**：

- 半双工定长事务，前导相位 `CMD(8) + ADDR(8) + DUMMY(8)`，数据相位长度取 4 的倍数（从机硬约束）。
- **共享寄存器握手**（从机用 `spi_slave_hd_write_buffer()` 发布 6 个 u32：`SF_READY / SF_TX_PENDING / SF_RX_ROOM / SF_ALIVE / SF_ERRSTAT / SF_CMDRSP`），主机轮询后决定读多少、写多少。**寄存器读非原子**（Espressif `segment_mode` 例子注释的既有事实）——但**快照取法是单读，不是连读比对**（V1.6 修订）：从机每段完成/10 ms ALIVE 跳变都会即时改写寄存器，双读比对在链路越忙时越容易失败并升级成失联急停，正确做法是让每个消费端容忍单读可能产生的撕裂值（READY 魔数错 → 静默 LINK_DOWN 自愈；`TX_PENDING`/`RX_ROOM` 越界即钳位；判活只看 `SF_ALIVE`）。
- **主机拉取模型**：C6 无法主动推送。命令下行 = C6 更新寄存器并拉 IRQ → TC275 泵**每圈采 P23.0 电平**，高即开读事务；线为低时按 2 ms 保活节拍轮询（**电平轮询是主路径**：P23.x 产生不出边沿中断，§18 C9）。上行遥测 = TC275 按 20 ms 周期主动写事务。
- SF 帧承载于数据段之上，**CRC16 + SEQ 全部保留**——SPI 解决电气失步，不解决位翻转，§6 的端到端语义不得因物理层升级而削弱。
- **突发收尾是硬要求**（§18 C10）：`RDDMA` 突发后补一帧 `INT0`、`WRDMA` 突发后补一帧 `WR_END`，从机才释放缓冲；线上命令字节取 `spi_ll.h` 的 `SPI_LL_BASE_CMD_HD_*`，不是 `spi_types.h` 的 `BIT(n)` 枚举。

**诚实的代价**：换到主机拉取模型后，"命令下行最坏时延"从 UART push 的 ~1 ms 变成 `轮询周期 + 事务时间` ≈ 2.3 ms。50 ms 端到端预算下无影响，但不得记成"SPI 全面优于 UART"。

**后果与升级路径**：V1.1 议题不再是"要不要换 SPI"，而是 (a) 量产线束定长屏蔽后把基线提到 10 MHz；(b) 双/四线（QUAD）需 C6 侧补 WP/HOLD 两脚与 TC275 备用数据线，当前不付费；(c) 出现 TC275 大流量示波数据回传时启用第二通道或改 `spi_slave_hd` 的 append 模式。TWAI 仅在产品路线加入车联网总线和多机协同（11-requirements.md V3.0 毫米波雷达共享目标列表等）时启用。风险关联：R7（波形兼容性，G1 门禁）、R8–R11 见 §16。

---

## 4. 存储与内存规划

### 4.1 TC275 内存地图（实测自 Lcf_Tasking_Tricore_Tc.lsl + 数据手册）

| 区域 | 容量 | 量产分配 |
|---|---|---|
| PFlash0 @0x80000000（2 MB） | bank0 | BMH + SBL（0x80000000–64KB，含双 BMH 头）+ **App A**（0x80010000 起，≤1 MB）+ SBL 参数镜像 |
| PFlash1 @0x80200000（2 MB） | bank1 | **App B / OTA 下载槽**（0x80210000 起，≤1 MB，与 App A 同构镜像） |
| DFlash0 @0xAF000000（128 KB） | EEPROM 仿真 | 配置页 ×2（双页提交）+ 黑匣子环形区（§8.4），磨损均衡。**已落地占用：逻辑扇区 15 = `0xAF01E000`（8 KB）单槽 `CalibRecord` 标定记录（34 §8，CPU0 唯一写者）**；扇区 0..14 留给本节。**DF0 真实扇区数待裁决**：iLLD 的 `IfxFlash_dFlashTableLog` 列 48 个逻辑扇区（至 `0xAF05FFFF`，族上限表），与本行的 16 扇区口径冲突，见 34 §12 Q1 |
| DFlash1 @0xA0000000（64 KB） | 出厂数据 | SN、密钥/证书、槽选择与回滚计数、标定数据镜像、产测结果 |
| DSPR0/1/2（112/120/120 KB） | 数据 RAM | CPU0: FreeRTOS 堆 48 KB + 任务栈预算 24 KB + xcore v2 缓冲；CPU1: PI/编码器状态 <4 KB；CPU2: SPI 段缓冲 2×256 B（4 B 对齐）+ 握手寄存器镜像 32 B + OTA 流缓冲 8 KB |
| PSPR0/1/2（24 KB ×3） | 代码 RAM | **CPU2 的 Flash 编程例程与关键 ISR 从 PSPR 执行**（擦写 PFlash 时必须离开被擦 bank，§9.2）；CPU1 时间临界环可选用 |

约束：运行 bank 自编程时，执行代码与中断向量必须位于另一 bank 或 RAM——这是 `fw_stream` 把擦写例程搬 PSPR 的原因（iLLD `IfxFlash` 标准做法）。

### 4.2 ESP32-C6 分区表（8 MB）

| 分区 | 大小 | 用途 |
|---|---|---|
| nvs / nvs_cert | 32 KB / 16 KB | 配网信息、SN、配对 token 密钥（nvs 加密） |
| otadata | 8 KB | A/B 指针 |
| ota_0 / ota_1 | 各 3 MB | 固件 A/B（esp_ota_ops 标准 A/B） |
| assets | 512 KB | gzip 控制页/图标（随固件版本发布） |
| coredump | 64 KB | 崩溃转储（网页可导出，接 diag） |
| factory_ota_cache | 1 MB | TC275 固件中转缓存（可选，见 §9.3 路线 B） |

### 4.3 配置与标定数据模型（DFlash0，双页提交）

```c
typedef struct {                 /* 每页 = 头(16B) + 载荷 + CRC32 */
    uint32 seq;                  /* 单调递增，双页取大者为有效 */
    uint16 version;              /* 结构版本，迁移函数链 */
    uint16 length;
    /* --- 用户配置 --- */
    uint8  speedModeLimit;       /* 限速档 0..100 (%) */
    uint8  steerRatio;           /* 转向比 50..150 (%) */
    uint8  lightMode; uint8  lang;
    /* --- 标定（产线写入） --- */
    int16  motorGain[4];         /* PWM→转速前馈系数 (cts/s per ‰duty) */
    int16  steerTrim;            /* 直行配平 */
    uint16 battAdcK;             /* 电池分压系数 mV/LSB */
    /* --- 统计 --- */
    uint32 odoTotalM;            /* 总里程(米)，1min 持久化一次 */
} ConfigRecord;
```

写入策略：整页构建 → CRC → 写备份页 → 写主标志；读取时双页取 `seq` 大且 CRC 正确者，**掉电任意时刻均有一份有效配置**。写入寿命：配置类 <100 次/生命周期、里程 1/min ≈ 5 万次/年——DFlash0 128 KB 提供 ≥2 万次擦写 + 环形多页（16 页轮转）→ 满足寿命，超出后只丢统计不丢标定（分级存储：标定/配置与里程分页）。

---

## 5. 模块详细设计（TC275）

> 每个模块给出：职责 / 上下文 / 关键接口 / 失败行为。全部模块禁止动态内存（除 CPU0 FreeRTOS 堆在初始化期），全部跨模块数据经 xcore v2 或只读快照。

### 5.1 rt/encoder —— 霍尔编码器接口（量产核心模块；**已实现 `rt/encoder.c`，demo 期位于 `Bsp/encoder.c`**）

- **职责**：4 路霍尔正交信号解码，输出每侧轮速（mm/s，1 kHz）与累计里程；为伺服/保护/产测/遥测提供统一数据源。
- **硬件方案（2026-09-26 随实现纠错）**：**GTM0 TIM 组 0 八通道 TIEM（输入事件模式）双边沿中断 + 软件 ×4 正交解码**，A 相进偶数通道、B 相进奇数通道；引脚 P33.0~P33.7 = X2-28~35（接线表 23-wiring.md §8.2）。原稿的 "TIM UDC 硬件正交（`UDCCTRL/CLS/DUTC`）" 是 **GTM gen2 寄存器，TC275 的 gen3 TIM 没有 UDC**（模式仅 TPWM/TPIM/TIEM/TIPM/TBCM/TGPS，`IfxGtm_regdef.h` 可证），GPT12 增量口与 ERU 输入又都不在引出脚上（P33.x 无 ERU 通路，详见 23-wiring §8.3）——ISR 解码不丢计数的代价是空载 500 rpm 四路合计 ≈1.7k 中断/s、13 V 推荐上限 ≈3k/s，≪1% CPU1（13 PPR = 52 边沿/电机转，V1.12 刻度更正）。霍尔开漏上拉至 3V3（禁 5V），TIM 通道配 2 µs 去毛刺滤波。
- **已实现接口（`rt/encoder.c/h`，属主 CPU1，原 `Bsp/encoder.c/h` 随 §3.4 目录重排迁入）**：
  - `ENCODER_getSpeeds(int32 v[2])` —— 左/右侧轮速 mm/s（8 ms 滑窗**均值**（2026-10-01 由中值改为均值：每侧 1 计数/ms = 71 mm/s，逐 ms 整数增量取中值会量化成 71 mm/s 台阶、低速恒读 0；窗内求和保留全部计数，分辨率 ≈8.9 mm/s；判向标定 0x70 同批修正为“按当前符号取反”而非强置 -1）；轮径 `ENCODER_WHEEL_DIA_MM 48`（MG310 实配 48 mm 胎）为**默认值**，运行时可改，见下"参数运行时化"）；
  - `ENCODER_getOdometer(uint32 m[2])`；`ENCODER_getRawCounts(int32 c[4])`（产测判向用）；
  - `ENCODER_isAlive()` —— 500 ms 窗口内有边沿即 alive（**V1.16 起语义明示为"近期有运动"指示**：任一侧动过即真、静止超窗即假，**不是健康位**——单侧失效会被对侧运动掩蔽，静止的健康传感器反而报假）；`ENCODER_task()` 在 1 kHz 算法环内运行，经 `ENCODER_publish()` → `XCORE_encoderPublish` 一次发布**两个单位域**：实测速度 percent×10（alive 时 CPU0 用它覆盖状态块 leftSpeed/rightSpeed）+ 物理域 mm/s 与左右侧里程 mm（CPU2 取用填遥测 `vMeasL/R`、`odoSession`，§6.3，2026-09-27 起生效）。
  - **按侧健康原语（V1.16，BUG-ENC-1 P0 修复，根因推导与已知边界见 [51](../51-encoder-health-bugfix.md)）**：快照新增 `edgeAgeMs[2]`——每侧"距该侧最近一次边沿的毫秒数"，饱和 0xFFFF（含"自启动无边沿"），由 ISR 时间戳在发布时合成；阈值 `ENCODER_EDGE_FRESH_MS=100`（侧速 1 mm/s ≈ 每 71 ms 一边沿，100 ms 覆盖到 ~0.7 mm/s，更慢与静止不可分）。**消费端语义 = "被驱侧必须持续出边沿"**：侧速请求非零而边沿龄 >100 ms 持续累计 500 ms 起步宽限（`FUSION_ENC_GRACE_MS`，覆盖斜坡+起动滞后，松杆清零——直接拿旧 `alive` 当健康位会造成"静止超 500 ms → 前进即锁存 → 轮子永不转"的死锁，这是运动位不能当健康位的结构性原因）后判该侧失效。融合（`FUSION_ENCODER_OK`、速度锚、轮速/陀螺一致性）与伺服（measOk 按侧，死侧回退开环清积分）都改按侧消费；"发布序号变化"只证明发布任务活性，不再作为编码器有效依据。同侧两轮之一失效仍不可分（按侧求和、无逐轮冗余判定），留作增强项。
- **量产增量（本仓库未含）**：`ENCODER_selfCheck()`（静止漂移 + 单侧脉冲注入比对）、`ERR_ENC_DEAD` → `motor_guard` 安全态联动。**速度系数标定写入 DFlash 已做**（V1.10，见下条与 [34 §8](../30-tc275/34-calib-dpt.md)，取代 §15.3 的"写宏重编"设想）。
- **标定参数运行时化 + 持久化（V1.10 落地）**：`ENCODER_FULL_SCALE_MM_S` 与轮径（初版 65 mm，V1.12 起默认 48 mm）由宏/字面量改为 CPU1 静态变量 `g_fullScaleMmS / g_wheelDiaMm`（初值 = 宏默认，`ENCODER_set*/get*` 读写，越界回落默认），测速与 pct 发布**共用同一变量**——**换算公式一字未动**（改公式 = 重调整个闭环，33 §5.2 红线）。值由 CPU0 从 **DFlash0 扇区 15** 的 `CalibRecord` 加载（magic + CRC16 + 范围三重校验，任一失败整体回落默认并 `src=0` 上报），经 xcore `RecordLive{version, rec}` 广播，CPU1 在 1 kHz 环检到 `version` 边沿即应用（invert / 两参数）；CPU2 的 vTarget 遥测换算读同一块（§4.2 D4）。写入只由 CPU0 的 robot 任务执行（喂狗→关中断→擦→逐页写→回读→开中断），且**车辆运动中延迟到静止**。真源 [34 §8/§11](../30-tc275/34-calib-dpt.md)。
- **关键行为**：计数 32 位累加（ISR 单写者）；判向符号表 `g_encInvert[4]` 默认全 `+1`，**运行时可写**（`ENCODER_setInvert`，CPU1 属主，ISR 即时生效）——台架判向走 **`0x70` 自动判向**（CPU1 逐轮 +12% duty 脉冲 250 ms，按计数符号翻转符号表，结果 `ENCCAL=` 日志行，急停即中止；**轮子必须离地**，见 23-wiring §8.4 步骤 2 的自动化版，§15.3 产测自动判向的前身）。
- **失败行为（量产）**：电机通电而对应编码器计数恒 0（500 ms）→ 上报 `ERR_ENC_DEAD` → `motor_guard` 进入安全态。

### 5.2 rt/servo —— 闭环速度伺服（替代 demo motor_algo.c）

- **上下文**：CPU1 1 kHz。
- **控制结构**（每侧相同）：

```
目标轮速(mm/s, 来自CPU0, 带E2E)
   │ 双斜率限幅: 加速 ≤ a_acc, 减速 ≤ a_dec (配置), 急停旁路
   ▼
[ 前馈 duty_ff = v * motorGain⁻¹ ] ──►(+)──► duty ──► MOTOR_setSpeed
   ▲                                  (-)
   │        ┌── Kp + Ki(抗饱和) ◄── e = v_target - v_meas ◄── ENCODER_getSpeeds
```

- **参数**：Kp/Ki 按标定增益自整定初值，产测模式可在线整定并回存 `motorGain`；积分限幅 ±30% duty；输出钳位 ±1000（继承 demo BSP 钳位，三层一致原则）。
- **保护联动**（`motor_guard`，同任务内）：
  - 堵转：|v_target|>20% 满速 且 |v_meas|<5% 持续 500 ms → `ERR_STALL` → 安全态（STBY 拉低）；
  - 滑差：左右轮实测速差 > 配置阈值且同目标 → 仅上报 `ERR_SLIP`（不干预，供遥测）；
  - 欠压：`adc_batt` 低于警告阈值 → 目标全局限幅 50%；低于临界 → 受控减速停车 + `ERR_BATT_LOW`（区别于急停的立即刹车）；
  - **STBY 硬线**：`bsp/stby` 独占控制 TB6612 STBY，任何 `ERR_STALL/ERR_BATT_CRIT/急停/看门狗复位路径` 直接拉低——与软件 PWM 形成双通道断电。**落地状态（V1.15，2026-10-04）与一处待裁决语义**：STBY 已从"跳线常接板载 3V3"改为 TC275 GPIO 受控（**P22.2 / X1-16 → D24A J4-2**，一根线管四路，真源 [23 §3/§3.2](23-wiring.md) V1.23），控制函数落在 `bsp/motor.c:MOTOR_setEnabled()`（未新开 `bsp/stby` 文件），当前只用于 **① `MOTOR_init` 末步使能（8 根 IN/PWM 已置低、duty=0 才拉高）② 未跑固件/MCU 复位时由高阻 + TB6612 片内 200 kΩ 下拉自然落回 standby**。"**故障/急停路径拉低 STBY"这半仍未接**，原因是与现实现冲突：`rt/motor_algo.c:MOTOR_ALGO_brakeAll()` 是 IN1=IN2=H 的**短接刹车**，STBY 一拉低即退化为高阻滑行、急停距离变长——属安全语义决策，需用户放行后与 `motor_guard` 同批做。另：D24A 把两片 TB6612 的 VCC 接板载 **5 V** ⇒ `VIH(STB)=VCC×0.7=3.5 V`，3.3 V GPIO 在保证门限之下（与原跳线接法同级裕量，非新引入缺陷），spec-clean 需 PMOS 抬到 5 V，本批不做。
- **失败行为**：目标流 E2E 失效或 150 ms 无更新（继承 demo 看门狗）→ 目标置零减速停车。

> **落地状态（2026-09-27，V1.7）**：**闭环本体已落地**——`rt/servo.c/.h`（每侧 PI：前馈 + Kp + 抗饱和积分限幅 ±30% duty、输出钳位 ±1000 + 死区 0.5%、`ENCODER_getSpeeds` 换为 xcore 编码器快照的 percent×10 域）+ `rt/motor_algo.c` 接入（目标斜坡 → servo → BSP，急停/失联看门狗语义不变，新增 1 Hz `SRV=` 台架行）。与上图的两处**诚实偏差**：① 目标域暂为 percent×10（§11 物理运动学未标定，CPU0 只产百分比），mm/s 仅经 `ENCODER_FULL_SCALE_MM_S` 出现在遥测；② 增益 `Kp=0.8 / Ki=0.01/ms / FF_GAIN=1.0` 是"近似线性被控对象"的**待标定初值**，台架按 `SRV=` 行整定（§15.3）。**逐电机 jog（V1.10 新增，34 §9.3）**：`0x71 MOTOR_JOG {motor, duty}` 经 xcore `Jog` 块进 CPU1，优先级 **急停 > 标定 > jog > servo**（`MOTOR_ALGO_task` 内即此次序），duty 钳 ±500、**受故障锁存门禁、不发心跳**，`jogSeq` 每次刷新续期、**300 ms 无刷新自动归零并 `SERVO_reset` + `MOTOR_stopAll`**（防页面掉线后推杆僵在高位）；jog 是纯开环，只用于台架单轮转向核对。**判向结果回传 + 自动持久化**：标定结束（DONE / ABORT / **BUSY 拒绝且不重启**）由 CPU1 发布 `CalibResult`，CPU0 组 EVT 0x22（含 `saved`）经 xcore 出站队列由 CPU2 发送，DONE 时自动写 DFlash（34 §3/§8.3/§11.2 D1）。**闭环安全次序（硬性门禁）**：首次闭环前必须先执行 §5.1 的 `0x70` 自动判向并核对 `ENCCAL=` 结果——某轮符号错的闭环是**正反馈**（越推越快），判向先于闭环，不是建议。**V1.11 起该门禁由固件强制执行**（34 §13 闭环使能门 `g_closedLoopOk`）：标定记录 `src=0`（首次烧录/校验失败回落/0x74 清记录）时 CPU1 把 `SERVO_update` 的 `measValid` 钳 FALSE，走既有失活兜底（duty=目标、清积分）——"编码器已接线但方向未标定"不再可能闭环跑飞（原点回中电机仍转现象的根治）；0x70 DONE（CPU0 取结果即翻转 RecordLive version，DFlash 物理写是延迟队列）/0x73 REC_SET 自动开门，0x74 关门，翻变各打一行 `SERVO open-loop (record src=0; calibrate via 0x70)` / `SERVO closed-loop enabled`。**未做**：motor_guard 全部保护项（堵转/滑差/欠压；STBY 的"上电使能 + 未跑固件即断电"已随 23 V1.23 落进 `bsp/motor.c`，**故障/急停拉低仍未做**，见上一条）、双斜率两参数、产测在线整定与回存。

### 5.3 app/mission —— 运行模式状态机（CPU0，替代并扩展 robot.c）

```
        上电 POST 通过
BOOT ───────────────► STANDBY ──配对成功──► NORMAL ◄──► FACTORY(产测)
                        │                    │  ▲  │
                        │ POST失败            │  └──┴── OTA(传输/切换)
                        ▼                    ▼
                      FAULT ◄──任意模式── 故障触发(ERR_*)
                                          FAULT ──按键/命令确认清除──► STANDBY
```

- **BOOT**：POST（§8.3）不过 → FAULT 并闪烁错误码；通过 → STANDBY（电机许可关闭，网络可连）。
- **NORMAL**：接收 drive 命令（需已配对会话）；心跳超时/断链 → 目标置零（状态回 STANDBY，**不锁死**，区别于急停）；急停 → FAULT 锁存（修复 demo P0-3：心跳超时同样**锁存** `ERR_LINK_TIMEOUT`，运动许可随许可位一起失效）。
- **急停语义修正（吸收评估报告 P0-1）**：急停命令 = xcore 队列 + CPU1 旁路位双路；**旁路位只能由 FAULT 清除流程边沿复位**，CPU0 周期任务不再无条件清除；CPU0 见到旁路位为真即使队列丢失也自行锁存 FAULT。
- **FACTORY**：仅产测命令可进入（§10），出厂后烧保險丝位禁止。

### 5.4 app/config、app/diag、app/telemetry、app/post

- **config**：§4.3 模型的读写服务；接口 `CFG_get()/CFG_set()/CFG_commit()`；所有**用户配置**写路径统一走此模块。**DFlash0 现有第二个写者**：标定记录由 `mw/calib/calib_store.c` 独占 **扇区 15（0xAF01E000）**（34 §8），§4.3 的页分配必须避开该扇区；两模块共用同一"喂狗→关中断→擦→写→回读"序列，未来若加磨损均衡应下沉成公共层。
- **diag**：16 位错误码 `[module:4][severity:2][code:10]`；活动错误表 + 历史环形（DFlash 黑匣子：最近 16 条故障快照：错误码/时间戳/目标速度/实测速度/电池电压/复位原因）；接口 `DIAG_report()/DIAG_getSnapshot()/DIAG_export()`（网页下载 JSON）。
- **telemetry**：20 ms 聚合帧（§6.3），字段带序号；向下经 xcore 交 CPU2 封成 SF TYPE 0x02。
- **post**：上电自检（Flash 整 bank CRC32、栈水位初值、DFlash 双页校验、编码器静止漂移、GTM/ASCLIN/QSPI 寄存器回读、链路握手寄存器 `SF_READY` 魔数确认）+ 运行自检（每 10 s：栈水位、堆水位、任务调度抖动、LINK 误码率/SPI 事务超时计数）。POST 结果写入黑匣子并在 Web 端展示。

### 5.5 mw/xcore v2 —— 跨核消息

相对 demo 的升级点：

1. **每条消息带 E2E 头**：`{len, msgId, aliveCounter, crc8}`——aliveCounter 逐消息递增，接收方检测跳变（丢消息）、重复（stuck-at）；关键方向（CPU0→CPU1 目标、CPU2→CPU0 命令）强制。
2. **类型化消息表**替代裸结构体拷贝：`MSG_MOTOR_TARGET(8B) / MSG_TELEMETRY(32B) / MSG_CMD(20B) / MSG_LOG / MSG_LINKSTATE / MSG_FWCHUNK`，各方向独立队列深度与丢弃策略（命令不丢、遥测可丢旧）。
3. 锁策略保留单自旋锁（持有 <1 µs），但**头文件写死约束：仅任务/超循环上下文可用，ISR 禁用**（修复 demo P2-2/P2-3 的隐患，g_logWr 声明 volatile）。
4. 初始化时序继承 demo（任一核越过同步事件前完成清零）。

### 5.6 com/link —— SPI LINK 事务调度器（CPU2，替代 wifi_at.c）

- **职责**：以 QSPI3 主机身份驱动 §3.7 的事务模型，把半双工 DMA 段装配成 SF 帧流；对上只暴露少量入口（当前实现：`LINK_init / LINK_main / LINK_send / LINK_sendTelemetry / LINK_isUp / LINK_getHealth`，加控制面的 `LINK_gen / LINK_setClock`）。帧类型走 SF TYPE + `PROTO_CMD_*` 语义，命令入队前经过 §6.1c 的三层白名单，细则见 [22-link-spi-design.md](22-link-spi-design.md) §5.2/§5.5。
- **一个链路周期**（超循环）：① **单读**握手寄存器组（一次 `RDBUF` 块读 24 B，**不做连读比对**，V1.6 修订见上）→ ② `SF_READY` 魔数不对 → **静默 `LINK_DOWN`**（禁数据事务，下一好读即恢复，不判失联）→ ③ `SF_TX_PENDING>0` 则发 `RDDMA` 段读突发（越 `LINK_RX_BURST_MAX=640` 即钳位；单段 ≤260 B、4 字节对齐）取段、解 SF 帧入命令队列，**突发结束后补一次 `INT0` 事务**（E12）→ ④ 有待发帧且 `SF_RX_ROOM` 足够则发 `WRDMA` 段写突发（越 `LINK_RX_ROOM_MAX=1024` 即钳位），**结束后补 `WR_END`** → ⑤ 残帧超时（4 ms）回收 + 错误统计。事务不完整只计 `spiErrors` 并跳过本周期，**不对从机状态下结论**。
- **握手取法：P23.0 电平轮询，不是中断**。TC275 无 ERU、P23.x 不在 IOM 监视输入内，P23.0 **产生不出边沿中断**（[22 §2 E11](22-link-spi-design.md)），原稿"IRQ 置事件位唤醒泵"作废：泵每圈 `IfxPort_getPinState` 采电平，高即开读事务，低时按 2 ms 保活节拍轮询。代价已计入 §14 时延预算（命令下行 ≈2.3 ms）。QSPI3 自身的 TX/RX/ER 三个 ISR **必须声明在 0 号向量表**（§18 C1）。
- **丢弃策略**：命令/ACK 类不丢（队列满即反压并上报 `ERR_LINK_OVF`）；遥测/日志类丢旧留新。
- **链路健康判定（V1.6 收紧为单判据）**：**唯一**失联判据 = `SF_ALIVE` 500 ms 未推进 → `ERR_LINK_LOST`（目标置零受控停车；电机停止另由 CPU0 的 100 ms 心跳看门狗兜底）。事务超时、CRC 失败、READY 魔数错**一律只计数、不判死**——单读快照下撕裂值是常态，把它们升级成失联就是"越忙越断连"的根因。READY 重新出现的沿只做 `SF_parserInit` 重锁 RX（本机 TX SEQ 保持连续，从机按 SEQ 重锁自愈）；**不再发 `GEN RESET_LINK` 握手**——从机不回执时该握手会无限占住泵（状态 READY 却永不行数据事务），`LINK_gen()` 保留作台架/诊断入口。心跳帧 TYPE 0x04 兼作 RTT 测量，主机在事务边界取时间戳（比 UART 时代更准）。
- **时钟档位管理**：1/2/5/10/20 MHz 固定档位，由产测/诊断命令切换并持久化到 DFlash1；**无运行时自适应降速**（UART 的 0x44 BAUD 协商概念作废）。切换入口 `LINK_setClock(tier)`：先改本地 QSPI3，再镜像一发 `GEN CLOCK_SET` 让从机记账（payload 单位 **MHz**，从机只存诊断不回读，[22 §4.3 + E14](22-link-spi-design.md)）。**DFlash1 持久化尚未做**，掉电后回到构建默认档。
- 帧格式与命令表见 §6；鉴权见 §7；波形兼容性门禁见 §16 R7 与 [22-link-spi-design.md](../20-design/22-link-spi-design.md) §8。
- **回退通道开关（2026-09-26 决策：UART 链路弃用，SPI 为唯一板间链路）**：`USE_SPI_LINK` 已在 `.cproject` 的**两个 TASKING 构建配置**（Debug/Release）中定义，日常构建产出的就是 SF-over-SPI 路径；`wifi_at.c` + ASCLIN1 UART 分支**不再是任何配置的默认**，只在手动从 `-D` 列表里删掉 `USE_SPI_LINK` 时才参与编译，其定位相应降为两件事——① G1 波形门禁失败时的**应急返修通道**（删符号 + 重刷 esp-at 镜像，代价一次刷机），② C6 侧**调试控制台**（`23 §2`）。G1 之后按计划把开关极性翻正为 `USE_WIFI_AT`（默认 SPI、UART 需显式开启），届时本节口径无需再改。**GCC 两个配置未加该符号**，本项目不以 GCC 构建；若有人用 GCC 配置出镜像，得到的是已弃用的 UART 分支，须自行核对。

---

## 6. 通信协议（两段两帧：LINK 段 SF 帧 + 手机段 v2 帧）

链路换向 SPI 后，"一条帧协议打通两端"的前提不再成立（半双工事务、4 字节段对齐、寄存器握手是 SPI 侧硬约束）。V1.2 起**协议分层为两段**：手机 WS 保持 v2 帧（对外兼容），板间 LINK 用 SF 帧；C6 的 bridge 做字段级映射（§6.1c）。

### 6.1a LINK 段：SF 帧（SPI 专用）

```
| 0x5A(MAGIC) | VER(0x01) | TYPE | SEQ | FLAGS | LEN(u16 LE) | CID | 载荷[LEN] | CRC16-CCITT-FALSE(2) |
   偏移 0        1         2      3     4       5-6           7     8..          末 2 字节
FLAGS: bit0=FRAG(段内后续还有分片)  bit1=FRAG_END(本帧末片)  bit2..7=保留，必须发 0
段对齐：整帧含 CRC 末尾补 0x00 到 4 的倍数；一个数据段可串接多帧，帧边界由 LEN 递推。LEN ≤ 248。
```

- 继承 v2 的语义要素（MAGIC/VER/TYPE/LEN/CRC16/SEQ），新增的是 SPI 专属的 4 B 对齐、段内多帧与 FRAG 位；
- **ACK 不是 FLAGS 位**：确认是 `TYPE=0x03` 的一种帧（22 §5.2）。`bit1` 归分片段组（`FRAG_END`），`bit2..7` 保留且必须为 0——FLAGS 参与 CRC 覆盖，自造一位只会换来一次格式错。此处 V1.2 原稿把 bit1 写成 `ACK`、bit2 写成 `RESYNC`，与已烧录的从机 `c6_sf/sf_frame.h` 冲突，2026-09-26 按从机更正（22 §2 E13）；
- TYPE/CID 通道表见 [22-link-spi-design.md](../20-design/22-link-spi-design.md) §5.2（0x01 CMD / 0x02 TEL / 0x03 ACK / 0x04 HBT / 0x05 EVT / 0x06-0x07 OTA / 0x08 DBG / 0x0F VND）；
- **SEQ 为 1 字节、每方向独立、严格前进窗口 `1 ≤ seq-last ≤ 32`**；命令方向旧 SEQ 直接拒收（重放防护）；**越窗重锁（V1.6）**：连续 `SF_SEQ_RELOCK_RUN`(8) 帧越窗即判定"对端已重启、SEQ 不会自己回来"，丢弃窗口、下一帧重锁——对端复位后最坏拒收 8 帧（若不重锁，最多拒收到 u8 回绕 ≈224 帧/7.5 s，即"复位后连不上"的另一半根因）；
- 握手帧交换 `{protoVer, fwVer, boardId, capabilities}` 走 SF TYPE 0x01，语义与 §6.1b 相同，不匹配则降级或拒绝。

### 6.1b 手机段：v2 帧（对外不变）

```
| AA | 55 | VER(1) | CMD(1) | SEQ(1) | LEN(1) | DATA[LEN] | CRC16-CCITT(2) |
```

相对 demo（异或 CRC、无 SEQ/VER）的变更：CRC16 防突发误码；SEQ 供上层 E2E 与重放防护；VER 支持版本协商。`LEN ≤ 64`。

### 6.1c v2 ↔ SF 映射（C6 bridge 执行）

| v2 CMD（手机段） | SF TYPE/CID（LINK 段） | 变化 |
|---|---|---|
| 0x01..0x09 方向档 / 0x20 GET_STATUS / 0x21 HBT / 0x30..0x32 复位·清障·急停 | 0x01 / 0x01 | `{u8 op, i16 0, i16 0}`，**固定 LEN=5**；v/w 恒 0 |
| 0x10 SET_SPEED | 0x01 / 0x01 | `{op, v, w}`，v/w 是 **±100 百分比**（§6.2）；TC275 窄化成 2 字节 `sint8` 复用旧命令容器，无损 |
| 0x50 DRIVE | 0x01 / 0x01 | `{op, v:i16 mm/s, ω:i16}`；TC275 混控成左右轮百分比按 SET_SPEED 入队（22 §5.5：600 mm/s / 300 deg/s ≙ ±100% 摇杆满偏锚点，§11 标定后替换），且**兼作心跳**——CPU0 对 STOP/方向档/SET_SPEED 一律喂 `ROBOT_cmdHeartbeat`，否则 100 ms 看门狗会在 30 Hz 摇杆流的命令间隙不停清零轮速 |
| 0x41 遥测 | 0x02 / 0x10 | **38 B 逐字节不变**（从机复用 v2 解码器，不复建表，§6.3）；字段名保留 `linkErrRate`，语义 = SPI 侧 `(crcErr+seqErr)`/已收帧，单位 0.1% |
| 0x52 配置 | 0x01 / **0x02** | 载荷**原样**、op 已被从机剥掉 → 首字节是键号**不是命令码** |
| 0x51 配对 | 0x01 / **0x05** | 载荷**原样**（`token[16]`）→ 首字节是 token **不是命令码** |
| 0x53 诊断 / 0x42 LINK_STATE | 0x01 / **0x03** | `{u8 op, ...}`，op 保留 |
| 0x44 BAUD REQ/ACK/NAK | **删除** | UART 专有概念；SPI 时钟档位改由 SF `GEN` 事务 + 产测/诊断命令控制，不做运行时自适应 |
| 0x60..0x6F OTA | 0x06/0x07 | CHUNK 载荷上限 **62 B → 240 B**（1 MB 镜像分片数 16k → 4.3k）；CID 非连续：`0x30 BEGIN/0x31 CHUNK/0x35 ABORT` 在 0x06，`0x32 ACK/0x33 STATUS/0x34 SWAP` 在 0x07 |
| 0x70..0x7F 产测 | 0x01 / 0x04 | `{u8 op, ...}`，op 保留 |

**TC275 侧消费规则（2026-09-26 定，`com/link.c:link_dispatch`）**：上表五条 CMD 通道**形状不统一**，所以不存在"CMD 帧的 `payload[0]` 就是命令字节"这种通则——照它实现会把配置键号或配对 token 当命令真的执行。现行是 TYPE→CID→长度**三层白名单**，任何一层不过就整帧拒收并计数，绝不按偏移猜；细则与依据见 [22-link-spi-design.md](../20-design/22-link-spi-design.md) §5.2/§5.5。

### 6.2 命令表（v2 段，0x01–0x32 语义与 demo 兼容；LINK 段经 §6.1c 映射为 SF TYPE/CID）

| CMD | 名称 | 载荷 | 说明 |
|---|---|---|---|
| 0x02/0x03/0x04/0x05 | 方向档 | — | **语义修正**：LEFT/RIGHT=差速转 `(0,S)/(S,0)`，ROTATE=原地旋（修复 demo P1-1） |
| 0x10 | SET_SPEED | s8 或 s16×2 | 钳位 ±100（修复 P1-3） |
| 0x50 | DRIVE | v:i16, ω:i16 | 摇杆驾驶流（§11），兼作心跳 |
| 0x51 | PAIR_REQ/CONFIRM | token:16 | 配对（需车侧物理按键确认，§7.2） |
| 0x52 | CFG_GET/SET/COMMIT | k,v | 用户配置 |
| 0x53 | DIAG_READ | 类型 | 活动错误/黑匣子/统计 |
| 0x60–0x6F | OTA 帧组 | §9 | BEGIN/CHUNK/END/STATUS/SWAP；**CHUNK 载荷经 SF 段提升到 ≤240 B**（v2 段仍 ≤64 B，C6 侧聚合后下发，§6.1c） |
| 0x70–0x7F | 产测帧组 | §10 | ENTER/LED/MOTOR_RUN/ENC_READ/CAL/CAL_SAVE/SN_WRITE/AGING/REPORT（**该名字表是 C6 侧遗留口径**）。TC275 侧 V1.10 起 0x70~0x74 **已有新语义**：0x70 判向标定 / 0x71 MOTOR_JOG / 0x72 REC_GET / 0x73 REC_SET / 0x74 REC_CLEAR，真源 [34 §9.1](../30-tc275/34-calib-dpt.md)；同一字节两仓库两名，C6 只按 `0x70..0x79` 整段路由到 `CID_DPT`，**不得**再按旧名字发 0x71~0x74（34 §11.4 C4） |

### 6.3 遥测表（0x41 / SF `TYPE=0x02 CID=0x10`，20 ms 周期，**固定 38 字节**）

偏移即契约：小端、**无对齐填充**（偏移 9/11/…/33 是刻意非对齐的）。真源三处必须同步——本表、`mw/sf/sf_telemetry.h`、从机 `proto_frames.c:proto_telemetry_*`，由 `test/host/test_sf_telemetry.c` 编译从机源码做双向交叉锁死。

| 偏移 | 字段 | 类型 | 说明 | TC275 现状 |
|---|---|---|---|---|
| 0 | seq | u32 | E2E 计数，严格前进 | ✅ 泵自增 |
| 4 | uptime | u32 | 运行时间 ms | ✅ |
| 8 | state | u8 | mission 状态（§6.2 状态码） | ✅ |
| 9 | faultCode | u16 | 活动最高级错误 | ✅ |
| 11 | vTarget L/R | i16 ×2 | mm/s，目标 | ✅（V1.7）：CPU0 目标 percent×10 经 `ENCODER_FULL_SCALE_MM_S` 换算，与 `vMeas` 共用同一标定常数（伺服闭环的作用对象） |
| 15 | vMeas L/R | i16 ×2 | mm/s，实测 | ✅ CPU1 编码器（§5.1）经 xcore 物理域 |
| 19 | battery_mV | u16 | 实测电压 | ✅（V1.9）：CPU1 VADC G0 CH4（D24A J6-1 分压 **VIN/11** → kit **X2-23/AN4**，23 §5.5），100 Hz + 1/16 EMA，经 xcore 电池块 |
| 21 | battPct | u8 | 估算电量 | ✅（V1.9）：按 `ADC_BATT_CELLS=3` 每节 3.3~4.2 V 估算——**节数 2S/3S 待硬件确认**（§16 Q1 配置化），mV 是测量、pct 是显示 |
| 22 | odoSession | u32 | 本次里程 mm | ✅ 左右侧里程的无溢出平均（编码器，§5.1） |
| 26 | odoTotal | u32 | 累计里程 mm | ⚠️ 同上（且需 DFlash 持久化，§4.3） |
| 30 | linkRtt | u16 | SF HBT 事务边界 RTT ms | ⚠️ 填 0：HBT 未打点 |
| 32 | linkErrRate | u8 | **0.1% 单位**，SPI 侧 `(crcErr+seqErr)`/已收帧 | ✅ |
| 33 | fwVer | u32 | `0x00MMmmpp` | ✅ |
| 37 | hwRev | u8 | 板级标识 | ⚠️ 填 0：来源未定 |

- **长度是硬约束，不是"字段可缺省"**：从机对 TEL 帧的接受条件是 CID 正确**且 `LEN ≥ 38`**（`c6_link/link.c:sf_to_v2`），少一个字节就是**整帧丢弃、手机页面全无数据**。V1.0 的做法是缺来源的字段**显式写 0**（页面显示 0 是如实），绝不为凑长度省字段或发短帧。
- `linkRtt/linkErrRate` 语义已从 UART 时代的波特率链路指标改为 SPI 事务口径；`spiErrStat` 这个改名**未采用**，保持与 v2 解码器同名，避免两侧各一张表。

### 6.4 错误码初始表（`[module:4][sev:2][code:10]`，摘要）

| 错误 | Sev | 响应 |
|---|---|---|
| ERR_STALL 堵转 | 3（致命） | 安全态 + 锁存，需确认清除 |
| ERR_ENC_DEAD 编码器失效 | 3 | 安全态 + 锁存 |
| ERR_BATT_CRIT 欠压临界 | 3 | 受控停车 + 锁存 |
| ERR_ESTOP 急停 | 3 | 立即刹车 + 锁存 |
| ERR_LINK_TIMEOUT 失联/心跳 | 2 | 减速停车，恢复后自动清除（修复 demo P0-3 的锁存缺失） |
| ERR_BATT_LOW / ERR_SLIP / ERR_TEMP | 1 | 限幅/告警，不停车 |
| ERR_CFG_CRC / ERR_POST_x / ERR_OTA_x | 2–3 | 对应恢复流程 |

---

## 7. 安全架构（Safety）

### 7.1 故障模型与 FMEA 摘要

| 故障 | 检测 | 响应 | 时间 |
|---|---|---|---|
| CPU1 挂死 | CPU1 看门狗 | 硬复位（boot 后 POST） | ≤200 ms；复位期间 STBY 由 GPIO 高阻 + TB6612 片内 200 kΩ 下拉落回 standby（V1.15，见下方口径注） |
| CPU0 挂死 | CPU0 看门狗（SM 看门狗） | 安全复位 | ≤200 ms |
| CPU2 挂死 | CPU2 看门狗 | 复位 CPU2；LINK 断 → 停车不复位整车 | ≤200 ms |
| C6 死机/断链 | **唯一判据**：握手寄存器 `SF_ALIVE` 500 ms 不推进（事务失败/魔数错不判死，V1.6） | 目标置零受控停车 | ≤520 ms |
| SPI 事务毛刺/总线电气失步 | 单读快照异常：READY 魔数不对、事务不完整（`spiErrors` 计数） | 静默 `LINK_DOWN`（禁数据事务）或跳过本周期；下一好读即恢复，真失联由上一行兜底 | 即时恢复 |
| 目标消息丢失/重复/乱序 | E2E aliveCounter（xcore）+ SF/v2 SEQ 前进窗口 | 拒收该帧 + 计数；连续 8 帧越窗判对端已重启 → 丢窗重锁（`SF_SEQ_RELOCK_RUN`）；链路失联只由 `SF_ALIVE` 判定（V1.6） | 即时 |
| 帧误码 | CRC16 | 丢弃 | 即时 |
| 堵转/卡死 | 编码器 vs 目标 | `ERR_STALL` → STBY 安全态 | 500 ms |
| 5V 掉电/电压跌落 | EVRC brownout + ADC | TC275 复位至安全态；C6 brownout 自保护 | 硬件级 |
| PFlash 位翻转 | 整 bank CRC（POST）+ 常数段 CRC（RUNST） | 拒绝启动/进入 FAULT | 上电 / 10 s 周期 |

> **V1.8 口径更正（本表前两行；V1.13 再更正）**：CPU0/CPU1 看门狗现已启用，故"硬复位"这条路径在纸面上已经存在（≈**1.4 s**，非 ≤200 ms，见 §7.2；V1.8 曾误写 0.3~0.5 s）——但 V1.13 揭示它**到 tc275_car v1.1.2 才真正闭合**（WDT 到期只发 NMI，此前无复位钩子，到期即假死）。
> **V1.15 再更正（本表第 1 行的 STBY 假设）**：V1.13 写的"复位期间 STBY 上电默认拉低 → 电机断电**与实物不符**"在当时是对的——D24A 的 STBY 用跳线接板载 3V3 **常使能**，MCU 复位对它没有任何影响。现在 **STBY 已改 GPIO 受控（P22.2/X1-16 → J4-2，[23 §3.2](23-wiring.md) V1.23）**，且 TB6612 的 STBY（连同六根 IN/PWM）**内部都有 200 kΩ 下拉**（datasheet 引脚表），所以：MCU 复位/未烧录 → GPIO 高阻 → 片内下拉把 STBY 拉到 0 → standby、输出关断；即便 STBY 为高，浮空 IN 也被各自拉低，真值表 `L L / PWM=L / STBY=H` = **OFF（高阻）**。**纸面答案自此为"复位期间电机滑行、不会带着最后占空比继续转"**，但两点未结：① **实物尚未改线**（跳线帽未拔 = 常使能照旧，本条结论对旧接线不成立）；② 复位瞬间的实际波形/电机行为仍是 §7.2 台架项，实测记入 51-verification-log。另外本表"堵转 → STBY 安全态"与 §5.2 的"急停直接拉低"仍未接入（会与短接刹车互斥，见 §5.2 落地状态），**故障路径的"许可关闭"仍属 §17/§5.2 未做项**。

### 7.2 看门狗链（吸收并关闭评估报告 P0-4）

```
SM(安全)看门狗 ←── CPU0 mission 任务喂 (条件: CPU1状态seq在推进 && LINK健康位)
CPU1 看门狗   ←── 1kHz 环喂 (条件: 本环周期抖动 < 500µs && 编码器自检通过)
CPU2 看门狗   ←── 泵巡检喂 (条件: RX/TX 环未溢出)
链式含义: 任一核的"喂狗条件"不满足 = 该核功能已异常 → 对应复位路径, 而不是仅复位挂死的核
```

调试构建用 `DEBUG_WATCHDOG=0` 统一关闭（避免量产配置漂移，评估报告 P0-4 建议）。

> 该开关**尚未实现**，且 V1.8 的落地选择与之相反：**Debug 与 Release 都启用 CPU0/CPU1 看门狗**，调试期靠 TriCore OCD 在 halt 时挂起看门狗（不是关狗）来避免误咬。量产前若补 `DEBUG_WATCHDOG` 宏，必须同时保证"Release 强制 =1、不可配置漂移"，否则等于把这条底线又撤掉。

**demo 现状（V1.8，2026-09-27）**——上面三级链已通两级，且**与上文相反：CPU 看门狗现在是开的**：

| 看门狗 | 现状 | 喂狗点 / 不启用理由 |
|---|---|---|
| CPU1 | **已启用** | `rt/motor_algo.c:MOTOR_ALGO_run()` 1 kHz 循环内 `WDG_serviceCpu()`。这是"电机核跑飞 → 复位 → 上电回安全态"这条底线，评估报告 §6 第一条 🔴 就此关闭 |
| CPU0 | **已启用** | `Cpu0_Main.c:vRobotControlTask()` 10 ms 循环顶部 `WDG_serviceCpu()`；同时作为 FreeRTOS 栈溢出钩子的复位手段（见下） |
| SM(安全) | 有意关闭 | 其喂狗条件（CPU1 seq 推进 && LINK 健康位）是 §7.2 的**量产权项**，demo 尚无该判据；无条件开启只会变成"没喂就咬"的噪声源 |
| CPU2 | 有意关闭 | "只复位故障核"需 SMU 告警路由（§7.2 链式含义）；现在开 CPU2 WDT 的动作是**整机复位**——链路核挂死并不使电机失控（CPU1 有 150 ms 失联看门狗自行刹停），用整机复位响应它属于过度反应 |

**超时反应（V1.13，2026-10-03，"看门狗假死"根治）**：TC27x 的硬件事实是 **CPU 看门狗到期只向本核发 NMI（trap class 7），不会自行复位**——iLLD 默认的 NMI 钩子（`IfxCpu_Trap.c:IFX_CFG_CPU_TRAP_NMI_HOOK`）是空宏、直接返回，于是 V1.8 交付的"断喂 → 复位"链**从未真正闭合**：看门狗到期后核被 NMI 打死、复位永远不来，整车冻结而非回安全态（台架 2026-10-03 复现的"假死"）。修复：`Configurations/Ifx_Cfg.h` 定义 `IFX_CFG_CPU_TRAP_NMI_HOOK` → `IfxCpu_triggerSwReset()`（`SCU_SWRSTCON.SWRSTREQ=1`，无密码无 ENDINIT，NMI 上下文唯一安全的复位原语；钩子**不返回**）。NMI 上下文约束：不能打印（`blockingWrite` 等 TX ISR → 死锁）、不能走 xcore（自旋锁可能被被打断的上下文持有 → 死锁），只有寄存器写复位合法。三核 trap 表共用该钩子；实际只有 CPU0/CPU1 的 WDT 被武装，故实践中它就是"喂狗断流 → 整机软复位 → 上电安全态"的终点。**栈溢出钩子与排障手册的"断喂复位"叙事自此才真正成立**（V1.8 时是未闭合的假设）。待台架：拔喂狗复现实测复位窗口。

复位后的安全态由软件默认值保证，不依赖硬件：`MOTOR_init` duty=0 + `MOTOR_ALGO_init` 的 `MOTOR_stopAll()` + 零目标 + 应用层看门狗（CPU1 150 ms 失联 / CPU0 100 ms 心跳）。**V1.15 起还多一层硬件保证**：`MOTOR_init` 把 STBY 拉高是**最后一步**（此前 8 根 IN/PWM 已置低、duty=0），复位/未烧录期间 STBY 由 GPIO 高阻 + 片内 200 kΩ 下拉留在 standby ⇒ 驱动根本没解锁。**台架待验证项**：① **改线后的复位瞬间**实测——高阻期间 STBY 是否真被片内下拉拉低、电机是否只滑行（D24A 原为板载 3V3 常使能，23 §3.2；**改线未做前此项不适用**）；② 改线前后各跑一次"带占空比复位"，对比 §7.1 表第 1 行的纸面结论；实测结果记入 51-verification-log。

窗口值 `REL=0xF800`（`bsp/wdg.h`）按官方示例 `Watchdog_1_KIT_TC275_LK` 的 `0xE000`≈1.3 s **线性换算 ≈1.4 s**（0xF800 > 0xE000；V1.8 曾写"≈0.3~0.5 s"，与自己的换算锚点算术矛盾，V1.13 更正），**尚未台架实测**；实测后按 §7.1 目标收紧至 ≤200 ms——注意下限是 DF0 保存路径（34 §8.3：一次完整擦除窗口关中断不喂狗）的实测时长。调试器挂接时 TriCore OCD 会挂起看门狗，因此启用不影响 flash 调试，仅脱机运行才真复位。

栈溢出钩子（`Cpu0_Main.c:vApplicationStackOverflowHook`）从原先的静默 spin 改为：打印任务名 → `UART_flushPolling()` 轮询泵出 TX FIFO（钩子可能在 tick ISR 级运行，不能依赖 TX 中断）→ `__disable()` 冻结现场 → 空转，由已启用的 CPU0 看门狗在窗口内复位（V1.13 起复位链真正闭合：超时 → NMI 钩子 → 软复位，见上）。这是评估报告 §6 第二条 🔴 的落地。

### 7.3 POST / RUNST

上电：BMH 校验 → SBL 槽校验（CRC32 + 签名摘要）→ App POST（Flash CRC 增量校验 10 ms 级分片完成、DFlash 页、编码器漂移、外设寄存器回读、 栈油漆初值）。运行期每 10 s：栈/堆水位、任务抖动、LINK 误码率、DFlash 页 CRC。任一失败 → diag 记录并按严重度处置。

---

## 8. 信息安全（Security）

### 8.1 威胁模型（摘要）

| 资产 | 威胁 | 对策 |
|---|---|---|
| 控制权 | 陌生客户端连接并驾驶 | 每机唯一 SSID/密码（SN 派生随机，标签印刷）+ **配对 token（车侧物理按键确认）** + 会话 token（断线 30 s 后需重配对） |
| 重放 | 录制并重放驾驶帧 | 帧内 SEQ 单调 + 会话 nonce，旧 SEQ 拒收 |
| 固件被篡改/降级 | 恶意 OTA | ed25519 签名（公钥固化 SBL/NVS），版本回滚下限（anti-rollback 计数） |
| 调试口 | 现场读取固件/数据 | C6: secure boot v2 + flash 加密 + JTAG eFuse 熔断；TC275: DAP/调试口锁定（DMU） |
| 数据 | 用户里程/配置泄露 | NVS 加密；无云端时数据不出设备 |

### 8.2 配对流程（UX 与安全平衡）

首次连接 AP → 页面提示“按下车顶配对键 3 秒”→ 车侧生成一次性 token 经 LINK 广播 → 手机提交后成为**控制端**（此后免按键重连）；换手机/重置需再次按键。产线可用 DPT 批量预配对。

---

## 9. OTA 与版本管理

### 9.1 版本规范

`MAJOR.MINOR.PATCH+buildhash`，协议 `protoVer` 独立编号；握手帧互报，兼容矩阵在发布说明维护；App/网页显示双板版本。

### 9.2 TC275 OTA 流程（PFlash 双 bank）

1. CPU2 `fw_stream` 经 LINK 接收分片（SF TYPE 0x06，CHUNK ≤240 B；SPI 5 MHz 有效吞吐 ≈ 550 KB/s，1 MB 镜像 ≈ 2 s），边收边写 **PFlash1**（擦写例程驻 PSPR，CPU2 独占执行；期间 CPU0/CPU1 不访问 PFlash1）；
2. 全量后 CRC32 + 签名验证（ed25519 验签在 CPU0 空闲时执行，~100 ms 级）；
3. `CFG` 写槽选择标志 + boot_counter=3 → 软复位；
4. **SBL**：校验新槽（CRC+签名）→ 跳转；App 启动后 30 s 内向 DFlash 写 `boot_confirm`，否则 boot_counter-1 并回落旧槽（三次失败 → 标记坏槽，锁定旧槽并上报 `ERR_OTA_ROLLBACK`）；
5. 断电任意时刻：SBL 都能凭标志回到"最近确认的好槽"。

### 9.3 C6 OTA 与 TC275 固件的来源

路线 A（V1.0）：手机 WS 上传两板固件包（`.tar: c6.bin + tc275.bin`）→ C6 自身走 esp_ota A/B；TC275 包由 C6 经 LINK 流式转发。路线 B（V1.1）：C6 联网（STA 模式）从固件服务器拉取，支持批量车队升级。两条路线共用签名验签与回滚机制。

---

## 10. 可生产性（产测与标定）

### 10.1 进入方式与通道

整机装配后，治具通过 **BLE**（或产线 Wi-Fi）连接；上电后 3 s 内发 `DPT_ENTER` + 治具令牌（或检测到专用治具跳线帽）进入产测模式；产测模式中运动命令需治具令牌签名，防产线程序泄露后被滥用。

### 10.2 产测序列（目标 ≤ 90 s/台）

| 步 | 项 | 方法 | 判据 |
|---|---|---|---|
| 1 | 版本/身份 | 读 fwVer/hwRev/SN | 与工单一致 |
| 2 | 电池电压 | ADC 读数 vs 台表 | 误差 <2% |
| 3 | **电机-编码器自动校验**（霍尔编码器带来的关键能力） | 逐台电机的驱动 + 计数判定：正转 1 s → 读 `ENCODER_rawCounts`，反转 1 s | 四台计数 > 阈值、极性与 PWM 方向一致、无 `ERR_ENC_DEAD` → **自动完成接线判向，替代人工目检** |
| 4 | 速度标定 | 每侧恒 duty 20%/40% 两点运行 2 s，编码器测速 | 拟合 `motorGain[4]`，回存 DFlash1 |
| 5 | 直行配平 | duty=30% 直行 2 m 计数差 | 写 `steerTrim` |
| 6 | 急停/安全链 | 治具发急停 → 测 STBY 电平与 PWM；屏蔽 LINK → 测失联停车时间 | 停车 ≤100 ms / ≤520 ms |
| 7 | 老化模式 | 30 min 变速循环 + 温度/电压监控，看门狗复位计数 | 0 意外复位 |
| 8 | SN/密钥写入 + 报告 | 写 SN、Wi-Fi 密钥、配对密钥；下载 JSON 报告 | 报告入 MES |

### 10.3 标定数据有效性

`motorGain/steerTrim/battAdcK` 写 DFlash1 出厂区（带 CRC + 版本），用户区配置不可覆盖；维修换电机后可重跑步骤 3–5 单独刷新。

---

## 11. 用户体验设计（量产版）

继承交互方案全部结论（摇杆驾驶流、50 Hz 仪表、Captive Portal、多客户端、RTT 显示、断链即停），量产级增补：

1. **首次开机向导**：连 AP → 自动弹页（Portal）→ 引导按键配对 → 命名车辆 → 完成；全程无 App 安装。
2. **控制端 UI**：摇杆 + 实测/目标双速度条 + 电量环 + 里程 + 状态灯；故障以**人话**呈现（“电机堵转，已断电保护”+ 错误码 + “导出日志”），并给出清除指引。
3. **OTA UI**：一键检查（路线 B 联网后）→ 进度条 → “更新中请勿断电”，失败自动回滚提示。
4. **低电量策略**：20% 页面横幅 + 车灯慢闪；10% 受控减速停车并锁存，需充电后解锁——避免用户在远端把电池跑空。
5. **多语言**：`config.lang`，页面资源按语言包加载。
6. **可选升级项（V1.1）**：BLE 近场配置/固件（免 Wi-Fi 产线与售后）、车队云端看板。

---

## 12. 可靠性与环境

- **复位原因分类**（STM/SCU 复位状态寄存器）：上电/看门狗/软复位/掉电——黑匣子记录并区分“用户断电”与“异常复位”，老化判据只看后者。
- **掉电安全**：配置双页提交（§4.3）；OTA 任意断点可回滚（§9.2）；运动数据不持久化（安全态不依赖存储）。
- **EMC 软件配合**：SF/v2 帧 CRC16 + 重传由上层语义兜底，SPI 时钟档位以 5 MHz 为 EMC 预扫基线（更高档需重扫）；LINK 线束定长 + SCLK 就近共地回流；编码器输入滤波；PWM 频率 20 kHz 避开可听频段（硬件走线评审项）。
- **温度**：预留 NTC 通道，过温降额 50% → `ERR_TEMP`。

---

## 13. 质量工程

| 项 | 方案 | 门限 |
|---|---|---|
| 编码规范 | MISRA C:2012 必要子集 + 工程附加规则（禁动态内存/禁跨核直接调用/单返回点宽松版） | 静态分析 0 违规（分级豁免走评审） |
| 静态分析 | TASKING 编译器最高告警 + PC-lint/clang-tidy（主机端） | 每次提交 |
| 单元测试 | 主机端 Unity：`sf` + `proto`（各含模糊测试：随机字节流 10⁷ 帧不死机不越界；`sf` 额外覆盖 4 B 补齐、段内多帧、FRAG 残段超时）、`mission` 状态机全迁移覆盖、`servo` 数值域、`config` 双页提交（含掉电注入点）、`xcore` E2E | 行覆盖 ≥80%，sf/proto/mission ≥95%，MC/DC 对安全模块 |
| HIL 台架 | 第二块 TC275 作激励/采集（编码器仿真、急停注入、LINK 事务超时与断 IRQ/断 CS 注入），跑回归脚本 | 每夜执行 |
| CI | ADS headless 构建（Debug/Release）+ 主机单测 + 静态分析 + 双板固件包产物归档（版本号+hash） | 合并门禁 |
| 发布检查单 | POST/看门狗开启核对、签名公钥核对、兼容矩阵、产测程序同步更新、回退方案 | 评审签发 |
| 缺陷追溯 | 错误码/需求编号/用例 ID 三向映射表随库维护 | — |

---

## 14. 资源预算汇总（V1.0 目标态）

| 资源 | TC275 | ESP32-C6 |
|---|---|---|
| CPU | CPU0 ~15% / CPU1 ~20%（4 路编码器 + 2×PI）/ CPU2 ~10% | ~15%（HTTPD+3 WS+bridge） |
| RAM | CPU0: 堆 48 KB + 栈 24 KB；CPU1 <4 KB；CPU2 <16 KB | ~120 KB（含 WS 会话） |
| Flash | App ≤1 MB ×2 槽（4 MB bank 用满一半）| 固件 ~1.2 MB ×2 + 资产 0.5 MB（8 MB 内余量充足） |
| 链路带宽 | LINK = SPI @5 MHz，有效吞吐 ≈ 550 KB/s；稳态占用 **<1%**（命令 30 Hz×16 B + 遥测 50 Hz×44 B ≈ 2.7 KB/s），OTA 突发时占满 | Wi-Fi 有效吞吐 >10 Mbps（OTA 下载走它） |
| 关键时延 | 命令 ≤50 ms（其中板间段 ≤2.3 ms，含 2 ms 保活轮询兜底）；断链停车 ≤520 ms；急停旁路 ≤5 ms | 转发 <1 ms |

---

## 15. 开发里程碑

| 里程碑 | 内容 | 验收门（节选） |
|---|---|---|
| **M0 量产基线**（1 周） | demo 安全 P0 修复（看门狗链/急停竞态/心跳锁存/CPU2 栈）、仓库瘦身、CI headless 构建、主机单测框架。**看门狗链进度（V1.8）：CPU0/CPU1 已启用 + 无条件周期喂狗；SM/CPU2 仍有意关闭，"带条件喂狗"与 STBY 硬线待补** | 评估报告 P0 清零；CI 绿 |
| **M1 架构重构**（2 周） | §3 目录结构落地、xcore v2 + E2E、SF + v2 双段协议、config/diag/telemetry、C6 固件骨架 + **SPI 链路打通（R7/G1 波形兼容为 M1 入口门禁）** | 主机单测达标；G1 通过；1/2/5 MHz 各档 CRC 误码 0（30 min/档） |
| **M2 实时闭环**（2 周） | 编码器 ×4 解码（**解码层已实现** `rt/encoder.c`，原 `Bsp/encoder.c`，TIEM 边沿中断 + 软件正交，见 §16 R2）、速度 PI + 双斜率、堵转/滑差/欠压保护、STBY 硬线、电池 ADC | 坡道/负载速度误差 <5%；堵转 500 ms 进安全态 |
| **M3 OTA + 安全 + 产测**（2 周） | SBL + 双 bank 切换、双板 OTA、签名/secure boot、配对、产测序列 + 标定 + SN | 断电回滚 100/100；产测 ≤90 s/台 |
| **M4 试产验证**（2 周） | HIL 回归、30 台试产、老化、EMC 预扫、制造/维修文档 | 72 h 老化 0 异常复位；试产直通率 ≥95% |

---

## 16. 风险与开放问题

| # | 事项 | 影响 | 处置 |
|---|---|---|---|
| R1 | 霍尔信号电平/开漏上拉与 3.3 V 兼容性未确认 | 编码器不可用 | EE 评审第 1 项；`encoder.h` 接口已隔离解码方案；实测 E1A 静态 3.3 V 可直连（23-wiring §8.1） |
| R2 | ~~GPT12 四块引脚与现有 PWM/DIR/UART 冲突~~ → **已定案并实现**：GPT12 增量口与 ERU 输入均不在引出脚，GTM gen3 TIM 无 UDC，最终为 **GTM0 TIM 八通道 TIEM 双边沿中断 + 软件 ×4 正交，P33.0~P33.7 = X2-28~35**（纠错依据 23-wiring §8.3） | 闭环测速实现路径（`rt/encoder.c` 已落地） | 接线按 23-wiring.md §8.2 实施后，按 §8.4 台架判向 |
| R3 | PFlash1 擦写期间 CPU0 取指抖动 | OTA 时控制周期抖动 | 擦写全程仅 CPU2、例程驻 PSPR；OTA 中限速 50% |
| R4 | （已作废为量产风险）UART 通道在整机线束上的信号完整性 → **2026-09-26 起 UART 已弃用（§5.6 末条、§18 C15），本项只影响 C6 调试控制台与应急返修窗口** | 不影响量产链路 | 调试控制台固定 115200，不追求带宽；不作为业务通道设计 |
| R5 | ed25519/TLS 在 C6 上的资源 | 固件体积 | C6 侧 mbedTLS 成熟；TC275 只做 ed25519 验签（OTA 时执行） |
| R6 | 车队/云（路线 B）的服务端选型 | V1.1 范围 | V1.0 仅预留接口，不绑定 |
| **R7** | **主链路新风险（取代原 2 Mbps SI 项）**：AURIX QSPI 无命令/地址相位硬件概念，iLLD 主驱动亦无对应封装；用数据字节模拟 `CMD+ADDR+DUMMY` 能否被 Espressif `spi_slave_hd` 正确解析**未证** | SPI 方案根本可行性。**2026-09-26 起 UART 已弃用（§5.6 末条），G1 失败的退路代价变高** | **G1 门禁前置**（双 ESP32 抓官方例程参考波形 → TC275 复现比对）；退路 ①C6 自写寄存器级从机驱动（纯数据相位 + 固定段长）②回退 UART：删 `USE_SPI_LINK` 重编 + C6 重刷 esp-at（两侧都要动，代价一次刷机）并回写本文档 |
| R8 | 从机 RX 缓冲须 4 字节对齐、DMA 能力、前导位数为 8 的倍数 | 帧/段设计 | 已由 SF 帧"补 0 到 4 倍数 + LEN 只数字节载荷"吸收（§6.1a） |
| R9 | 握手寄存器逐字节搬运、读值非原子 | 主机误判待发长度 | **单读 + 消费端钳位容错**（V1.6 重写）：READY 魔数错 → 静默 LINK_DOWN 自愈；`TX_PENDING`/`RX_ROOM` 越界即钳位（640/1024）；判失联只由 `SF_ALIVE` 承担。原"连读两次相同 + 重试 3 次 + 不等判失联"**已废弃**——从机每段完成/10 ms 都改写寄存器，比对只会把撕裂读放大成"越忙越断连"（§5.6/§7.1/22 V1.6 §4.2） |
| R10 | C6 自身 OTA 写 flash 期间 SPI 从机是否掉事务 | OTA 过程链路抖动 | 台架实测；必要时写块间隙保活、OTA 期间限速 50%（对齐 R3） |
| R11 | IRQ 开漏 + 杜邦线长导致**电平读错**（握手是电平轮询，P23.x 无边沿中断能力，§5.6，依据见 [22 §2 E11](22-link-spi-design.md)）。**2026-09-26 实物确认加剧**：两板之间全部杜邦线直连、**无任何外部元件**，IRQ 高电平只由 TC275 **片内上拉**撑起（C6 侧显式关掉了内部上拉）——驱动阻抗高、边沿慢，真源与 coding 后果见 [23 §9.3](23-wiring.md) | 命令下行时延退化、偶发空读事务 | 2 ms 保活轮询兜底 + 寄存器**单读快照的消费端钳位**（V1.6 口径）；**判活只看 `SF_ALIVE`，禁止用 IRQ 电平判从机在位**；量产线束定长屏蔽 + 补外部上拉后才谈提速档 |
| Q1 | 电池节数与 BMS 断流阀值（影响欠压曲线） | 电压测量已落地（V1.9 `bsp/adc`，遥测 `batteryMv`）；**节数 2S/3S 仍待硬件确认**（`batteryPct` 暂按 3S 估算） | 配置项 `cellCount` 预留；确认节数后改 `bsp/adc.h` 的 `ADC_BATT_CELLS` 并回写本行 |
| Q2 | 灯效/蜂鸣硬件是否上（影响 UX 文案与引脚） | 待产品定义 | 配置驱动，默认无 |

---

## 17. 附录：demo 代码继承/废弃映射

| demo 文件 | 量产去向 |
|---|---|
| `App/robot.c` | 拆分：`app/mission`（状态机+许可）+ `app/drive_policy`（速度策略）；心跳锁存/急停竞态按 §5.3 修复 |
| `App/motor_algo.c` | 演进为 `rt/servo`（PI）+ `rt/motor_guard`；双斜率参数进配置 |
| `Middleware/xcore.c` | 升级 xcore v2（E2E 头、类型化消息），锁与日志环设计保留 |
| `Middleware/protocol.c` | 不再是 LINK 段**容器**真源：SF 编解码落地于 `mw/sf/`（§6.1a），主机单测对象。命令**码表**（`PROTO_CMD_*`，现 `mw/proto/protocol.h`）仍是唯一真源并被 `link.c` 直接复用为 SF 载荷首字节，被取代的只是 UART 时代的 `AA 55` 容器；`mw/proto` 只保留手机 WS 段 v2 帧（§6.1b，在 C6 侧实现） |
| `Middleware/wifi_at.c` | 现位于 `com/wifi_at.c`。已退出板间链路（**2026-09-26 决策：UART 弃用**，两个 TASKING 配置都定义 `USE_SPI_LINK`，§5.6 末条）；保留为**C6 调试控制台 + G1 失败应急返修 + 产线返工**通道，只在手动删除该符号时才编译；极性翻正为 `USE_WIFI_AT` 的动作仍排在 G1 之后；前端页面字符串迁移至 C6 assets |
| （新增，无 demo 对应） | 已落地：`com/link.c`（QSPI3 主机事务调度器，§5.6；V1.6 起为单读快照 + ALIVE 判活）、`com/spi_hal_pins.c`（引脚/时钟档/前导模拟）、`mw/sf/sf_frame.c`（SF 编解码，`test/host/test_sf.c` **2948** 断言通过，含 SEQ 越窗重锁 `test_seq_relock` 与 V1.10 的 DPT EVT/记录编解码 3 个用例组）、`mw/sf/sf_telemetry.c`（38 B 遥测 codec，`test/host/test_sf_telemetry.c` 跨侧模式 154 断言、无跨侧 116 项，含编译从机解码器的交叉验证，§6.3）、`rt/servo.c`（闭环速度 PI，§5.2 落地状态注记；motor_algo 自 V1.7 起为任务属主 + 保护钩位）。尚未落地：`com/auth`、`com/fw_stream`（OTA 走 SF TYPE 0x06/0x07，§9）、`rt/motor_guard`（堵转/滑差/欠压 + **STBY 的故障/急停拉低**，§5.2；STBY 的上电使能与 GPIO 受控本身已在 `bsp/motor.c` 落地，V1.15）；`rt/encoder` 解码层已落地（原 `Bsp/encoder.c`，判向已可运行时标定 + 0x70 自动判向），§5.1 的量产增量（自检、`ERR_ENC_DEAD` 联动、标定回存 DFlash）仍待做 |
| `Bsp/motor.c` | 保留，增加 STBY 控制与钳位职责确认。**STBY 控制已落地（V1.15）**：`MOTOR_STBY_PORT/PIN`（P22.2 → D24A J4-2，23 §3.2）+ `MOTOR_setEnabled(boolean)` + `MOTOR_init()` 末步使能；**未新开 `bsp/stby` 文件**（单路 GPIO 不值得一个模块，职责归电机 BSP 属主 CPU1），**故障/急停路径调用它仍未做**（§5.2 语义冲突待裁决） |
| `Bsp/uart.c` / `Bsp/stime.c` | 保留（console 归 diag；时基归 `rt/timebase` 并承担喂狗） |
| `Cpu0/1/2_Main.c` | 重写为 §3.4 初始化时序（POST → 任务创建 → 看门狗链启动） |
| FreeRtos/ 仓库膨胀、aws/ SDK | 仓库瘦身（评估报告 P3-1），仅保留 Kernel + Tasking 移植 |

---

## 18. 工程级实现约束（demo 实测得出，量产新增代码必须遵守）

以下条目不是设计选择而是**工具链/驱动的既成事实**，无法从规格推导，踩过一次代价极高。量产任何新模块（QSPI 段调度、GTM 编码器、C6 调试控制台串口）落地前先对照本表。

| # | 约束 | 后果与验证方式 |
|---|---|---|
| C1 | **向量表只有 0 号表生效**：lsl 中 `__INTTAB_CPU0/1/2` 同址，Tasking lsl 只收集 0 号表的 `IFX_INTERRUPT` 条目。CPU1/CPU2 的中断也必须写成 `IFX_INTERRUPT(fn, 0, prio)`，目标核由 SRC 的 `typeOfService = IfxSrc_Tos_cpuN` 决定 | 声明成 1/2 号表时 ISR 体被链接器按 unreferenced 删除，**链接与编译全部通过、中断永远不进**，表现为"收不到任何数据/节拍"。验证：查 `.map` 的 *Removed Sections* 里有无 `.*Isr.*` |
| C2 | **ISR 优先级是跨核共享的全局资源**，编号不得重复。当前占用：CPU0 = 1（FreeRTOS 上下文切换）/2（STM0 tick）/4·8·12（ASCLIN0 RX/TX/ER）；CPU2 = 5·7·13（ASCLIN1 RX/TX/ER）+ **6·9·10（QSPI3 TX/RX/ER，`com/spi_hal_pins.c` 已登记）**；CPU1 = **16~23（GTM TIM0 编码器八通道 NEWVAL，`rt/encoder.c` 已登记）** + **11·14·15（QSPI1 TX/RX/ER，`bsp/imu.c` 已登记，低于编码器块——轮沿优先于 FIFO 服务）** + **24（SCU ERU OGU0，IMU INT1 数据就绪计数，同文件；ISR 只做 `g_drdyCount++`，严禁在 ISR 里发起 SPI 事务）** | 新增中断（GTM TIM 溢出、G1 失败时的应急返修 UART）从余下档位取并在本行登记；抢同优先级会让两核互相吞中断。验证：查 `.map` 的 *Removed Sections* 无 `.*Isr.*`，且 SRC 表无重复优先级 |
| C3 | **调试串口只归 CPU0**：iLLD ASC 的软件 FIFO 与临界区仅在属主核内互斥 | 跨核直接 `printf` 会踩 FIFO 状态（偶发乱码/死循环）。CPU1/CPU2 日志统一走日志环（§5.5）由 CPU0 落串口 |
| C4 | **FreeRTOS API 只允许 CPU0 调用**：移植层的 tick（STM0）、上下文切换中断与 CCPN 屏蔽全部只绑 CPU0 | CPU1/CPU2 的时基直读 STM0 自由计数（unsigned 减法回绕安全），不经 OS 抽象 |
| C5 | **三核共用一个二进制**，启动期靠 `IfxCpu_emitEvent/waitEvent` 同步；共享数据必须在同步点**之前**由 CPU0 完成初始化 | 否则 CPU1/CPU2 可能读到未初始化锁/队列。量产初始化时序（§3.4）保留该前置条件 |
| C6 | TC275 无数据 Cache，跨核共享内存落在默认数据段（CPU0 DSPR）即可，**无需 Cache 维护** | 不要为跨核同步添加 `__sync()`/Cache 无效化代码，那是无效噪声 |
| C7 | **片选脚固定用 QSPI3 SLSO5 = P23.4，不得改用 SLSO7 = P33.7**（P33.0~P33.7 已被编码器八通道占满，见 [23-wiring.md](../20-design/23-wiring.md) §8/§9） | 改脚即与 E1B 短路，硬件级冲突 |
| C8 | 看门狗现状（V1.8 起）：**CPU0/CPU1 已启用并周期喂狗**（CPU1 1 kHz 环、CPU0 robot 任务 10 ms），SM 与 CPU2 看门狗**有意保持关闭**并各附理由（§7.2 表）。**超时反应已闭合（V1.13）**：TC27x 的 CPU WDT 到期只发 NMI 不自行复位，`Configurations/Ifx_Cfg.h` 的 `IFX_CFG_CPU_TRAP_NMI_HOOK` 接 `IfxCpu_triggerSwReset()` 兜底——没有这个钩子，看门狗到期 = 整车假死（iLLD 默认空钩子直接返回） | 新增核/新循环必须自带喂狗点，否则该核在 ~1.4 s 内（V1.13 更正：`REL=0xF800` > 官方锚点 `0xE000`≈1.3 s，旧稿"0.3~0.5 s"算术有误）触发 NMI→软复位。**喂狗必须走 `bsp/wdg.h` 的 `WDG_serviceCpu()`（clear+set 平衡对）**：`ENDINIT` 是**饱和计数器**，直接调 `IfxScuWdt_serviceCpuWatchdog()`（纯 set）每次给计数 +1，16 次后饱和在 15，此后单次 clear 再也解不开 ENDINIT → §9 的 OTA/Flash 解锁会静默卡死。**不得在 NMI 钩子里加打印/xcore 调用**（上下文约束见 §7.2）。SM 看门狗仍属 11-requirements.md F09 未完成项，交付前必须按 §7.2 补上 |
| C9 | **P23.x 上做不出 GPIO 边沿中断**：TC27x 的 GPIO 边沿事件只有 ERU 与 IOM 两条硬件通路，二者都不覆盖 P23.x（TC27D 无 ERU 模块目录、无 `IfxIom_PinMap.h`，iLLD `Iom/` 也不提供 `initRiseInterrupt/initFallInterrupt`）。证据：[22 §2 E11](22-link-spi-design.md) | 握手线只能**输入+内部上拉 + 电平轮询**。若按原稿去配 IOM/边沿中断，会浪费一轮调试才发现"中断永不触发"；同理任何"排针 GPIO 触发中断"的需求在 TC275 上都要先查这两条通路 |
| C10 | **`spi_slave_hd` 的 DMA 突发必须由额外事务收尾**：一次 `RDDMA` 突发要再发一帧 `INT0`(0x08)、一次 `WRDMA` 突发要再发一帧 `WR_END`(0x07)，从机才会计数完毕并释放缓冲。线上命令字节取自 `spi_ll.h` 的 `SPI_LL_BASE_CMD_HD_*`（`WRBUF 0x01 / RDBUF 0x02 / WRDMA 0x03 / RDDMA 0x04 / SEG_END 0x05 / EN_QPI 0x06 / WR_END 0x07 / INT0 0x08`）；`spi_types.h` 的 `BIT(n)` 是内部枚举，**照抄到线上必错**。证据：[22 §2 E12](22-link-spi-design.md) | 漏掉收尾事务 = 从机 TX/RX 槽位永久卡住，链路表现为"握手寄存器全零、只在第一次能通"。G1 台架若出现"能通一帧后死掉"，先查本条 |
| C11 | **跨侧协议常量必须"整表覆盖式"比对，并且用编译对方源码的测试锁死**。已烧录的一侧是事实真源；文档、注释、自己的记忆都不是。证据：[22 §2 E13](22-link-spi-design.md) | 2026-09-26 一轮比对查出三处同名不同义（`FLAGS bit1`、OTA CID 缺 `0x35`、`ERRSTAT` 整张位表），**抽查关心的项查不出来**。后果不是崩溃而是"错误位读反 / 台架上莫名格式错"，定位成本极高。验证：`test/host/test_sf_telemetry.c` 把从机 `proto_frames.c` 编进同一可执行文件双向交叉（154 断言）——改过任一侧的 `sf` 目录必须重跑 |
| C12 | **定长载荷"短一点"等于"全丢"**：从机对 TEL 帧要求 `CID` 正确**且 `LEN ≥ 38`**，不满足是整帧丢弃。缺来源的字段必须**显式写 0**，不允许发短帧或省字段（§6.3） | 主机初版按旧 UART 容器发 6 字节，编译、链接、发送全部"成功"，现象是手机页面**一个字段都没有**——短载荷不是"数据少"，是"没有数据"。同理任何按偏移解定的结构都禁止用 `struct` 直接 cast 字节（TriCore 大端 + 编译器填充） |
| C13 | **命令分派不得假设"首字节就是命令码"**：SF 的五条 CMD 通道载荷形状不统一，`CFG`/`PAIR` 首字节分别是键号与配对 token（§6.1c） | 按偏移猜会真的执行错命令（token 首字节 `0x20` → `GET_STATUS`、`0x02` → "前进"），是**安全级**缺陷而不是解析瑕疵。规则：白名单外的 TYPE/CID 一律整帧丢弃 + 计数器可见 |
| C14 | **两板之间所有连线都是杜邦线直连、不含任何外部元件**（无外部上拉/下拉、无串联端接、无电平转换；2026-09-26 实物确认，真源 [23 §9.3](23-wiring.md)）。IRQ 为 C6 开漏 + **仅 TC275 片内上拉**（`com/spi_hal_pins.c` 的 `IfxPort_InputMode_pullUp`；C6 侧 `esp32c6_car/components/c6_link/link.c` 显式 `GPIO_PULLUP_DISABLE`，其"external 10k"注释与实物不符） | 三条写代码时的硬约束：① 握手线只能做**静态电平判读**，禁止边沿计数/"跳变即事件"逻辑（与 C9 同源）；② **`IRQ 高 ≠ 从机在位`、`MISO 读回 0x00/0xFF ≠ 应答`**——那可能只是片内上拉的默认态，判活唯一依据是 `SF_ALIVE` 推进（V1.6 起这也是**唯一**失联判据，快照撕裂值由消费端钳位容忍）；③ SCLK/CS/MTSR 无端接 → 反射与振铃**没有硬件抑制手段**，提速只能随时钟档位走（G5 实测数据先于改档，`LINK_setClock()` 不得预设 >5 MHz）。此外：任何"加电阻分压去接 5 V"的想法属硬件变更，先回写 23 再动 `bsp/`（本板逻辑不兼容 5 V）。 |
| C15 | **UART 不再是板间链路**（2026-09-26 用户决策弃用）：`USE_SPI_LINK` 已定义进 `.cproject` 的 Debug 与 Release 两个 TASKING 配置，`Cpu2_Main.c` 的 `#else`（AT/UART）分支只在手动删除该符号时才编译；`com/wifi_at.c` 代码保留但不再承担命令、遥测与鉴权。P15.0/P15.1 ↔ GPIO6/7 这组线的现行用途只有**C6 侧调试控制台**（`23 §2`） | 三条后果：① **不得新增依赖 UART 收发业务帧的代码路径**，也不得在任何文档/注释里把 UART 写成"当前默认"或"正在跑的链路"；② G1 失败时"退回 UART"是**双侧动作**（TC275 删符号重编 + C6 重刷 esp-at），不是一句 `#ifdef`，排障计划要按一次刷机的代价排；③ 链路问题的可观测手段只剩 SPI 自身计数器（`crcErrors`/`seqErrors`/事务超时）与 C6 串口日志，**没有第二条业务通道可交叉验证**，所以 §5.6 的计数器与 `linkErrRate` 上报必须保持可读 |


> 本文档为设计基线 V1.7，接口签名以代码落地时的头文件为准；任何架构级变更需回写本文档并升版。板间 SPI 链路的详细设计（接线表、事务模型、SF 帧、两固件改动清单、台架门禁）见 [22-link-spi-design.md](../20-design/22-link-spi-design.md)。


## 遥控驾驶辅助落地补充（2026-10-04，App v1.2.0）

CPU0 在心跳/故障处理后增加编码器速度与前向 ToF 距离安全包络，含正向轮目标受距离、帧龄、区域有效性及编码器标定/发布新鲜度约束；保护停车需松杆及 3 个新健康帧解锁，倒车限速退出。停杆立即制动，静止不持续制动，保留台架操作。IMU 安装轴向待标定，暂不参与姿态制动及航向控制；标定入口、参数、数据接口和实测边界见 [37 · 传感器融合](../30-tc275/37-sensor-fusion.md)。这批不启用自动巡航/绕行。


### 2026-10-04：v1.2.1 开发者日志优化

周期日志改为字段名+单位，稳定融合 5 秒、IMU/ToF/车速 10 秒、链路 30 秒；短暂编码器活动不重置伺服日志节流。关键状态变化保留事件，队列溢出可观察。采样、控制和遥测周期保持原值。当前格式和全字段字典见 [38](../30-tc275/38-developer-logging.md)。

### 2026-10-04：TC275-C6 事务协议（TC275 v1.3.0 已实现）

用户确定以 iOS Remote 为出厂检查和验收入口，先冻结两板交互协议。沿用 SPI/SF、驾驶与现有遥测，新增 DIAG 0x53/sub 0x60 和 EVT 0x28/0x29 的版本化事务层；16 B 请求、32 B 以内事件适配现有跨核上限。通过会话隔离、请求 ID 去重、明确 ACK/RESULT、终态缓存与查询、租约、有限采集及数据完整性核对支持 IMU/ToF 和后续扩展。

完整字节布局、ID 登记、iOS-C6 边界、错误码、重试/复位语义、调度预算和验收门禁冻结在 [共享契约](../../../contracts/link/transaction-v1.md)，机器可读登记和黄金向量见 [transaction-v1.json](../../../contracts/link/transaction-v1.json)。TC275 实现边界和检查判据见 [39](../30-tc275/39-diagnostic-transactions.md)。C6 桥接与 iOS 新入口尚待实现；接口副本一致性不能代替硬件/故障注入验收。
