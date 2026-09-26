# SmartDrive 板间链路换向：UART → SPI（SF 帧详细设计）

| 项 | 内容 |
|---|---|
| 文档编号 | **22**（域：设计·硬件）· 定位：21（SDD）§3.7/§6 的 LINK 段展开 · 上级索引 [00-index.md](../00-index.md) |
| 文档版本 | V1.2（SDD V1.2b 的 LINK 段展开，结论**已回写基准**；V1.1 追加两侧代码落地状态（§7）与 E11/E12 两条实测证据；**V1.2 = C6 已烧录 SPI 固件后完善 TC275 侧的回写**：新增证据 **E13（两侧常量表三处不一致，按从机更正）**、**E14（`GEN` 回执跨两次从机任务、`CLOCK_SET` 单位 MHz）**，§4.3 补 `SF_GEN` 与 `ERRSTAT` 位表、§5.1 定 `FLAGS`、§5.2 补五条 CMD 通道的载荷形状、§5.5 分派规则改为三层白名单 + 38 B 遥测硬契约、§7 撤掉"主机缺 `LINK_gen()`"、§8 G2 已过项加遥测交叉单测、§9 加 R12。实施仍待 G1 台架门禁通过，未拍板项见 §11） |
| 日期 | 2026-09-26 |
| 参考体例 | [21-software-design.md](../20-design/21-software-design.md) §3.7（板间通信选型）、§6（协议）、§16（风险） |
| 决策范围 | C6↔TC275 主链路物理层 = **SPI**；帧协议 = **新定 SPI 专用 SF 帧**；拓扑 = **TC275 QSPI3 主机 ↔ ESP32-C6 SPI2 从机（`spi_slave_hd`）+ 1 根握手线** |
| 交付状态 | **设计 + 两侧固件代码 + 文档回写均已落地**：C6 侧见 `c6_car` `22e15f2`（§7.1），TC275 侧见 `Middleware/com/`+`Middleware/sf/`（§7.2），SDD **V1.2b**、23-wiring.md **V1.6**。主机单测：帧层 2855 断言 + 遥测层 154 断言（后者**把从机 `proto_frames.c` 编进同一可执行文件**做双向交叉）。**未做**：G1 台架波形验证、一次 TASKING IDE 构建确认链接闭合、整车通电联调。本轮**不提交**（等用户确认） |
| 前提变化 | ① C6 改为自研固件（`c6_car` 工程），不再受 esp-at 约束；② `c6_car` 的 proto v2 与 myCar 的 demo 异或 CRC 帧**本来就不兼容**，链路换向时一并按 SPI 特性重定帧，成本最低 |

---

## 1. 决策摘要（与 SDD V1.1 的差异）

| 项 | SDD V1.1（现文本） | 本方案（V1.2 目标态） | 变更理由 |
|---|---|---|---|
| V1.0 主链路 | UART @2 Mbps（921600 起步 + 0x44 握手自动降速） | **SPI，1 MHz 起、阶梯提速到 5 MHz 量产基线**（上限 20 MHz 硬件在环验证后再提） | 自研 C6 固件后 SPI 从机不再是 esp-at 的黑盒，联调风险从"驱动是否有"变成"波形是否兼容"，可用台架一次性关死；吞吐天花板从 ≈200 KB/s 提到 ≈550 KB/s（5 MHz）/≈2 MB/s（20 MHz），V2 高频遥测与日志回传不必再换链路 |
| SPI 定位 | V1.1 升级路径，仅预留焊位 | **V1.0 选定**；**UART 降级为调试备份 + 回退通道**（保留物理接线） | 用户拍板 |
| 帧协议 | 帧协议 v2（`AA 55 VER CMD SEQ LEN DATA CRC16`），LINK 与手机 WS 同构 | **拆成两层**：手机 WS 仍用 v2 帧（C6 侧对外语义不变）；**LINK 改用 SF 帧**（4 字节对齐、分片位、寄存器握手，面向半双工 DMA 事务） | SPI 从机的硬约束（半双工、RX 长度须 4 的倍数、需状态寄存器握手）与"字节流上的自描述帧"不是一回事，强行同构会得到两头将就的帧 |
| 波特率协商 | 0x44 BAUD REQ/ACK/NAK（115200↔921600↔2 Mbps） | **删除**，改为**时钟阶梯 + 误码统计**（1/2/5/10/20 MHz 各档 30 min CRC 误码率门槛） | 波特率是 UART 专有概念；SPI 的对应问题是信号完整性，用固定档位 + 判据更可测 |

**一句话**：安全模型、三核分区、xcore v2、OTA 双 bank、产测序列全部不动；只把"C6↔TC275 这一段"从异步字节流换成主机拉取的定长事务，并为此重定义该段上的帧。

---

## 2. 事实基线（全部为本地可复核证据，非推断）

| # | 事实 | 证据 |
|---|---|---|
| E1 | ESP32-C6 的 SPI 从机半双工（HD）**硬件支持**，IDF 驱动可用 | `soc/esp32c6/include/soc/soc_caps.h:352` `#define SOC_SPI_SUPPORT_SLAVE_HD_VER2 1`；驱动 `esp_driver_spi/include/driver/spi_slave_hd.h`；HAL `esp_hal_gpspi/hal/spi_slave_hd_hal.c` 编入条件即该宏 |
| E2 | `spi_slave_hd` 是**半双工**：每一次事务要么主机读（RDDMA）要么主机写（WRDMA），不存在同时双向 | `spi_slave_hd.h` 通道枚举 `SPI_SLAVE_CHAN_TX`（主读）/`SPI_SLAVE_CHAN_RX`（主写）；头文件即名 HalfDuplex |
| E3 | 从机收缓冲**长度必须是 4 的倍数**，缓冲必须 DMA 能力；前导相位 `command_bits/address_bits/dummy_bits` 均为 8 的倍数且 ≥8 | `spi_slave_hd.h` 结构体注释（`len` 与 `command_bits` 三行）；`spi_slave_hd_slot_config_t` |
| E4 | 官方给的数据通路模型 = **共享寄存器握手**：从机用 `spi_slave_hd_write_buffer()` 发布 5 个 4 字节寄存器（READY_FLAG / MAX_TX_BUF_LEN / MAX_RX_BUF_LEN / TX_READY_BUF_SIZE / RX_READY_BUF_NUM），主机轮询后决定读多少、写多少 | `examples/peripherals/spi_slave_hd/segment_mode/seg_slave/main/app_main.c:36-49,83,98,292-301`；主机侧 `seg_master/main/app_main.c:37-49,140-200` |
| E5 | **寄存器读值非原子**：SPI 逐字节搬运，从机可能在读的过程中改值 → 官方做法是"连读两次直到相同" | `seg_master/main/app_main.c:146-159` 注释原文："if the value is changed by Slave at this time, Master may get wrong data" |
| E6 | 从机硬件按命令字节识别 4 类事件（CMD7/CMD8/CMD9/CMDA）并产生中断 | `esp_hal_gpspi/.../spi_slave_hd_hal.c:43-151`（`SPI_LL_INTR_CMD7/8/9/A`） |
| E7 | TC275 侧 QSPI3 四线可全部落在 LITE kit X1 空闲脚，且与编码器 8 线（P33.0~7 = X2-28~35）**零交集** | `myCar/Libraries/iLLD/TC27D/Tricore/_PinMap/IfxQspi_PinMap.h:160,194,226,287`：`IfxQspi3_SCLK_P33_11_OUT` / `_MTSR_P33_12_OUT` / `_MRST_P33_13_OUT` / `_SLSO5_P23_4_OUT`；空闲清单见 23-wiring.md §9 与记忆脚位表 |
| E8 | **AURIX TC275 QSPI 没有"命令/地址相位"硬件概念，iLLD 主驱动也没有对应封装**（只有数据相位 + CS/时钟参数） | 全量 grep `Libraries/iLLD/TC27D/Tricore/Qspi` 无 `command_bits/address_bits/IfxQspi_AddrMode` 任何符号 |
| E9 | **（结论已过时，保留作决策依据）** myCar 工程当时**没有任何** `IfxQspi` 使用点：量产链路的 SPI 侧在 TC275 上是全新代码 → 现已由 `Middleware/com/spi_hal_pins.c` 补上 | 采集时点 `App/`、`Bsp/`、`Middleware/` 内 grep `IfxQspi` 无命中；现有 WIFI 链路 = `Middleware/wifi_at.c`（ASCLIN1，P15.0/P15.1）上的 AT 文本 + `Middleware/protocol.c` 的 `AA 55 CMD LEN DATA XOR-CRC` 帧（`PROTO_MAX_PAYLOAD 16`，无 VER/SEQ） |
| E10 | **（结论已过时，保留作决策依据）** c6_car 当时链路层是纯 UART 实现，且含 UART 专有的波特率协商状态机 → 现已改为 `spi_slave_hd`（`22e15f2`），0x44 状态机与 `C6_LINK_TX/RX_GPIO` 删除（§7.1） | 采集时点 `c6_car/components/c6_link/link.c`（`LINK_UART_NUM UART_NUM_1`、`uart_set_baudrate`、0x44 BAUD REQ/ACK/NAK）；`c6_car/components/c6_link/Kconfig`（`C6_LINK_TX_GPIO default 10` / `RX_GPIO default 11`） |
| **E11**（2026-09-26 新增，作废"§3.1/§4.1 原稿的 IOM 上升沿中断"） | **P23.0 在本工程里做不出边沿中断，只能电平轮询**。TC27x 的 GPIO 边沿事件只有两条硬件通路：ERU 或 IOM 输入；两者都不覆盖 P23.x | ① `Libraries/iLLD/TC27D/Tricore/` 下**无 Eru 模块目录**（`ls` 无匹配），TC275 无 ERU；② `_PinMap/` 下**无 `IfxIom_PinMap.h`**（`ls` 无匹配），即 TC27D 没有"GPIO → IOM 输入"的映射表，P23.0 无法作为 IOM 监视输入；③ `Iom/` 全量 grep `initRiseInterrupt\|initFallInterrupt` **零命中**，iLLD 的 IOM 驱动只暴露 LAM/ISM 事件窗（`IfxIom.h` 的 `IfxIom_Lam*` 枚举），源是 GTM/Ccu6 捕获量而非任意 GPIO |
| **E12**（2026-09-26 新增，前导相位的命令字节已可冻结） | Espressif HD 从机的**上 wire 命令字节**已用本地 IDF 5.4.4 源码确认，1 线模式下就是基命令值（`cmd_mod = 0x00`）：`WRBUF=0x01 / RDBUF=0x02 / WRDMA=0x03 / RDDMA=0x04 / SEG_END=0x05 / EN_QPI=0x06 / WR_END=0x07 / INT0=0x08`。注意 `spi_types.h` 里的 `SPI_CMD_HD_* = BIT(n)` 只是**内部枚举**（WRDMA=0x04、RDDMA=0x08…），不是 wire 值，照抄必错。且 **RDDMA 突发必须以一次 `INT0` 事务收尾、WRDMA 突发必须以 `WR_END` 收尾**，否则从机不释放缓冲 | `components/hal/esp32c6/include/hal/spi_ll.h:81-90`（`SPI_LL_BASE_CMD_HD_*` 实际值）、`:1575-1598`（`spi_ll_get_slave_hd_command()`：1 线时 `cmd_mod=0x00`，返回 `cmd_base \| cmd_mod`）、`:1605-1608`（`dummy_bits` 恒为 8）；对比 `components/hal/include/hal/spi_types.h:67-76`（内部枚举位值）；成对收尾规则见 `components/driver/test_apps/components/esp_serial_slave_link/essl_spi.c:184-250`（`essl_spi_rddma()` 末段后固定调 `essl_spi_rddma_done()`→`INT0`；`essl_spi_wrdma()` 固定调 `essl_spi_wrdma_done()`→`WR_END`）；寄存器地址按**字节**寻址并做 `addr % 72`（同文件 `:104,136`），例 `seg_master/main/app_main.c:37-48` 用 0/4/8/12/16；从机侧 `command_bits=address_bits=dummy_bits=8`、`mode=0`、`SPI_DEVICE_HALFDUPLEX`（`app_main.c:65-82`） |
| **E13**（2026-09-26 新增，本轮完善 TC275 侧时发现的同名不同义） | 主机侧初版与已烧录的从机固件有**三处常量表分歧**，一律以**从机为准**修正（从机已烧录 = 事实真源）：① `FLAGS` bit1 从机是 `FRAG_END`（一帧的末片），主机原写 `ACK`——ACK 按 §5.2 是 TYPE `0x03`，不是 flag，位值复用会让分片帧被误判；② OTA 控制 CID 为 `0x30 BEGIN / 0x31 CHUNK / 0x32 ACK / 0x33 STATUS / 0x34 SWAP / 0x35 ABORT`（即 v2 CMD−0x30 关系保持），主机原写 `SF_CID_OTA_END` 且缺 `0x35`；③ `SF_ERRSTAT` 位图真值是 `CRC 0x01 / FMT 0x02 / SEQ 0x04 / RXOVFL 0x08 / TXOVFL 0x10 / TRUN 0x20 / LINKLOST 0x40`（各自粘滞），主机原表是臆造值 → G1 读到的错误位会被完全错读 | 从机侧 `c6_car/components/c6_sf/sf_frame.h:37-39,70-75,94-101`；主机侧改动见 §7.2 `sf_frame.h`/`link.h` 行；38 B 遥测偏移另由 `test/host/test_sf_telemetry.c` **直接编译从机 `proto_frames.c`** 做双向交叉验证（154 断言 / 0 失败） |
| **E14**（2026-09-26 新增，`GEN` 回执的时序契约） | `GEN` 事务**不是** SPI 中断里同步应答的：从机 `cb_buffer_rx()`（ISR）只置 `gen_notif` 标志，真正的 `link_handle_gen()` 在从机链路任务里执行，处理完才 `regs_set_u32(SF_REG_CMDRSP, cmd \| result<<8)` 并 `regs_publish()`。因此主机侧"写 GEN → 读回执"必然跨**两次从机任务调度**，必须带超时而非死等。另两点同源事实：`SF_GEN_NOP(0)` 落到 `default` 分支 → 回执 `result = RSP_UNKNOWN(1)`（不是 OK），正好可当**清粘滞回执的探针**；`CLOCK_SET` 的 payload 单位是 **MHz**（从机 `payload * 1000000u`） | `c6_car/components/c6_link/link.c:561-569`（ISR 置标志）、`:603-607`（任务里处理）、`:484-518`（`link_handle_gen()`，`:510` 的 `* 1000000u`、`:512-514` 的 default→UNKNOWN、`:516` 回执拼装）；主机侧实现 `Middleware/com/link.c:link_genWriteAndWait()`/`LINK_gen()`/`LINK_setClock()`，超时 `LINK_GEN_TIMEOUT_MS = 20` |

**由 E8 得到的关键结论（本方案唯一的真风险）**：TC275 作主机时，E1/E4 所要求的 `8bit CMD + 8bit ADDR + 8bit DUMMY` 前导相位只能用**普通数据字节**在 CS 有效期内连续发出（QSPI 的移位器把 CMD/ADDR/DATA 一律当数据搬）。Espressif 的 HD 从机是否会把这段波形正确解析，属于**必须在台架上一次性验证**的事项，不能靠读手册下结论 → 见 §8 门禁 G1。

---

## 3. 物理层与接线表

> **接线状态（2026-09-26）：本节五行 + 共地已按下方表完成实物接线**（DevKitC-1 走 J3 长排针，TC275 走 X1）。G1 的硬件前置条件已满足，剩余工作是两固件改动清单（§7）与台架验证（§8）。**两项需确认**：① GPIO21 IRQ 的 10 kΩ 上拉是否已接（未接则握手失效，链路退化为纯轮询）；② 本表 X1/J3 孔位号是按手册推得的文档标注，若实物丝印编号不同请回报修正（脚位本身零冲突）。

### 3.1 拓扑

```
TC275 LITE kit (QSPI3 主机, X1 侧)              ESP32-C6-DevKitC-1 (SPI2 从机 HD, J3 侧)
 X1-3   P33.11 SCLK   ─────────────────────►     GPIO19  SCLK      J3-9
 X1-4   P33.12 MTSR   ─────────────────────►     GPIO18  MOSI      J3-10
 X1-5   P33.13 MRST   ◄─────────────────────     GPIO20  MISO      J3-8
 X1-12  P23.4  SLSO5  ─────────────────────►     GPIO23  CS        J3-5
 X1-8   P23.0  电平轮询 ◄───────────────────     GPIO21  IRQ(OD)   J3-7
 GND     X1-40/X2-40 ──────────────────────────  GND               J3-1/12/15
 （可选预留，本版不接）TX_RDY：TC275→C6 流控线，落 P23.5 ↔ C6 空闲 GPIO
 电源：C6 仍按 23-wiring.md §5 由车载 DC-DC 5V 供电（J1-14），禁止从 J1-1 反灌 3V3
```

| 信号 | TC275 | X1 孔位 | 方向 | C6 | J3 孔位 | 依据 |
|---|---|---|---|---|---|---|
| SCLK | P33.11 | X1-3 | 主→从 | GPIO19 | J3-9 | E7 / GPIO 矩阵 |
| MOSI（MTSR 主发） | P33.12 | X1-4 | 主→从 | GPIO18 | J3-10 | E7 |
| MISO（MRST 主收） | P33.13 | X1-5 | 从→主 | GPIO20 | J3-8 | E7 |
| CS | P23.4（SLSO5） | X1-12 | 主→从 | GPIO23 | J3-5 | E7 |
| IRQ（数据就绪，开漏 + 上拉） | P23.0 | X1-8 | 从→主 | GPIO21 | J3-7 | C6 侧 esp-at SPI Kconfig 的 handshake 默认脚；TC275 侧**只能电平轮询**，见 E11 |
| GND | X1-40/X2-40 | — | — | GND | J3-1/12/15 | 必须共地（23-wiring.md §5.4） |

> 复核项（落地前 1 分钟动作，不阻塞评审）：X1 的 P23.0/P23.4 **孔位号**以官方手册 Figure 4 再对一次丝印；P23.x 组已在"X1 空闲"清单内，脚位本身无板载复用冲突。

### 3.2 电气与线束

- 两端 3.3 V 逻辑，直连，无分压。
- 线长 ≤ 20 cm，SCLK 与三线同束、就近共地回流；IRQ 为开漏 + 10 kΩ 上拉到 C6 的 3V3。
- 时钟档位：**1 MHz（G1 波形兼容）→ 2 → 5（量产基线）→ 10 → 20（探索）**，每档 30 min CRC 误码率门槛（§8）。杜邦线下 5 MHz 是保守工程值；换定长扁平线束后再谈 10/20 MHz。
- UART 通道（23-wiring.md §2 的 P15.0/P15.1 ↔ C6）**保留接线不删**：C6 自研固件里作 115200 控制台/日志，G1 失败时的回退链路。

---

## 4. SPI 事务模型（谁在什么时候发起什么）

### 4.1 角色与时域

- **TC275 = 唯一时序主人**：所有事务由 CPU2 链路泵发起（超循环 + P23.0 **电平轮询**）。C6 的 Wi-Fi/HTTPD 抖动（毫秒级、不可控）不会污染总线时序（SDD §3.7 原论证保留）。
- **半双工后果**：C6 **不能主动推送**。命令下行的时延下界 = TC275 下一次轮询/读取事务的时刻。原稿设想的"IRQ 上升沿中断把轮询变成事件驱动"**在 TC275 上不成立**（E11：TC275 无 ERU，P23.x 不在 IOM 监视输入内，iLLD 也没有 GPIO 边沿中断 API），实现改为：
  - C6 收到手机命令 → 帧入从机 TX 队列 → `spi_slave_hd_write_buffer()` 更新 `TX_PENDING` 寄存器 → 拉高 IRQ；
  - TC275 泵每圈采样 P23.0：**线为高则立刻发起寄存器快照与排空读**（等价于原事件驱动，只是粒度受超循环节拍限制）；
  - 线为低时仍以 **2 ms 保活轮询**兜底（`LINK_KEEPALIVE_MS`）。这一条不是"备份路径"而是**主路径**：上拉缺失、杜邦线松动、C6 未接时链路仍按 2 ms 节拍工作，只是时延退化到 2 ms 档（§6 预算内）。
- **上行（遥测）**：TC275 按 20 ms 节拍主动发起写事务，与排空读在同一泵里按 `读 > 写` 优先级排（§4.2 序 2 先于序 3）。

### 4.2 一次"链路周期"的事务序列（CPU2 泵）

```
loop:
  1. 读寄存器组（4 个 u32，一次事务打包）→ 连读两次取相同值（E5）
       READY_FLAG / TX_PENDING / RX_ROOM / ALIVE
  2. TX_PENDING > 0  → 发 RDDMA 事务读 ceil(len/4)*4 字节 → 解 SF 段 → 入命令队列
  3. 有待发帧(遥测/ACK/OTA) 且 RX_ROOM ≥ 帧长 → 发 WRDMA 事务写
  4. ALIVE 未在 500 ms 内推进 → 报 ERR_LINK_LOST（等价 demo 的心跳判据）
  5. 任一事务超时（CS 有效期内无有效 MISO 回读 / 状态寄存器回 0x00 或 0xFF）→ 计 SPI_ERR，连续 N 次判链路故障
```

### 4.3 共享寄存器映射（C6 从机发布，主机读；每项 u32 小端，地址 = 4×序）

| 地址 | 名称 | 语义 | 谁更新 |
|---|---|---|---|
| 0 | `SF_READY` | 就绪魔数 `0x5F534601`（"_SF1"）；未就绪时主机不得发数据事务 | C6 初始化末尾一次 |
| 4 | `SF_TX_PENDING` | 主机可读字节数（从机→主机方向待出队总长） | C6 每次入队/出队 |
| 8 | `SF_RX_ROOM` | 从机可接字节数（主机写入不得超过） | C6 每次消费/追加 |
| 12 | `SF_ALIVE` | 从机心跳计数器，10 ms 递增 | C6 esp_timer |
| 16 | `SF_ERRSTAT` | 从机侧链路错误位图（位值见下方列表，**各自粘滞**） | C6 |
| 20 | `SF_CMDRSP` | 主机 `GEN` 命令的执行回执，编码 `cmd \| result<<8`（`result`：`0=OK`，`1=UNKNOWN`） | C6 |
| 24 | `SF_GEN` | **唯一由主机写的寄存器**：`WR_REG`(WRBUF) 写 4 B `{cmd, p0, p1, p2}`，`p = p0\|p1<<8\|p2<<16`（24 bit） | TC275 |

- `SF_ERRSTAT` 位值（E13，从机 `sf_frame.h:95-101`）：`CRC 0x01`（帧 CRC 失败）、`FMT 0x02`（帧格式/VER 错）、`SEQ 0x04`（SEQ 前进窗口违例）、`RXOVFL 0x08`（RX 队列溢出/丢帧）、`TXOVFL 0x10`（TX 队列满 BUSY）、`TRUN 0x20`（跨段残帧）、`LINKLOST 0x40`（连续 5 次 CRC 失败）。主机 `Middleware/com/link.h` 的 `LINK_ERRSTAT_SLAVE_*` 与此逐项对齐，G1 判读只看这七位。
- 从机发布区共 **28 B**（含 `GEN` 槽回显），主机每次 `RD_REG` 只读前 **24 B**（偏移 0..23）——读的是前缀，不冲突；`GEN` 是主写槽，主机读回它没有意义。
- **`GEN` 命令表与时序（E14，主机侧 `LINK_gen()` 已按此实现）**：`NOP 0`（探针）、`RESET_LINK 1`（从机重置解析器与 RX SEQ）、`SILENCE_ON 2` / `SILENCE_OFF 3`（强制/恢复 IRQ 静默，G4 的台架注入手段）、`CLOCK_SET 4`（**payload 单位 MHz**，从机只记入诊断回显，真正的时钟归主机）。回执**跨两次从机任务调度**才可见，故 `LINK_gen()` 必须带超时（`LINK_GEN_TIMEOUT_MS = 20`），超时计 `stats.genNoAck`。因 `NOP` 走从机 `default` 分支（回执 `result=UNKNOWN`），`LINK_gen()` 的用法是：**先写一发 NOP 清掉粘滞的旧回执，再发真命令**，这样"重复下发同一条命令"也不会被旧回执假命中。
- 前 5 项与官方 `segment_mode` 例子的 5 寄存器一一对应（E4），只是字段语义按本产品重定义；例子的读法（连读两次相同）作为硬性规范继承（E5）。
- 命令/地址相位取值：`command_bits=8, address_bits=8, dummy_bits=8`（E3），低 4 位需命中 E6 的 CMD7/8/9/A 事件编码。**编码表已按 E12 的源码证据冻结**（不再等 G1 猜测）；G1 要验的是"TC275 用数据相位模拟出的这段前导，从机能否正确解析"，而不是"取值对不对"。

### 4.4 事务类型（取值已按 E12 冻结，见 `Middleware/com/spi_hal_pins.h`）

| 事务 | 方向 | CMD 字节 | ADDR 字节 | 数据相位 | 收尾事务 | 用途 |
|---|---|---|---|---|---|---|
| `RD_REG` | 主读 | `0x02` RDBUF | 寄存器**起始字节地址**（0，一次读满 §4.3 的 24 B） | 4×N 字节 | 无 | 轮询寄存器组 |
| `WR_REG` | 主写 | `0x01` WRBUF | 寄存器起始字节地址 | 4×N 字节 | 无 | 主机向从机寄存器区下发命令（`GEN` 用这条，不再另立编码） |
| `RDDMA` | 主读 | `0x04` RDDMA | `0x00`（段内偏移由从机硬件自增） | N ≤ 260，4 的倍数 | **必须补一次 `0x08` INT0** | 取走 C6→TC275 帧 |
| `WRDMA` | 主写 | `0x03` WRDMA | `0x00` | N ≤ 260，4 的倍数 | **必须补一次 `0x07` WR_END** | 下发 TC275→C6 帧 |

- 前导 = 同一次片选有效期内连续三个 8 位数据字 `[CMD][ADDR][DUMMY=0x00]`（E8 的模拟方案）；`SPIHD_PREAMBLE_LEN = 3`。
- 读事务的 MISO 前 3 字节是从机在前导期的回送，不含载荷，驱动层已丢弃。
- **收尾事务是硬要求**：Espressif 主机驱动在 RDDMA/WRDMA 突发结束后固定发 `INT0`/`WR_END`（E12），从机以此释放缓冲/落段。少发一次就把从机卡死，且现象与"从机挂死"无法区分——G2 回环必须先验证这一点。
- 段长上限 260 B = 一帧 padded 最大值（§5.1），对应 `SPIHAL_MAX_DATA`；超过 `TX_PENDING` 声明值时按 4 向上取整再截断。

---

## 5. SF 帧（SPI 链路专用，取代该段上的 v2 帧）

### 5.1 格式

```
偏移  0      1     2     3     4      5-6      7      8 .. (8+LEN-1)   尾 2 B
| MAGIC | VER | TYPE | SEQ | FLAGS | LEN(u16LE) | CID  |   载荷    | CRC16 |
  0x5A   0x01  见5.2  见5.3  bit0-1   ≤248       通道/命令 ID  CRC16-CCITT-FALSE(前 8+LEN 字节)
段对齐：整帧（含 CRC）末尾补 0x00 到 4 的倍数；一个 SPI 数据段可串接多帧，帧间偏移由 LEN 递推
```

**FLAGS 位定义（E13：与从机 `c6_sf/sf_frame.h:37-39` 逐位同值）**

| bit | 名称 | 含义 | V1.0 |
|---|---|---|---|
| 0 | `FRAG` | 续片，本帧未结束 | 不使用 |
| 1 | `FRAG_END` | 末片 | 不使用 |
| 2..7 | 保留 | — | **必须发 0** |

- **"ACK" 不是 flag**：确认是 `TYPE=0x03` 的一种帧（§5.2），占用 bit1 会把分片语义与确认语义混掉，主机初版正是这么写的，已按从机改回 `FRAG_END`。
- **V1.0 一定不分片**：`LEN ≤ 248` → 整帧 `8+LEN+2` 补齐 4 的倍数后 **≤ 260 B，正好等于 §4.4 的单段上限**，一帧必然落在一个段内，所以 `FRAG/FRAG_END` 恒为 0。副作用要写清：**最长帧独占一段**，"段内多帧"只对短帧（如 38 B 遥测 → 48 B 线上）有意义，G5 的 OTA 吞吐估算按 260 B/段 就是单帧一段，别按多帧打包算。
- 保留位必须为 0 的硬理由：FLAGS 参与 CRC 覆盖（从机 `sf_frame.c:143` 原样写入 `out[4]`），主机自造一位只会换来一次 CRC/格式错，换不来任何功能——所以主机侧连"本地自用"的位定义都不保留（原 `SF_FLAG_RESYNC 0x04` 零引用，已删）。

设计对应关系：
- **MAGIC/VER/TYPE/LEN/CRC16** 继承 v2 的语义（便于 `mw/proto` 解析器骨架与主机单测复用）；
- **4 字节对齐 + 段内多帧 + `FLAGS.FRAG`** 是为半双工 DMA 段新增的，v2 里不需要；
- **无帧尾"整帧长度字节"歧义**：LEN 只数字节载荷，MAGIC 唯一（`0x5A`），解析器状态机遇到非法字节即重同步。

### 5.2 TYPE 与 CID 通道

| TYPE | 名称 | 方向 | CID 取值 | 说明 |
|---|---|---|---|---|
| 0x01 | CMD | C6→TC275 | 0x01 驾驶 / 0x02 配置 / 0x03 诊断 / 0x04 产测 / 0x05 配对 | 命令类**不丢**（队列满则反压 + 告警） |
| 0x02 | TEL | TC275→C6 | 0x10 遥测 | 遥测类**丢旧留新**（SDD §5.5 策略） |
| 0x03 | ACK | 双向 | 被确认帧的 TYPE | 携带被确认 SEQ；OTA/产测命令必需 |
| 0x04 | HBT | 双向 | — | 心跳，兼作链路 RTT 测量（主机在事务边界取时间戳，比 UART 时代更准） |
| 0x05 | EVT | TC275→C6 | 0x20 错误 / 0x21 状态迁移 | 错误码/状态变化即时上报 |
| 0x06/0x07 | OTA_D / OTA_C | 双向 | `0x06`：0x30 BEGIN / 0x31 CHUNK / 0x35 ABORT；`0x07`：0x32 ACK / 0x33 STATUS / 0x34 SWAP | SDD §9 的 OTA 通道。CID **不是**连续的 0x30..0x34：`ABORT=0x35` 挂在数据 TYPE 上，`ACK/STATUS/SWAP` 挂在控制 TYPE 上（E13，与从机 `sf_frame.h:70-75` 同值） |
| 0x08 | DBG | TC275→C6 | 0x40 日志环快照 | 诊断导出用，可丢 |
| 0x0F | VND | 双向 | — | 厂商/保留 |

**`TYPE=0x01` 各 CID 的载荷形状（关键：五个通道三种形状，从机 `c6_link/link.c:v2_to_sf` 实况）**

| CID | 名称 | 载荷 | 首字节是不是命令码 |
|---|---|---|---|
| 0x01 | DRIVE | `{u8 op, i16 v, i16 w}` | **是**（v2 命令码，§5.5） |
| 0x03 | DIAG | `{u8 op, ...}`，`op` = 0x53 DIAG / 0x42 LINK_STATE | **是** |
| 0x04 | DPT | `{u8 op, ...}`，`op` = 0x70..0x79 | **是** |
| 0x02 | CFG | v2 `0x52` 载荷**原样**（`{k, v...}`） | **不是**——从机已把 op 剥掉 |
| 0x05 | PAIR | v2 `0x51` 载荷**原样**（`token[16]`） | **不是**——同上 |

> 这张表是 §5.5 分派规则的**依据**：如果主机按"payload[0] 总是命令字节"处理所有 CMD 帧，一条 `CFG` 配置项的键号、一条 `PAIR` token 的首字节就会被当成 `PROTO_CMD_*` **真的执行**（例如 token 首字节 0x20 会被当成 `GET_STATUS`，0x02 会被当成"前进"）。这不是理论风险，是初版代码的真实缺陷，已按 CID 白名单修掉，见 §5.5。

手机 WS 侧仍是 §6 v2 帧；C6 的 `bridge` 做 **v2 ↔ SF 的字段级映射**（命令码、遥测字段一一对应，只是编码容器不同）。映射表在本文件 §5.5 与 SDD §6 各留一份，互为镜像。

### 5.3 SEQ / E2E

- 每方向独立 1 字节 SEQ，单调递增（mod 256），接收侧**严格前进窗口** `1 ≤ (seq - last) ≤ 32`（沿用 c6_car `ws_sessions` 的窗口思路，拒绝旧帧与超窗洪水）；
- 命令方向：SEQ 不前进即拒收（重放防护，SDD §8.1 的"帧内 SEQ 单调 + 会话 nonce"保留）；
- 链路级 E2E 由 §4.3 的 `SF_ALIVE`/`SF_TX_PENDING` 寄存器 + SF 帧 SEQ **双层**承担：寄存器保证"数据在不在"，SEQ 保证"这一帧新不新"。

### 5.4 失败行为

| 现象 | 检测 | 处置 |
|---|---|---|
| 帧 CRC 失败 | 解析器 | 丢弃 + `SF_ERRSTAT`/SPI_ERR 计数；连续 5 帧失败 → `ERR_LINK_LOST` |
| SEQ 回退/超窗 | 前进窗口 | 丢弃 + 计数（不锁存，区别于安全故障） |
| 段内残帧（跨事务未收完） | 状态机超时 4 ms 无续字节 | 丢弃残段，重同步到 MAGIC |
| IRQ 线读不到（上拉缺失/线松） | 电平轮询本身就是主路径（E11） | 无感（时延回到 2 ms 档，吞吐不变） |
| 寄存器连读两次不等 | §4.3 规范 | 重试 3 次，仍不等 → 判从机状态不可信 → `ERR_LINK_LOST` |
| 从机挂死/掉电 | `SF_ALIVE` 500 ms 不推进、读回 0x00/0xFF | 主机事务超时 → `ERR_LINK_LOST` → mission 目标置零受控停车（**不导致失控**，SDD §3.1 原则成立） |
| 主机泵挂死 | CPU2 独立看门狗（SDD §7.2 不变） | 复位 CPU2，链路断 → 停车 |

### 5.5 v2（手机/WS）↔ SF 字段映射（摘要）

| v2 CMD | SF TYPE/CID | 变化 |
|---|---|---|
| 0x50 DRIVE | 0x01/0x01 | 载荷不变（`{op, v:i16 mm/s, ω:i16}`）；但 **TC275 当前拒收此 op**（见下表与 §5.5），原因是 mm/s→两轮目标需要 §11 运动学 + 编码器闭环，本构建未落地 |
| 0x02..0x05 方向档 | 0x01/0x01 | 子码区分 |
| 0x10 SET_SPEED | 0x01/0x01 | `v/w` 为 **±100 百分比**（21 §6.2、11 §7），不是 mm/s；主机窄化成 2 字节 `sint8` 复用旧命令容器，无损 |
| 0x41 遥测 | 0x02/0x10 | **38 B 逐字节不变**（从机用 v2 解码器解，不复建表）；字段名保留 `linkErrRate`，语义换成 SPI 口径 = `(crcErr+seqErr)` / 已收帧，单位 0.1% |
| 0x44 BAUD REQ/ACK | **删除** | 时钟档位改由 `GEN` 事务 + 产测/诊断命令控制，不再运行时自适应 |
| 0x60..0x6F OTA | 0x06/0x07 | CHUNK 载荷上限由 62 B 提到 **240 B**（SF LEN ≤248）→ 1 MB 镜像分片数从 16k 降到 4.3k |
| 0x70..0x7F 产测 | 0x01/0x04 | 不变 |

**TC275 侧已实现的分派规则**（`Middleware/com/link.c:link_dispatch`，2026-09-26 按已烧录从机固件重写）：**不存在"payload[0] 就是命令字节"这一条通用约定**——上一版文档这么写，而 §5.2 表里 `CFG`/`PAIR` 两个通道的首字节并不是命令码，照那句话实现就会把配置键号或配对 token 当成命令执行。现行规则是**三层白名单，任何一层不过就整帧拒收并计数，绝不按偏移猜**：

1. **TYPE 白名单**：只有 `0x01 CMD` 会被当作命令；其它 TYPE（TEL/EVT/DBG…是 TC275 自己发的方向）落到 `stats.unhandledType`。
2. **CID 白名单**：CMD 里只认 `DRIVE(0x01)`、`DIAG(0x03)`、`DPT(0x04)` 三条"带 op 前缀"的通道；`CFG(0x02)`、`PAIR(0x05)` 的首字节不是命令码，落到 `stats.cmdUnsupportedCid` 直接丢——**本工程 V1.0 不消费这两条通道**，将来要用必须按 §5.2 载荷表单独解析，不能复用命令入队路径。
3. **长度/取值校验**：`LEN < 1` → `cmdBadLen`；`CID=DRIVE` 要求 `LEN == 5`（`{op,i16,i16}` 形状由发送端固定，不符即版本错配，按偏移解出来的是错命令而不是"部分命令"）→ 否则 `cmdBadLen`；`LEN-1 > PROTO_MAX_PAYLOAD(16)` → `cmdOversize`（**不截断**，截断会造出"看起来合法但语义错"的命令）。

过检后的入队方式，按 op 分类（`Middleware/protocol.h` 的 `PROTO_CMD_*` 仍是命令码唯一真源）：

| op | 处理 | 依据 |
|---|---|---|
| `0x20 GET_STATUS` | **不入队**，直接返回 | 状态已由 20 ms 遥测流持续应答（§5.5 下条），CPU0 再解一次是重复工作 |
| `0x10 SET_SPEED` | 载荷里两个 `i16` 是 **±100 百分比**（21 §6.2、11 §7），窄化成 2 字节 `sint8` 后按现有 UART 时代的命令容器入队 → **窄化无损** | 单位域由文档确认过，不是猜的；`-100..100` 落得进 `sint8` |
| `0x50 DRIVE`（v mm/s + ω） | **拒收并计 `cmdUnsupportedOp`** | 本构建没有 §11 运动学与编码器闭环（21 §5.4/§11 未落地），收到就会算出错误的两轮目标。这是"能力未就绪"的诚实拒绝，不是协议缺陷 |
| 其余离散命令（0x01..0x09、0x30..0x32） | `{v,w}` 恒为 0，无可携带参数 → 只转发 op，`link_forward(op, NULL, 0)` | 从机对非 DRIVE/SET_SPEED 的 op 固定填 0/0（§5.2） |
| `DIAG`/`DPT` | 转发 op + 原样数据 `payload[1..]` | 这两条通道确实带 op（§5.2） |

- **demo 状态里的 `heartbeatOk` / `emergencyStop` 两个布尔在 38 B 布局中没有槽位**（从机解码器的字段表是固定的，不能私自加字节）。它们的语义由 `state` + `faultCode` 承载：急停在 `App/robot.c:ROBOT_cmdEmergencyStop()` 里就是 `state=ROBOT_STATE_FAULT(0x0A)` + `fault=EMERGENCY_STOP`，心跳超时同理。**手机页面要判"是否急停"就读 `state`，不要再找独立标志位**。
- **遥测方向（TC275→C6，`TYPE=0x02 / CID=0x10`）的 38 字节载荷是硬契约**：偏移表见 SDD §6.3 与 `Middleware/sf/sf_telemetry.h`，与 v2 `0x41` **逐字节相同**（从机不复建表，直接用 `proto_frames.c:proto_telemetry_decode` 解）。从机对 TEL 帧的接受条件是"CID 正确**且长度 ≥ 38**"（`c6_link/link.c:sf_to_v2`），所以短载荷不是"遥测少几个字段"，而是**整帧被丢弃、手机页面无数据**——主机初版发 6 字节正是这个故障形态，现已改为 38 字节并由 `test/host/test_sf_telemetry.c` 编译从机解码器做双向交叉验证。`linkErrRate` 在 SPI 口径下由 `crcErr + seqErr` 对已收帧数算出（0.1% 单位）；`linkRttMs` 需 `HBT` 事务打点才有真值，当前填 0。
- **急停旁路在 `link_forward()` 里、白名单之后**：`op == 0x32 (EMERGENCY_STOP)` 时先落 `XCORE_estopRequest()`（不等 CPU0 出队）再照常入队。这条路径是通的，因为从机把 16 条驾驶族命令（含 `0x30/0x31/0x32`、`0x20/0x21`）**全部**送进 `CID_DRV` 且固定 `LEN=5`（`c6_link/link.c:v2_is_drv`，已逐条核对）；也就是说急停**只能**走 CMD/DRIVE 这一条路，`CFG`/`PAIR` 通道里没有急停，白名单挡掉它们不会挡住急停。
- CPU0 命令队列满 → 计数 `cmdRejectedQueue` + 一条日志，**不覆盖旧命令**；该计数器必须为 0，否则说明 §5.2 的"命令不丢"没做到。

---

## 6. 性能与资源预算（替换 SDD §14 的链路行）

| 项 | 数值（5 MHz 基线） | 说明 |
|---|---|---|
| 单段事务开销 | 前导 3 B + 段 N，半双工单向 | 128 B 段 ≈ 210 µs @5 MHz |
| 一个链路周期 | 轮询 + 读 + 写 ≈ 3 事务 ≈ 50–300 µs | 事件驱动时只在有数据时才凑满 |
| 有效吞吐 | ≈ 550 KB/s（含寄存器轮询与事务间隙） | 20 MHz 时 ≈ 2 MB/s |
| 稳态链路占用 | 命令 30 Hz×16 B + 遥测 50 Hz×44 B ≈ 2.7 KB/s → **<1%** | 与 UART 时代同量级（UART 2 Mbps 也是 <2%） |
| **命令端到端（触屏→车轮）** | ≈ WS 1–2 + C6 入队 <1 + **IRQ+读事务 ≤2.3** + xcore 1 + mission 2 + servo 1 ≈ **8–10 ms** | 预算 ≤50 ms，**余量 5 倍** |
| 遥测端到端 | 20 ms 聚合 + ≤230 µs 事务 + WS 广播 | 满足 SDD §3.5 |
| OTA 1 MB | 传输 ≈ 2 s + 校验/编程 ≈ 1 s → **≤3 s**（5 MHz） | SDD §2.2 原 ≤30 s 指标可收紧为 ≤10 s（含握手与回滚余量） |
| TC275 CPU2 负载 | 事务发起 + DMA/FIFO 搬运 ≈ 5–8% | SDD 预算 10% 内 |
| C6 CPU 负载 | 从机中断 + 回调 ≈ 3–5% | 低于 UART@2 Mbps 的中断风暴 |
| RAM | TC275 CPU2：段缓冲 2×256 B + 寄存器镜像 32 B；C6：DMA 段 2×512 B | 与原 2×2 KB UART 环同量级 |

**诚实的代价**：主机拉取模型让"命令下行最坏时延"从 UART 的 push（~1 ms）变成 `轮询周期 + 事务`（≤2.3 ms）。在本产品 50 ms 预算下无关紧要，但**必须写进文档**，避免后续误以为 SPI 全面优于 UART。

---

## 7. 两固件改动清单（**两侧代码均已落地**，逐行状态见"实测"列；剩余未做项 = G1 台架门禁）

### 7.1 C6 侧（`c6_car`）—— **已完成**（提交 `22e15f2` 链路换向 + `3f464af` 真机缺陷修复，另有 `c6_car/doc/` 同步）

| 文件 | 动作 | 实测状态 |
|---|---|---|
| `components/c6_link/link.c` | 传输层 UART → `spi_slave_hd`：删 `uart_*`、波特率协商状态机（0x44）、`LINK_UART_NUM`；新增 SPI2 从机初始化（`command/address/dummy=8`、`queue_size`、DMA 通道）、§4.3 寄存器发布、IRQ 脚驱动 | **已完成**：0x43/0x44 在代码里只剩一条"已删除"注释；段缓冲 `LINK_SEG_SIZE=512`、`LINK_RX_BUFFERS=2`；IRQ 为**电平**语义（有帧待取则拉高，`SILENCE_ON` 可强制静默作台架注入） |
| `components/c6_link/Kconfig` | 删 `C6_LINK_TX/RX_GPIO`（顺带消除与 23-wiring.md §2 GPIO6/7 的既有不一致），增 `C6_LINK_SPI_*_GPIO`（默认 19/18/20/23/21）、`C6_LINK_SPI_CLOCK_HZ`、`C6_LINK_UART_DEBUG`（控制台） | **已完成**：SPI 五线默认值与 23 §9.1 逐脚一致；`C6_LINK_DEBUG_UART_RX/TX_GPIO` 显式固化为 **6/7**（原 10/11 的不一致随之消除）。遗留：`C6_LINK_SPI_IRQ_GPIO` 的 help 文本仍写 "IOM edge IRQ"，与本文 E11（TC275 做不出边沿中断）不符，属 C6 仓库文档瑕疵，待该仓库下轮修正 |
| `components/c6_link/link.h` | 对外 API（`LINK_send/recv/health`）**签名不变** → `bridge`/`ota_relay` 不感知物理层 | **已完成** |
| `components/c6_sf/`（新增） | SF 帧编解码（纯 C，主机端可测）；v2↔SF 映射 | **已完成**，但映射位置与原计划不同：落在 `c6_link` 内部而非 `c6_bridge`（差异记录见 `c6_car/doc/14-sf-link.md` D1） |
| `components/c6_ota/*` | CHUNK 上限 62 B → 240 B（同步 §5.5）；分片计数逻辑随之简化 | **已完成**（`link_send_ota_chunk` 快路径） |
| `c6_car/doc/04-link.md`、`02-proto.md`、`08-bridge.md` | 同步（UART 段落改为 SPI；波特率协商章节删除） | **已完成**（`b8e12bf`） |

**两侧兼容性静态核对（2026-09-26 第一轮，源码比对）**：`Middleware/com/link.h` 与 `components/c6_sf/sf_frame.h` 逐项比对结果为——寄存器偏移 `READY/TX_PENDING/RX_ROOM/ALIVE/ERRSTAT/CMDRSP = 0/4/8/12/16/20` 一致、`SF_READY_MAGIC = 0x5F534601` 一致、`SF_MAX_PAYLOAD = 248` 一致、ALIVE 步进 10 ms 对 500 ms 判活窗口一致、主机单段 260 B ≤ 从机段缓冲 512 B 一致。主机侧一次 `RDBUF` 读 24 B（偏移 0..23），从机发布 28 B（含 `GEN` 槽回显）→ 读的是前缀，不冲突。**这只是源码比对，不等于波形通**（G1 未做）。

**同一轮比对的失败项（诚实记录，E13）**：上一段那句"逐项一致"当时是**按我列的清单**比对的，结论不完整——把从机头文件的**全部**常量拉过来逐行比之后，查出三处主机侧臆造/错位（`FLAGS` bit1 名、OTA CID 缺 `0x35`、`ERRSTAT` 位图整张表），另查出两处**行为**级缺陷（分派不看 CID、遥测只发 6 B）。教训已固化成两条做法：① 常量对齐必须"整表覆盖"而不是"抽查关心的项"；② 跨侧布局由 `test_sf_telemetry.c` 那种**直接编译对方源码**的测试来保证，文档承诺不算验证。

**主机侧 `GEN` 下发（原"尚未实现"项，现已补齐）**：`LINK_gen()` / `LINK_setClock()` 已落地（`Middleware/com/link.c`），语义与时序见 §4.3 与 E14 —— `WRBUF` 写 `SF_REG_GEN(24)` 4 B，NOP 探针清粘滞回执，20 ms 超时计 `genNoAck`。这解锁了 **G4 的 `SILENCE_ON` 故障注入**（台架上可强制从机不拉 IRQ）与 G5 的**运行时档位镜像**（`CLOCK_SET` 只让从机记录诊断，时钟真值仍归主机）。

### 7.2 TC275 侧（`myCar`）—— **2026-09-26 已落地，状态见"实测"列**

> 目录名按本工程既有布局落在 `Middleware/` 下（原稿写的 `com/`、`mw/sf/` 是 SDD 目标态目录，`Middleware/` 已是本工程 include 根，故未新建顶层目录）。

| 文件 | 动作 | 实测状态 |
|---|---|---|
| `Middleware/sf/sf_frame.c/.h` | SF 帧编解码（纯 C99，无 OS/iLLD 依赖，TriCore/RISC-V/主机三目标同份源码） | **已完成**：`test/host/test_sf.c` 2855 项断言 0 失败（含 0..248 全长度往返、段内多帧、乱字节重同步、SEQ 窗口与回绕、残帧超时、400 万字节随机风暴 + 金库哨兵未越界）→ **门禁 G2 的帧层部分已过**。本轮按 E13 把 `FLAGS`/OTA CID 与从机对齐，并删掉零引用、从机也没有的 `SF_FLAG_RESYNC 0x04` |
| `Middleware/sf/sf_telemetry.c/.h`（新增） | 38 B 遥测载荷 codec（纯 C99、显式小端、不做结构体强转——TriCore 大端） | **已完成并验证**：`test/host/test_sf_telemetry.c` 把**从机自己的 `proto_frames.c` 编进同一可执行文件**做双向交叉（我编→从机解 / 从机编→我解，含"短于 38 B 必须拒解"），`154 断言 / 0 失败`，`-Wall -Wextra -Werror` 干净。全表仅此一条测试直接证明"两侧字节一致"，其余都是源码比对 |
| `Middleware/com/spi_hal_pins.c/.h` | QSPI3 主机：E7 符号集中、§4.4 前导模拟、时钟档位表、一次裸事务。**ISR 声明在 0 号向量表**（SDD §18 C1），优先级 **TX=6 / RX=9 / ER=10**（C2 表中 CPU2 空档，避开 ASCLIN1 的 5/7/13） | 已完成，未经 IDE 编译（本机无 TriCore 工具链）。已核对：`channelBasedCs=disabled` + `mode=short` 下 iLLD 全程保持 CS 有效（"begin stream" BACON + 末字 LAST=1，`deactivateSlso()` 仅在 `rx.remaining==0` 时执行）→ 前导与数据在同一次片选内 |
| `Middleware/com/link.c/.h` | §4.2 泵：寄存器连读两次取稳定快照（重试 3 次）、`TX_PENDING` 门控 RDDMA 突发 + `INT0` 收尾、`RX_ROOM` 门控 WRDMA 突发 + `WR_END` 收尾、`SF_ALIVE` 500 ms 判活、P23.0 **电平轮询 + 2 ms 保活**（原稿"IOM 中断"作废，E11）、§5.5 命令入队；本轮再加 `LINK_gen()`/`LINK_setClock()`（§4.3 + E14）、`LINK_sendTelemetry()`、按 TYPE/CID 白名单的 `link_dispatch` | 已完成（主机 `-Wall -Wextra -Werror` 语法干净）；`Link_Health` 作为 G1/G5 的观测点（CPU2 无可打印串口，见 SDD §18 C3）。**本轮修掉的自身缺陷**：① 分派不看 CID（会把 `CFG`/`PAIR` 首字节当命令执行，§5.2）；② `txFrames` 按"段"而非"帧"计数，G5 的吞吐判据会虚高；③ `ERRSTAT` 位表臆造（E13）；④ 遥测 6 B（E13/§5.5）。新增观测计数器：`cmdBadLen`/`cmdUnsupportedCid`/`cmdUnsupportedOp`/`unhandledType`/`genWrites`/`genNoAck`，G1 台架上这几个**必须全 0 或可解释** |
| `Cpu2_Main.c` | 挂载 `LINK_init(SPIHAL_CLK_1M)` + `LINK_main()` 超循环 + 20 ms 遥测发送 | 已完成，**默认仍走 demo UART 路径**，SPI 路径需 `-D USE_SPI_LINK` 打开（G1 未过前不改默认，见下表 `wifi_at.c` 行）。遥测已改填完整 `SF_Telemetry`：**有真源的**只有 `seq`/`uptimeMs`/`state`/`faultCode`/`fwVer`(0x00010200)/`linkErrRate`；**其余显式填 0 并在代码里逐条注明缺什么**——`v_target/v_meas` 缺编码器测速与 §11 运动学、`battery_mv/pct` 缺 ADC 通道、`odo_*` 缺编码器累计、`linkRttMs` 缺 HBT 打点、`hwRev` 缺板级标识来源。手机页面据此显示 0 是**如实**，不是丢包 |
| `.cproject` | 两个 TASKING 配置解除 `Libraries/iLLD/TC27D/Tricore/Qspi{,/.Std,.SpiMaster}` 与 `Dma{,/.Dma,.Std}` 排除项（`Qspi/SpiSlave` 仍排除）；`test/` 加入排除（主机单测不得进 TriCore 构建） | 已完成，**待一次 IDE 构建确认链接闭合**（`IfxQspi_SpiMaster.c` 即使 `useDma=FALSE` 也引用三个非内联 `IfxDma_Dma_*`，故 Dma 必须一并放开） |
| `Middleware/wifi_at.c` | 量产构建默认关闭（`USE_WIFI_AT`），保留为 G1 失败回退与产线返工通道 | **未做**：当前是反向开关（默认 UART，`USE_SPI_LINK` 才切 SPI）。G1 通过后翻转默认值，同步改 SDD §5.6 末条（该条已按此口径写明"现状与目标态相反"） |
| `Middleware/protocol.c` | 不再是 LINK 帧真源；命令语义迁移进 `mw/sf` | 保持不动：命令**码表**仍是唯一真源（`link.c` 把白名单通道 `CMD/DRIVE·DIAG·DPT` 的 op 字节直接当 `PROTO_CMD_*` 用，`CFG`/`PAIR` 不参与，§5.2/§5.5），被取代的只是 UART 时代的 `AA 55` 容器 |
| [23-wiring.md](../20-design/23-wiring.md) §9、§2 | §9 标题从"预研方案，待拍板"改"V1.0 选定"；§2 标注 UART 降级为调试/回退通道 | 已随 V1.4 接线更新完成；V1.5 再按本文 E11/E12 改握手与 §9.2 落地状态 |

### 7.3 不在本轮范围

- 不碰 PWM/编码器/电机接线（P33.0~7 与 P33.11~13 无冲突，但编码器 §8 的确认仍独立待办）；
- 不做 20 MHz 承诺（先 5 MHz 基线，档位提速以误码数据说话）；
- 不动手机/WS 协议（v2 对外语义保持）。

---

## 8. 验证计划与门禁（逐档，不可跳级）

> **执行状态（2026-09-26 第二轮）**：**G2 的"主机单测"子项已过两件事**——① SF 编解码（`test/host/test_sf.c`，2855 断言 / 400 万随机字节）；② 38 B 遥测布局（`test/host/test_sf_telemetry.c`，154 断言，且是把从机 `proto_frames.c` 编进来做双向交叉，这条才算跨侧证据）。**其余门禁全部未执行**。前置条件**只剩一件**：TC275 侧一次 TASKING IDE 构建（确认 QSPI+DMA 链接闭合，§7.2）——上一轮列的"缺 `LINK_gen()`"已补齐（§7.1 末），G4 的注入手段在软件侧不再欠账。

| 门禁 | 内容 | 通过判据 | 失败动作 |
|---|---|---|---|
| **G1 波形兼容（唯一硬风险，E8）** | 两台 ESP32 跑官方 `seg_master/seg_slave` 抓参考波形作基线；再由 TC275 QSPI3 主机以数据字节模拟 `CMD+ADDR+DUMMY`，C6 从机 HD 收 | C6 侧 `RD_REG`/`WRDMA` 均能拿到正确字节、CMD7/8/9/A 事件按预期触发 | ① 换 C6 自写寄存器级从机驱动（去掉 CMD/ADDR，纯数据相位 + 固定段长）；② 仍不行则回退 UART，本方案作废并回写 SDD |
| G2 事务正确性 | SF 编解码主机单测 + 遥测布局交叉单测 + 板端回环（自环 CS 或第二块 TC275） | 10⁷ 随机字节不崩、不越界；回环 CRC 误码 0 | 修解析器，不进下一步 |
| G3 链路时延 | 触屏→TC275 收到命令打点 | 最坏 ≤5 ms（握手为电平轮询，判据含"P23.0 拉高后 ≤ 一个泵节拍"） | 调轮询周期 |
| G4 安全语义 | 拔 C6 电源 / 拉 GND 断 IRQ / `GEN SILENCE_ON` 注入（**主机侧 `LINK_gen()` 已可用，§7.1 末**；注意 CPU2 无串口（SDD §18 C3），注入需在 `Cpu2_Main.c` 临时加一次调用点并单独构建，不是"发条命令就能触发"） | `ERR_LINK_LOST` ≤520 ms 内、受控停车 ≤100 ms | 阻断发布 |
| G5 提速阶梯 | 1→2→5→10→20 MHz，各档 30 min 连续 + OTA 全流程 | 每档 CRC 误码率 = 0（安全相关）；OTA 1 MB ≤3 s @5 MHz | 停在上一档，量产基线取实测最高稳定档 |
| G6 老化 | 72 h 连续（SDD §2.2） | 0 意外复位、链路错误计数不增长 | 排查 DMA/中断优先级 |

---

## 9. 风险与开放问题（SDD §16 增量）

| # | 事项 | 影响 | 处置 |
|---|---|---|---|
| R7（新增，取代 R4 的主风险位） | **E8：AURIX QSPI 无 CMD/ADDR 相位，用数据字节模拟能否被 Espressif HD 从机正确解析未证** | 方案根本可行性 | G1 前置一次性验证；已备两条退路（自写从机驱动 / 回退 UART） |
| R8 | C6 从机 RX 须 4 字节对齐 + DMA 缓冲约束 | 帧/段设计 | 已通过"段末补 0 到 4 倍数 + LEN 只数字节载荷"吸收（§5.1） |
| R9 | 寄存器读非原子（E5） | 主机误判待发长度 | 连读两次相同 + 重试 3 次 + 不可信即判失联（§4.3/§5.4） |
| R10 | C6 自身 OTA 写 flash 期间 SPI 从机是否掉事务 | OTA 过程链路抖动 | 台架实测项；必要时 OTA 写块间隙插入 HBT 保活，或 OTA 期间限速/限动（对齐 SDD R3 的"OTA 中限速 50%"） |
| R11 | IRQ 开漏上拉缺失/杜邦线长 → **电平读错**（握手是轮询不是中断，E11） | 偶发空读事务（只耗轮询带宽）、命令下行时延退化 | 2 ms 保活轮询 + 寄存器连读校验兜底；量产线束定长屏蔽，产测勾选该脚静态电平 |
| R12 | **两侧常量表各自漂移**（E13 已实测发生三次：FLAGS bit1、OTA CID、ERRSTAT 位图）。协议改动后只改一侧、或文档与代码不同步，都会在台架上表现为"莫名 CRC/格式错"或"错误位读反" | 联调期定位成本，且可能只在提速档出现 | ① `sf_frame.h` 与从机 `c6_sf/sf_frame.h` 的常量必须**整表覆盖式**比对，不抽查；② 38 B 载荷由 `test_sf_telemetry.c` 编译对方源码来锁死，**改过 `Middleware/sf/` 或 `c6_car/components/c6_sf` 必须重跑**；③ 本轮起该测试与门禁 G2 绑定（§8） |
| Q3 | 20 MHz 是否值得（收益 vs 线束成本） | 量产成本 | 5 MHz 已满足全部指标；20 MHz 仅作探索档，不承诺 |
| Q4 | 是否要第二根握手线 `TX_RDY` | 时延微优化 | 本版不接，留焊位；G3 数据不支持再评估 |

---

## 10. 量产文档回写清单（**已按本方案执行**，供评审逐条核对）

| 文档 | 位置 | 改法 |
|---|---|---|
| SDD | 头部表 | 版本升 **V1.2**；修订记录追加本决策；上游文档加 [22-link-spi-design.md](22-link-spi-design.md) |
| SDD §1.1 | 不变量第 3 行 | `AA 55 CMD LEN DATA CRC` 从"继承不变量"降级为"**命令语义集合继承，帧编码另起 SF 帧**"（三不变量变两条半，须诚实标注） |
| SDD §1.3 | 术语 LINK | 改为 SPI（QSPI3 ↔ C6 SPI2，P33.11/12/13 + P23.4 CS + P23.0 IRQ）；新增 **SF 帧** 术语行 |
| SDD §2.2 | OTA 行 | `1 MB ≤30 s（2 Mbps≈6 s）` → `≤10 s（SPI 5 MHz，传输 ≈2 s）` |
| SDD §3.2 | 部署图 | `LINK: 2 Mbps 全双工帧` → `LINK: SPI 5 MHz 半双工事务 + IRQ 握手` |
| SDD §3.4 | `com/link.c` 注释 | `LINK 2 Mbps 非阻塞帧泵` → `SF 帧主机侧 QSPI 事务调度器（替代 wifi_at.c）`；目录补 `com/spi_hal_pins`、`mw/sf` |
| SDD §3.5 | 数据流 | `──LINK帧──►` → `──SF over SPI──►`；时延预算补"命令下行受主机轮询节拍约束 ≤2.3 ms" |
| SDD §3.7 | 全节 | 对比表"结论"列：SPI → **V1.0 选定**、UART → **回退/调试通道**；决策段改写为 SPI；删除"切换即按此落地，不做二次论证"的保留语，替换为本文 §3–§5 摘要 + 指向本文件；升级路径反转（V1.1 谈 20 MHz 与双线全双工/QUAD 预留） |
| SDD §5.6 | com/link | 整节重写为 §4.2 泵 + §5.4 失败行为 + 保留 `USE_WIFI_AT` 回退说明 |
| SDD §6 | 协议 | 拆 §6.1a（LINK 段 = SF 帧，含 §5.1 格式与 §5.2 TYPE 表）与 §6.1b（手机 WS 段 = v2 帧）；§6.3 遥测表 `linkRtt/linkErrRate` 语义更新为 SPI 口径；OTA CHUNK ≤240 B 同步到 §6.2 |
| SDD §12 | EMC | `LINK CRC16 + 重传` 保留，补"SPI 时钟辐射：GND 回流 + 定长线束，5 MHz 为 EMC 预扫基线档" |
| SDD §14 | 链路带宽行 | `LINK 占用 <2%` → `SPI 5 MHz 有效吞吐 ≈550 KB/s，稳态占用 <1%`；关键时延行补"命令下行 ≤2.3 ms（事务内）" |
| SDD §15 | M1 | `LINK 2 Mbps 打通` → `SPI 链路打通（G1 波形兼容为 M1 入口门禁）` |
| SDD §16 | R4 | 2 Mbps SI 风险作废，替换为本文 R7–R11（R4 保留为 UART 回退通道项） |
| SDD §17 | 映射表 | `Middleware/wifi_at.c` → `USE_WIFI_AT 回退通道（非量产链路）`；新增行：`Middleware/protocol.c → mw/sf`（SF 编解码）；`（无）→ com/spi_hal_pins.c`（QSPI3 主机引脚/时钟档） |
| 23-wiring.md | §9 | 标题"（C6 自研固件预研方案，待拍板）"→"（V1.0 选定链路）"；§9.2 实施要点按本文 §4/§5 重写，删"备忘，未开工" |
| 23-wiring.md | §2 | 标注"UART 降级为调试控制台/回退通道"；并修正既有不一致：c6_car `Kconfig` 的 `C6_LINK_TX/RX_GPIO` 默认 10/11 与本文 GPIO6/7 记录冲突，自研固件里统一为调试用途并显式写入 Kconfig help |
| SDD §18 | 新增 | 收入 demo 实测出的工程级铁律（0 号向量表、ISR 优先级全局分配、串口属主、裸机时基、SLSO5 唯一可用片选等），使 31-firmware-architecture.md 不再是这些约束的唯一出处 |
| doc 基线 | 全文档 | 确立 SDD 为唯一设计基准：`ux-performance-plan.md`（结论已并入 SDD §3.5/§11/§14）与 `esp32c6-fw-design.md`/`esp32c6-fw-coding-plan.md`（C6 侧 LLDD 已落在 `c6_car/doc/`）三份文档删除；`11-requirements.md`/`31-firmware-architecture.md`/`01-getting-started.md`/`README.md` 口径对齐（esp-at/UART 降级为 demo 现状与回退通道） |

---

## 11. 评审需要你拍板的三件事

> **状态更新（2026-09-26，V1.1）**：实物接线已按 §9.1 完成（23-wiring.md V1.5），**两侧固件代码也已落地**（§7.1/§7.2）。三问仍然开放；第 1 问缺的只剩**逻辑分析仪**——两侧验证代码不再缺，但 TC275 侧还需要一次 TASKING IDE 构建确认 QSPI+DMA 链接闭合（本机无 TriCore 工具链，这一条我无法自证）。

1. **G1 前置验证的投入**：需要一个"两台 ESP32 抓参考波形 + 逻辑分析仪"的台架动作。若手边没有逻辑分析仪，G1 可退化为"TC275 发、C6 只看事件/计数"的黑盒判定，但失败时定位成本高得多。
2. **量产基线时钟档**：建议 **5 MHz**（保守、杜邦线可跑、指标全部达标）。若确定量产用定长屏蔽线束，可直接以 10 MHz 为基线目标，省一轮返工。
3. **`TX_RDY` 第二握手线**：本版按不接（留焊位）。如果你希望"遥测刷新零等待"的极致节拍，可以现在就多占一根线（P23.5 ↔ C6 空闲 GPIO）。
