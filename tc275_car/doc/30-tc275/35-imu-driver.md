# 35 · TC275 侧 LSM6DSV16BX 六轴 IMU 驱动（QSPI1）——设计基准与实现现状

| 项 | 内容 |
|---|---|
| 文档编号 | 35 |
| 域 | TC275 侧（3x） |
| 版本 | V1.1（2026-10-03，**失联诊断落地**——`IMU=` 行只在读数分支打，器件失联时串口上只剩一句不带信息的 `IMU absent ...`，四种完全不同的病因（无供电/相位错/CS 未到/中断丢失）无法区分。本轮：`imu_probe()` 由 `boolean` 改回 `ImuStatus`（`IMU_ERR_NO_DEV` 自此有了使用者）、init 与 1 Hz 重探的失败路径按 0.5 Hz 打 `IMUERR st/id/err/clk=` 行，§7.1 补判读表；**不改任何总线行为、不改接线、不动配置默认值**。同批现状更新：实物已接线（用户确认 VCC 3.3 V、模块确认 LSM6DSV16X），驱动已在台架运行，**首验 ① 未过（报失联）——病因待 §7.1 新行的实测结果**。V1.0（2026-10-03 首版：驱动落地——`bsp/imu.c/.h` + xcore `XcoreImu` 块 + 1 kHz 挂载 + 主机单测 `test_imu`（55 断言）与 `test_xcore` IMU 块回归（1095→1109）；`bsp/imu.c` 驱动主体无单测覆盖、需台架首验（§8）） |
| 需求定位 | **F13（11-requirements V1.2，V2.0 感知层）**——按 23 §10 的提前锁位决策先落数据源与首验工具，融合/导航算法不在本篇范围 |
| 代码基线 | `main`（imu 驱动本轮工作树） |
| 读者 | 在 tc275_car 作业的 AI / 开发者（动 IMU 相关代码前必读，按 [33-ai-codebase-guide.md](33-ai-codebase-guide.md) 进入） |
| 上位文档 | [23 §10](../20-design/23-wiring.md)（**接线与脚位真源**）、[21 §18 C1/C2](../20-design/21-software-design.md)（向量表/优先级约束）、[33 §4](33-ai-codebase-guide.md)（xcore 规约）、[31](31-firmware-architecture.md)（现状架构） |

> **定位**：TC275 侧 IMU 驱动的**单一真源入口**——寄存器事实（§2）、总线/中断归属（§3）、配置默认值（§4）、数据通路与单位域（§5）、运行时行为（§6）、台架首验判读（§7）、范围界定（§8）。与 21/23 冲突时按 [00-index §3](../../00-index.md) 裁决顺序取权威（接线以 23 §10 为准，设计以本文为准）。

---

## 1. 背景与范围

23 §10 已把 LSM6DSV16BX（3.3 V 六轴 IMU）的脚位锁进真源（QSPI1 + INT1/INT2，待实物接线），需求定位 F13（V2.0 感知层）。本篇交付的是**感知层的第一块地基**：

- **v1 交付**：QSPI1 主机 + LSM6DSV16BX 寄存器层 + 固定配置采样 + xcore 发布 + INT1 ERU 计数——够做 23 §10.2 的上电首验（WHO_AM_I / ODR 核对），给 V2.0 融合算法一个稳定的数据源。
- **非目标（v1 明确不做）**：遥测 38 B 帧扩展（动它必须跨库同步 C6 + 手机端，另立项）、协议命令入口（IMU 参数暂无运行时调整需求）、INT2 运动事件（23 §10 标注"可选"，硬件可不接）、滤波/零偏标定/姿态融合、DRDY 边沿驱动的采样调度。

## 2. 硬件事实（寄存器级，已交叉核对）

> **核对来源**：ST 官方驱动 `stm32duino/LSM6DSV16X` 的 `lsm6dsv16x_reg.h/.c`（WHO_AM_I=0x70 同源）。**警告：DSV16X 的寄存器布局与旧款 LSM6DSL 明显不同**，拿 DSL 的经验直接套会写错位——ODR 在 CTRL1/2 的**低 4 位**（DSL 在高位）、陀螺 FS 独立在 **CTRL6 且是 4 位域**、加速度 FS 在 **CTRL8**。

### 2.1 接线（摘自 23 §10.1，真源在那边）

| 信号 | TC275 | 说明 |
|---|---|---|
| SCLK/MTSR/MRST/CS | P11.6 / P11.9 / P11.3 / **SLSO3 P11.10** | QSPI1，X1-36/35/37/34 |
| INT1 | **P15.4**（X1-31） | ERU REQ0，**可边沿中断**（与 P23.x/P33.x 不同） |
| INT2 | P15.5（X1-30，可选） | ERU REQ13，v1 **不配置** |
| VCC/GND | X1-2 VEXT / X1-40 | 3.3 V，LDO G1 预算内（23 §5.2） |

### 2.2 本驱动用到的寄存器（`bsp/imu.h` 为代码真源）

| 寄存器 | 地址 | 用法 |
|---|---|---|
| `WHO_AM_I` | 0x0F | 探测，期望 **0x70** |
| `CTRL1` | 0x10 | 加计：`ODR_XL[3:0]`（低 4 位）+ `op_mode_xl[6:4]`（0=高性能） |
| `CTRL2` | 0x11 | 陀螺：`ODR_G[3:0]` + `op_mode_g[6:4]`，同上 |
| `CTRL3` | 0x12 | `SW_RESET` bit0（写 1 自清）、`IF_INC` bit2、`BDU` bit6 |
| `CTRL6` | 0x15 | 陀螺 `FS_G[3:0]`：125/250/500/1000/2000 = 0x0..0x4，**4000 = 0xC** |
| `CTRL8` | 0x17 | 加计 `FS_XL[1:0]`：2/4/8/16 g = 0..3 |
| `INT1_CTRL` | 0x0D | `drdy_xl` bit0 + `drdy_g` bit1 → INT1 每 ODR 周期一个脉冲 |
| `OUT_TEMP_L` | 0x20 | **突发起点**：温度 2 B + 陀螺 6 B（0x22 起）+ 加计 6 B（0x28 起）= 连续 14 B，一次 `IMU_readRegs(0x20, buf, 14)` 全取 |

ODR 编码（高性能模式）：0x0=关、0x3=15、0x4=30、0x5=60、0x6=120、0x7=240、0x8=480、0x9=960 Hz。

### 2.3 灵敏度与换算（定点整数，官方驱动常数）

| 量 | 换算（`bsp/imu.h` 纯函数，主机单测锁定） |
|---|---|
| 加计 mg | `raw × {61,122,244,488} / 1000`（2/4/8/16 g），对称舍入 |
| 陀螺 mdps | `raw × 35 / {8,4,2,1}`（125/250/500/1000 dps）、`raw × 70`（2000）、`raw × 140`（4000），对称舍入 |
| 温度 0.01 °C | `2500 + raw × 100 / 256`（raw 0 = 25.00 °C） |

## 3. 软件结构：核属主、总线与中断

- **属主 = CPU1**（与编码器、电池 ADC 同核，"实时感知/执行核"分工不变）。`IMU_init()` 在 `MOTOR_ALGO_init()`、`IMU_task()` 在 `MOTOR_ALGO_task()`（`ADC_task()` 旁，急停 return 之前——每个分支都必须跑）。**其他核禁止直接调 bsp/imu**，数据一律走 xcore。
- **总线**：QSPI1 主机（iLLD `IfxQspi_SpiMaster`，与 `com/spi_hal_pins.c` 同驱动不同模块实例）。**SPI 模式 3**（CPOL=1/CPHA=1）：iLLD 映射是 `clockPolarity=idleHigh` + `shiftClock=shiftTransmitDataOnLeadingEdge`（= ECON.CPOL=1/CPH=1；CPH 语义是"主机采样沿"，与字段名读感相反——QSPI3 通道注释里同一个坑的反向 case，代码里有完整注释）。帧 = 1 B 命令（bit7 R/W + 7 位地址）+ 数据，`channelBasedCs=disabled` 保证 CS 全帧不抬（ST 器件要求）。时钟档位 1/2/5/10 MHz，**默认 1 MHz**（§4），台架逐档上探纪律同 23 §9.1。
- **中断（21 §18 C2 已登记）**：向量表 0、TOS=CPU1。**QSPI1 TX=11 / RX=14 / ER=15**（全局空档，低于编码器 16~23——轮沿永远优先于 FIFO 服务），**ERU DRDY=24**（INT1 数据就绪边沿，ISR 只做"清 ERU 事件标志 `SCU_FMR.FC0` + `g_drdyCount++`"——计数即 23 §10.2 ③ 的台架证据，v1 不用它调度采样；**严禁在 ISR 里发起 SPI 事务**）。**清事件标志是必需的**：`EIFR.INTF0` 是锁存标志，OGU0 的服务请求由它电平驱动，硬件 ACK 清掉 `SRC.SR` 后标志仍为 1 会立刻重新挂起 → 240 Hz 沿变成中断洪水、CPU1 1 kHz 环饿死（看门狗复位）。
- **ERU 通路**：P15.4 → 输入通道 0（`IfxScuEru_InputChannel_0` + `ExternalInputSelection_0` = REQ0/RxSel a）→ OGU0（alwaysActive）→ `MODULE_SRC.SCU.SCU.ERU[0]`（即 UM 名 `SRC_SCUERU0`，地址 0xF0038CD4；`MODULE_SRC.SCU` 是 `Ifx_SRC_GSCU` 包装层，真寄存器在第二层 `.SCU`），上升沿（INT1 推挽高有效；引脚下拉——**若改开漏配置须同批改 pullUp**，23 §10.2）。

## 4. 配置默认值（`bsp/imu.h` 宏，改默认值 = 改这里 + 重跑单测）

| 项 | 默认 | 理由 |
|---|---|---|
| ODR | **240 Hz**（CTRL1/2 = 0x07） | 1 kHz 环每 5 ms 读一次（200 Hz）落在 ODR 之下，BDU 保证每次拿到完整新样本；240 给 V2.0 融合留了 2 倍余量 |
| 加计 FS | **±4 g** | 小车颠簸/碰撞瞬时过 2 g，±2 g 会削顶；±4 g 下 0.122 mg/LSB 的分辨率仍远超需求 |
| 陀螺 FS | **±500 dps** | 本车最大偏航率 <360 dps，留余量；17.5 mdps/LSB |
| 时钟 | **1 MHz** | 杜邦线直连（23 §9.3 电气口径沿用），1→2→5→10 逐档台架验证，上限 10 MHz（手册口径） |

## 5. 数据通路与单位域

```
IMU_task (CPU1 1kHz，每 5 tick)
  └─ IMU_readRegs(0x20, 14B)  ← QSPI1 模式3 突发（~120 µs @1MHz）
  └─ 纯函数换算（imu.h，有单测）→ XcoreImu → XCORE_imuPublish（seq 由 publish 内部递增）
        ├─ CPU0：日志/未来欠压同款的显示消费
        └─ CPU2：未来遥测扩展（v1 未接）
```

`XcoreImu`（`mw/xcore/xcore.h`，单自旋锁、临界区一次整块拷贝）：

| 字段 | 单位域 | 说明 |
|---|---|---|
| `seq` | 计数 | **publish 内部递增**，0 = 从未发布（消费者判新鲜度看它，不猜数据） |
| `alive` | bool | WHO_AM_I OK 且读事务连续成功（§6 失联判定） |
| `whoAmI` | — | 最近一次探测值（0x70） |
| `accMilliG[3]` | **mg** | sint16（±16 g 满量程 = ±15990 mg，域内） |
| `gyroMilliDps[3]` | **mdps** | sint32（±500 dps = ±500000，超出 sint16 必须用 32 位） |
| `tempCentiC` | **0.01 °C** | 芯片温度 |
| `drdyCount` / `errCount` | 计数 | INT1 边沿数 / SPI 事务失败数（台架判读用） |

单位域红线：协议/robot 层 ±100、电机域 pct×10、**IMU 域 mg / mdps / 0.01 °C**——三套域互不换算，遥测侧要用什么单位由消费端自己决定。

## 6. 运行时行为

- **初始化**（`IMU_init`）：QSPI1 模块 + 通道（1 MHz）→ ERU → `WHO_AM_I` 探测 → `SW_RESET`（等自清，~10 ms 界）→ 5 ms 稳定 → 写 CTRL3(`IF_INC|BDU`)/CTRL1/CTRL2/CTRL6/CTRL8/INT1_CTRL → 首读 + 首发布。成功打 `IMU ready (WHOAMI=0x70, ODR 240Hz, +-4g/+-500dps, 1MHz)`。
- **1 kHz 任务**（`IMU_task`）：每 5 tick 一次 14 B 突发 → 换算 → 发布；**连续 20 次读失败（=100 ms）判总线丢失**，`alive=FALSE` 并打 `IMU bus lost`，之后 **1 Hz 重探**——探测成功即重新配置恢复。**传感器没接时上电不阻塞**：打 `IMU absent ... retrying 1Hz`，接好线 1 秒内自愈，**不需要重启**（台架工作流）。
- **台架日志**：每 2 s 一行 `IMU= whoAmI ax ay az gx gy gz tempC drdy err`（`XCORE_logi`，有符号十进制；tempC 即 `tempCentiC`，mg/mdps/0.01°C 原域）。0.5 Hz 是刻意的——2 KB 日志环要留给链路与伺服行。
- **失联诊断行**：`IMU=` 只在读数成功的分支里打，所以器件失联时它一行都不会出现。失联改由同节律（0.5 Hz，与 1 Hz 重探解耦）打一行 `IMUERR st/id/err/clk= <st> <whoAmI 原字节> <errCount> <实际 SCLK Hz>`——`st` 是 `ImuStatus`，判读见 §7.1。**没有这一行，"芯片型号不对"和"总线没通"在串口上长得一模一样**（旧的 `IMU absent ...` 只说"失败了"）。
- **时钟提档**：`IMU_setClockTier()`（忙时拒收）+ `IMU_actualClockHz()`（回 iLLD 实际分频值）。v1 无命令入口，提档由调试器/临时代码调（§7），改默认档必须与实测同批提交（纪律同 23 §9.1 档位表）。

## 7. 台架首验规程（对应 23 §10.2 ①②③ 的固件侧判读）

前置：23 §10.1 接线完成、共地确认。串口 115200（X4 虚拟 COM）。

1. **① 供电/在位**：上电看首行。`IMU ready (WHOAMI=0x70, ...)` = 总线通、器件在位；`IMU absent (WHOAMI mismatch or bus dead), retrying 1Hz` = 查 VCC（X1-2 应 3.3 V）/四根信号线/共地，接好后等下一行 `IMU found (WHOAMI=0x70), configured`（**不用重启**）。
   该行的括号是历史遗留——它本身不区分病因，**判读依据是紧跟其后的 `IMUERR st/id/err/clk=` 行**（§6）：

   | st | 含义 | 先看什么 |
   |---|---|---|
   | 2 `NO_DEV` + id=0x00 | MISO 从未被器件驱动 | X1-37（P11.3）线、共地；`err` 非 0 说明事务也在失败 |
   | 2 `NO_DEV` + id=0xE0 / 0x38 | 整帧移了 1 位 = **时钟相位/极性不对**（0x70 左移/右移） | 本文 §2.2 的模式 3 两极（CPOL=1/CPHA=1）与线序 |
   | 2 `NO_DEV` + id=0xFF | CS 没落到器件（器件全程高阻） | X1-34（P11.10）线；**2 号 Shield2Go 座是否插了板**（23 §10.1 冲突条） |
   | 2 `NO_DEV` + id 为其他值 | 总线上的不是这块芯片 | 与模块丝印核对型号 |
   | 4 `HW` | QSPI 锁存了错误字 | 档位/接线质量，先确认停在 1 MHz 档（`clk` 字段即实际值） |
   | 5 `TIMEOUT` | 事务从未完成（服务请求丢失，非总线电气问题） | CPU1 中断是否已开、优先级 11/14/15 占用（21 §18 C2） |
   | 3 `BUSY` | 上一笔仍持模块锁 | 是否有 `IMU_task` 之外的调用者抢总线 |
   | 0 `OK` | WHO_AM_I 已匹配，失败发生在**配置写**那几笔 | 看 `err` 计数与 CS/复位线 |

   （`st=1 PARAM` 在探测路径不可达——参数在编译期固定；见到即为代码 bug。）
2. **② 静置判读**（车不动，等 `IMU=` 行）：
   - `ax ay az` ≈ `0 0 +1000`（Z 轴 1 g 重力，方向依模块贴装，±50 mg 内漂移正常）；
   - `gx gy gz` ≈ 0（±2 mdps×静止噪声，未做零偏校准前 ±100 mdps 内正常）；
   - `tempC` ≈ 环境温度（raw 0 = 25.00 °C，芯片自热 +3~8 °C 正常）；
   - `err` 应恒 0（非 0 = 总线质量/档位问题，先回 1 MHz）。
3. **③ INT1/ODR 核对**：静置时 `drdy` 字段每 2 s 应增加 ≈ **480**（240 Hz ODR × 2 s，±2%）；示波器看 X1-31（P15.4）脉冲周期 ≈ 4.17 ms 双重印证。计数不走 = ERU 通路/接线问题（数据仍正常说明只是 INT1 线断）。
4. **提速**：上述全过后按 23 §9.1 纪律逐档 `IMU_setClockTier`（2→5→10），每档 30 min 观 `err` 不增长；提档值要落回本文 §4 并升版。

## 8. 范围界定与验证状态

- **主机单测已覆盖**：换算数学（`test_imu` 55 断言：灵敏度锚点/对称舍入/FS 码映射/命令字节）、xcore 块语义（`test_xcore` IMU 块：seq 单调、发布者持锁、零态=未发布）。CI 已加 `test_imu` step。
- **无单测、需台架**（诚实声明）：`bsp/imu.c` 驱动主体（QSPI1 事务、ERU、SW_RESET 序列、失联自愈，**以及 V1.1 新增的失联诊断行**）依赖 iLLD/硬件，主机编不了也没法测——首验按 §7 走。"出问题先查接线再查代码"这条自 V1.1 起才有实测依据：`IMUERR` 行的 `st`/`id` 两字段就能把"接线/电气"与"芯片/相位"分开，此前两者在串口上不可区分。
- **构建与运行状态**：V1.0 工作树**已在 AURIX Development Studio 构建并在台架运行**——上电打出 `IMU absent ...`，该行只存在于 `bsp/imu.c`，即驱动主体已在真硬件上跑过；但**首验 ① 未过（报失联），病因待 §7.1 实测**。V1.1 本轮改动**未过 TASKING 构建**（主机/CI 无法编译固件）：全部落在 `imu.c` 的静态函数与两处调用点，无头文件、无 xcore/协议接口变化，`test_imu`/`test_xcore` 断言数不变。
- **后续挂钩（都不在本篇 v1）**：遥测扩展（动 38 B 帧 → 跨库契约，另立项）、V2.0 融合（届时再议 DRDY 驱动采样 + 时间戳）、FS/ODR 运行时配置化（有需求再走 0x7x DPT 命令族先例）、INT2 运动事件（硬件可选线，固件未配）。
