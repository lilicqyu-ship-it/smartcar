# 36 · VL53L5CX 驱动与 I2C 首验故障修复

| 项 | 内容 |
|---|---|
| 版本 | V1.1（2026-10-04）：复测仍失败、berr=3 且未捕获波形；增加清标志/STOP/复位前的首故障快照。V1.0：FIFO 握手修复 |
| 实现 | `bsp/tof.c`、`bsp/tof.h`、ST ULD 客户端 `platform.h` |
| 接线真源 | 23 §11：I2C0，SCL=P13.1、SDA=P13.2、INT=P10.7 |
| 验证边界 | TASKING 构建和主机请求模型通过；修复后的硬件测距未复测 |

## 1. 现场证据

用户提供 `DSLogic PLus-la-261004-005431.csv`，50 MHz 数字采样，约 1.018 s 的采集窗口。根据 START/时钟边沿推断通道 0=SCL、通道 1=SDA；CSV 本身没有信号名。有效活动集中在 0.400～0.480 s。

| START（s） | 地址字节 | 第九位 | ACK 后 SCL 低电平 |
|---|---|---|---|
| 0.40002250 | 0x52（7 位地址 0x29，写） | 0，ACK | 19.9708 ms |
| 0.42004348 | 0x52 | 0，ACK | 19.9497 ms |
| 0.44004310 | 0x52 | 0，ACK | 19.9498 ms |
| 0.46004280 | 0x52 | 0，ACK | 19.9496 ms |

每次地址均被应答，但没有后续索引/载荷，低电平等待接近 `TOF_XFER_TIMEOUT_MS=20` 后总线重新释放。四次地址对应 ULD `is_alive` 的四次平台调用，不代表应用每 20 ms 重探。探测重试仍为 1 Hz。数字 CSV 不能测模拟上升时间，也不能仅凭 ACK 证明芯片身份。

日志 `TOF 1 0 0 8 0 398406 16 0 0 1 0 0` 含义：

| 字段（从 1 开始） | 含义 | 本次值 |
|---|---|---|
| 1～3 | 状态、alive、成功帧数 | PROBE、0、0 |
| 4～6 | 失败 I2C 消息数、初始化耗时 ms、iLLD 反算总线 Hz | 8、0、398406 |
| 7～9 | 区数、最近距离 mm、目标状态 | 16、0、0 |
| 10～12 | INT 原始电平、device ID、revision ID | 1、0、0 |

`TOFERR berr/st/step/err=4 66 0 12`：4=总线超时；66=ULD `VL53L5CX_MCU_ERROR`（0x42）；step 0=探测阶段；12=失败消息累计。每次探测四个失败消息，所以每两秒 TOF 行 +8。ID=0/0 是失败读取清零，不是读到了一个身份为零的设备；固件下载尚未开始。

## 2. 上游边界

ST ULD 在 `Libraries/ST/vl53l5cx`，现有源文件声明来自 `STMicroelectronics/stm32-vl53l5cx` v1.0.7；本次没有改上游文件。客户平台回调在 `bsp/tof.c`，配置声明在客户 `platform.h`。

索引为 16 位高字节在前，读写均发送原始索引。读写方向只在从机地址最低位：0x52 写、0x53 读。读寄存器 0x0000 时索引应为 `00 00`，不是 `80 00`。依据 [ST DS13754 §4](https://www.st.com/resource/en/datasheet/vl53l5cx.pdf)。

## 3. 集成与属主

CPU0 `vTofTask` 是唯一 I2C0 属主，优先级低于 robot 任务；CPU1/CPU2 不操作 I2C0。ULD 头文件用相对于 `bsp/tof.c` 的路径包含。本次未改变任务、SPI 链路、看门狗、标定或运动控制。

## 4. 根因与修复

本地 iLLD `IfxI2c_configureAsMaster()` 设置 `TXFC=RXFC=1`、burst=1 word。原 `tof_txFill()` 写 FIFO 后直接返回，`tof_probeAddress()` 等待 `TX_END`，却把 FIFO 请求清除放在等待之后。iLLD 原件在地址写入后立即清请求，在载荷中逐字等待请求并清除。现场地址 ACK 后停顿与此遗漏吻合。

现实现每个 TX word：检查 FIFO 空间 → 写入 → 有界等待 DTR 请求 → 清请求。只接受四种 DTR 位，不能把 RX 模式等协议位误当成 FIFO 请求。每个 RX word：读地址 TX 握手 → RX 模式 → 等待 RX 请求 → 读 RXD → 清请求，最后才等 `TX_END`。协议等待同时检查 NACK、仲裁丢失、FIFO 错误，防止把这些故障都拖成超时。

全部等待保持中断开启，使用 FreeRTOS tick 20 ms 界限。超时或总线/FIFO 错误重置 I2C0；读取失败（包括 STOP 失败）将目标缓冲区清零。写载荷块 24 B，读块 32 B，每个块有显式索引，不能跨越 16 位地址边界。

请求流控说明可参见 [Infineon I2C 文档](https://documentation.infineon.com/aurixtc3xx/docs/ijt1710345337103)；TC275 实际调用顺序以本仓库 TC27D iLLD 原件为依据，未用 TC3xx 引脚或寄存器配置替代 TC27D。

## 5. 测距配置

当前 4×4、15 Hz、5 ms integration、autonomous、closest、每区一个目标。输出保留目标数/距离/状态，临时缓冲区下限 1024 B。`TOF_CFG_8X8` 可切换 8×8。INT 日志仅为原始电平，不能用静态高电平判断测距成功；数据就绪由 ULD 轮询确认。距离尚未接入避障消费路径。

## 6. 验证与复测

主机回归执行生产 `tof.c` 的总线函数，使用请求未应答便不产生完成事件的 I2C 模型：

```powershell
$env:PATH='C:\msys64\ucrt64\bin;'+$env:PATH
python test/host/test_tof_bus.py --cc C:\msys64\ucrt64\bin\gcc.exe
scons -Q
```

覆盖单字节/24 B 写、1～32 B 读、逐字请求、地址及索引字节顺序、NACK/仲裁丢失/FIFO 错误、请求缺失和 FIFO 满超时、地址边界。模型不能代替硬件电气验证。TASKING Debug 编译和链接通过，生成 `build/tasking-debug/tc275_car_v1.1.2.hex`。

上板复测判据：

1. 抓包应在地址 ACK 后继续出现索引/载荷，消除每次固定约 20 ms 的停顿；第一条页选择写可见 `52 7F FF 00`（其前还有单独的地址探测及重复 START）。
2. ID 应为 `240 2`（F0/02），初始化耗时非零，出现 `ToF ranging ...`，状态进入 4、alive=1，成功帧计数持续递增。
3. 手置于前方 10～30 cm，距离随位置变化；错误计数在稳定运行时不再增长。先验证 4×4，再考虑 8×8/提高总线速度。

同段日志里的 LINKERR 是另一条 SPI 链路：READY=0x5F534601、state=1，TC275 timeout/hwErr/spiErr/crc/seq 均为 0；C6 sticky errStat=7 表示 CRC/FMT/SEQ 历史标志，txQFull=1、cmdqRej=7 为累计背压/拒收。两行计数未增长，不能由此断言 SPI 正在持续报错，也不能归因于 ToF。进一步判断需要 C6 同期日志或清除历史标志后的新计数；本次没有依据修改 SPI。

## 7. 第二次上板结果与诊断固件

用户复测：`TOFERR 3 66 0 12/20/28/36`，PROBE/ID=0/0 不变，逻辑分析仪未捕获波形。**首轮修复尚未让传感器初始化成功**。错误从 4 变成 3 仅表示软件检测到总线错误，不能证明传输已推进。3 合并了仲裁丢失、FIFO 错误及入口总线不空闲，旧日志缺少区分证据，不应猜测某一种为已证实根因。

此次用户确认 C6 未通电/连接，因而 `LINKERR state=0`、READY/ERRSTAT 等全部 4294967295（0xFFFFFFFF）符合无从机状态，本批不修改 SPI。

诊断固件在 TOFERR 后增加同节律日志（十进制）：

```text
TOFBUS phase/rw/reg/len/pirq/err/ris/bs/ffs/scl/sda= ...
```

每条 I2C 事务保存首次故障，后续 STOP 失败不能覆盖它；捕获发生在错误标志清除及模块复位之前。SCL/SDA 是当时 P13.1/P13.2 的原始输入电平，不能据此推断谁驱动了低电平。

| phase | 失败位置 |
|---|---|
| 1 | 入口总线不空闲，尚未设置本条事务 TPS |
| 2 | TX FIFO 空间等待 |
| 3 | 写 FIFO 后等待 TX DTR 请求 |
| 4 | 协议 TX_END 等待/检查 |
| 5 | 读地址后 RX 模式等待 |
| 6 | RX DTR 请求等待 |
| 7 | STOP |

`rw`：0 写、1 读；`reg/len`：本次寄存器与长度。`pirq` 的 bit3=仲裁丢失（掩码 8），bit4=NACK（16）；`err` 的 bit0=RX underflow（1）、bit1=RX overflow（2）、bit2=TX underflow（4）、bit3=TX overflow（8），按位解码，可能同时置位。依据本仓库 TC27D `IfxI2c_regdef.h`，不是依据日志标签猜测。

主机模型验证快照保存了清除前 AL/NACK/FIFO 错误、STOP 不覆盖首故障、入口总线忙读取清零。诊断增加了寄存器读取，不改变 I2C 配置及传输策略；测距故障仍待这份固件的 TOFBUS 行或新抓包确定。

无波形复测时先使用无协议触发采集：通道 0 接 P13.1（SCL）、通道 1 接 P13.2（SDA）、分析仪与 TC275 共地，连续采集至少 3 s。同时回传 TOFERR/TOFBUS：若 phase=1，重点看 bs 和引脚电平；若 err 非零，按 FIFO 标志定位；若 pirq 含 AL，核对 SDA 在主控释放时是否仍被拉低。数字采集无活动不能独立证明没有 START（触发、采集窗口或接线也须排除）。
