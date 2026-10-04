# TC275 开发者日志字典

版本 V1.1，2026-10-04，适用 APPFW **v1.2.1 起**（V1.1：落地链路改非阻塞字节泵，见下）。ASCLIN0/COM16，115200、8N1。本文是当前日志格式和频率的真源；35/36/37 号文档里的旧数字串保留为历史抓取。

## 输出策略

| 模块 | 稳定状态周期 | 事件策略 |
| --- | --- | --- |
| FUSION / FUSION_CONTROL / FUSION_HEALTH | 5 秒一组 | 健康/动作变化最短间隔 1 秒；进入停车动作立即记录。持续同一故障仍按 5 秒汇总 |
| IMU / IMU_ERROR | 10 秒 | 初始化、失联、恢复保留事件；重探错误按 10 秒节流 |
| TOF / TOF_CONFIG / TOF_ERROR | 10 秒 | 首次失败立即；持续失败每 10 秒带总线与释放快照。初始化/恢复事件保留 |
| WHEELS | 10 秒 | 静止仍输出，用于确认编码器发布与速度状态 |
| SERVO | 运动中 5 秒 | 首次活动立即；之后短暂静止/抖动不重置 5 秒节流，完全静止时不输出 |
| LINK / LINK_SLAVE / LINK_ERRORS / LINK_TRAFFIC | 30 秒一组 | 状态、时钟、READY 变化立即；首次错误立即，持续错误变化最短 5 秒 |
| LINK_COMMAND | 首次立即，重复最短 5 秒 | 同时给本周期拒绝数与累计拒绝数 |
| CAL_SLOT / CAL_FLASH / CAL_SAVE_ERROR / ENCODER_CALIBRATION | 事件 | 启动、保存或标定时记录 |

这里只降低日志频率，采样、控制、ToF 总线速度和网络遥测周期不变。无效从机寄存器不计为真实错误；C6 断开时 `registers_valid=0 data=unknown` 是预期状态。

格式为 `[MODULE] field=value ...`，字段顺序不作为解析契约。`uptime_ms` 是各生产者的 STIME 毫秒时基，32 位回绕，区别于电脑接收时间。日志经跨核环形队列输出，可能有延迟或相邻模块穿插；同一次诊断组共享时间。落地侧（CPU0 `XCORE_logService`，10 ms 控制任务内）是**非阻塞字节泵**：每周期只把字节排进 ASCLIN0 驱动的 256 B 软件 TX FIFO（TX ISR 按线速后台移出），FIFO 满即停、下一周期续排，字节序与 CRLF 帧化在跨周期续排间保持不变——控制任务绝不等待串口线速。积压仍只能按线速清空（115200 波特 ≈ 11.5 KB/s ≈ 每 256 B 行 45 行/秒），突发时的排队与延迟由这条线速决定；环满丢整行（旧实现的"每周期最多排一行"及其最坏 ~22 ms 的控制任务阻塞已移除）。单行最长 256 字节，长记录在完整字段之间分行并重复模块标签，续行可能没有时间字段。标签/字段名最多 48 字节；过长字符串会以 `...` 明示截断。当前字段均在限制内。日志队列满时丢整行，累计量见 `log_dropped_total`。

单位：`_ms` 毫秒，`_hz` 赫兹，`_mm` 毫米，`_mm_s` 毫米/秒，`_mg` 千分之一重力加速度，`_mdps` 千分之一度/秒，`_centi_c` 百分之一摄氏度，`_pct_x10` 百分比乘 10（250=25%，-1000=-100%）。`_total` 是本次启动以来累计计数，可回绕。布尔字段 1=成立/0=不成立；`*_level` 1=引脚高电平/0=低电平，不能直接当设备健康。`0x...` 为原始十六进制，不转成带符号十进制。

## 融合与驱动

| 模块 / 字段 | 含义 |
| --- | --- |
| FUSION `action` | 本轮对驾驶请求的动作：`none` 无干预；`limit_speed` 限速；`stop_obstacle` 障碍停车；`stop_tof_unavailable` 前向距离不可用；`stop_tilt` 倾斜停车；`stop_encoder_unavailable` 编码器不可用 |
| `forward` | 前进许可：`allowed`、`limited_tof_coverage`（仅低速）或 `blocked_wheel_calibration`/`blocked_encoder`/`blocked_tof`/`blocked_near_obstacle`/`blocked_release_required`/`blocked_tilt`。它与 action 分开：静止时可以 action=none 但前方太近，前进仍被禁止 |
| `nearest_mm` | 融合有效区中的最近障碍距离；0=无有效距离，并非贴着障碍 |
| `forward_cap_mm_s` | 当前安全包络允许的最大前进速度；0=前进不可用；不是用户请求速度 |
| `speed_mm_s` | 融合后的纵向速度，正向前、负向后 |
| `tof_age_ms` | 最近 ToF 帧的年龄，饱和到 65535；必须结合 tof_valid 读，不能仅凭 alive 判断帧新鲜 |
| `valid_zones` | 本帧通过距离与目标状态过滤的区域数；ToF 有效还要求至少半数区域有效（16 区≥8，64 区≥32）、中央区域有目标、帧龄≤250 ms |
| FUSION_CONTROL `target_left_pct_x10` / `target_right_pct_x10` | 融合限速/停车后的左右侧有效速度目标 |
| `brake` | 本轮融合要求制动，不等同整车急停故障码 |
| `health_flags` | 位掩码：0x01 ToF 覆盖充分，0x02 IMU 新鲜，0x04 编码器新鲜，0x08 轴向已标定，0x10 轮速与陀螺不一致，0x20 陀螺零偏就绪，0x40 停车锁存需松杆，0x80 ToF 持续更新但覆盖不足（低速降级） |
| FUSION_HEALTH `tof_valid` / `imu_fresh` / `encoder_fresh` | 对应上述健康位，强调数据新鲜而非有无供电 |
| `imu_axes` | `calibrated`/`uncalibrated`；未标定时不启用依赖车体轴向的航向/倾斜功能 |
| `gyro_bias` | `learning`/`ready`；零偏就绪不能代替车体轴向标定 |
| `wheel_gyro_mismatch` | 轮速与陀螺不一致提示，可能是滑移；日志不把它直接宣称为确定打滑 |
| `release_required` | 松开驾驶请求后才能解除停车锁存 |
| `log_dropped_total` | 跨核日志队列满导致的丢行累计数，应为 0 |
| WHEELS `left_mm_s` / `right_mm_s` | 编码器左右侧实测速度，正向前、负向后 |
| `session_odo_mm` | 当前上电会话累计里程，绝对路程而非带符号位移 |
| `recent_encoder_edges` | 最近窗口是否见到编码器边沿；静止为 0 正常，不等同 encoder_fresh=0 |
| SERVO `target_*_pct_x10` / `measured_*_pct_x10` / `duty_*_pct_x10` | 每侧的速度目标、编码器归一化实测速度、实际 PWM 占空比；不是 mm/s |
| ENCODER_CALIBRATION `invert_A/B/C/D` | 四电机编码器判向反转开关，1=反转 |
| `delta_A/B/C/D_counts` | 判向标定运行期间的四路编码器累计变化量，带符号计数 |

## IMU

| 字段 | 含义 |
| --- | --- |
| `alive` | 驱动认为设备可用；结合 imu_fresh 判断数据新鲜 |
| `axes=sensor_raw` | X/Y/Z 是传感器原生轴，尚不能当车头/车左/车上 |
| `whoami` | 探测缓存的芯片身份字节，当前期望 0x71；缓存存在不证明当前通信健康 |
| `acc_x/y/z_mg` | 原生三轴加速度，包含重力 |
| `gyro_x/y/z_mdps` | 原生三轴角速度 |
| `temp_centi_c` | 芯片温度，例如 2500=25.00°C |
| `drdy_edges_total` | DRDY 中断边沿累计，结合采样时基与计数差值判断 |
| `spi_errors_total` | IMU SPI 事务错误累计 |
| IMU_ERROR `cause` | ok / invalid_parameter / identity_mismatch / spi_busy / spi_hw_error / spi_timeout |
| `spi_hz` | 驱动报告的量化 SPI 频率；不是示波器实测频率，时钟树异常仍需测量 |

## ToF

| 模块 / 字段 | 含义 |
| --- | --- |
| TOF `state` | dead / probe / init / config / ranging 状态机阶段 |
| TOF_ZONE `frame` / `zone` / `distance_mm` / `status_raw` / `targets` | 10 秒摘要中的各区域原始数据：累计帧号、传感器顺序索引、毫米距离、ULD 目标状态、目标数；状态 255 且 targets=0 的距离不能当作有效障碍 |
| `alive` | ULD 探测/驱动可用状态；能否用于驾驶还要看 FUSION tof_valid |
| `frames_total` / `errors_total` | 完整测距帧累计 / 驱动失败累计 |
| `nearest_mm` | 驱动缓存最近有效目标距离，0=没有有效目标；旧缓存必须结合融合帧年龄判读 |
| `target_status_raw` | 上述目标的 ULD 状态；本驱动接受 5/9，0 通常对应无选中目标。不是 I2C 错误码 |
| `int_level` | 模块 INT 引脚电平，低有效；高电平本身不证明故障 |
| TOF_CONFIG `init_ms` | 最近一次 ULD 初始化耗时 |
| `i2c_hz` | 分频后报告的 I2C 频率，非示波器实测频率 |
| `zones` | 输出区域数（16 或 64） |
| `device_id` / `revision_id` | 缓存身份字节，期望 0xF0/0x02；未读到时为 0 |
| TOF_ERROR `cause` | ok / invalid_parameter / no_ack / bus_error / timeout / uld_error |
| `uld_status` | ST ULD 原始状态/组合错误，不能当驱动 cause 枚举解码 |
| `config_step` | 配置子步骤：0 尚未配置，1 唤醒，2 分辨率，3 测距模式，4 积分时间，5 目标排序，6 帧率，7 启动测距；失败时保留最后执行步骤 |
| TOF_BUS `phase` | 首故障发生阶段：none / bus_free / tx_space / tx_request / tx_end / rx_mode / rx_request / stop |
| `direction` / `register` / `length_bytes` | read 或 write / 16 位寄存器地址 / 该调用请求的字节数 |
| `protocol_irq` / `error_irq` / `raw_irq` | 首故障时 I2C0 PIRQSS / ERRIRQSS / RIS 原始寄存器快照，位定义见 iLLD 与 TC27x 手册 |
| `bus_state_raw` | BUSSTAT.BS：0 idle，1 started，2 busyMaster，3 remoteSlave |
| `fifo_words` | FFSSTAT.FFS 的 FIFO 填充字数 |
| `scl_level` / `sda_level` | 首故障时实测管脚逻辑电平 |
| `packet_bytes` | 当前 TX 包总字节数，包括总线地址字节 |
| `wire_address` | 实际入队的 8 位地址字节，例如 0x52=7 位 0x29 的写地址；不是 7 位地址 |
| TOF_CLEAR `result` | idle=无需释放，released=脉冲后恢复，scl_stuck=SCL 持低，sda_stuck=SDA 仍持低 |
| `pulses` / `scl_level` / `sda_level` | 本次释放脉冲次数 / 释放后的两线电平；恢复成功不证明后续事务一定成功 |
| TOF_GPIO `status_raw` | 启动一次 GPIO 地址探测：0 ACK，1 NACK，2 SCL 持低，3 SDA 持低，4 地址位争用 |
| `address_echo` / `ack_line_level` / `first_different_bit` | 实际采样的地址位 / ACK 位电平（0=ACK）/ 第一个不一致地址位（1..8，0=无不一致）；地址回显可能因提前退出不完整 |
| TOF_ISOLATION `step` / `requested_scl` / `requested_sda` | 仅编译启用隔离测试时输出：步骤号与 GPIO 请求电平；高=开漏释放 |
| `scl_level` / `sda_level` / `port_in` / `port_out` / `iocr4` / `gpctl` / `runctrl` | 实测引脚 / 端口 IN/OUT/IOCR4 / I2C GPCTL/RUNCTRL 原始值 |
| TOF_BIAS `internal_pull_up` | 仅隔离测试：1 内部弱上拉，0 无内部上下拉；其余字段同上 |
| TOF_HOLD | 仅隔离测试驻留状态：两线电平与端口/I2C 快照，正常固件不输出 |

## 板间链路与标定存储

| 模块 / 字段 | 含义 |
| --- | --- |
| LINK `state` | down 尚未连通，ready 已连通，lost 存活超时 |
| `registers_valid` | READY 寄存器等于协议魔数 0x5F534601 时为 1 |
| `spi_hz` / `irq_asserted` / `ready_reg` | 当前 SPI 分频后频率 / 从机 IRQ 是否断言 / READY 原始值；断开常见 0xFFFFFFFF |
| LINK_SLAVE `data=unknown` | 从机寄存器未确认有效，不能把其原始值作为容量或错误 |
| `tx_pending_bytes` / `rx_room_bytes` | 从机声称的待发字节数 / 接收剩余空间 |
| `alive_age_ms` | 自从机 ALIVE 最后前进以来的毫秒数；旧底层的 0 也可能表示尚未见到 ALIVE，结合 LINK 状态看 |
| `error_bits` | 从机 ERRSTAT 原始位掩码 |
| LINK_ERRORS `spi_timeout_total` / `spi_hw_error_total` | 主机 SPI 超时 / QSPI 硬件错误累计 |
| `spi_error_total` / `crc_error_total` / `sequence_error_total` | 链路事务失败 / SF CRC 拒绝 / SF 序号拒绝累计 |
| `tx_queue_full_total` / `cmd_rejected_total` | 主机发送队列满 / CPU0 命令队列拒绝累计 |
| LINK_TRAFFIC `spi_transactions_total` | 主机 SPI 事务累计；递增只能证明主机尝试通信，不能证明 C6 收到 |
| `write_segments_total` / `read_segments_total` / `tx_frames_total` | WRDMA 分段 / RDDMA 分段 / 已发 SF 帧累计 |
| LINK_COMMAND `rejected_since_last_log` / `rejected_total` | 上一次拒绝日志后新增拒绝数 / 本次启动累计拒绝数 |
| CAL_SLOT `stage` / `address` | boot / before_erase / after_save_failure / DFlash 槽地址 |
| `bytes_0_3` 至 `bytes_16_19` | 前 20 个原始字节每四字节按显示顺序拼成十六进制；不是 CPU 本机字序整数。记录格式见 34 号与 calib_record.c |
| CAL_FLASH `sector_address` | 本次操作所选 DFlash 扇区地址 |
| CAL_SAVE_ERROR `step_raw` | 1 擦除，2 页模式，3 编程，4 校验，5 ENDINIT |
| `fsr_before` / `fsr_after_command` / `fsr_failure` | FLASH0_FSR 的操作前 / 命令后 / 判失败时快照 |

## 示例与维护

`[FUSION] action=none forward=blocked_near_obstacle nearest_mm=75 forward_cap_mm_s=0 ...` 表示目前静止、前方太近，禁止前进；不是融合故障。

`[IMU] axes=sensor_raw acc_x_mg=680 gyro_z_mdps=350 temp_centi_c=2150 ...` 表示原生 X 轴 0.680g、Z 轴 0.350°/s、21.50°C。

新增日志使用 `XCORE_LOG_FIELDS` 和 XL_U/XL_I/XL_H/XL_S（单次宏 1..16 字段），并在本文补充单位和异常含义；不要重新引入无字段名数字串。格式化在锁外完成；锁只保护入队与丢行计数，不调用 printf 或分配堆内存。主机 test_xcore 覆盖数值极限、字段分行、字符串界限、溢出可见性、状态变化节流与时钟回绕。



## 本轮验证

- TASKING Debug 构建通过：ROM 188081 B，RAM 83530 B；最终 v1.2.1 镜像通过 `just fw-flash app --no-build` 写入与读回校验。
- COM16 从启动采集约 45 秒，确认 APPFW v1.2.1 与 SBLFW v1.0.1。丢弃 APPFW 横幅前串口缓存的上一运行记录后：IMU、WHEELS 间隔均为 10000 ms；TOF 为 10000–10003 ms；稳定 LINK 诊断组间隔 30000 ms；FUSION 稳定 5000 ms，状态变化出现 1000/2080 ms 的提前记录。
- 这轮设备静止，无 SERVO 周期行；主机直接抽取生产 `MOTOR_ALGO_diag` 验证编码器短暂活动、静止间隙和再次活动不会绕过 5 秒节流。未发送驱动车辆指令。
- IMU/ToF 错误累计为 0，ToF 帧持续更新，I2C 报告频率 990099 Hz；gyro_bias=ready、imu_axes=uncalibrated、log_dropped_total=0。现场 LINK 为 ready；断开 C6 的无效寄存器处理通过主机模型验证。
- `test_xcore`：1144 checks / 0 failures（新增用例的日志环溢出改写 300 行，100 行 ×9 B 不足以填满 2048 B 环）；`test_log_policy.py`：无效从机 0xFFFFFFFF、稳定心跳、状态变化、重复错误节流、伺服抖动通过；ToF 总线/帧更新、融合与状态回传、标定持久化回归通过。

新增策略测试运行：`CC=gcc python test/host/test_log_policy.py`（Windows 设置 `$env:CC`，并确保 GCC 的运行时目录在 PATH；CI 的 Linux runner 用默认 `cc`，已作为独立步骤运行）。它抽取生产日志函数，硬件和时钟由主机替身提供，不验证真实 SPI 电气时序。
